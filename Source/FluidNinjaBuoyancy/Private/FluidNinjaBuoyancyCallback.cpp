#include "FluidNinjaBuoyancyCallback.h"

#include "Chaos/Framework/Parallel.h"
#include "Chaos/Particle/ParticleUtilities.h"
#include "FluidNinjaBuoyancyStats.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeExit.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "Stats/Stats.h"

namespace
{
	/** fnb.Physics.Parallel: 1 steps the bodies in parallel when there are four or more (Chaos' own switch, p.Chaos.DisablePhysicsParallelFor,
	 * still rules), 0 one after the other on the physics thread. The forces reach the particles in the bodies' order either way. */
	int32 PhysicsParallel = 1;
	static FAutoConsoleVariableRef CVarPhysicsParallel(TEXT("fnb.Physics.Parallel"), PhysicsParallel,
		TEXT("1 (the default; any value but 0): the water's forces on the floating bodies are computed in parallel on the physics step when there are four bodies or more ")
		TEXT("(p.Chaos.DisablePhysicsParallelFor still rules); 0: one body after the other on the physics thread. The forces are the same either way; 0 only costs time."));

	/** The rest radiation boost (the radiation damping in OnPreSimulate_Internal): how many times the physical radiation damping a body at
	 * rest gets, fading to the physical value as it moves faster than the Fast thresholds. 1 is no boost, which Phase 2's added mass and
	 * a radiation damping at the body's own frequency should allow without a rebuild. The thresholds are for a body of about 50 cm and
	 * scale with the body's size (see the callback). */
	float RestBoost = 1.f;
	float RestBoostHeaveSlowCmS = 4.f;
	float RestBoostHeaveFastCmS = 12.f;
	float RestBoostSpinSlowRadS = 0.03f;
	float RestBoostSpinFastRadS = 0.1f;
	static FAutoConsoleVariableRef CVarRestBoost(TEXT("fnb.Physics.RestBoost"), RestBoost,
		TEXT("FluidNinja Waves: how many times its physical radiation damping a floating body at rest gets (bodies with waves and radiation damping only). ")
		TEXT("1 (the default): no boost, now that the added mass and the damping at the body's own frequency are in; 3 was the calibration before them. ")
		TEXT("Above 1 holds resting floaters stiller; below 1 weakens their damping at rest; negative counts as 0. ")
		TEXT("Fades to 1 between the fnb.Physics.RestBoost*Slow* and *Fast* thresholds as the body moves faster."));
	static FAutoConsoleVariableRef CVarRestBoostHeaveSlow(TEXT("fnb.Physics.RestBoostHeaveSlowCmS"), RestBoostHeaveSlowCmS,
		TEXT("Heave speed (cm/s, for a body of 50 cm; scaled by the body's height over 50 cm) up to which the rest radiation boost (fnb.Physics.RestBoost) is whole. ")
		TEXT("Default 4; keep it below fnb.Physics.RestBoostHeaveFastCmS. No effect while fnb.Physics.RestBoost is 1."));
	static FAutoConsoleVariableRef CVarRestBoostHeaveFast(TEXT("fnb.Physics.RestBoostHeaveFastCmS"), RestBoostHeaveFastCmS,
		TEXT("Heave speed (cm/s, for a body of 50 cm; scaled by the body's height over 50 cm) from which the rest radiation boost (fnb.Physics.RestBoost) is gone. ")
		TEXT("Default 12; keep it above fnb.Physics.RestBoostHeaveSlowCmS. No effect while fnb.Physics.RestBoost is 1."));
	static FAutoConsoleVariableRef CVarRestBoostSpinSlow(TEXT("fnb.Physics.RestBoostSpinSlowRadS"), RestBoostSpinSlowRadS,
		TEXT("Roll or pitch rate (rad/s, for a body of 50 cm; scaled by 50 cm over the body's height) up to which the rest radiation boost (fnb.Physics.RestBoost) is whole. ")
		TEXT("Default 0.03; keep it below fnb.Physics.RestBoostSpinFastRadS. No effect while fnb.Physics.RestBoost is 1."));
	static FAutoConsoleVariableRef CVarRestBoostSpinFast(TEXT("fnb.Physics.RestBoostSpinFastRadS"), RestBoostSpinFastRadS,
		TEXT("Roll or pitch rate (rad/s, for a body of 50 cm; scaled by 50 cm over the body's height) from which the rest radiation boost (fnb.Physics.RestBoost) is gone. ")
		TEXT("Default 0.1; keep it above fnb.Physics.RestBoostSpinSlowRadS. No effect while fnb.Physics.RestBoost is 1."));

	/** fnb.Physics.AddedMassWaterAccel: 1 pushes the heave added mass along with the vertical acceleration of the waves under the body,
	 * A·a_w (TrackWaterAcceleration, StabilizeStep's WaterAccelerationZ); 0 leaves the push out while the added mass stays in the step's
	 * mass, for A/B runs against the term. The tracking runs either way, so switching it back on mid-run uses a settled estimate. */
	int32 AddedMassWaterAccel = 1;
	static FAutoConsoleVariableRef CVarAddedMassWaterAccel(TEXT("fnb.Physics.AddedMassWaterAccel"), AddedMassWaterAccel,
		TEXT("1 (the default; any value but 0): a floating body's heave added mass is pushed along with the vertical acceleration of the waves under it (the others' waves, ")
		TEXT("low-passed and bounded); 0: no push, the added mass still weighs in the step (A/B tests). The estimate keeps running either way."));

	/** Patches further apart than this (s) are not differenced: the body took turns at the gather, or its water came back late. */
	constexpr double WaterAccelerationMaxGapSeconds = 0.25;
	/** A felt wave rising or falling faster than this (cm/s) is a jump (the body put somewhere, the waves drawn afresh), not water moving:
	 * the samples start over. */
	constexpr float WaterAccelerationMaxRateCmS = 1000.f;
	/** A patch whose gap from the patch before differs from the running median gap by more than this share of the median is left out. */
	constexpr double WaterAccelerationGapTolerance = 0.5;
	/** The low-pass's cut-off (Hz), and the most of the patches' Nyquist frequency, 1 / (2 × the median gap), it may reach: 3.75 Hz is a
	 * quarter of the 15 Hz at 30 fps, so 30 and 60 fps filter alike, and above the heave the term is for (0.6-1.2 Hz for the bench's
	 * crates with their added mass, by their footprint); a body whose patches come further apart is filtered lower. */
	constexpr double WaterAccelerationCutoffHz = 3.75;
	constexpr double WaterAccelerationCutoffOfNyquist = 0.25;
	/** The felt wave's slow mean (s): taken out of what the bound reads, so a steady residue of B - B_self (a static head the own field
	 * does not quite cancel) does not hold the bound open. A wave of period 2π s keeps 95 % of its swing through it (and leads by 18°,
	 * less on faster waves), so on waves of 0.25-0.8 Hz, under the 20 kg crate's corner of 0.88 Hz, the bound still keeps 94-100 % of
	 * the term's in-phase part (an offline run of this estimator on jittered patches). */
	constexpr double WaterAccelerationMeanSeconds = 3.0;
	/** c of the bound |A·a_w| ≤ c·k·|ζ'| (TrackWaterAcceleration): the push is held within half what the same swing of the felt wave gives
	 * the body's waterplane. */
	constexpr float WaterAccelerationBound = 0.5f;

	/**
	 * The water's vertical acceleration under the body, a_w (cm/s²), for its heave added mass A (FluidNinjaBuoyancy::StabilizeStep pushes
	 * A·a_w along with the water). Only the waves the body did not make count: ζ = κ (B - B_self), the patch's Wave at the body's centre of
	 * mass times its feel κ (FWaterPatch::Sample). FluidNinja's own drawn surface W is left out: a 2D flow's picture whose height changes
	 * slowly, and into which FluidNinja's tracking paints part of the body's own push, so its second difference echoed the body's heave.
	 *
	 * Why an echo rings. Heave: (m + A) z̈ + b ż + k z = A a_w, k = ρ g A_wp (FStepResult::HeaveStiffness). If a_w holds the body's own
	 * heave, a_w = ε z̈(t - d) (ε of its motion read back as water; d the readback's 0.03-0.05 s plus this estimator's lag, about 0.06 s
	 * more: half the second difference's span and the low-pass's phase), then at s = iΩ the echo takes ε A cos Ωd off the mass and adds a
	 * damping -ε A Ω sin Ωd, an injected damping ratio
	 *     ζ_echo = μ ε sin Ωd / (2 (1 - μ ε cos Ωd)),   μ = A / (m + A),   Ω² = k / ((m + A)(1 - μ ε cos Ωd)),
	 * which grows without bound as μ ε nears 1 (past 1 the neutral equation's roots s = ln(μ ε)/d + 2πin/d are unstable at every n). With ε
	 * near 1, d 0.04 s and k from the crate's footprint (81 cm square for its 104 kg of added mass): the 20 kg demo crate (μ 0.84) gets
	 * about 0.77, the 80 kg one (0.57) 0.2, the 320 kg one (0.25) 0.03; the first bobbed for ever, the others rest. Leaving W out takes the
	 * echo's main path away; the bound keeps what is left harmless.
	 *
	 * The bound: |A a_w| ≤ c k |ζ'|, c = 0.5, ζ' the felt wave's swing about its slow mean through the same low-pass, in phase with a_w (the
	 * callback applies it with the step's own k and A). For a wave of frequency ω, A a_w = -A ω² ζ: the term is whole while A ω² ≤ c k,
	 * ω ≤ ω_A √c with ω_A = √(k / A) (for a square footprint B wide, A = ρ π B³ / 16 by strip theory, so the deep-water waves 2.5 B long and
	 * longer, those the long-wave added mass describes), and on shorter waves it is held at half the Froude-Krylov push of the same wave.
	 * On an echo, ζ = ε z(t - d), the bound engages once A Ω² > c k (for ε near 1, μ above about c / (1 + c), a third) and turns the term
	 * into a delayed spring of at most c ε k, which injects c ε sin Ωd / (2 (1 + c ε cos Ωd)), Ω² = k (1 + c ε cos Ωd) / (m + A), and never
	 * more than c ε / (2 √(1 - c² ε²)) at any delay (0.29 for c = 0.5 and ε = 1; c = 1 has no ceiling). Over all μ and ε ≤ 1, at d ω_n
	 * up to 0.75 (ω_n² = k / (m + A); the 20 kg crate's 7.2 rad/s at d 0.1 s): at most 0.19 with c = 0.5, at the edge of engaging, and 0.4
	 * with c = 1 (at μ near 0.6). So with c = 0.5 no body's echo, whatever its added mass, outgrows what the 80 kg crate carried before and
	 * rested with. It also holds the noise: a flicker of ζ pushes at most c k times itself, half what the body's buoyancy already takes
	 * from it (so the old deadband on the rate, against W's flicker, is gone).
	 *
	 * Against the readback's jitter: ζ is taken once per new patch, at the patch's own time (the gather's dispatch). A patch whose gap
	 * from the one before differs from the running median of the last nine gaps by more than half the median is left out (a short gap
	 * raises the second difference's noise as 1/gap², a long one is a late readback); the gap still joins the median, so a new rhythm (the
	 * body taking turns at the gather) is taken up within five patches, and the next patch is differenced against the last ones used,
	 * over the true times. Three samples give the second difference exact for a parabola at uneven times, 2 ((ζ2 - ζ1)/h2 - (ζ1 - ζ0)/h1)
	 * / (h1 + h2), the acceleration at the middle sample (the old (rate - previous rate) / gap came out 1.5 and 0.75 times it on
	 * alternating 2- and 1-frame gaps), held within ±2 g, then low-passed by one first-order stage at the cut-off above (stepped by the
	 * time between middle samples, so uneven gaps filter alike). The sample follows the body, so a hull moving along a wave reads the
	 * water's acceleration where it is; the frame's steps all read its last patch's. A gap past 0.25 s, no waves (κ 0, or none on the
	 * patch) or no water under the centre start everything over, and a jump in ζ or 0.25 s since the last sample used start the samples
	 * over: a_w is 0 until three more patches, and rises in with the low-pass.
	 */
	void TrackWaterAcceleration(FFluidNinjaBuoyancyWaterTrack& Track, const FluidNinjaBuoyancy::FWaterPatch& Patch, float WaveFeel, const FVector& At, float Gravity)
	{
		if (Patch.Time == Track.SeenTime)
		{
			return;   // this patch is taken already (the frame's other steps)
		}
		const double Gap = Patch.Time - Track.SeenTime;
		Track.SeenTime = Patch.Time;
		float Height = 0.f;
		float Rate = 0.f;
		FVector2f Flow = FVector2f::ZeroVector;
		float Wave = 0.f;
		if (WaveFeel == 0.f || !Patch.bHasWave || !Patch.Sample(At.X, At.Y, Height, Rate, Flow, &Wave) || Gap <= 0.0 || Gap > WaterAccelerationMaxGapSeconds)
		{
			Track.Clear();
			return;
		}

		// The gap against the running median of the gaps before it (once there are three), sorted by insertion: at most nine, once a patch.
		double Rhythm = Gap;
		bool bOddGap = false;
		if (Track.NumGaps >= 3)
		{
			double Sorted[FFluidNinjaBuoyancyWaterTrack::GapWindow] = {};
			for (int32 Index = 0; Index < Track.NumGaps; ++Index)
			{
				int32 Place = Index;
				while (Place > 0 && Sorted[Place - 1] > Track.Gaps[Index])
				{
					Sorted[Place] = Sorted[Place - 1];
					--Place;
				}
				Sorted[Place] = Track.Gaps[Index];
			}
			Rhythm = Sorted[Track.NumGaps / 2];
			// 1 µs for the times' rounding: a gap of two frames against a median of one is out, one against two is exactly half off and in.
			bOddGap = FMath::Abs(Gap - Rhythm) > WaterAccelerationGapTolerance * Rhythm + 1e-6;
		}
		Track.Gaps[Track.NextGap] = Gap;
		Track.NextGap = (Track.NextGap + 1) % FFluidNinjaBuoyancyWaterTrack::GapWindow;
		Track.NumGaps = FMath::Min(Track.NumGaps + 1, FFluidNinjaBuoyancyWaterTrack::GapWindow);
		if (bOddGap)
		{
			++Track.SkippedPatches;
			return;
		}

		const float Felt = WaveFeel * Wave;
		if (Track.NumSamples > 0)
		{
			const double Since = Patch.Time - Track.Times[Track.NumSamples - 1];
			if (Since > WaterAccelerationMaxGapSeconds || FMath::Abs(Felt - Track.Waves[Track.NumSamples - 1]) > WaterAccelerationMaxRateCmS * Since)
			{
				Track.ClearSamples();
			}
		}
		if (Track.NumSamples == 3)
		{
			Track.Times[0] = Track.Times[1];
			Track.Times[1] = Track.Times[2];
			Track.Waves[0] = Track.Waves[1];
			Track.Waves[1] = Track.Waves[2];
			Track.NumSamples = 2;
		}
		Track.Times[Track.NumSamples] = Patch.Time;
		Track.Waves[Track.NumSamples] = Felt;
		++Track.NumSamples;
		++Track.UsedPatches;
		if (Track.NumSamples < 3)
		{
			return;
		}

		const double H1 = Track.Times[1] - Track.Times[0];
		const double H2 = Track.Times[2] - Track.Times[1];
		const double Second = 2.0 * ((Track.Waves[2] - Track.Waves[1]) / H2 - (Track.Waves[1] - Track.Waves[0]) / H1) / (H1 + H2);
		const float Acceleration = FMath::Clamp(static_cast<float>(Second), -2.f * Gravity, 2.f * Gravity);
		// The low-pass steps from the last middle sample it took to this one (the first after starting over spans h1, and starts the slow
		// mean at the sample, so the swing, and the bound, open from 0).
		const double Middle = Track.Times[1];
		const double Step = Track.FilterTime >= 0.0 ? Middle - Track.FilterTime : H1;
		if (Track.FilterTime < 0.0)
		{
			Track.Mean = Track.Waves[1];
		}
		Track.FilterTime = Middle;
		const double Cutoff = FMath::Min(WaterAccelerationCutoffHz, WaterAccelerationCutoffOfNyquist / (2.0 * Rhythm));
		const float Alpha = static_cast<float>(1.0 - FMath::Exp(-2.0 * UE_DOUBLE_PI * Cutoff * Step));
		const float MeanAlpha = static_cast<float>(1.0 - FMath::Exp(-Step / WaterAccelerationMeanSeconds));
		Track.Mean += MeanAlpha * (Track.Waves[1] - Track.Mean);
		Track.Acceleration += Alpha * (Acceleration - Track.Acceleration);
		Track.Swing += Alpha * ((Track.Waves[1] - Track.Mean) - Track.Swing);
	}
}

FName FFluidNinjaBuoyancyCallback::GetFNameForStatId() const
{
	const static FLazyName StaticName("FFluidNinjaBuoyancyCallback");
	return StaticName;
}

void FFluidNinjaBuoyancyCallback::OnPreSimulate_Internal()
{
	using namespace Chaos;
	SCOPE_CYCLE_COUNTER(STAT_FNB_PhysicsStep);
	++Step;
	const FFluidNinjaBuoyancyInput* Input = GetConsumerInput_Internal();
	if (!Input || Input->Bodies.IsEmpty())
	{
		return;
	}
	const double StartSeconds = FPlatformTime::Seconds();
	const float DeltaSeconds = static_cast<float>(GetDeltaTime_Internal());
	// How far into its game frame this step begins. The frame's input carries each body's water as of the frame's start (Extrapolate is
	// the patch's age then), and the steps that share it come one step's time after another, so each carries the water forward by its
	// own offset on top, up to the same cap, rather than every step of the frame reading the surface where its first did. The offset is
	// the simulation time since the first step this input was seen in, so a step run again (a resimulation) reads the same water.
	if (Input->Frame != InputFrame)
	{
		InputFrame = Input->Frame;
		InputStartTime = GetSimTime_Internal();
	}
	const float StepOffset = static_cast<float>(FMath::Max(GetSimTime_Internal() - InputStartTime, static_cast<Chaos::FReal>(0.0)));
	FFluidNinjaBuoyancyOutput& Output = GetProducerOutputData_Internal();
	ON_SCOPE_EXIT
	{
		Output.StepSeconds += static_cast<float>(FPlatformTime::Seconds() - StartSeconds);
	};
	// The tunables read once, so every body of the step sees the same (the console sets them from the game thread).
	const double Boost = FMath::Max(static_cast<double>(RestBoost), 0.0);
	const double HeaveSlow = RestBoostHeaveSlowCmS;
	const double HeaveFast = RestBoostHeaveFastCmS;
	const double SpinSlow = RestBoostSpinSlowRadS;
	const double SpinFast = RestBoostSpinFastRadS;
	const bool bWaterAccel = AddedMassWaterAccel != 0;

	// Pass one, serial: what each body's step needs read off its particle, its entry in States, and its slot in the output. Nothing
	// here reads or writes Chaos once the bodies run.
	Work.Reset();
	Work.Reserve(Input->Bodies.Num());
	Output.Bodies.Reserve(Input->Bodies.Num());
	for (int32 InputIndex = 0; InputIndex < Input->Bodies.Num(); ++InputIndex)
	{
		const FFluidNinjaBuoyancyBodyInput& BodyInput = Input->Bodies[InputIndex];
		if (!BodyInput.Proxy || !BodyInput.Hull.IsValid())
		{
			continue;
		}
		FRigidBodyHandle_Internal* Body = BodyInput.Proxy->GetPhysicsThreadAPI();
		if (!Body)
		{
			continue;
		}
		const EObjectStateType State = Body->ObjectState();
		if (State != EObjectStateType::Dynamic && State != EObjectStateType::Sleeping)
		{
			continue;
		}
		// One step per body: a second input with the same id this step would have two bodies writing one state at once.
		int64& Last = LastStep.FindOrAdd(BodyInput.Id);
		if (Last == Step)
		{
			continue;
		}
		Last = Step;
		States.FindOrAdd(BodyInput.Id);
		WaterTracks.FindOrAdd(BodyInput.Id);

		FBodyWork& Item = Work.AddDefaulted_GetRef();
		Item.InputIndex = InputIndex;
		const FRigidTransform3 Pose = FParticleUtilitiesXR::GetActorWorldTransform(Body);
		Item.BodyState.Transform = FTransform(Pose.GetRotation(), Pose.GetTranslation());
		Item.BodyState.CenterOfMass = FParticleUtilitiesXR::GetCoMWorldPosition(Body);
		Item.BodyState.Velocity = Body->V();
		Item.BodyState.AngularVelocity = Body->W();
		Item.BodyState.Mass = static_cast<float>(Body->M());

		Item.Water.Patch = BodyInput.Patch.Get();
		// A cap below the frame's own age (an input that set none) holds the step at the frame's extrapolation, as before the offset.
		Item.Water.Extrapolate = FMath::Min(BodyInput.Extrapolate + StepOffset, FMath::Max(BodyInput.MaxExtrapolate, BodyInput.Extrapolate));
		Item.Water.bAmbientFlow = BodyInput.bAmbientFlow;
		Item.Water.bFlat = BodyInput.bFlat;
		Item.Water.FlatHeight = BodyInput.FlatHeight;
		Item.Water.WaveFeel = Item.Water.Patch && Item.Water.Patch->bHasWave ? BodyInput.WaveFeel : 0.f;

		// The inertia where the step will want it: stable steps, or the art-directed damping (Integrate's AngularDampingRate is 0
		// without FParams::AngularDamping).
		if (BodyInput.bStableSteps || !BodyInput.Params.AngularDamping.IsZero())
		{
			Item.Inertia = FParticleUtilitiesXR::GetWorldInertia(Body);
		}
		Item.GravityZ = Body->GravityEnabled() ? -BodyInput.Params.Gravity : 0.f;

		Item.OutputIndex = Output.Bodies.Num();
		FFluidNinjaBuoyancyBodyOutput& BodyOutput = Output.Bodies.AddDefaulted_GetRef();
		BodyOutput.Id = BodyInput.Id;
		BodyOutput.Mass = Item.BodyState.Mass;
		BodyOutput.CenterOfMass = Item.BodyState.CenterOfMass;
	}
	// The state entries only once every id of the step is in the maps: adding to one moves its entries.
	for (FBodyWork& Item : Work)
	{
		Item.State = States.Find(Input->Bodies[Item.InputIndex].Id);
		Item.WaterTrack = WaterTracks.Find(Input->Bodies[Item.InputIndex].Id);
	}

	// Pass two: each body's step on its own data, into its own slot. The arithmetic and its order are one body's whatever runs it beside.
	auto StepBody = [&](FBodyWork& Item)
	{
		const FFluidNinjaBuoyancyBodyInput& BodyInput = Input->Bodies[Item.InputIndex];
		FFluidNinjaBuoyancyBodyOutput& BodyOutput = Output.Bodies[Item.OutputIndex];
		// The water tracking's counts on every step, wet or in the air (as of the step before: they only ever grow).
		BodyOutput.WaterPatchesUsed = Item.WaterTrack->UsedPatches;
		BodyOutput.WaterPatchesSkipped = Item.WaterTrack->SkippedPatches;
		const FluidNinjaBuoyancy::FBodyState& BodyState = Item.BodyState;
		FluidNinjaBuoyancy::Integrate(*BodyInput.Hull, BodyState, Item.Water, BodyInput.Params, DeltaSeconds, *Item.State, BodyOutput.Result, BodyInput.bWaterline,
			BodyInput.bWaveImpulses, BodyInput.FinestHull.Get());

		const FluidNinjaBuoyancy::FStepResult& Result = BodyOutput.Result;
		if (!Result.bTouchesWater && Result.ThrustApplied == 0.f && Result.Force.IsNearlyZero())
		{
			return;   // in the air: leave it (and its sleep) alone
		}
		FVector Force = Result.Force;
		FVector Torque = Result.Torque;
		// FluidNinja Waves' radiation damping, synchronous: the body feels the waves of everything but itself, so the drag of its
		// own waves is put back here, against its heave, roll and pitch, as much as it is wet. It joins the drag and spin damping, so
		// stable steps take it implicitly where it is stiffer than the step. Its share of the drag is kept apart for the impulses below:
		// the waves it stands for are the body's own static head moving, which the impulses must not count again.
		double RadiationHeave = 0.0;
		const double DragHeave = Result.DragDamping.Z;
		if (!BodyInput.RadiationDamping.IsZero() && Result.bTouchesWater && BodyInput.RestWettedArea > 0.f)
		{
			const double Wet = FMath::Clamp(static_cast<double>(Result.WettedArea) / BodyInput.RestWettedArea, 0.0, 1.0);
			const FVector RollAxis = BodyState.Transform.TransformVectorNoScale(BodyInput.RadiationRollAxis).GetSafeNormal();
			const FVector PitchAxis = BodyState.Transform.TransformVectorNoScale(BodyInput.RadiationPitchAxis).GetSafeNormal();
			// RestRadiationBoost: fnb.Physics.RestBoost times the physical damping on slow motion, fading to the physical value by Fast.
			// Floaters at rest that make and feel waves share a collective mode near 1 Hz that the physical value lets grow
			// (WaveRadiationCalibration in FluidNinjaBuoyancyWaves.cpp); it starts from rest, so it is held there, while a drop, a slam or
			// a jump bobs and rocks freely. The thresholds are set for a body of about 50 cm and scale with the body's size, taken as the
			// hull's height in its attitude (what its rest draft is a share of; the input carries no draft): the heave thresholds with
			// it, since a bigger body's rest motion is bigger in the same measure (4 cm/s is a lot for a cork and nothing for a barge);
			// the spin thresholds against it, since at the same wave a bigger body turns slower, the same edge speed over a longer arm.
			const auto RestRadiationBoost = [Boost](double Speed, double Slow, double Fast)
			{
				return 1.0 + (Boost - 1.0) * (1.0 - FMath::SmoothStep(Slow, Fast, Speed));
			};
			const FVector3f Extent = BodyInput.Hull->BoundsMax - BodyInput.Hull->BoundsMin;
			const FVector Up = BodyState.Transform.InverseTransformVectorNoScale(FVector::UpVector);
			const double Size = FMath::Max(FMath::Abs(Up.X) * Extent.X + FMath::Abs(Up.Y) * Extent.Y + FMath::Abs(Up.Z) * Extent.Z, 1.0) / 50.0;
			RadiationHeave = BodyInput.RadiationDamping.X * Wet * RestRadiationBoost(FMath::Abs(BodyState.Velocity.Z), HeaveSlow * Size, HeaveFast * Size);
			const double Roll = BodyInput.RadiationDamping.Y * Wet
				* RestRadiationBoost(FMath::Abs(FVector::DotProduct(BodyState.AngularVelocity, RollAxis)), SpinSlow / Size, SpinFast / Size);
			const double Pitch = BodyInput.RadiationDamping.Z * Wet
				* RestRadiationBoost(FMath::Abs(FVector::DotProduct(BodyState.AngularVelocity, PitchAxis)), SpinSlow / Size, SpinFast / Size);
			Force.Z -= RadiationHeave * BodyState.Velocity.Z;
			Torque -= RollAxis * (Roll * FVector::DotProduct(BodyState.AngularVelocity, RollAxis)) + PitchAxis * (Pitch * FVector::DotProduct(BodyState.AngularVelocity, PitchAxis));
			// Stable steps see it as drag along world Z, and as spin damping about each world axis by how much of the roll and pitch
			// axes lies along it (the diagonal of the damping turned into the world).
			FluidNinjaBuoyancy::FStepResult& Damped = BodyOutput.Result;
			Damped.DragDamping.Z += static_cast<float>(RadiationHeave);
			Damped.SpinDamping += FVector(RollAxis.X * RollAxis.X, RollAxis.Y * RollAxis.Y, RollAxis.Z * RollAxis.Z) * Roll
				+ FVector(PitchAxis.X * PitchAxis.X, PitchAxis.Y * PitchAxis.Y, PitchAxis.Z * PitchAxis.Z) * Pitch;
		}
		if (!Result.AngularDampingRate.IsZero())
		{
			Torque += Item.Inertia * Result.AngularDampingRate;
		}
		// FluidNinja Waves' added mass: the water the body drags along as it heaves, rolls and pitches, as much as it is wet. Stable steps
		// carry it (the step is solved for the heavier system, against the body's acceleration relative to the water's own under it: the
		// others' waves' vertical acceleration, low-passed and bounded, TrackWaterAcceleration; none with fnb.Physics.AddedMassWaterAccel
		// 0); the added inertia goes to the world axes by how much of the roll and pitch axes lies along each, as the spin damping does.
		float AddedMassHeave = 0.f;
		float WaterAccelerationZ = 0.f;
		FVector AddedInertia = FVector::ZeroVector;
		if (!BodyInput.AddedMass.IsZero() && Result.bTouchesWater && BodyInput.RestWettedArea > 0.f)
		{
			const double Wet = FMath::Clamp(static_cast<double>(Result.WettedArea) / BodyInput.RestWettedArea, 0.0, 1.0);
			const FVector RollAxis = BodyState.Transform.TransformVectorNoScale(BodyInput.RadiationRollAxis).GetSafeNormal();
			const FVector PitchAxis = BodyState.Transform.TransformVectorNoScale(BodyInput.RadiationPitchAxis).GetSafeNormal();
			AddedMassHeave = static_cast<float>(BodyInput.AddedMass.X * Wet);
			AddedInertia = FVector(RollAxis.X * RollAxis.X, RollAxis.Y * RollAxis.Y, RollAxis.Z * RollAxis.Z) * (BodyInput.AddedMass.Y * Wet)
				+ FVector(PitchAxis.X * PitchAxis.X, PitchAxis.Y * PitchAxis.Y, PitchAxis.Z * PitchAxis.Z) * (BodyInput.AddedMass.Z * Wet);
			if (Item.Water.Patch && AddedMassHeave > 0.f)
			{
				FFluidNinjaBuoyancyWaterTrack& Track = *Item.WaterTrack;
				TrackWaterAcceleration(Track, *Item.Water.Patch, Item.Water.WaveFeel, BodyState.CenterOfMass, BodyInput.Params.Gravity);
				// The bound, with this step's waterplane stiffness and wet added mass: |A a_w| ≤ c k |ζ'| (TrackWaterAcceleration says why).
				// A hull with no waterplane (pushed right under) gets no push.
				const float Bound = WaterAccelerationBound * FMath::Max(Result.HeaveStiffness, 0.f) * FMath::Abs(Track.Swing) / AddedMassHeave;
				if (bWaterAccel && BodyInput.bStableSteps)
				{
					WaterAccelerationZ = FMath::Clamp(Track.Acceleration, -Bound, Bound);
					BodyOutput.WaterAccelerationZ = WaterAccelerationZ;
					BodyOutput.bWaterAccelerationBounded = FMath::Abs(Track.Acceleration) > Bound;
				}
			}
		}
		// Stable steps: the part of the water's stiffness and drag this step is too long for, taken implicitly (light bodies, long steps),
		// and the added mass.
		if (BodyInput.bStableSteps)
		{
			const FVector InertiaDiagonal(Item.Inertia.M[0][0], Item.Inertia.M[1][1], Item.Inertia.M[2][2]);
			double StableDragChangeZ = 0.0;
			double StableAddedChangeZ = 0.0;
			if (FluidNinjaBuoyancy::StabilizeStep(Result, BodyState, InertiaDiagonal, Item.GravityZ, DeltaSeconds, AddedMassHeave, AddedInertia, WaterAccelerationZ, Force, Torque,
					&StableDragChangeZ, &StableAddedChangeZ)
				&& !Result.RippleImpulses.IsEmpty())
			{
				// The implicit change of the radiation damping is not the water's dynamic push on the hull: only the drag's share of the
				// change counts (split by their damping coefficients, which share the step's velocity change).
				if (RadiationHeave > 0.0)
				{
					StableDragChangeZ *= DragHeave / FMath::Max(DragHeave + RadiationHeave, 1e-9);
				}
				// Ripples: the water gets the reaction to what the body actually got, and no more. Each impulse is I_down = F.Z * dt, the
				// upward part of the dynamic force (drag, suction, slam) the water put on the body: the water pushing the body up is the
				// body pushing the water down, so I_down and the force on the body share one sign, +Z up on the body = down into the
				// water. What the body actually got from the water's dynamic forces this step is the explicit dynamic force (Force less
				// the hydrostatic lift and the air) plus the implicit changes that are dynamic: the drag's, dF = -c * dv (the drag at the
				// step's end velocity), and the added mass's, A * (a_w - dv / dt) (the water moving with the hull taking the momentum the
				// body lost, or giving it); the implicit heave spring's change is the lift at the step's end position, hydrostatic, and
				// moves no water of its own. The impulses are held within that, as a sum: Σ I_down ≤ (Fdyn + dF_drag + dF_added) * dt
				// (scaled down to it, same convention, +Z up on the body; 0 when the body got pushed the other way). Without it a body
				// whose added mass outweighs it (a flat row boat: ten times its mass) pressed its capped slam into the water step after
				// step while the water, not the body, took the momentum, and its ring grew to many times its own momentum's worth.
				TArray<FVector4f>& StepImpulses = BodyOutput.Result.RippleImpulses;
				double Injected = 0.0;
				for (const FVector4f& RipplePiece : StepImpulses)
				{
					Injected += RipplePiece.Z;
				}
				const double ExplicitDynamicZ = Result.Force.Z - Result.BuoyancyForce.Z - Result.AirForce.Z;
				const double Budget = (ExplicitDynamicZ + StableDragChangeZ + StableAddedChangeZ) * DeltaSeconds;
				if (FMath::Abs(Injected) > 1e-6)
				{
					const float RippleScale = Budget * Injected > 0.0 ? static_cast<float>(FMath::Min(Budget / Injected, 1.0)) : 0.f;
					if (RippleScale < 1.f)
					{
						for (FVector4f& RipplePiece : StepImpulses)
						{
							RipplePiece.Z *= RippleScale;
						}
					}
				}
			}
		}
		if (!Force.ContainsNaN() && !Torque.ContainsNaN())
		{
			Item.Force = Force;
			Item.Torque = Torque;
			Item.bApply = true;
		}
		else
		{
			BodyOutput.Result.RippleImpulses.Reset();   // nothing reaches the body, so nothing reaches the water either
		}
	};
	// Four bodies or more share the step's workers (Chaos' parallel for, so a game-thread or a physics-thread step keeps its context on
	// them); fewer, or fnb.Physics.Parallel 0, run one after the other, as the step always did.
	if (PhysicsParallel != 0 && Work.Num() >= 4)
	{
		PhysicsParallelFor(Work.Num(), [this, &StepBody](int32 Index)
		{
			StepBody(Work[Index]);
		});
	}
	else
	{
		for (FBodyWork& Item : Work)
		{
			StepBody(Item);
		}
	}

	// Pass three, serial: the particles get their forces in the bodies' order.
	for (const FBodyWork& Item : Work)
	{
		if (Item.bApply)
		{
			FRigidBodyHandle_Internal* Body = Input->Bodies[Item.InputIndex].Proxy->GetPhysicsThreadAPI();
			Body->AddForce(Item.Force);
			Body->AddTorque(Item.Torque);
		}
	}

	// Forget bodies that have not come for a while (destroyed, unregistered).
	if ((Step & 255) == 0)
	{
		for (auto It = LastStep.CreateIterator(); It; ++It)
		{
			if (Step - It.Value() > 600)
			{
				States.Remove(It.Key());
				WaterTracks.Remove(It.Key());
				It.RemoveCurrent();
			}
		}
	}
}
