#include "FluidNinjaBuoyancyComponent.h"

#include "Algo/Sort.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/AudioComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/ScopeLock.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#include "FluidNinjaBuoyancyCore.h"
#include "FluidNinjaBuoyancyLog.h"
#include "FluidNinjaBuoyancySettings.h"
#include "FluidNinjaBuoyancyStats.h"
#include "FluidNinjaBuoyancySubsystem.h"
#include "PhysicsEngine/BodyInstance.h"

namespace
{
	/** The body's own asset, else the project default. */
	template <typename T>
	T* LoadOr(const TSoftObjectPtr<T>& Own, const FSoftObjectPath& Default)
	{
		if (!Own.IsNull())
		{
			return Own.LoadSynchronous();
		}
		return Default.IsValid() ? Cast<T>(Default.TryLoad()) : nullptr;
	}

	/** A slam at least this strong on the project's splash scale also throws a crown of splashes out to every side. */
	constexpr float CrownMinStrength = 4.f;

	/** Splashes weaker than this on the project's scale are not drawn: the swimmers' floor (FluidNinjaBuoyancySwimmers.cpp), so a 2 W
	 * slam from an arrow reads as 0.31, not as the 0.6 a hull once clamped up to. */
	constexpr float SmallestSplash = 0.2f;

	/** An entry's splash (sized by the entry power) fires within this long of the body going in (seconds); after that later slams are
	 * sized as slams. */
	constexpr double EntrySplashSeconds = 0.25;

	/** A slam waiting for its splash (the splash interval, the frame's budget) keeps its hardest value this long (seconds), then is
	 * dropped as stale. */
	constexpr double PendingSlamSeconds = 0.25;

	/** The piece of water line bow spray is sized by when the contact carries no area (cm²): a hand-sized piece of hull. */
	constexpr float NominalPieceAreaCm2 = 400.f;

	/** Bow spray's power is the water a piece of water line of area A drives in at speed v: ρ·A·v³. With ρ in kg/m³, A in cm² and v
	 * in cm/s this many watts per unit (1e-4 m²/cm² × 1e-6 m³/cm³). */
	constexpr float BowSprayWattsPerUnit = 1e-10f;

	/** How far outside the water line a splash at a contact is thrown (cm), along the hull's outward normal. */
	constexpr double ContactSplashOffsetCm = 10.0;

	/** The range a coarse hull level's buoyancy scale is held to (UFluidNinjaBuoyancyComponent::ComputeLevelBuoyancyScales). A level that
	 * needs more than twice or less than half the finest hull's lift at the rest waterline is a shell too crude for the shape (a round
	 * section cut to a square): scaled that far it would rest at the right draft on the wrong waterplane, so it is held here and named. */
	constexpr float MinLevelBuoyancyScale = 0.5f;
	constexpr float MaxLevelBuoyancyScale = 2.f;

	// The splash budget. Every FluidNinja Buoyancy component counts the splash systems it spawns in a frame (GFrameCounter) in one
	// shared counter, so a hundred crates meeting one wave front cannot issue a thousand Niagara spawns at once. The rule, nearest
	// first: a body within Lod Distances X (EffectReach 0) may spawn while the counter is under the budget; a body beyond it (reach 1)
	// only while the counter is under half the budget, and then only its main splash or bow spray, never a crown or plunge splashes;
	// a body beyond Lod Distances Y, or not drawn for the last second (reach 2), spawns nothing. Near bodies spend the second half of
	// the budget alone, and when the budget is gone a slam keeps waiting (PendingSlamSeconds) for a frame with room.
	int32 SplashBudgetPerFrame = 8;
	static FAutoConsoleVariableRef CVarSplashBudgetPerFrame(TEXT("fnb.Effects.SplashBudgetPerFrame"), SplashBudgetPerFrame,
		TEXT("The most splash systems every FluidNinja Buoyancy body together may spawn in one frame (slams, their crowns and plunge splashes, bow spray): ")
		TEXT("bodies within their Lod Distances X take the whole budget, farther ones only its first half (rounded down), bodies beyond Lod Distances Y or not drawn ")
		TEXT("for a second none (those distances scaled by fnb.Effects.ReachScale). A slam over budget waits for a frame with room. Lower it to cap the Niagara spawn cost ")
		TEXT("when many bodies hit the water at once. 0 spawns none (A/B), negative removes the limit. Default 8. Set by sg.WaterQuality (2 / 4 / 6 / 8 / 16, Low to Cinematic)."),
		ECVF_Scalability);
	uint64 SplashBudgetFrame = 0;
	int32 SplashesThisFrame = 0;

	// The tier's other hands on the effects (sg.WaterQuality): how far the effects reach (a scale on Lod Distances Cm for the effects alone;
	// fnb.Lod.DistanceScale scales them for the hulls and the gather), whether a hard slam throws its crown, and whether the wet line is kept.
	float EffectsReachScale = 1.f;
	static FAutoConsoleVariableRef CVarEffectsReachScale(TEXT("fnb.Effects.ReachScale"), EffectsReachScale,
		TEXT("Scales every body's Lod Distances Cm for its effects alone (splashes, spray, the crown): 0.5 stops them at half the distance, 2 lets them reach twice as far. ")
		TEXT("Default 1; values under 0.01 count as 0.01. Set by sg.WaterQuality (0.5 / 0.75 / 0.85 / 1 / 1.5, Low to Cinematic)."),
		ECVF_Scalability);
	int32 EffectsCrown = 1;
	static FAutoConsoleVariableRef CVarEffectsCrown(TEXT("fnb.Effects.Crown"), EffectsCrown,
		TEXT("1 (the default; any value but 0): a slam of strength 4 or more throws a crown of splashes out to every side; 0: the main splash and the plunge splashes alone. ")
		TEXT("Set by sg.WaterQuality (off at Low and Medium)."),
		ECVF_Scalability);
	int32 EffectsWetLine = 1;
	static FAutoConsoleVariableRef CVarEffectsWetLine(TEXT("fnb.Effects.WetLine"), EffectsWetLine,
		TEXT("1 (the default; any value but 0): bodies with Wet Line on write their water line into their Custom Primitive Data every tick; 0: none does, and each hull ")
		TEXT("that had a wet line is written dry once. Set by sg.WaterQuality (off at Low)."),
		ECVF_Scalability);

	// The spray sheets (Spray Sheets on a body): whether they run at all, and how thick (sg.WaterQuality).
	int32 EffectsSpraySheets = 1;
	static FAutoConsoleVariableRef CVarEffectsSpraySheets(TEXT("fnb.Effects.SpraySheets"), EffectsSpraySheets,
		TEXT("1 (the default; any value but 0): bodies with Spray Sheets on throw their whisker sheets and rooster tail while they drive fast through the water; 0: none ")
		TEXT("(the sheets running die out; their landing foam stops). Set by sg.WaterQuality (off at Low and Medium)."),
		ECVF_Scalability);
	float EffectsSprayRateScale = 1.f;
	static FAutoConsoleVariableRef CVarEffectsSprayRateScale(TEXT("fnb.Effects.SprayRateScale"), EffectsSprayRateScale,
		TEXT("Scales how many drops every spray sheet throws (each body's Spray Sheet Rate times this): 0.5 halves the particles, 2 doubles them. Default 1; ")
		TEXT("negative counts as 0. Set by sg.WaterQuality (0.5 / 0.75 / 0.75 / 1 / 1.5, Low to Cinematic)."),
		ECVF_Scalability);

	/** A spray sheet's drops at full strength (particles/s), and the most a sheet throws however fast the hull goes. */
	constexpr float SheetParticlesPerSecond = 320.f;
	constexpr float SheetMaxParticlesPerSecond = 900.f;
	/** The whisker sheet leaves at this elevation above the water (degrees), outward from the hull, and the fastest it may go (cm/s). */
	constexpr float SheetElevationDeg = 28.f;
	constexpr float SheetMaxSpeedCmS = 1500.f;
	/** A sheet runs from its start speed and stops below this share of it. */
	constexpr float SheetHysteresis = 0.85f;
	/** The linear drag that slows the sheets' drops (1/s). It mirrors the Drag module of NS_FNB_HullSpray (Tools/make_hull_spray.py,
	 * INPUTS), so the landing foam goes where the drops come down: change both together. */
	constexpr float SprayDropDragPerSecond = 0.35f;
	static_assert(SprayDropDragPerSecond > 0.f, "the spray landing's reach divides by the drag");

	/** Takes one splash from the frame's budget for a body of this reach; false when there is none left for it. */
	bool TakeSplashBudget(int32 Reach)
	{
		if (SplashBudgetFrame != GFrameCounter)
		{
			SplashBudgetFrame = GFrameCounter;
			SplashesThisFrame = 0;
		}
		if (SplashBudgetPerFrame >= 0)
		{
			const int32 Allowed = Reach == 0 ? SplashBudgetPerFrame : SplashBudgetPerFrame / 2;
			if (SplashesThisFrame >= Allowed)
			{
				return false;
			}
		}
		++SplashesThisFrame;
		return true;
	}
}

bool FluidNinjaBuoyancy::TakeSharedSplashBudget(int32 Reach)
{
	// The same counter the bodies spend (the subsystem's water contacts take from it too).
	return TakeSplashBudget(Reach);
}

UFluidNinjaBuoyancyComponent::UFluidNinjaBuoyancyComponent()
{
	// Wet line, spray and sound follow the physics results each frame.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

UPrimitiveComponent* UFluidNinjaBuoyancyComponent::GetSimulatedPrimitive() const
{
	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		return nullptr;
	}
	if (!PrimitiveName.IsNone())
	{
		for (UActorComponent* Component : Owner->GetComponents())
		{
			if (Component && Component->GetFName() == PrimitiveName)
			{
				return Cast<UPrimitiveComponent>(Component);
			}
		}
	}
	return Cast<UPrimitiveComponent>(Owner->GetRootComponent());
}

void UFluidNinjaBuoyancyComponent::FillParams(FluidNinjaBuoyancy::FParams& Out) const
{
	Out.BuoyancyMultiplier = BuoyancyMultiplier;
	Out.PressureDragLinear = PressureDragLinear;
	Out.PressureDragQuadratic = PressureDragQuadratic;
	Out.PressureFalloff = PressureFalloff;
	Out.SuctionDragLinear = SuctionDragLinear;
	Out.SuctionDragQuadratic = SuctionDragQuadratic;
	Out.SuctionFalloff = SuctionFalloff;
	Out.ViscousMultiplier = ViscousMultiplier;
	Out.SlamMultiplier = SlamMultiplier;
	Out.FlowCoupling = FlowCoupling;
	Out.AirDragCoefficient = AirDragCoefficient;
	Out.AngularDamping = AngularDamping;
	// The keel, with or without propulsion (its aspect ratio divides by the area, which is above 0 whenever it acts).
	Out.KeelLocal = KeelLocation;
	Out.KeelArea = FMath::Max(KeelAreaCm2, 0.f);
	Out.KeelDepth = FMath::Max(KeelDepthCm, 1.f);
	Out.KeelCrossflow = FMath::Clamp(KeelCrossflowCoefficient, 0.f, 2.f);
	Out.bPropulsion = bPropulsion;
	Out.ThrustLocal = ThrustLocation;
	Out.ThrustDirectionLocal = ThrustDirection.IsNearlyZero() ? FVector::ForwardVector : ThrustDirection;
	Out.MaxThrust = MaxThrustNewtons * 100.f;   // N to kg·cm/s²
	Out.Throttle = FMath::Clamp(Throttle, -1.f, 1.f);
	Out.PropWashSpeed = PropWashSpeed;
	Out.RudderLocal = RudderLocation;
	Out.RudderArea = RudderAreaCm2;
	Out.RudderAngle = FMath::DegreesToRadians(MaxRudderAngle) * FMath::Clamp(Rudder, -1.f, 1.f);
	Out.bSteerableThrust = bSteerableThrust;

	// The hull's flow: planing on a triangle hull only (the point models have no faces to plane on).
	const bool bPlaning = IsPlaningHull();
	Out.HullFlow = bPlaning ? FluidNinjaBuoyancy::EHullFlow::Planing : FluidNinjaBuoyancy::EHullFlow::Displacement;
	Out.PlaningPressureCoefficient = FMath::Clamp(PlaningPressureCoefficient, 0.1f, 1.5f);
	Out.WaveResistanceScale = FMath::Clamp(WaveResistanceScale, 0.f, 3.f);
	Out.HullPieceCm = FMath::Max(HullResolutionCm, 2.f);

	// The propeller model, in the core's units (kg, cm, s).
	Out.PropulsionModel = PropulsionModel == EFluidNinjaPropulsionModel::Propeller ? FluidNinjaBuoyancy::EPropulsionModel::Propeller
		: FluidNinjaBuoyancy::EPropulsionModel::Simple;
	if (Out.PropulsionModel == FluidNinjaBuoyancy::EPropulsionModel::Propeller)
	{
		Out.EnginePower = FMath::Clamp(EnginePowerKw, 1.f, 2000.f) * 1e7f;   // kW to kg·cm²/s³
		Out.EngineMaxRpm = FMath::Clamp(EngineMaxRpm, 1000.f, 15000.f);
		Out.EngineIdleRpm = FMath::Clamp(EngineIdleRpm, 300.f, 0.9f * Out.EngineMaxRpm);
		Out.EngineResponseSeconds = FMath::Clamp(EngineResponseSeconds, 0.01f, 5.f);
		Out.EngineInertia = FMath::Max(EngineInertiaKgM2, 0.001f) * 1e4f;   // kg·m² to kg·cm²
		Out.GearRatio = FMath::Clamp(GearRatio, 0.25f, 10.f);
		Out.PropDiameter = FMath::Clamp(PropDiameterCm, 5.f, 300.f);
		// A bronze or steel propeller's own inertia grows with about the fifth power of its size; 200 kg·cm² at 28 cm (0.02 kg·m²).
		Out.PropInertia = 200.f * FMath::Pow(Out.PropDiameter / 28.f, 5.f);
		Out.PropWakeFraction = FMath::Clamp(PropWakeFraction, 0.f, 0.9f);
		Out.ReverseEfficiency = FMath::Clamp(ReverseEfficiency, 0.f, 1.f);
		Out.VentilatedFactor = FMath::Clamp(VentilatedThrustFactor, 0.f, 1.f);
		Out.PropHand = bLeftHandedProp ? -1.f : 1.f;
		// The open-water curves: the propeller's own (both curves given), else the generic series for its pitch ratio.
		const FRichCurve* Thrust = PropThrustCurve.GetRichCurveConst();
		const FRichCurve* TorqueCurve = PropTorqueCurve.GetRichCurveConst();
		if (Thrust && TorqueCurve && Thrust->GetNumKeys() >= 2 && TorqueCurve->GetNumKeys() >= 2)
		{
			float MinTime = 0.f;
			float MaxTime = 1.f;
			Thrust->GetTimeRange(MinTime, MaxTime);
			Out.PropTableStep = FMath::Max(MaxTime, 0.05f) / static_cast<float>(FluidNinjaBuoyancy::PropTableSize - 1);
			for (int32 Index = 0; Index < FluidNinjaBuoyancy::PropTableSize; ++Index)
			{
				const float J = Index * Out.PropTableStep;
				Out.PropKT[Index] = Thrust->Eval(J);
				Out.PropKQ[Index] = TorqueCurve->Eval(J);
			}
		}
		else
		{
			FluidNinjaBuoyancy::FillPropellerSeries(PropPitchRatio, Out);
		}
	}
}

float UFluidNinjaBuoyancyComponent::GetBottomZ() const
{
	return Hulls.IsEmpty() ? 0.f : RestedBottom;
}

bool UFluidNinjaBuoyancyComponent::SetUp(FString& OutReport)
{
	UPrimitiveComponent* Primitive = GetSimulatedPrimitive();
	if (!Primitive)
	{
		OutReport = TEXT("no primitive to float (set Primitive Name, or give the actor a root primitive)");
		return false;
	}
	// The finest hull of the chosen model, then (triangles) two coarser levels for distance. The finest triangle surface is kept for
	// FluidNinja Waves whatever the model (they measure how deep the hull sits in each cell).
	TArray<TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe>> Built;
	TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe> SurfaceForWaves;
	FString Why;
	FluidNinjaBuoyancy::FHull Surface;
	if (!FluidNinjaBuoyancy::BuildHull(*Primitive, HullResolutionCm, MaxHullTriangles, Surface, Why))
	{
		OutReport = FString::Printf(TEXT("%s has %s"), *Primitive->GetName(), *Why);
		return false;
	}
	if (HullModel == EFluidNinjaHullModel::Triangles)
	{
		Built.Add(MakeShared<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe>(Surface));
		SurfaceForWaves = Built[0];
		for (int32 Level = 1; Level <= 2; ++Level)
		{
			FluidNinjaBuoyancy::FHull Coarse;
			const float Factor = static_cast<float>(1 << Level);
			if (FluidNinjaBuoyancy::BuildHull(*Primitive, HullResolutionCm * Factor, FMath::Max(12, FMath::RoundToInt32(MaxHullTriangles / (Factor * Factor))), Coarse, Why)
				&& Coarse.Triangles.Num() < Built.Last()->Triangles.Num())
			{
				Built.Add(MakeShared<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe>(MoveTemp(Coarse)));
			}
		}
	}
	else
	{
		FluidNinjaBuoyancy::FHull Points;
		bool bOk = false;
		if (HullModel == EFluidNinjaHullModel::Voxels)
		{
			bOk = FluidNinjaBuoyancy::BuildVoxels(Surface, VoxelSizeCm, VoxelCount, Points, Why);
		}
		else
		{
			TArray<FluidNinjaBuoyancy::FPontoon> Spheres;
			for (const FFluidNinjaPontoon& Pontoon : Pontoons)
			{
				FluidNinjaBuoyancy::FPontoon& Sphere = Spheres.AddDefaulted_GetRef();
				Sphere.Location = Pontoon.Location;
				Sphere.Radius = Pontoon.Radius;
				Sphere.LiftWeight = Pontoon.LiftWeight;
				Sphere.DragWeight = Pontoon.DragWeight;
			}
			bOk = FluidNinjaBuoyancy::BuildSpheres(Surface, Spheres, Points, Why);
		}
		if (!bOk)
		{
			OutReport = FString::Printf(TEXT("%s: %s"), *Primitive->GetName(), *Why);
			return false;
		}
		Built.Add(MakeShared<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe>(MoveTemp(Points)));
		SurfaceForWaves = MakeShared<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe>(MoveTemp(Surface));
	}
	const FluidNinjaBuoyancy::FHull& Finest = *Built[0];
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	FluidNinjaBuoyancy::FParams Params;
	FillParams(Params);
	Params.Rho = Settings.WaterDensity * 1e-6f;
	const UWorld* World = GetWorld();
	Params.Gravity = World ? FMath::Max(1.f, -World->GetGravityZ()) : 980.f;
	const bool bApply = World && World->IsGameWorld();

	// The mass: what the hull, lying in its rest attitude, displaces at the rest draft; or the body's own.
	const FQuat Attitude = RestAttitude.Quaternion();
	FluidNinjaBuoyancy::RestedExtent(Finest, Attitude, RestedBottom, RestedTop);
	// (Body Mass: the body's own, read from its body instance; GetMass warns on a body not simulating yet. In play it is read again below,
	// once physics is on.)
	const FBodyInstance* MassBody = Primitive->GetBodyInstance();
	float Mass = MassMode == EFluidNinjaBuoyancyMass::RestDraft ? Params.Rho * FluidNinjaBuoyancy::DisplacedVolume(Finest, RestDraftCm, Params, Attitude) : (MassBody ? MassBody->GetBodyMass() : 0.f);
	FVector CenterOfMassLocal = FVector(Finest.Centroid) + CenterOfMassOffset;
	if (bApply)
	{
		if (!Primitive->IsSimulatingPhysics())
		{
			Primitive->SetSimulatePhysics(true);
		}
		if (FBodyInstance* Body = Primitive->GetBodyInstance())
		{
			Body->InertiaTensorScale = InertiaScale;
			Body->COMNudge = CenterOfMassOffset;
			if (MaxDepenetrationSpeed > 0.f)
			{
				Body->SetMaxDepenetrationVelocity(MaxDepenetrationSpeed);
			}
			if (MassMode == EFluidNinjaBuoyancyMass::RestDraft)
			{
				Body->SetMassOverride(Mass, true);
			}
			Body->UpdateMassProperties();
			Mass = Body->GetBodyMass();
			const FTransform Frame(Primitive->GetComponentQuat(), Primitive->GetComponentLocation());
			CenterOfMassLocal = Frame.InverseTransformPosition(Body->GetCOMPosition());
		}
	}
	const float Draft = FluidNinjaBuoyancy::DraftForMass(Finest, Mass, Params, Attitude);
	RestingDraft = Draft;
	RestingDraftMassKg = Mass;
	// The coarser levels' lift, scaled to hold the body at the finest hull's rest draft; reported with each level's triangle count.
	TArray<float> Scales;
	ComputeLevelBuoyancyScales(Built, Draft, Params, Scales);
	const float Height = RestedTop - RestedBottom;
	FString Levels;
	for (int32 Level = 1; Level < Built.Num(); ++Level)
	{
		Levels += FString::Printf(TEXT("%s%d (buoyancy x%.2f)"), Level == 1 ? TEXT("; farther levels ") : TEXT(", "), Built[Level]->Elements(), Scales[Level]);
	}
	OutReport = FString::Printf(TEXT("%s: hull from %s%s; volume %.0f L, %.2f x %.2f x %.2f m; mass %.1f kg"), *GetNameSafe(GetOwner()), *Finest.Source,
		*Levels, Finest.Volume / 1000.f, (Finest.BoundsMax.X - Finest.BoundsMin.X) / 100.f, (Finest.BoundsMax.Y - Finest.BoundsMin.Y) / 100.f,
		(Finest.BoundsMax.Z - Finest.BoundsMin.Z) / 100.f, Mass);
	RollGM = PitchGM = 0.f;
	if (Draft < 0.f)
	{
		OutReport += TEXT("; it SINKS (heavier than its whole hull's water)");
	}
	else
	{
		RollGM = FluidNinjaBuoyancy::MetacentricHeight(Finest, Mass, CenterOfMassLocal, Draft, 0, Params, Attitude);
		PitchGM = FluidNinjaBuoyancy::MetacentricHeight(Finest, Mass, CenterOfMassLocal, Draft, 1, Params, Attitude);
		OutReport += FString::Printf(TEXT("; rests %.1f cm deep (%.0f%% of its height%s); centre of mass %.0f cm above the keel; metacentric height roll %.0f cm, pitch %.0f cm"),
			Draft, 100.f * Draft / FMath::Max(Height, 1.f), RestAttitude.IsNearlyZero() ? TEXT("") : *FString::Printf(TEXT(", turned %s"), *RestAttitude.ToCompactString()),
			Attitude.RotateVector(CenterOfMassLocal).Z - RestedBottom, RollGM, PitchGM);
		if (RollGM <= 0.f || PitchGM <= 0.f)
		{
			OutReport += TEXT(" -- UNSTABLE: it will capsize; lower the centre of mass (Center Of Mass Offset Z)");
		}
	}
	Hulls = MoveTemp(Built);
	LevelBuoyancyScales = MoveTemp(Scales);
	WaveHull = MoveTemp(SurfaceForWaves);
	return true;
}

void UFluidNinjaBuoyancyComponent::ComputeLevelBuoyancyScales(const TArray<TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe>>& Levels, float Draft,
	const FluidNinjaBuoyancy::FParams& Params, TArray<float>& OutScales) const
{
	// Every level is a shell over the same collision, but the coarser ones are cut from fewer grid points or a cruder icosphere: their faces
	// are chords inside the true surface and their lowest vertex often misses the true bottom, so they hold less water below the same
	// waterline and the same mass floats deeper on them (fnb.Lod.Bias 2 measured a boat at 16.1 cm against its 13.0 cm rest, a barrel at
	// 42.8 cm against 25.0). Each level's lift is scaled by s = V_0 / V_L, the finest hull's displaced volume below its rest waterline over
	// the level's below that same waterline, both resting in the rest attitude, so ρ g s V_L = ρ g V_0 = M g and every level holds the body
	// at the finest hull's rest draft. The waterline is a height in the turned body frame (the finest hull's lowest point plus Draft), not a
	// draft: DisplacedVolume measures its draft from each hull's own lowest vertex, and a coarse hull standing on a higher vertex would be
	// cut too high, its volume read too large and its scale too small. A body heavier than its whole hull's water (Draft < 0) lies all
	// under it, so it compares the whole volumes.
	// The match holds at that waterline and attitude only. Heeled, or pressed deeper, a level answers with its own shape scaled by s: its
	// heave and tilt stiffness are s times its own waterplane's, and its metacentric heights stay what its coarse geometry gives (RollGM and
	// PitchGM are the finest hull's).
	OutScales.Reset(Levels.Num());
	if (Levels.IsEmpty() || !Levels[0].IsValid())
	{
		return;
	}
	OutScales.Add(1.f);
	const FQuat Attitude = RestAttitude.Quaternion();
	const FluidNinjaBuoyancy::FHull& Finest = *Levels[0];
	const bool bSinks = Draft < 0.f;
	float FinestBottom = 0.f;
	float FinestTop = 0.f;
	FluidNinjaBuoyancy::RestedExtent(Finest, Attitude, FinestBottom, FinestTop);
	const float Waterline = FinestBottom + FMath::Max(Draft, 0.f);   // cm, in the turned body frame
	const float FinestVolume = bSinks ? Finest.Volume : FluidNinjaBuoyancy::DisplacedVolume(Finest, FMath::Max(Draft, 0.f), Params, Attitude);   // cm³
	for (int32 Level = 1; Level < Levels.Num(); ++Level)
	{
		const FluidNinjaBuoyancy::FHull& Hull = *Levels[Level];
		// A body that displaces nothing at rest (no mass) has nothing to match: its levels keep their own lift.
		if (!(FinestVolume >= 1.f))
		{
			OutScales.Add(1.f);
			continue;
		}
		float Bottom = 0.f;
		float Top = 0.f;
		FluidNinjaBuoyancy::RestedExtent(Hull, Attitude, Bottom, Top);
		const float LevelVolume = bSinks ? Hull.Volume : FluidNinjaBuoyancy::DisplacedVolume(Hull, FMath::Max(Waterline - Bottom, 0.f), Params, Attitude);   // cm³
		const float Wanted = LevelVolume > 0.f ? FinestVolume / LevelVolume : TNumericLimits<float>::Max();
		const float Scale = FMath::Clamp(Wanted, MinLevelBuoyancyScale, MaxLevelBuoyancyScale);
		OutScales.Add(Scale);
		if (Wanted < MinLevelBuoyancyScale || Wanted > MaxLevelBuoyancyScale)
		{
			// Said once per body and level: RefreshRestingDraft computes the scales again whenever the mass moves, and Play registers the body
			// again every time (the same pattern as BuildHull's overlap warning).
			const UPrimitiveComponent* Primitive = GetSimulatedPrimitive();
			const AActor* Owner = GetOwner();
			const FString Who = FString::Printf(TEXT("%s (%s)"), Owner ? *Owner->GetActorNameOrLabel() : TEXT("no actor"), *GetNameSafe(Primitive));
			static FCriticalSection WarnedLock;
			static TSet<FString> Warned;
			bool bWarned = false;
			{
				FScopeLock Lock(&WarnedLock);
				Warned.Add(FString::Printf(TEXT("%s level %d"), *Who, Level), &bWarned);
			}
			if (!bWarned)
			{
				UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("FluidNinja Buoyancy on %s: hull level %d holds %.1f L below the finest hull's rest waterline against the finest hull's %.1f L, a buoyancy scale of %s; it is held at %.2f, so on that level the body floats %s than at level 0. The level's shell is too coarse for the shape: lower Hull Resolution Cm (the farther levels are built at 2 and 4 times it) or raise Max Hull Triangles."),
					*Who, Level, LevelVolume / 1000.f, FinestVolume / 1000.f, LevelVolume > 0.f ? *FString::Printf(TEXT("%.2f"), Wanted) : TEXT("beyond any (it does not reach the waterline)"),
					Scale, Wanted > MaxLevelBuoyancyScale ? TEXT("deeper") : TEXT("higher"));
			}
		}
	}
}

bool UFluidNinjaBuoyancyComponent::CopyWaveHullSurface(TArray<FVector3f>& OutVertices, TArray<FIntVector3>& OutTriangles) const
{
	const TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe> Hull = WaveHull;
	if (!Hull.IsValid() || Hull->Vertices.IsEmpty() || Hull->Triangles.IsEmpty())
	{
		OutVertices.Reset();
		OutTriangles.Reset();
		return false;
	}
	OutVertices = Hull->Vertices;
	OutTriangles = Hull->Triangles;
	return true;
}

void UFluidNinjaBuoyancyComponent::RefreshRestingDraft()
{
	const UPrimitiveComponent* Primitive = GetSimulatedPrimitive();
	const FBodyInstance* Body = Primitive ? Primitive->GetBodyInstance() : nullptr;
	if (!Body || !Body->IsValidBodyInstance() || Hulls.IsEmpty())
	{
		return;
	}
	const float Mass = Body->GetBodyMass();
	if (!(Mass > 0.f) || FMath::IsNearlyEqual(Mass, RestingDraftMassKg, 0.005f * Mass))
	{
		return;
	}
	FluidNinjaBuoyancy::FParams Params;
	FillParams(Params);
	// The project's water, as SetUp and the physics steps use it: FParams' own default is fresh water (1000 kg/m³), and the draft (so the
	// waterline the levels' buoyancy scales are matched at) would be off for any other density.
	Params.Rho = UFluidNinjaBuoyancySettings::Get().WaterDensity * 1e-6f;
	const FQuat Attitude = RestAttitude.Quaternion();
	const FluidNinjaBuoyancy::FHull& Finest = *Hulls[0];
	RestingDraft = FluidNinjaBuoyancy::DraftForMass(Finest, Mass, Params, Attitude);
	RestingDraftMassKg = Mass;
	// The levels' buoyancy scales follow the new rest waterline (a heavier body sits where a coarse shell's chords cut a different share).
	ComputeLevelBuoyancyScales(Hulls, RestingDraft, Params, LevelBuoyancyScales);
	RollGM = PitchGM = 0.f;
	if (RestingDraft >= 0.f)
	{
		const FTransform Frame(Primitive->GetComponentQuat(), Primitive->GetComponentLocation());
		const FVector CenterOfMassLocal = Frame.InverseTransformPosition(Body->GetCOMPosition());
		RollGM = FluidNinjaBuoyancy::MetacentricHeight(Finest, Mass, CenterOfMassLocal, RestingDraft, 0, Params, Attitude);
		PitchGM = FluidNinjaBuoyancy::MetacentricHeight(Finest, Mass, CenterOfMassLocal, RestingDraft, 1, Params, Attitude);
	}
}

void UFluidNinjaBuoyancyComponent::BeginPlay()
{
	Super::BeginPlay();
	if (!SetUp(Report))
	{
		UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("FluidNinja Buoyancy on %s: %s"), *GetNameSafe(GetOwner()), *Report);
		return;
	}
	UE_LOG(LogFluidNinjaBuoyancy, Log, TEXT("FluidNinja Buoyancy %s"), *Report);
	if (const UPrimitiveComponent* Placed = GetSimulatedPrimitive())
	{
		StartTransform = FTransform(Placed->GetComponentQuat(), Placed->GetComponentLocation());
		if (!bWake && Interaction == EFluidNinjaBuoyancyInteraction::Wakes && !Placed->GetGenerateOverlapEvents())
		{
			UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy on %s: Wake is off, so it floats but leaves no ripples. Turn Wake on (see Docs/UserGuide.md, Interacting with FluidNinja)."),
				*GetNameSafe(GetOwner()));
		}
	}
	if (UWorld* World = GetWorld())
	{
		if (UFluidNinjaBuoyancySubsystem* Hydro = World->GetSubsystem<UFluidNinjaBuoyancySubsystem>())
		{
			Hydro->Register(this);
		}
	}

	// Held still until its water is read (UFluidNinjaBuoyancySubsystem settles it), so it does not drop onto the water at the start.
	if (bSettleOnStart && GetWorld() && GetWorld()->IsGameWorld())
	{
		if (UPrimitiveComponent* Held = GetSimulatedPrimitive())
		{
			Held->SetSimulatePhysics(false);
			bAwaitingWater = true;
			AwaitingSince = GetWorld()->GetTimeSeconds();
		}
	}

	// Effects: the body's own, else the project defaults.
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	LoadedSplash = LoadOr(SplashSystem, Settings.SplashSystem);
	LoadedSlap = LoadOr(SlapSound, Settings.SlapSound);
	LoadedCreak = LoadOr(CreakSound, Settings.CreakSound);
	LoadedDrip = DripSound.IsNull() ? nullptr : DripSound.LoadSynchronous();   // the body's own only: the project has no default drip
	LoadedAttenuation = LoadOr(Attenuation, Settings.HullAttenuation);
	UPrimitiveComponent* Primitive = GetSimulatedPrimitive();
	if (USoundBase* Wash = LoadOr(WashSound, Settings.WashSound); Wash && Primitive && GetOwner())
	{
		WashAudio = NewObject<UAudioComponent>(GetOwner(), MakeUniqueObjectName(GetOwner(), UAudioComponent::StaticClass(), TEXT("FluidNinjaHullWash")));
		WashAudio->bAutoActivate = false;
		WashAudio->bAutoDestroy = false;
		WashAudio->SetSound(Wash);
		WashAudio->AttenuationSettings = LoadedAttenuation;
		WashAudio->SetupAttachment(Primitive);
		WashAudio->RegisterComponent();
	}
	// The engine loop (the propeller model), at the propeller; made on the first tick instead when the propeller model is taken up later.
	if (bPropulsion && PropulsionModel == EFluidNinjaPropulsionModel::Propeller && Primitive)
	{
		MakeEngineAudio(*Primitive);
	}
	if (bSpraySheets)
	{
		LoadSheetSystem();
	}
}

void UFluidNinjaBuoyancyComponent::MakeEngineAudio(UPrimitiveComponent& Primitive)
{
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	bEngineAudioTried = true;
	EngineAudioSoundTried = EngineSound.IsNull() ? Settings.EngineSound : EngineSound.ToSoftObjectPath();
	AActor* Owner = GetOwner();
	USoundBase* Loop = Owner ? LoadOr(EngineSound, Settings.EngineSound) : nullptr;
	if (!Loop)
	{
		return;   // no engine sound set (the plugin ships none), or it did not load
	}
	EngineAudio = NewObject<UAudioComponent>(Owner, MakeUniqueObjectName(Owner, UAudioComponent::StaticClass(), TEXT("FluidNinjaEngine")));
	EngineAudio->bAutoActivate = false;
	EngineAudio->bAutoDestroy = false;
	EngineAudio->SetSound(Loop);
	EngineAudio->AttenuationSettings = LoadedAttenuation;
	EngineAudio->SetupAttachment(&Primitive);
	// Thrust Location is at world size (the body's scale is not applied to it), but a child's relative location is scaled by its parent's
	// world scale: divided by that here, axis by axis, so the loop sits at the propeller at any scale.
	const FVector BodyScale = Primitive.GetComponentScale();
	FVector AtPropeller = ThrustLocation;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		AtPropeller[Axis] = FMath::Abs(BodyScale[Axis]) > 1e-6 ? AtPropeller[Axis] / BodyScale[Axis] : 0.0;
	}
	EngineAudio->SetRelativeLocation(AtPropeller);
	EngineAudio->RegisterComponent();
}

void UFluidNinjaBuoyancyComponent::LoadSheetSystem()
{
	const FSoftObjectPath& DefaultSheets = UFluidNinjaBuoyancySettings::Get().SpraySheetSystem;
	bSheetSystemTried = true;
	SheetSystemTried = SpraySheetSystem.IsNull() ? DefaultSheets : SpraySheetSystem.ToSoftObjectPath();
	LoadedSheetSystem = LoadOr(SpraySheetSystem, DefaultSheets);
	// An empty path is the documented "no sheets"; a system that is set and does not load is named once (it is not tried again until it
	// changes).
	if (!LoadedSheetSystem && SheetSystemTried.IsValid())
	{
		UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("FluidNinja Buoyancy on %s: the Spray Sheet System %s did not load as a Niagara system, so the body throws no spray sheets. Set Spray Sheet System on the body, or Project Settings -> FluidNinja Buoyancy -> Effects -> Spray Sheet System, to a system with the user parameters SprayRate, SprayVelocity, SprayCone, DropletSize and WaterZ (NS_FNB_HullSpray)."),
			*GetNameSafe(GetOwner()), *SheetSystemTried.ToString());
	}
}

void UFluidNinjaBuoyancyComponent::SettleOnWater(float WaterZ)
{
	UPrimitiveComponent* Primitive = GetSimulatedPrimitive();
	if (!Primitive || Hulls.IsEmpty())
	{
		ReleaseHold();
		return;
	}
	const float Draft = RestingDraft > 0.f ? RestingDraft : RestDraftCm;
	FVector Location = Primitive->GetComponentLocation();
	Location.Z = WaterZ - Draft - RestedBottom;   // the resting hull's bottom, Draft below the water
	// Lowered onto that pose from above with a sweep, so it never starts inside a rock (physics would launch it out). The pose: its
	// rest attitude, turned to the heading it has.
	const FQuat Attitude = RestAttitude.Quaternion();
	const FQuat Heading(FVector::ZAxisVector, FMath::DegreesToRadians((Primitive->GetComponentQuat() * Attitude.Inverse()).Rotator().Yaw));
	const FQuat Level = Heading * Attitude;
	const FVector Above = Location + FVector(0.0, 0.0, 300.0);
	Primitive->SetWorldLocationAndRotation(Above, Level, false, nullptr, ETeleportType::TeleportPhysics);
	FHitResult Hit;
	Primitive->SetWorldLocation(Location, true, &Hit, ETeleportType::TeleportPhysics);
	if (Hit.bStartPenetrating)
	{
		Primitive->SetWorldLocation(Above, false, nullptr, ETeleportType::TeleportPhysics);   // even the drop point is inside something
	}
	UE_LOG(LogFluidNinjaBuoyancy, Log, TEXT("FluidNinja Buoyancy %s settled on water at %.1f (bottom %.1f cm under it)%s"), *GetNameSafe(GetOwner()), WaterZ, Draft,
		Hit.bBlockingHit ? *FString::Printf(TEXT(", resting on %s %.0f cm higher"), *GetNameSafe(Hit.GetActor()), Primitive->GetComponentLocation().Z - Location.Z) : TEXT(""));
	MarkTeleported();   // put on the water, not dropped into it: no waves from the move
	ReleaseHold();
}

void UFluidNinjaBuoyancyComponent::ReleaseHold()
{
	MarkTeleported();   // let go where it was held: the waves start its static head and own field over from there
	bAwaitingWater = false;
	if (UPrimitiveComponent* Primitive = GetSimulatedPrimitive())
	{
		Primitive->SetSimulatePhysics(true);
		Primitive->SetPhysicsLinearVelocity(FVector::ZeroVector);
		Primitive->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector);
	}
}

void UFluidNinjaBuoyancyComponent::RestartAt(const FTransform& Transform)
{
	UPrimitiveComponent* Primitive = GetSimulatedPrimitive();
	UWorld* World = GetWorld();
	if (!Primitive || !World)
	{
		return;
	}
	Primitive->SetWorldLocationAndRotation(Transform.GetLocation(), Transform.GetRotation(), false, nullptr, ETeleportType::ResetPhysics);
	MarkTeleported();
	if (Primitive->IsSimulatingPhysics())
	{
		Primitive->SetPhysicsLinearVelocity(FVector::ZeroVector);
		Primitive->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector);
	}
	Readout = FFluidNinjaBuoyancyReadout();
	Contacts.Reset();
	FrameSlamWatts = 0.f;
	PendingSlamWatts = 0.f;
	PendingEntryWatts = 0.f;
	bHasWetPlane = false;
	WetTop = -1e9f;
	LastAngularVelocity = FVector::ZeroVector;
	if (UFluidNinjaBuoyancySubsystem* Hydro = World->GetSubsystem<UFluidNinjaBuoyancySubsystem>())
	{
		Hydro->Reregister(this);
	}
	// Held still until its water is read, then set on it, as at the start.
	if (bSettleOnStart)
	{
		Primitive->SetSimulatePhysics(false);
		bAwaitingWater = true;
		AwaitingSince = World->GetTimeSeconds();
	}
	else
	{
		ReleaseHold();
	}
}

float UFluidNinjaBuoyancyComponent::GetLowestPointZ() const
{
	const UPrimitiveComponent* Primitive = GetSimulatedPrimitive();
	if (!Primitive || Hulls.IsEmpty())
	{
		return 0.f;
	}
	const FluidNinjaBuoyancy::FHull& Hull = *Hulls[0];
	const FTransform Frame(Primitive->GetComponentQuat(), Primitive->GetComponentLocation());
	double Lowest = TNumericLimits<double>::Max();
	if (!Hull.Vertices.IsEmpty())   // the surface (point models keep it too)
	{
		for (const FVector3f& Vertex : Hull.Vertices)
		{
			Lowest = FMath::Min(Lowest, Frame.TransformPosition(FVector(Vertex)).Z);
		}
	}
	else
	{
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector Point((Corner & 1) ? Hull.BoundsMax.X : Hull.BoundsMin.X, (Corner & 2) ? Hull.BoundsMax.Y : Hull.BoundsMin.Y, (Corner & 4) ? Hull.BoundsMax.Z : Hull.BoundsMin.Z);
			Lowest = FMath::Min(Lowest, Frame.TransformPosition(Point).Z);
		}
	}
	return static_cast<float>(Lowest);
}

void UFluidNinjaBuoyancyComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	UPrimitiveComponent* Primitive = GetSimulatedPrimitive();
	if (!Primitive || Hulls.IsEmpty() || DeltaTime <= 0.f)
	{
		Washes.Reset();   // a body not updated washes nothing (GetWashes hands out only this frame's in any case)
		return;
	}
	UpdateEffectReach(*Primitive);
	UpdateWetLine(*Primitive, DeltaTime);
	UpdateSpray(DeltaTime);
	UpdateSheetsAndWash(*Primitive, DeltaTime);
	UpdateSound(*Primitive, DeltaTime);
	UpdateEngineSound(*Primitive, DeltaTime);
	FrameSlamWatts = 0.f;   // the sounds have had this frame's slam; the splash keeps its running max (PendingSlamWatts) until it fires
}

void UFluidNinjaBuoyancyComponent::DriveSheet(int32 Index, const FVector& Launch, const FVector& Velocity, float Rate, float ConeDeg, float DropletCm, float WaterZ)
{
	static const FName RateName(TEXT("User.SprayRate"));
	static const FName VelocityName(TEXT("User.SprayVelocity"));
	static const FName ConeName(TEXT("User.SprayCone"));
	static const FName DropletName(TEXT("User.DropletSize"));
	static const FName WaterZName(TEXT("User.WaterZ"));
	if (Sheets.Num() <= Index)
	{
		Sheets.SetNum(Index + 1);
	}
	UNiagaraComponent* Sheet = Sheets[Index];
	if (Rate <= 0.f)
	{
		// Stopped: no new drops, and the system deactivated, once, on the stop: the drops in the air fall and die where they land, and then
		// the system completes instead of running on empty (NS_FNB_HullSpray loops forever while it is active).
		if (Sheet)
		{
			Sheet->SetVariableFloat(RateName, 0.f);
			if (Sheet->IsActive() && Sheet->GetRequestedExecutionState() == ENiagaraExecutionState::Active)
			{
				Sheet->Deactivate();
			}
		}
		return;
	}
	if (!Sheet)
	{
		AActor* Owner = GetOwner();
		if (!LoadedSheetSystem || !Owner)
		{
			return;
		}
		// Not attached: its drops live in the world, so the sheet left behind by a fast hull stays where it was thrown; the emitter is
		// moved to its launch point every tick.
		Sheet = NewObject<UNiagaraComponent>(Owner);
		Sheet->SetAsset(LoadedSheetSystem);
		Sheet->SetAutoActivate(false);
		Sheet->SetAutoDestroy(false);
		Sheet->SetUsingAbsoluteLocation(true);
		Sheet->SetUsingAbsoluteRotation(true);
		Sheet->SetUsingAbsoluteScale(true);
		Sheet->RegisterComponent();
		Sheets[Index] = Sheet;
	}
	Sheet->SetWorldLocation(Launch);
	Sheet->SetVariableFloat(RateName, Rate);
	Sheet->SetVariableVec3(VelocityName, Velocity);
	Sheet->SetVariableFloat(ConeName, ConeDeg);
	Sheet->SetVariableFloat(DropletName, DropletCm);
	Sheet->SetVariableFloat(WaterZName, WaterZ);
	// Started when it is not running: never started, completed, or still letting the last run's drops fall after a stop (a deactivated
	// system counts as active until they have landed). Activating resets the system (EResetMode::ResetSystem), which keeps the
	// particles: the drops still in the air fall on.
	if (!Sheet->IsActive() || Sheet->GetRequestedExecutionState() != ENiagaraExecutionState::Active)
	{
		Sheet->Activate(true);
	}
}

/** A push's level after a frame: up to its target at once, down toward it no faster than full to none in FadeSeconds (0: at once). */
static float FadeLevel(float Level, float Target, float FadeSeconds, float DeltaTime)
{
	if (Target >= Level || FadeSeconds <= 0.f)
	{
		return Target;
	}
	return FMath::Max(Target, Level - DeltaTime / FadeSeconds);
}

void UFluidNinjaBuoyancyComponent::UpdateSheetsAndWash(UPrimitiveComponent& Primitive, float DeltaTime)
{
	Washes.Reset();
	SternRidge.Reset();
	WashesFrame = GFrameCounter;   // GetWashes hands these out this frame only
	const UWorld* World = GetWorld();
	const bool bLive = World && Readout.bInWater && !Readout.bDormant && !Hulls.IsEmpty() && Hulls[0].IsValid();
	const FluidNinjaBuoyancy::FHull* Hull = bLive ? Hulls[0].Get() : nullptr;
	const FTransform Frame(Primitive.GetComponentQuat(), Primitive.GetComponentLocation());
	const FVector Forward = Frame.GetUnitAxis(EAxis::X).GetSafeNormal2D();
	const FVector Right = Frame.GetUnitAxis(EAxis::Y).GetSafeNormal2D();
	const FVector Velocity = Primitive.IsSimulatingPhysics() ? Primitive.GetPhysicsLinearVelocity() : FVector::ZeroVector;
	const FVector Through(Velocity.X - Readout.FlowCmS.X, Velocity.Y - Readout.FlowCmS.Y, 0.0);
	const float Speed = static_cast<float>(Through.Size());
	const float Gravity = FMath::Max(1.f, World ? -World->GetGravityZ() : 980.f);
	// The water at the hull: the mean height of its water line.
	float WaterZ = static_cast<float>(Frame.GetLocation().Z);
	if (!Contacts.IsEmpty())
	{
		double Sum = 0.0;
		for (const FFluidNinjaWaterlineContact& Contact : Contacts)
		{
			Sum += Contact.Position.Z;
		}
		WaterZ = static_cast<float>(Sum / Contacts.Num());
	}
	// Where a drop launched at From with Launch lands on the water, and the patch it whitens there. The time it takes to fall back to WaterZ
	// is gravity's alone; over that time the drops' linear drag k (SprayDropDragPerSecond, the system's own) slows their way over the
	// water, so they carry x = v0 (1 - e^(-k t)) / k rather than v0 t (about 16 % shorter after a second in the air).
	const auto Landing = [Gravity, WaterZ](const FVector& From, const FVector& Launch, FVector2D& OutWhere, float& OutReach)
	{
		const double Up = Launch.Z;
		const double Drop = FMath::Max(From.Z - WaterZ, 0.0);
		const double Seconds = (Up + FMath::Sqrt(FMath::Max(Up * Up + 2.0 * Gravity * Drop, 0.0))) / Gravity;
		const double Drag = static_cast<double>(SprayDropDragPerSecond);
		const double Carried = (1.0 - FMath::Exp(-Drag * Seconds)) / Drag;
		OutWhere = FVector2D(From.X + Launch.X * Carried, From.Y + Launch.Y * Carried);
		OutReach = static_cast<float>(FVector2D(Launch.X, Launch.Y).Size() * Carried);
	};

	// ---- The whitewater, into FluidNinja Waves' foam wash: small foam patches scattered over where real whitewater forms around a moving
	// hull: fresh foam (bright, fading in seconds) and aged foam (the long pale lace). Four sources share the patch budget:
	//   the propeller trail: one or more jets (Prop Wash Jets) merging into one broad band behind the stern;
	//   the transom churn: the water closing behind a dry transom, a beam wide;
	//   the quarter wakes: the diverging waves breaking off the transom corners, two ragged bands spreading at the wake angle, darker water
	//     between them and the propeller trail;
	//   the sides: the spray root's white rim running aft along the hull, and the bow wave breaking at the stem.
	// Each patch's rate comes from the time a point of water spends under its source, so the foam reaches the same whiteness at any speed,
	// patch size or count. A band whose patches the water passes along at V (trail, churn, sides) leaves F = rate x N 2 pi s^2 / (W V) over a
	// band W wide; a band sweeping sideways over the water (a quarter wake at the wake angle a) leaves F = rate x N 2 pi s^2 / (L V sin a)
	// over a band L long (N patches of spread s). Rates are per second at a patch's centre (FFluidNinjaWaterWash).
	if (Hull && Speed > 30.f)
	{
		FRandomStream Rng(static_cast<int32>((GFrameCounter * 2654435761ull) ^ static_cast<uint64>(GetUniqueID())));
		const float Ragged = FMath::Clamp(WhitewaterRaggedness, 0.f, 1.f);
		const auto Gauss = [&Rng]() { return (Rng.FRand() + Rng.FRand() + Rng.FRand() - 1.5f) * 2.f; };   // about a unit normal
		const auto Flicker = [&Rng, Ragged]() { return 1.f - Ragged * Rng.FRand(); };
		const float HullLength = FMath::Max(Hull->BoundsMax.X - Hull->BoundsMin.X, 20.f);
		const float Beam = FMath::Max(Hull->BoundsMax.Y - Hull->BoundsMin.Y, 20.f);
		const float MidY = 0.5f * (Hull->BoundsMin.Y + Hull->BoundsMax.Y);
		const FVector Back = -Through.GetSafeNormal2D(UE_SMALL_NUMBER, Forward);
		const FVector Across(-Back.Y, Back.X, 0.0);
		const FVector SternCentre = Frame.TransformPosition(FVector(Hull->BoundsMin.X, MidY, 0.0));
		const FVector Centre = Frame.GetLocation();
		const float Froude = Speed / FMath::Sqrt(Gravity * HullLength);
		const float V = FMath::Max(Speed, 200.f);
		const float TwoPi = 2.f * PI;
		const auto Add = [this](const FVector& At, float Spread, float Fresh, float Aged)
		{
			if (!(Fresh > 0.f) && !(Aged > 0.f))
			{
				return;
			}
			FFluidNinjaWaterWash& Wash = Washes.AddDefaulted_GetRef();
			Wash.Location = FVector2D(At.X, At.Y);
			Wash.RadiusCm = Spread;
			Wash.FreshRate = FMath::Max(Fresh, 0.f);
			Wash.AgedRate = FMath::Max(Aged, 0.f);
		};

		// Which sources run, and how strongly.
		const bool bJets = bPropulsion && PropulsionModel == EFluidNinjaPropulsionModel::Propeller && PropWashFoam > 0.f && Readout.Gear != 0
			&& Readout.PropImmersion > 0.05f;
		const float FanStrength = AirFanWashFoam > 0.f ? FMath::Clamp(AirFanThrottle, 0.f, 1.f) : 0.f;
		const float JetStrength = bJets ? Readout.PropImmersion * FMath::Clamp(0.25f + Readout.PropJetExcessCmS / 400.f + 0.5f * Speed / 1500.f, 0.f, 1.5f) : 0.f;
		const float Clear = 2.5f * FMath::Sqrt(Gravity * FMath::Max(RestingDraft > 0.f ? RestingDraft : RestDraftCm, 2.f));
		const float SternRamp = IsPlaningHull() && SternWashFoam > 0.f ? FMath::SmoothStep(0.6f * Clear, 1.3f * Clear, Speed) : 0.f;
		const float QuarterRamp = IsPlaningHull() && QuarterWakeFoam > 0.f ? FMath::SmoothStep(0.4f, 0.9f, Froude) : 0.f;
		const float SideRamp = (IsPlaningHull() || bPropulsion) && HullSideFoam > 0.f && !Contacts.IsEmpty() ? FMath::SmoothStep(150.f, 600.f, Speed) : 0.f;
		const float Weights[5] = {JetStrength > 0.01f ? 0.35f : 0.f, SternRamp > 0.f ? 0.15f : 0.f, QuarterRamp > 0.f ? 0.35f : 0.f, SideRamp > 0.f ? 0.15f : 0.f,
			FanStrength > 0.01f ? 0.25f : 0.f};
		const float WeightSum = Weights[0] + Weights[1] + Weights[2] + Weights[3] + Weights[4];
		const int32 Budget = FMath::Clamp(WhitewaterPatches, 16, 600) / (EffectReach <= 1 ? 1 : 4);
		const auto Share = [&](int32 Index) { return WeightSum > 0.f ? FMath::Max(FMath::RoundToInt32(Budget * Weights[Index] / WeightSum), Weights[Index] > 0.f ? 4 : 0) : 0; };
		const float MinSpread = 25.f;   // cm, about a wave cell: finer patches fall between the cells

		// The propeller trail: each jet's boil behind the disc, the jets spreading at about 7 degrees and merging; denser near the stern.
		if (const int32 N = Share(0); N > 0)
		{
			const int32 Jets = FMath::Clamp(PropWashJets, 1, 4);
			const float Diameter = FMath::Max(PropDiameterCm, 5.f);
			FVector Jet = -Readout.PropAxis * static_cast<double>(Readout.Gear);
			Jet.Z = 0.0;
			Jet = Jet.GetSafeNormal(UE_SMALL_NUMBER, Back);
			const FVector JetSide(-Jet.Y, Jet.X, 0.0);
			const float JetLength = 12.f * Diameter + 0.8f * HullLength;
			const float Slope = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(PropWashSpreadDeg, 0.f, 20.f)));
			const float Spacing = FMath::Max(PropWashJetSpacingCm, 0.f);
			const float Width = (Jets - 1) * Spacing + 2.f * Diameter + JetLength * Slope;
			const float Merge = 4.f * Diameter + 0.3f * HullLength;
			for (int32 Index = 0; Index < N; ++Index)
			{
				const float Offset = (static_cast<float>(Index % Jets) - 0.5f * static_cast<float>(Jets - 1)) * Spacing;
				const float U = Rng.FRand();
				const float Along = JetLength * U * U;
				const float Spread = FMath::Max(0.35f * Diameter + 0.06f * Along, MinSpread) * (0.75f + 0.5f * Rng.FRand());
				const float Lateral = Offset * FMath::Exp(-Along / Merge) + Gauss() * (0.25f * Diameter + Along * Slope) * (0.5f + Ragged);
				const FVector At = Readout.PropLocation + Jet * (Along + Gauss() * Ragged * Spread) + JetSide * Lateral;
				const float Rate = Width * V / (static_cast<float>(N) * TwoPi * Spread * Spread) * FMath::Exp(-Along / (0.6f * JetLength)) * Flicker();
				Add(At, Spread, 2.5f * PropWashFoam * JetStrength * Rate, 0.8f * PropWashFoam * JetStrength * Rate);
			}
		}

		// The air fan's slipstream: one broad, soft stream about the fan's width, from the transom (or the fan, behind it) back along the
		// hull's axis, widening at the spread angle and fading aft; milder than a propeller's boil, and denser near the stern.
		if (const int32 N = Share(4); N > 0)
		{
			const float Diameter = FMath::Clamp(AirFanDiameterCm, 50.f, 400.f);
			const FVector Blow = -Forward;
			const FVector BlowSide(-Blow.Y, Blow.X, 0.0);
			const FVector Origin = Frame.TransformPosition(FVector(FMath::Min(static_cast<double>(Hull->BoundsMin.X), AirFanLocation.X), AirFanLocation.Y, 0.0));
			const float StreamLength = 4.f * Diameter + 0.6f * HullLength;
			const float Slope = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(AirFanWashSpreadDeg, 0.f, 30.f)));
			const float Width = Diameter + StreamLength * Slope;
			for (int32 Index = 0; Index < N; ++Index)
			{
				const float U = Rng.FRand();
				const float Along = StreamLength * U * U;
				const float Spread = FMath::Max(0.3f * Diameter + 0.08f * Along, MinSpread) * (0.75f + 0.5f * Rng.FRand());
				const float Lateral = Gauss() * (0.3f * Diameter + Along * Slope) * (0.5f + Ragged);
				const FVector At = Origin + Blow * (Along + Gauss() * Ragged * Spread) + BlowSide * Lateral;
				const float Rate = Width * V / (static_cast<float>(N) * TwoPi * Spread * Spread) * FMath::Exp(-Along / (0.6f * StreamLength)) * Flicker();
				Add(At, Spread, 2.5f * AirFanWashFoam * FanStrength * Rate, 0.8f * AirFanWashFoam * FanStrength * Rate);
			}
		}

		// The transom churn: across the beam, from the transom back about half a beam plus a sixth of the hull's length.
		if (const int32 N = Share(1); N > 0)
		{
			const float Reach = 0.6f * Beam + 0.15f * HullLength;
			const float Width = 0.9f * Beam;
			for (int32 Index = 0; Index < N; ++Index)
			{
				const float Along = Reach * Rng.FRand();
				const float Spread = FMath::Max(0.12f * Beam, MinSpread) * (0.6f + 0.8f * Rng.FRand());
				const FVector At = SternCentre + Back * (Along + Gauss() * Ragged * Spread) + Across * ((Rng.FRand() - 0.5f) * Width);
				const float Rate = Width * V / (static_cast<float>(N) * TwoPi * Spread * Spread) * FMath::Exp(-Along / (0.6f * Reach)) * Flicker();
				Add(At, Spread, 2.f * SternWashFoam * SternRamp * Rate, 0.5f * SternWashFoam * SternRamp * Rate);
			}
		}

		// The quarter wakes: from each transom corner along the wake angle (Kelvin's 19.47 degrees, narrowing above a length Froude number of
		// about 0.6 as 1 / (sqrt(2 pi) Fr), Rabaud and Moisy 2013), ragged across the band, fading along it.
		if (const int32 N = Share(2); N > 0)
		{
			const float Alpha = FMath::Min(0.3398f, 1.f / (FMath::Sqrt(TwoPi) * FMath::Max(Froude, 0.01f))) * FMath::Clamp(QuarterWakeAngleScale, 0.2f, 1.5f);
			const float SinAlpha = FMath::Max(FMath::Sin(Alpha), 0.05f);
			const float CosAlpha = FMath::Cos(Alpha);
			const float ArmLength = FMath::Clamp(QuarterWakeLength, 0.5f, 10.f) * HullLength;
			const float ArmStart = FMath::Clamp(QuarterWakeStart, 0.f, 2.f) * HullLength;   // where the crests start breaking, behind the corner
			const int32 PerArm = FMath::Max(N / 2, 2);
			const float RootOut = FMath::Clamp(QuarterWakeOffset, 0.f, 1.f) * Beam;   // where the breaking starts, outside each corner
			// The breaking crests (Quarter Wake Crest Spacing Cm): diagonal streaks fixed on the water, so the foam each crest leaves stays
			// where it broke while the boat runs on. Crests run outward and aft at 55 degrees to the track; Spacing is along the track.
			const float CrestSpacing = FMath::Clamp(QuarterWakeCrestSpacingCm, 0.f, 2000.f);
			const float CrestStrength = CrestSpacing >= 10.f ? FMath::Clamp(QuarterWakeCrestStrength, 0.f, 1.f) : 0.f;
			const float CrestAngle = FMath::DegreesToRadians(55.f);
			const double CrestPeriod = CrestSpacing * FMath::Sin(CrestAngle);   // across the crests
			for (int32 Side = 0; Side < 2; ++Side)
			{
				const float CornerY = Side == 0 ? Hull->BoundsMin.Y + 0.08f * Beam : Hull->BoundsMax.Y - 0.08f * Beam;
				const FVector Corner = Frame.TransformPosition(FVector(Hull->BoundsMin.X, CornerY, 0.0));
				const double Sign = FVector::DotProduct(Corner - SternCentre, Across) >= 0.0 ? 1.0 : -1.0;
				const FVector Out = Across * Sign;
				const FVector Arm = Back * CosAlpha + Out * SinAlpha;
				const FVector Root = Corner + Out * RootOut;
				const FVector CrestLine = Back * FMath::Cos(CrestAngle) + Out * FMath::Sin(CrestAngle);
				const FVector CrestNormal = Back * FMath::Sin(CrestAngle) - Out * FMath::Cos(CrestAngle);
				for (int32 Index = 0; Index < PerArm; ++Index)
				{
					const float Breaking = ArmLength * FMath::Pow(Rng.FRand(), 1.5f);   // along the breaking band, from its start
					const float Along = ArmStart + Breaking;
					const float Spread = FMath::Max(0.06f * Beam + 0.02f * Along, MinSpread) * (0.6f + 0.8f * Rng.FRand());
					const FVector At = Root + Arm * (Along + Gauss() * Ragged * Spread) + Out * (Gauss() * (0.1f * Beam + 0.05f * Along) * (0.4f + Ragged));
					// On a crest c = 1, between crests 0; the crest lines wander a little so they do not read as ruled. c^2 x 8/3 averages 1.
					float Crest = 1.f;
					if (CrestStrength > 0.f)
					{
						const double CrestU = FVector::DotProduct(At, CrestNormal) / CrestPeriod;
						const double CrestV = FVector::DotProduct(At, CrestLine) / CrestSpacing;
						const double Phase = CrestU + 0.25 * FMath::Sin(2.0 * UE_DOUBLE_PI * CrestV / 1.7) + 0.15 * FMath::Sin(2.0 * UE_DOUBLE_PI * CrestU / 3.1);
						const float C = 0.5f + 0.5f * static_cast<float>(FMath::Cos(2.0 * UE_DOUBLE_PI * Phase));
						Crest = 1.f - CrestStrength + CrestStrength * C * C * (8.f / 3.f);
					}
					const float Rate = ArmLength * V * SinAlpha / (static_cast<float>(PerArm) * TwoPi * Spread * Spread) * FMath::Exp(-Breaking / (0.45f * ArmLength)) * Flicker() * Crest;
					Add(At, Spread, 1.6f * QuarterWakeFoam * QuarterRamp * Rate, 0.6f * QuarterWakeFoam * QuarterRamp * Rate);
				}
			}
		}

		// The stern ridge (Stern Ridge Height Cm): a steady upward push along the track behind the dry transom, strongest half way back (a
		// sin² profile), Stern Ridge Width wide at half height. Pieces about 0.75 σ apart along the track; the splat spreads each over
		// σ = FNB_WAVE_SPLAT_SPREAD (3) x half its area's disc radius, so the area that gives σ goes in as the piece's (negative: steady).
		// Each piece's impulse is the head's over the strip it stands for (its length along, the Gaussian's σ √(2π) across) over the frame.
		if (SternRidgeHeightCm > 0.f && IsPlaningHull())
		{
			SternRidgeLevel = FadeLevel(SternRidgeLevel, FMath::SmoothStep(0.6f * Clear, 1.3f * Clear, Speed), SternWaveFadeSeconds, DeltaTime);
			const float Ramp = SternRidgeLevel;
			const float Length = FMath::Clamp(SternRidgeLength, 0.1f, 4.f) * HullLength;
			const float Sigma = FMath::Max(FMath::Clamp(SternRidgeWidth, 0.1f, 2.f) * Beam / 2.3548f, 15.f);
			const int32 Count = FMath::Clamp(FMath::CeilToInt32(Length / FMath::Max(0.75f * Sigma, 10.f)), 2, 64);
			const float Strip = Length / static_cast<float>(Count);
			const float SplatArea = PI * FMath::Square(2.f * Sigma / 3.f);
			constexpr float WaterDensity = 0.001f;   // kg/cm³
			for (int32 Index = 0; Ramp > 0.f && Index < Count; ++Index)
			{
				const float Behind = (static_cast<float>(Index) + 0.5f) * Strip;
				const float Head = FMath::Clamp(SternRidgeHeightCm, 0.f, 80.f) * Ramp * FMath::Square(FMath::Sin(PI * Behind / Length));
				const FVector At = SternCentre + Back * Behind;
				const float Impulse = -WaterDensity * Gravity * Head * Sigma * FMath::Sqrt(TwoPi) * Strip * DeltaTime;
				SternRidge.Add(FVector4f(static_cast<float>(At.X), static_cast<float>(At.Y), Impulse, -SplatArea));
			}
		}

		// The quarter waves' lift (Quarter Wake Lift Cm): the same steady upward push along each quarter wake's line (its root Quarter Wake
		// Offset outside the corner, Quarter Wake Start behind it, at the wake angle the quarter wakes' whitewater takes), a sin² profile
		// over Quarter Wake Lift Length, Quarter Wake Lift Width wide at half height, once the hull is on the plane (the quarter wakes' ramp).
		if (QuarterWakeLiftCm > 0.f && IsPlaningHull())
		{
			QuarterLiftLevel = FadeLevel(QuarterLiftLevel, FMath::SmoothStep(0.4f, 0.9f, Froude), SternWaveFadeSeconds, DeltaTime);
			const float Ramp = QuarterLiftLevel;
			const float Angle = FMath::Min(0.3398f, 1.f / (FMath::Sqrt(TwoPi) * FMath::Max(Froude, 0.01f))) * FMath::Clamp(QuarterWakeAngleScale, 0.2f, 1.5f);
			const float Length = FMath::Clamp(QuarterWakeLiftLength, 0.2f, 4.f) * HullLength;
			const float Start = FMath::Clamp(QuarterWakeStart, 0.f, 2.f) * HullLength;
			const float Sigma = FMath::Max(FMath::Clamp(QuarterWakeLiftWidth, 0.1f, 2.f) * Beam / 2.3548f, 15.f);
			const int32 Count = FMath::Clamp(FMath::CeilToInt32(Length / FMath::Max(0.75f * Sigma, 10.f)), 2, 64);
			const float Strip = Length / static_cast<float>(Count);
			const float SplatArea = PI * FMath::Square(2.f * Sigma / 3.f);
			constexpr float WaterDensity = 0.001f;   // kg/cm³
			for (int32 Side = 0; Ramp > 0.f && Side < 2; ++Side)
			{
				const float CornerY = Side == 0 ? Hull->BoundsMin.Y + 0.08f * Beam : Hull->BoundsMax.Y - 0.08f * Beam;
				const FVector Corner = Frame.TransformPosition(FVector(Hull->BoundsMin.X, CornerY, 0.0));
				const FVector Out = Across * (FVector::DotProduct(Corner - SternCentre, Across) >= 0.0 ? 1.0 : -1.0);
				const FVector Line = Back * FMath::Cos(Angle) + Out * FMath::Sin(Angle);
				const FVector Root = Corner + Out * (FMath::Clamp(QuarterWakeOffset, 0.f, 1.f) * Beam);
				for (int32 Index = 0; Index < Count; ++Index)
				{
					const float Along = (static_cast<float>(Index) + 0.5f) * Strip;
					const float Head = FMath::Clamp(QuarterWakeLiftCm, 0.f, 80.f) * Ramp * FMath::Square(FMath::Sin(PI * Along / Length));
					const FVector At = Root + Line * (Start + Along);
					const float Impulse = -WaterDensity * Gravity * Head * Sigma * FMath::Sqrt(TwoPi) * Strip * DeltaTime;
					SternRidge.Add(FVector4f(static_cast<float>(At.X), static_cast<float>(At.Y), Impulse, -SplatArea));
				}
			}
		}

		// The sides: the spray root's rim on the water line from about mid-length aft (whiter toward the transom), just outside the hull, and
		// wherever the hull drives hard into the water (the bow wave breaking at the stem).
		if (const int32 N = Share(3); N > 0)
		{
			const float Width = 0.4f * Beam;
			for (int32 Index = 0; Index < N; ++Index)
			{
				const FFluidNinjaWaterlineContact& Contact = Contacts[Rng.RandRange(0, Contacts.Num() - 1)];
				const FVector Facing(Contact.Outward.X, Contact.Outward.Y, 0.0);
				const double Ahead = FVector::DotProduct(Contact.Position - Centre, Forward);
				const bool bSide = FMath::Abs(FVector::DotProduct(Facing.GetSafeNormal(), Right)) > 0.3;
				const float Rim = bSide ? 1.f - FMath::SmoothStep(-0.45f * HullLength, 0.2f * HullLength, static_cast<float>(Ahead)) : 0.f;
				const float Bow = FMath::Clamp(Contact.Push / 400.f, 0.f, 1.5f);
				const float Weight = FMath::Max(Rim, Bow);
				if (Weight < 0.02f)
				{
					continue;
				}
				const float Spread = FMath::Max(0.05f * Beam, MinSpread) * (0.7f + 0.6f * Rng.FRand());
				const FVector At = Contact.Position + Facing.GetSafeNormal() * (0.8f * Spread + 10.f + Rng.FRand() * Ragged * 0.15f * Beam)
					+ Back * (Gauss() * Ragged * Spread);
				const float Rate = Width * V / (static_cast<float>(N) * TwoPi * Spread * Spread) * Weight * Flicker();
				Add(At, Spread, 1.2f * HullSideFoam * SideRamp * Rate, 0.3f * HullSideFoam * SideRamp * Rate);
			}
		}
	}

	// The stern ridge and the quarter lift die away at their fade while the hull is too slow (or out of the water) to push them.
	if (!(Hull && Speed > 30.f))
	{
		SternRidgeLevel = FadeLevel(SternRidgeLevel, 0.f, SternWaveFadeSeconds, DeltaTime);
		QuarterLiftLevel = FadeLevel(QuarterLiftLevel, 0.f, SternWaveFadeSeconds, DeltaTime);
	}

	// The spray sheets.
	const bool bSheets = Hull && bSpraySheets && EffectsSpraySheets != 0 && EffectReach <= 1;
	if (bSheets && !LoadedSheetSystem)
	{
		// Spray Sheets turned on after Begin Play, or a system that did not load: tried once per system (Spray Sheet System, else the
		// project's), not again every tick until that changes.
		const FSoftObjectPath& DefaultSheets = UFluidNinjaBuoyancySettings::Get().SpraySheetSystem;
		if (!bSheetSystemTried || (SpraySheetSystem.IsNull() ? DefaultSheets : SpraySheetSystem.ToSoftObjectPath()) != SheetSystemTried)
		{
			LoadSheetSystem();
		}
	}
	const float RateScale = FMath::Max(SpraySheetRate, 0.f) * FMath::Max(EffectsSprayRateScale, 0.f);
	const float Deadrise = FMath::DegreesToRadians(FMath::Clamp(SprayDeadriseDeg, 0.f, 45.f));
	const float Elevation = FMath::DegreesToRadians(SheetElevationDeg);
	const FVector HullCentre = Frame.GetLocation();
	const float Length = Hull ? FMath::Max(Hull->BoundsMax.X - Hull->BoundsMin.X, 1.f) : 1.f;
	// Whisker sheets: off each side where the water line drives in hardest, forward of the stern's last third, launched at 2 V sin β out
	// and up (in the water's frame) with a little of the hull's way.
	for (int32 Side = 0; Side < 2; ++Side)
	{
		const float Sign = Side == 0 ? -1.f : 1.f;   // port, starboard
		const FFluidNinjaWaterlineContact* Root = nullptr;
		if (bSheets)
		{
			for (const FFluidNinjaWaterlineContact& Contact : Contacts)
			{
				const double Across = FVector::DotProduct(Contact.Outward, Right) * Sign;
				const double Ahead = FVector::DotProduct(Contact.Position - HullCentre, Forward);
				if (Across > 0.3 && Ahead > -Length / 6.0 && Contact.Push > 0.f && (!Root || Contact.Push > Root->Push))
				{
					Root = &Contact;
				}
			}
		}
		const float Start = FMath::Max(SpraySheetMinSpeedCmS, 1.f);
		const bool bRun = Root && Speed >= (bSheetOn[Side] ? SheetHysteresis * Start : Start);
		bSheetOn[Side] = bRun;
		if (!bRun)
		{
			DriveSheet(Side, FVector::ZeroVector, FVector::ZeroVector, 0.f, 0.f, 0.f, 0.f);
			continue;
		}
		const FVector Out = FVector(Root->Outward.X, Root->Outward.Y, 0.0).GetSafeNormal();
		const float SheetSpeed = FMath::Min(2.f * Speed * FMath::Sin(FMath::Max(Deadrise, FMath::DegreesToRadians(6.f))), SheetMaxSpeedCmS);
		const FVector Launch = Out * (SheetSpeed * FMath::Cos(Elevation)) + FVector::UpVector * (SheetSpeed * FMath::Sin(Elevation)) + Through.GetSafeNormal() * (0.1f * Speed);
		const float Rate = FMath::Min(SheetParticlesPerSecond * ((Speed - Start) / Start + 0.3f), SheetMaxParticlesPerSecond) * RateScale;
		const FVector From = Root->Position + Out * 8.0 + FVector(0.0, 0.0, 3.0);
		DriveSheet(Side, From, Launch, Rate, 18.f, 4.f, static_cast<float>(Root->Position.Z));
		if (SprayLandingFoam > 0.f && Rate > 0.f)
		{
			FFluidNinjaWaterWash& Wash = Washes.AddDefaulted_GetRef();
			float Reach = 0.f;
			Landing(From, Launch, Wash.Location, Reach);
			Wash.RadiusCm = FMath::Max(0.3f * Reach, 30.f);
			Wash.FreshRate = SprayLandingFoam * 1.5f * FMath::Min(Rate / SheetParticlesPerSecond, 2.f);
			Wash.AgedRate = 0.2f * Wash.FreshRate;
		}
	}
	// The rooster tail: thrown up behind a fast propelled planing stern (a planing Triangles hull, as for the stern's whitewater), falling
	// behind the boat.
	{
		const float Start = FMath::Max(RoosterTailMinSpeedCmS, 1.f);
		const bool bRun = bSheets && bRoosterTail && bPropulsion && IsPlaningHull() && Speed >= (bSheetOn[2] ? SheetHysteresis * Start : Start);
		bSheetOn[2] = bRun;
		if (!bRun)
		{
			DriveSheet(2, FVector::ZeroVector, FVector::ZeroVector, 0.f, 0.f, 0.f, 0.f);
		}
		else
		{
			const FVector Stern = Frame.TransformPosition(FVector(Hull->BoundsMin.X - 40.f, 0.5f * (Hull->BoundsMin.Y + Hull->BoundsMax.Y), 0.0));
			const FVector From(Stern.X, Stern.Y, WaterZ + 5.f);
			const FVector Launch = FVector::UpVector * FMath::Min(0.3f * Speed, 700.f) - Forward * (0.1f * Speed);
			const float Rate = FMath::Min(0.8f * SheetParticlesPerSecond * ((Speed - Start) / Start + 0.3f), SheetMaxParticlesPerSecond) * RateScale;
			DriveSheet(2, From, Launch, Rate, 14.f, 5.f, WaterZ);
			if (SprayLandingFoam > 0.f && Rate > 0.f)
			{
				FFluidNinjaWaterWash& Wash = Washes.AddDefaulted_GetRef();
				float Reach = 0.f;
				Landing(From, Launch, Wash.Location, Reach);
				Wash.RadiusCm = FMath::Max(0.4f * Reach, 50.f);
				Wash.FreshRate = SprayLandingFoam * 2.f * FMath::Min(Rate / SheetParticlesPerSecond, 2.f);
				Wash.AgedRate = 0.3f * Wash.FreshRate;
			}
		}
	}
}

const TArray<FFluidNinjaWaterWash>& UFluidNinjaBuoyancyComponent::GetWashes() const
{
	// This frame's only: a body whose tick has stopped (turned off, or no hull) would otherwise wash its last frame's whitewater into the
	// foam for ever. The component ticks in TG_PostPhysics, and the subsystem reads this after the tick groups of the same frame.
	static const TArray<FFluidNinjaWaterWash> NoWashes;
	return WashesFrame == GFrameCounter ? Washes : NoWashes;
}

const TArray<FVector4f>& UFluidNinjaBuoyancyComponent::GetSternRidge() const
{
	static const TArray<FVector4f> NoRidge;
	return WashesFrame == GFrameCounter ? SternRidge : NoRidge;
}

void UFluidNinjaBuoyancyComponent::UpdateEngineSound(UPrimitiveComponent& Primitive, float DeltaTime)
{
	// The engine loop is made the first time the propeller model runs, so a Propulsion Model set after Begin Play gets one too. A sound
	// that did not load is not tried again every tick: only once the propeller model is left and taken up again, or the sound (Engine
	// Sound, else the project's) changes.
	if (!EngineAudio)
	{
		if (!bPropulsion || PropulsionModel != EFluidNinjaPropulsionModel::Propeller)
		{
			bEngineAudioTried = false;
			return;
		}
		const FSoftObjectPath& DefaultEngine = UFluidNinjaBuoyancySettings::Get().EngineSound;
		if (bEngineAudioTried && (EngineSound.IsNull() ? DefaultEngine : EngineSound.ToSoftObjectPath()) == EngineAudioSoundTried)
		{
			return;
		}
		MakeEngineAudio(Primitive);
		if (!EngineAudio)
		{
			return;
		}
	}
	const bool bRun = bPropulsion && PropulsionModel == EFluidNinjaPropulsionModel::Propeller && !Readout.bDormant && SoundVolume > 0.f;
	if (!bRun)
	{
		if (EngineAudio->IsPlaying())
		{
			EngineAudio->Stop();
		}
		return;
	}
	if (!EngineAudio->IsPlaying())
	{
		EngineAudio->Play();
		for (float& Sent : EngineSent)
		{
			Sent = -1.f;   // a fresh play starts with every value sent
		}
	}
	EngineAudio->SetVolumeMultiplier(SoundVolume);
	// Each value goes to the audio thread only when it moves: the rpm by 10, the load and throttle by 0.02, the speed by 5 cm/s.
	const float Values[5] = {Readout.EngineRpm, Readout.EngineLoad, FMath::Abs(Throttle), Readout.bPropVentilated ? 1.f : 0.f, Readout.SpeedThroughWaterCmS};
	const float Steps[5] = {10.f, 0.02f, 0.02f, 0.f, 5.f};
	static const FName Names[5] = {FName(TEXT("RPM")), FName(TEXT("Load")), FName(TEXT("Throttle")), FName(TEXT("Ventilated")), FName(TEXT("Speed"))};
	for (int32 Index = 0; Index < 5; ++Index)
	{
		if (FMath::Abs(Values[Index] - EngineSent[Index]) > Steps[Index])
		{
			EngineAudio->SetFloatParameter(Names[Index], Values[Index]);
			EngineSent[Index] = Values[Index];
		}
	}
}

void UFluidNinjaBuoyancyComponent::UpdateEffectReach(const UPrimitiveComponent& Primitive)
{
	// The viewer: the first local player's camera, else its pawn. With no viewer at all (a dedicated server, or no player yet) the
	// distance is taken as zero and only whether the body was drawn decides, which on a server it never is.
	const UWorld* World = GetWorld();
	EffectReach = 2;
	if (!World || !Primitive.WasRecentlyRendered(1.f))
	{
		return;
	}
	double Distance = 0.0;
	if (const APlayerController* Controller = World->GetFirstPlayerController())
	{
		if (Controller->PlayerCameraManager)
		{
			Distance = FVector::Distance(Controller->PlayerCameraManager->GetCameraLocation(), Primitive.Bounds.Origin);
		}
		else if (const APawn* Pawn = Controller->GetPawn())
		{
			Distance = FVector::Distance(Pawn->GetActorLocation(), Primitive.Bounds.Origin);
		}
	}
	const FVector2D Limits = LodDistancesCm * FMath::Max(EffectsReachScale, 0.01f);
	EffectReach = Distance > Limits.Y ? 2 : Distance > Limits.X ? 1 : 0;
}

bool UFluidNinjaBuoyancyComponent::SpawnSplash(UWorld& World, const FVector& Where, const FRotator& Facing, float Strength, bool bSecondary)
{
	if (!LoadedSplash || Strength < SmallestSplash || EffectReach >= 2 || (bSecondary && EffectReach >= 1) || !TakeSplashBudget(EffectReach))
	{
		return false;
	}
	UNiagaraFunctionLibrary::SpawnSystemAtLocation(&World, LoadedSplash, Where, Facing, FVector(SplashScale * Strength), true, true, ENCPoolMethod::AutoRelease);
	INC_DWORD_STAT(STAT_FNB_SplashSpawns);
	return true;
}

void UFluidNinjaBuoyancyComponent::ReceiveContacts(TArrayView<const FFluidNinjaWaterlineContact> NewContacts, float Wash, float WaterlineLength)
{
	Contacts.Reset();
	Contacts.Append(NewContacts.GetData(), NewContacts.Num());
	ContactWash = Wash;
	ContactLength = WaterlineLength;
}

void UFluidNinjaBuoyancyComponent::UpdateWetLine(UPrimitiveComponent& Primitive, float DeltaTime)
{
	if (!bWetLine)
	{
		return;
	}
	if (EffectsWetLine == 0)
	{
		// The tier switched the wet line off: a hull that had one is written dry once (its water line far below it), then left alone.
		if (bHasWetPlane)
		{
			bHasWetPlane = false;
			WetTop = -1e9f;
			const float Dry[4] = {-1e6f, 0.f, 0.f, 0.f};
			for (int32 Index = 0; Index < 4; ++Index)
			{
				Primitive.SetCustomPrimitiveDataFloat(WetLineDataIndex + Index, Dry[Index]);
			}
		}
		return;
	}
	// The water line plane through the contacts, in the mesh's local space: z = A + B x + C y (least squares).
	const FTransform& Frame = Primitive.GetComponentTransform();
	double S[3][3] = {};
	double R[3] = {};
	int32 Count = 0;
	for (const FFluidNinjaWaterlineContact& Contact : Contacts)
	{
		const FVector Local = Frame.InverseTransformPosition(Contact.Position);
		const double Row[3] = {1.0, Local.X, Local.Y};
		for (int32 I = 0; I < 3; ++I)
		{
			for (int32 J = 0; J < 3; ++J)
			{
				S[I][J] += Row[I] * Row[J];
			}
			R[I] += Row[I] * Local.Z;
		}
		++Count;
	}
	bool bFit = false;
	FVector Plane = FVector::ZeroVector;
	if (Count >= 3)
	{
		auto Det = [](const double M[3][3])
		{
			return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
		};
		const double D = Det(S);
		if (FMath::Abs(D) > 1e-3)
		{
			double Solved[3];
			for (int32 Column = 0; Column < 3; ++Column)
			{
				double M[3][3];
				for (int32 I = 0; I < 3; ++I)
				{
					for (int32 J = 0; J < 3; ++J)
					{
						M[I][J] = J == Column ? R[I] : S[I][J];
					}
				}
				Solved[Column] = Det(M) / D;
			}
			Plane = FVector(Solved[0], FMath::Clamp(Solved[1], -1.0, 1.0), FMath::Clamp(Solved[2], -1.0, 1.0));
			bFit = true;
		}
	}
	if (!bFit && Count > 0)
	{
		Plane = FVector(R[0] / Count, 0.0, 0.0);
		bFit = true;
	}
	if (bFit)
	{
		if (!bHasWetPlane)
		{
			WetPlane = Plane;
			WetTop = static_cast<float>(Plane.X);
			bHasWetPlane = true;
		}
		WetPlane = FMath::Lerp(WetPlane, Plane, FMath::Min(1.f, DeltaTime * 12.f));
		WetTop = FMath::Max(static_cast<float>(WetPlane.X), WetTop - DryingSpeedCmS * DeltaTime);
	}
	else if (bHasWetPlane)
	{
		// Out of the water: the hull dries from the top down.
		WetPlane.X -= DryingSpeedCmS * DeltaTime;
		WetTop = static_cast<float>(WetPlane.X);
	}
	if (!bHasWetPlane)
	{
		return;
	}
	const float Above = FMath::Max(0.f, WetTop - static_cast<float>(WetPlane.X));
	const TArray<float>& Current = Primitive.GetCustomPrimitiveData().Data;
	const float Values[4] = {static_cast<float>(WetPlane.X), static_cast<float>(WetPlane.Y), static_cast<float>(WetPlane.Z), Above};
	bool bChanged = Current.Num() < WetLineDataIndex + 4;
	for (int32 Index = 0; Index < 4 && !bChanged; ++Index)
	{
		bChanged = FMath::Abs(Current[WetLineDataIndex + Index] - Values[Index]) > (Index == 1 || Index == 2 ? 0.002f : 0.2f);
	}
	if (bChanged)
	{
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Primitive.SetCustomPrimitiveDataFloat(WetLineDataIndex + Index, Values[Index]);
		}
	}
}

void UFluidNinjaBuoyancyComponent::UpdateSpray(float DeltaTime)
{
	UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	if (PendingEntryWatts > 0.f && Now - PendingEntryTime > EntrySplashSeconds)
	{
		PendingEntryWatts = 0.f;   // the entry threw no splash in time (the hold, the budget, the distance): later slams are sized as slams
	}
	if (PendingSlamWatts > 0.f && Now - PendingSlamTime > PendingSlamSeconds)
	{
		PendingSlamWatts = 0.f;   // a slam that waited this long for its splash is stale
	}
	if (!World || !LoadedSplash || EffectReach >= 2)
	{
		return;   // too far or unseen: the pending slam and entry wait out their time (nothing here is drawn)
	}
	const UFluidNinjaBuoyancySettings& HydroSettings = UFluidNinjaBuoyancySettings::Get();
	const float MaxStrength = FMath::Max(FMath::Min(MaxSplashStrength, HydroSettings.SplashStrengthCeiling), SmallestSplash);

	// A slam or an entry: a splash where it hit, on the project's splash scale (the cube root of its power over Splash Reference Watts, up
	// to Max Splash Strength; under 0.2 nothing is drawn), and up to three more where the water line plunges in at least half as fast as it
	// does anywhere, each smaller by the square root of how much slower. An entry (the body going in after being clear of the water) is
	// sized by the whole body, its entry power (weight × sinking speed³ / 2 × height), not by its hardest hull piece, so heavier and faster
	// bodies splash bigger at every Hull Resolution, level of detail and voxel size; it is gated by that power alone, while a slam on a
	// body already in the water (a bounce, a wave) must reach Splash Min Watts, the body's noise floor. The slam is the hardest since
	// the last slam splash, so a one-frame slam while the interval holds is not lost; and a slam does not wait for the bow spray's hold.
	float SplashWatts = 0.f;
	if (PendingEntryWatts > 0.f)
	{
		if (HydroSettings.SplashStrength(PendingEntryWatts) >= SmallestSplash)
		{
			SplashWatts = PendingEntryWatts;
		}
		else
		{
			PendingEntryWatts = 0.f;   // too gentle an entry for a splash: its slams are judged as slams
		}
	}
	if (SplashWatts <= 0.f && SplashMinWatts > 0.f && PendingSlamWatts >= SplashMinWatts)
	{
		SplashWatts = PendingSlamWatts;
	}
	if (SplashWatts > 0.f && Now >= NextSplashTime)
	{
		const float SlamStrength = FMath::Min(HydroSettings.SplashStrength(SplashWatts), MaxStrength);
		float MaxPlunge = 0.f;
		const FFluidNinjaWaterlineContact* Deepest = nullptr;
		for (const FFluidNinjaWaterlineContact& Contact : Contacts)
		{
			if (Contact.Plunge > MaxPlunge)
			{
				MaxPlunge = Contact.Plunge;
				Deepest = &Contact;
			}
		}
		// Where: the hardest slam since the last splash. An entry that slammed nowhere (a point hull going in slowly) splashes where the
		// water line plunges hardest, or under the body when there is no water line yet.
		FVector Where = PendingSlamLocation;
		if (PendingSlamWatts <= 0.f)
		{
			const UPrimitiveComponent* Primitive = GetSimulatedPrimitive();
			Where = Deepest ? Deepest->Position : Primitive ? Primitive->Bounds.Origin : FVector::ZeroVector;
		}
		if (!SpawnSplash(*World, Where, FRotator::ZeroRotator, SlamStrength, false))
		{
			return;   // no budget this frame: the slam and the entry keep waiting for one, until they go stale
		}
		TArray<const FFluidNinjaWaterlineContact*, TInlineAllocator<16>> Plunging;
		if (MaxPlunge > 0.f)
		{
			for (const FFluidNinjaWaterlineContact& Contact : Contacts)
			{
				if (Contact.Plunge > 0.5f * MaxPlunge)
				{
					Plunging.Add(&Contact);
				}
			}
			Algo::Sort(Plunging, [](const FFluidNinjaWaterlineContact* A, const FFluidNinjaWaterlineContact* B) { return A->Plunge > B->Plunge; });
			Plunging.SetNum(FMath::Min(Plunging.Num(), 3));
			for (const FFluidNinjaWaterlineContact* Point : Plunging)
			{
				const float PlungeStrength = FMath::Min(SlamStrength * FMath::Sqrt(Point->Plunge / MaxPlunge), MaxStrength);
				SpawnSplash(*World, Point->Position + Point->Outward * ContactSplashOffsetCm, Point->Outward.Rotation(), PlungeStrength, true);
			}
		}
		// A big slam throws a crown: spray out to every side around where it went in, not only up from it. The water it drives down
		// leaves sideways (a ball or a cube from a height, a crate or a boat dropped flat). It leaves at the water line, so the crown
		// stands on the contacts, facing outward, the hardest-pushing first (a boat's crown is then along its hull, not over its deck);
		// contacts that just threw a plunge splash are not given a second one. Only a body with no water line (a point entry) gets a
		// fixed ring around where it hit.
		if (SlamStrength >= CrownMinStrength && EffectsCrown != 0)
		{
			const int32 Count = FMath::Clamp(FMath::RoundToInt32(SlamStrength / 1.5f), 4, 8);
			const float CrownStrength = FMath::Min(SlamStrength * 0.6f, MaxStrength);
			if (!Contacts.IsEmpty())
			{
				TArray<const FFluidNinjaWaterlineContact*, TInlineAllocator<16>> Crown;
				for (const FFluidNinjaWaterlineContact& Contact : Contacts)
				{
					if (!Plunging.Contains(&Contact))
					{
						Crown.Add(&Contact);
					}
				}
				Algo::Sort(Crown, [](const FFluidNinjaWaterlineContact* A, const FFluidNinjaWaterlineContact* B) { return A->Push > B->Push; });
				for (int32 Index = 0; Index < FMath::Min(Crown.Num(), Count); ++Index)
				{
					const FFluidNinjaWaterlineContact& Point = *Crown[Index];
					SpawnSplash(*World, Point.Position + Point.Outward * ContactSplashOffsetCm, Point.Outward.Rotation(), CrownStrength, true);
				}
			}
			else
			{
				const UPrimitiveComponent* Primitive = GetSimulatedPrimitive();
				const double Reach = Primitive ? FMath::Max(FMath::Max(Primitive->Bounds.BoxExtent.X, Primitive->Bounds.BoxExtent.Y), 10.0) : 30.0;
				const double Turn = FMath::FRand() * UE_DOUBLE_TWO_PI;
				for (int32 Index = 0; Index < Count; ++Index)
				{
					const double Angle = Turn + Index * UE_DOUBLE_TWO_PI / Count;
					const FVector Outward(FMath::Cos(Angle), FMath::Sin(Angle), 0.0);
					SpawnSplash(*World, Where + Outward * Reach, Outward.Rotation(), CrownStrength, true);
				}
			}
		}
		PendingEntryWatts = 0.f;
		PendingSlamWatts = 0.f;
		NextSplashTime = Now + SplashInterval;
		NextBowSprayTime = Now + SplashInterval;   // the bow does not spray again right after its own slam
		return;
	}

	// Bow spray: where the water line drives into the water hardest, if at least Bow Spray Min Push. Its size is on the project's
	// splash scale too: the power of the water that piece of water line drives in, ρ·A·v³ (the piece's area when the physics measured
	// it, else a nominal one), through Splash Reference Watts' cube root and up to Max Splash Strength, as a slam of the same power.
	if (BowSprayMinPushCmS <= 0.f || Now < NextBowSprayTime)
	{
		return;
	}
	const FFluidNinjaWaterlineContact* Hardest = nullptr;
	for (const FFluidNinjaWaterlineContact& Contact : Contacts)
	{
		if (Contact.Push >= BowSprayMinPushCmS && (!Hardest || Contact.Push > Hardest->Push))
		{
			Hardest = &Contact;
		}
	}
	if (Hardest)
	{
		const float Area = Hardest->AreaCm2 > 0.f ? Hardest->AreaCm2 : NominalPieceAreaCm2;
		const float Watts = HydroSettings.WaterDensity * Area * FMath::Cube(Hardest->Push) * BowSprayWattsPerUnit;
		const float Strength = FMath::Min(HydroSettings.SplashStrength(Watts), MaxStrength);
		if (SpawnSplash(*World, Hardest->Position + Hardest->Outward * ContactSplashOffsetCm, Hardest->Outward.Rotation(), Strength, false))
		{
			NextBowSprayTime = Now + SplashInterval;
		}
	}
}

void UFluidNinjaBuoyancyComponent::SetSoundLayering(UAudioComponent& Audio, float Impact) const
{
	Audio.SetFloatParameter(TEXT("Impact"), Impact);
	Audio.SetFloatParameter(TEXT("Speed"), ContactWash);
	Audio.SetFloatParameter(TEXT("Length"), ContactLength);
	Audio.SetFloatParameter(TEXT("Mass"), Readout.MassKg);
}

void UFluidNinjaBuoyancyComponent::UpdateSound(UPrimitiveComponent& Primitive, float DeltaTime)
{
	UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	// This frame's hardest slam on the project's splash scale: the "Impact" a MetaSound layers on (0 when nothing slammed).
	const float Impact = UFluidNinjaBuoyancySettings::Get().SplashStrength(FrameSlamWatts);
	// Slaps: louder with the slam's power.
	if (LoadedSlap && SlapMinWatts > 0.f && FrameSlamWatts >= SlapMinWatts && Now >= NextSlapTime)
	{
		const float Loudness = FMath::Clamp(0.35f + 0.35f * FMath::LogX(10.f, FrameSlamWatts / SlapMinWatts), 0.2f, 2.f) * SoundVolume;
		if (UAudioComponent* Slap = UGameplayStatics::SpawnSoundAtLocation(this, LoadedSlap, FrameSlamLocation, FRotator::ZeroRotator, Loudness, 1.f, 0.f, LoadedAttenuation))
		{
			Slap->SetFloatParameter(TEXT("Pitch Shift"), FMath::FRandRange(-1.5f, 1.5f));
			SetSoundLayering(*Slap, Impact);
		}
		NextSlapTime = Now + 0.08;
	}
	// Creaks: when the hull is thrown about.
	const FVector Spin = Primitive.GetPhysicsAngularVelocityInRadians();
	const float SpinChange = static_cast<float>((Spin - LastAngularVelocity).Size()) / FMath::Max(DeltaTime, 1e-3f);
	LastAngularVelocity = Spin;
	if (LoadedCreak && Readout.bInWater && Now >= NextCreakTime && CreakMinAngularAcceleration > 0.f
		&& (SpinChange >= CreakMinAngularAcceleration || (SlapMinWatts > 0.f && FrameSlamWatts >= 4.f * SlapMinWatts)))
	{
		const float Loudness = FMath::Clamp(0.4f + 0.2f * SpinChange / CreakMinAngularAcceleration, 0.4f, 1.f) * SoundVolume;
		if (UAudioComponent* Creak = UGameplayStatics::SpawnSoundAtLocation(this, LoadedCreak, Primitive.Bounds.Origin, FRotator::ZeroRotator, Loudness, 1.f, 0.f, LoadedAttenuation))
		{
			Creak->SetFloatParameter(TEXT("Pitch Shift"), FMath::FRandRange(-2.f, 1.f));
			SetSoundLayering(*Creak, Impact);
		}
		NextCreakTime = Now + 0.7 + FMath::FRand() * 0.6;
	}
	// The wash: the water running past the water line (a little even at rest, as the waves lap).
	if (WashAudio)
	{
		const float Speed = FMath::Clamp(ContactWash / WashFullSpeedCmS, 0.f, 1.f);
		const float Length = FMath::Clamp(ContactLength / 300.f, 0.f, 1.f);
		const float Target = Readout.bInWater && !Readout.bDormant ? SoundVolume * Length * (0.15f + 0.85f * FMath::Pow(Speed, 1.3f)) : 0.f;
		WashGain = FMath::FInterpTo(WashGain, Target, DeltaTime, 4.f);
		if (WashGain > 0.01f)
		{
			if (!WashAudio->IsPlaying())
			{
				WashAudio->Play();
				for (float& Sent : WashSent)
				{
					Sent = -1.f;   // a fresh play starts with every layering value sent
				}
			}
			WashAudio->SetVolumeMultiplier(WashGain);
			WashAudio->SetFloatParameter(TEXT("Pitch Shift"), FMath::Lerp(-3.f, 2.f, Speed));
			// The layering values go to the audio thread only when they change (each is a command): a slam's Impact, the wash speed
			// within 2 cm/s, the water line within 5 cm, and the mass.
			const float Values[4] = {Impact, ContactWash, ContactLength, Readout.MassKg};
			const float Steps[4] = {0.f, 2.f, 5.f, 0.f};
			static const FName Names[4] = {FName(TEXT("Impact")), FName(TEXT("Speed")), FName(TEXT("Length")), FName(TEXT("Mass"))};
			for (int32 Index = 0; Index < 4; ++Index)
			{
				if (FMath::Abs(Values[Index] - WashSent[Index]) > Steps[Index])
				{
					WashAudio->SetFloatParameter(Names[Index], Values[Index]);
					WashSent[Index] = Values[Index];
				}
			}
		}
		else if (WashAudio->IsPlaying())
		{
			WashAudio->Stop();
		}
	}
}

void UFluidNinjaBuoyancyComponent::EndPlay(const EEndPlayReason::Type Reason)
{
	if (WashAudio)
	{
		WashAudio->Stop();
	}
	if (EngineAudio)
	{
		EngineAudio->Stop();
	}
	for (UNiagaraComponent* Sheet : Sheets)
	{
		if (Sheet)
		{
			Sheet->DestroyComponent();
		}
	}
	Sheets.Reset();
	Washes.Reset();
	if (UWorld* World = GetWorld())
	{
		if (UFluidNinjaBuoyancySubsystem* Hydro = World->GetSubsystem<UFluidNinjaBuoyancySubsystem>())
		{
			Hydro->Unregister(this);
		}
	}
	Super::EndPlay(Reason);
}

void UFluidNinjaBuoyancyComponent::AnalyzeHull()
{
	FString Analysis;
	const TArray<TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe>> Kept = Hulls;
	const TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe> KeptWaveHull = WaveHull;
	const TArray<float> KeptScales = LevelBuoyancyScales;   // they belong to the hulls they were measured on
	SetUp(Analysis);
	if (GetWorld() && !GetWorld()->IsGameWorld())
	{
		Hulls = Kept;   // the editor only reports
		WaveHull = KeptWaveHull;
		LevelBuoyancyScales = KeptScales;
	}
	UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy analysis %s"), *Analysis);
	Report = Analysis;
}

void UFluidNinjaBuoyancyComponent::ReceiveReadout(const FFluidNinjaBuoyancyReadout& NewReadout, float SlamWatts, const FVector& SlamLocation)
{
	Readout = NewReadout;
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	if (SlamWatts > FrameSlamWatts)
	{
		FrameSlamWatts = SlamWatts;
		FrameSlamLocation = SlamLocation;
	}
	// The splash's slam is the hardest since the last slam splash (UpdateSpray clears it when the splash is thrown, or when it is stale).
	if (SlamWatts > 0.f && PendingSlamWatts <= 0.f)
	{
		PendingSlamTime = Now;
	}
	if (SlamWatts > PendingSlamWatts)
	{
		PendingSlamWatts = SlamWatts;
		PendingSlamLocation = SlamLocation;
	}
	if (NewReadout.EntryWatts > 0.f)
	{
		PendingEntryWatts = NewReadout.EntryWatts;
		PendingEntryTime = Now;
	}
	if (SlamWatts >= SlamEventWatts && SlamWatts > 0.f)
	{
		OnSlam.Broadcast(SlamWatts, SlamLocation);
	}
	if (Readout.bInWater != bWasInWater)
	{
		bWasInWater = Readout.bInWater;
		// Leaving the water: the drip, once, told how high the hull is wet above its last water line (the wet line has not dried it yet
		// this frame: the subsystem reports after the component's tick). It fades with the hull's attenuation like the other sounds.
		if (!bWasInWater && LoadedDrip)
		{
			const float WetBand = bHasWetPlane ? FMath::Max(0.f, WetTop - static_cast<float>(WetPlane.X)) : 0.f;
			const UPrimitiveComponent* Primitive = GetSimulatedPrimitive();
			const FVector Where = Primitive ? Primitive->Bounds.Origin : FVector::ZeroVector;
			if (UAudioComponent* Drip = UGameplayStatics::SpawnSoundAtLocation(this, LoadedDrip, Where, FRotator::ZeroRotator, SoundVolume, 1.f, 0.f, LoadedAttenuation))
			{
				Drip->SetFloatParameter(TEXT("Impact"), WetBand);
			}
		}
		OnWaterContactChanged.Broadcast(bWasInWater);
	}
}

FString UFluidNinjaBuoyancyComponent::GetStatus() const
{
	return FString::Printf(TEXT("%s: %s, hull level %d, %.0f L displaced, %.2f m2 wet, force (%.0f, %.0f, %.0f) N, mass %.0f kg, water %s, current (%.0f, %.0f) cm/s%s%s%s"),
		*GetNameSafe(GetOwner()), Readout.bDormant ? TEXT("asleep") : Readout.bInWater ? TEXT("in water") : TEXT("dry"), Readout.HullLevel, Readout.DisplacedLitres, Readout.WettedAreaM2,
		Readout.ForceN.X, Readout.ForceN.Y, Readout.ForceN.Z, Readout.MassKg,
		Readout.WaterAgeSeconds >= 0.f ? *FString::Printf(TEXT("%.0f ms old"), Readout.WaterAgeSeconds * 1000.f) : TEXT("far field (water volume)"),
		Readout.FlowCmS.X, Readout.FlowCmS.Y,
		bPropulsion ? *(FString::Printf(TEXT(", throttle %.2f rudder %.2f, thrust %.0f N"), Throttle, Rudder, Readout.ThrustN)
			+ (PropulsionModel == EFluidNinjaPropulsionModel::Propeller
				? FString::Printf(TEXT(" (gear %d, engine %.0f rpm, prop %.0f rpm, %.1f kW, load %.2f, %.0f%% immersed%s)"), Readout.Gear, Readout.EngineRpm, Readout.PropRpm,
					Readout.PropPowerKw, Readout.EngineLoad, Readout.PropImmersion * 100.f, Readout.bPropVentilated ? TEXT(", VENTILATED") : TEXT(""))
				: FString())
			+ FString::Printf(TEXT(", %.1f m/s through the water, trim %.1f deg"), Readout.SpeedThroughWaterCmS / 100.f, Readout.TrimDeg)
			+ (IsPlaningHull() ? FString::Printf(TEXT(", wave resistance %.0f N"), Readout.WaveResistanceN) : FString())) : TEXT(""),
		KeelAreaCm2 > 0.f ? *FString::Printf(TEXT(", drift %.1f deg, keel %.0f N"), Readout.DriftAngleDeg, Readout.KeelForceN) : TEXT(""),
		// FluidNinja Waves' running estimate of the body's motion (the radiation damping is taken at these frequencies).
		!WaveMotionHz.IsZero() ? *FString::Printf(TEXT(", moving at %.2f Hz heave, %.2f roll, %.2f pitch"), WaveMotionHz.X, WaveMotionHz.Y, WaveMotionHz.Z) : TEXT(""));
}
