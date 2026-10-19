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
	Out.bPropulsion = bPropulsion;
	Out.ThrustLocal = ThrustLocation;
	Out.ThrustDirectionLocal = ThrustDirection.IsNearlyZero() ? FVector::ForwardVector : ThrustDirection;
	Out.MaxThrust = MaxThrustNewtons * 100.f;   // N to kg·cm/s²
	Out.Throttle = FMath::Clamp(Throttle, -1.f, 1.f);
	Out.PropWashSpeed = PropWashSpeed;
	Out.RudderLocal = RudderLocation;
	Out.RudderArea = RudderAreaCm2;
	Out.RudderAngle = FMath::DegreesToRadians(MaxRudderAngle) * FMath::Clamp(Rudder, -1.f, 1.f);
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
		WashAudio = NewObject<UAudioComponent>(GetOwner(), TEXT("FluidNinjaHullWash"));
		WashAudio->bAutoActivate = false;
		WashAudio->bAutoDestroy = false;
		WashAudio->SetSound(Wash);
		WashAudio->AttenuationSettings = LoadedAttenuation;
		WashAudio->SetupAttachment(Primitive);
		WashAudio->RegisterComponent();
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
		return;
	}
	UpdateEffectReach(*Primitive);
	UpdateWetLine(*Primitive, DeltaTime);
	UpdateSpray(DeltaTime);
	UpdateSound(*Primitive, DeltaTime);
	FrameSlamWatts = 0.f;   // the sounds have had this frame's slam; the splash keeps its running max (PendingSlamWatts) until it fires
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
	return FString::Printf(TEXT("%s: %s, hull level %d, %.0f L displaced, %.2f m2 wet, force (%.0f, %.0f, %.0f) N, mass %.0f kg, water %s, current (%.0f, %.0f) cm/s%s%s"),
		*GetNameSafe(GetOwner()), Readout.bDormant ? TEXT("asleep") : Readout.bInWater ? TEXT("in water") : TEXT("dry"), Readout.HullLevel, Readout.DisplacedLitres, Readout.WettedAreaM2,
		Readout.ForceN.X, Readout.ForceN.Y, Readout.ForceN.Z, Readout.MassKg,
		Readout.WaterAgeSeconds >= 0.f ? *FString::Printf(TEXT("%.0f ms old"), Readout.WaterAgeSeconds * 1000.f) : TEXT("far field (water volume)"),
		Readout.FlowCmS.X, Readout.FlowCmS.Y,
		bPropulsion ? *FString::Printf(TEXT(", throttle %.2f rudder %.2f"), Throttle, Rudder) : TEXT(""),
		// FluidNinja Waves' running estimate of the body's motion (the radiation damping is taken at these frequencies).
		!WaveMotionHz.IsZero() ? *FString::Printf(TEXT(", moving at %.2f Hz heave, %.2f roll, %.2f pitch"), WaveMotionHz.X, WaveMotionHz.Y, WaveMotionHz.Z) : TEXT(""));
}
