#pragma once

#include "CoreMinimal.h"
#include "Chaos/Core.h"
#include "Chaos/SimCallbackInput.h"
#include "Chaos/SimCallbackObject.h"
#include "FluidNinjaBuoyancyCore.h"

namespace Chaos { class FSingleParticlePhysicsProxy; }

/** One floating body, as the game thread hands it to the physics thread each frame. */
struct FFluidNinjaBuoyancyBodyInput
{
	Chaos::FSingleParticlePhysicsProxy* Proxy = nullptr;
	int32 Id = 0;
	TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe> Hull;
	/** The finest hull (level 0), whatever the level in use: an entry measures the body's height on it. */
	TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe> FinestHull;
	TSharedPtr<const FluidNinjaBuoyancy::FWaterPatch, ESPMode::ThreadSafe> Patch;
	/** How old the patch is at the frame's start (seconds): its heights are carried forward by their rate by this, plus the step's own
	 * time into the frame (FFluidNinjaBuoyancyInput::Frame), up to MaxExtrapolate (Max Extrapolation Seconds; 0 carries no further than this). */
	float Extrapolate = 0.f;
	float MaxExtrapolate = 0.f;
	bool bAmbientFlow = false;
	bool bFlat = false;
	float FlatHeight = 0.f;
	FluidNinjaBuoyancy::FParams Params;
	bool bWaterline = false;
	/** Stable steps (FluidNinjaBuoyancy::StabilizeStep). */
	bool bStableSteps = true;
	/** FluidNinja Waves: list the impulse each wet piece gives the water every step (FluidNinjaBuoyancy::FStepResult::RippleImpulses), held
	 * within the dynamic impulse stable steps actually gave the body: its explicit drag and slam plus their implicit changes and the
	 * added mass's (their implicit heave spring is hydrostatic and leaves the impulses alone). */
	bool bWaveImpulses = false;
	/** FluidNinja Waves: κ, how much of the waves of everything but itself (the patch's B - B_self) the body feels (0: none). */
	float WaveFeel = 0.f;
	/** FluidNinja Waves: the radiation damping of the body's own waves, applied in every step against the body's motion (the self field
	 * B_self takes the body's own waves out of what it feels, so this puts back the drag they would give, with no lag): heave (kg/s),
	 * roll and pitch (kg·cm²/s), each at the full rest footprint; 0 for none. Scaled by the wetted area against RestWettedArea. */
	FVector RadiationDamping = FVector::ZeroVector;
	/** The roll and pitch axes the radiation damping turns about, in the body's frame (unit; the rest attitude's world X and Y). */
	FVector RadiationRollAxis = FVector::XAxisVector;
	FVector RadiationPitchAxis = FVector::YAxisVector;
	/** The hull's wetted area at rest on still water (cm², the physics hull's own measure). */
	float RestWettedArea = 0.f;
	/** FluidNinja Waves: the water the body drags along, at the full rest footprint: heave (kg), roll and pitch (kg·cm², about the radiation
	 * axes), scaled by Wave Added Mass Scale; 0 for none. Scaled by the wetted area against RestWettedArea, and carried by stable steps. */
	FVector AddedMass = FVector::ZeroVector;
};

struct FFluidNinjaBuoyancyInput : public Chaos::FSimCallbackInput
{
	TArray<FFluidNinjaBuoyancyBodyInput> Bodies;
	/** The game thread's frame the input was made in (GFrameCounter): the steps that share one input are one frame's, and each after the
	 * first carries the water a step's time further forward than the one before. */
	uint64 Frame = 0;

	void Reset()
	{
		Bodies.Reset();
		Frame = 0;
	}
};

struct FFluidNinjaBuoyancyBodyOutput
{
	int32 Id = 0;
	float Mass = 0.f;
	FVector CenterOfMass = FVector::ZeroVector;
	FluidNinjaBuoyancy::FStepResult Result;
	/** The water's vertical acceleration the heave added mass was pushed along with this step, a_w (cm/s², after the bound; 0 without
	 * added mass, without a tracked wave, without stable steps, or with fnb.Physics.AddedMassWaterAccel 0), and whether the bound held it
	 * (the callback's TrackWaterAcceleration). For the test bench. */
	float WaterAccelerationZ = 0.f;
	bool bWaterAccelerationBounded = false;
	/** The body's patches so far that its water's acceleration was taken from, and those left out for a gap too far from the running
	 * median gap. For the test bench: how often the readback's jitter drops a patch. */
	int32 WaterPatchesUsed = 0;
	int32 WaterPatchesSkipped = 0;
};

/**
 * Per body, physics thread only: the water's vertical acceleration under it, for its added mass (TrackWaterAcceleration in the
 * callback). The felt wave κ (B - B_self) at its centre of mass is taken once per new patch; this keeps the gaps between the patches
 * (for their running median), the last three samples used (for a second difference at uneven times), the felt wave's slow mean, and the
 * low-pass of the acceleration and of the felt wave's swing about that mean (which the bound reads).
 */
struct FFluidNinjaBuoyancyWaterTrack
{
	/** How many of the last gaps between patches the running median is taken over (odd, so the median is one of them). */
	static constexpr int32 GapWindow = 9;

	double SeenTime = -1.0;          // the last patch looked at, used or not (world s)
	double Gaps[GapWindow] = {};     // the gaps between the last patches looked at (s), a ring written at NextGap
	int32 NumGaps = 0;
	int32 NextGap = 0;
	double Times[3] = {};            // the last samples used, oldest first (world s)
	float Waves[3] = {};             // the felt wave κ (B - B_self) at the body's centre of mass then (cm)
	int32 NumSamples = 0;            // samples used in a row, up to three (0: starting over)
	double FilterTime = -1.0;        // the middle sample the low-pass last took (world s; below 0: none since starting over)
	float Mean = 0.f;                // the felt wave's slow mean (cm)
	float Acceleration = 0.f;        // the second difference, low-passed (cm/s²): a_w before the bound
	float Swing = 0.f;               // the felt wave less its slow mean, low-passed the same way (cm): what the bound reads
	int32 UsedPatches = 0;           // patches the acceleration was taken from (never reset)
	int32 SkippedPatches = 0;        // patches left out for an odd gap (never reset)

	/** Starts the samples and the low-pass over (a_w 0 until three more patches), keeping the gaps' rhythm. */
	void ClearSamples()
	{
		NumSamples = 0;
		FilterTime = -1.0;
		Mean = 0.f;
		Acceleration = 0.f;
		Swing = 0.f;
	}

	/** Starts everything over but the counts: the samples, the low-pass and the gaps. */
	void Clear()
	{
		ClearSamples();
		NumGaps = 0;
		NextGap = 0;
	}
};

struct FFluidNinjaBuoyancyOutput : public Chaos::FSimCallbackOutput
{
	TArray<FFluidNinjaBuoyancyBodyOutput> Bodies;
	/** What the step cost on the physics thread. */
	float StepSeconds = 0.f;

	void Reset()
	{
		Bodies.Reset();
		StepSeconds = 0.f;
	}
};

/** Runs every physics step (each frame, each substep, or each async step): the water's forces on each floating body, at its current
 * pose. The step is three passes: one serial, reading each body's particle and finding its state; one over the bodies, each on its
 * own (in parallel when there are enough of them, fnb.Physics.Parallel), integrating the water's forces into a slot of its own; one
 * serial, giving the particles their forces in the bodies' order. */
class FFluidNinjaBuoyancyCallback : public Chaos::TSimCallbackObject<FFluidNinjaBuoyancyInput, FFluidNinjaBuoyancyOutput>
{
public:
	virtual FName GetFNameForStatId() const override;

private:
	virtual void OnPreSimulate_Internal() override;

	/** One body's step, from what the serial pass read off its particle to the force and torque the serial pass after gives it. */
	struct FBodyWork
	{
		int32 InputIndex = 0;    // into FFluidNinjaBuoyancyInput::Bodies
		int32 OutputIndex = 0;   // into FFluidNinjaBuoyancyOutput::Bodies (sized before the bodies run)
		FluidNinjaBuoyancy::FStepState* State = nullptr;   // this body's entry in States (made before the bodies run, so the map holds still)
		FFluidNinjaBuoyancyWaterTrack* WaterTrack = nullptr;   // this body's entry in WaterTracks (the same)
		FluidNinjaBuoyancy::FBodyState BodyState;
		FluidNinjaBuoyancy::FWaterView Water;
		/** The particle's world inertia, read when stable steps or the art-directed damping will want it (else zero). */
		Chaos::FMatrix33 Inertia = Chaos::FMatrix33(0.0, 0.0, 0.0);
		float GravityZ = 0.f;
		FVector Force = FVector::ZeroVector;
		FVector Torque = FVector::ZeroVector;
		bool bApply = false;   // the particle gets Force and Torque (not in the air, not NaN)
	};

	/** The bodies of the step in hand, reused step to step. Physics thread only. */
	TArray<FBodyWork> Work;

	/** Per body, physics thread only. */
	TMap<int32, FluidNinjaBuoyancy::FStepState> States;
	TMap<int32, FFluidNinjaBuoyancyWaterTrack> WaterTracks;
	TMap<int32, int64> LastStep;
	int64 Step = 0;
	/** The frame of the input in hand and the simulation time its first step began at: how far into the frame a step is. Physics thread only. */
	uint64 InputFrame = 0;
	Chaos::FReal InputStartTime = 0.0;
};
