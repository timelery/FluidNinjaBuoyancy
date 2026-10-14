#include "FluidNinjaBuoyancyComponent.h"

#include "Components/AudioComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#include "FluidNinjaBuoyancyCore.h"
#include "FluidNinjaBuoyancyLog.h"
#include "FluidNinjaBuoyancySettings.h"
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
	// The finest hull of the chosen model, then (triangles) two coarser levels for distance.
	TArray<TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe>> Built;
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
	float Mass = MassMode == EFluidNinjaBuoyancyMass::RestDraft ? Params.Rho * FluidNinjaBuoyancy::DisplacedVolume(Finest, RestDraftCm, Params, Attitude) : Primitive->GetMass();
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
	const float Height = RestedTop - RestedBottom;
	FString Levels;
	for (int32 Level = 1; Level < Built.Num(); ++Level)
	{
		Levels += FString::Printf(TEXT("%s%d"), Level == 1 ? TEXT("; farther levels ") : TEXT(", "), Built[Level]->Elements());
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
	return true;
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
		if (!bWake && !Placed->GetGenerateOverlapEvents())
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
	LoadedAttenuation = LoadOr(Attenuation, Settings.HullAttenuation);
	UPrimitiveComponent* Primitive = GetSimulatedPrimitive();
	if (USoundBase* Wash = LoadOr(WashSound, Settings.WashSound); Wash && Primitive && GetOwner())
	{
		WashAudio = NewObject<UAudioComponent>(GetOwner(), TEXT("LFHullWash"));
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
	ReleaseHold();
}

void UFluidNinjaBuoyancyComponent::ReleaseHold()
{
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
	if (Primitive->IsSimulatingPhysics())
	{
		Primitive->SetPhysicsLinearVelocity(FVector::ZeroVector);
		Primitive->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector);
	}
	Readout = FFluidNinjaBuoyancyReadout();
	Contacts.Reset();
	PendingSlamWatts = 0.f;
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
	UpdateWetLine(*Primitive, DeltaTime);
	UpdateSpray(DeltaTime);
	UpdateSound(*Primitive, DeltaTime);
	PendingSlamWatts = 0.f;
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
	if (!World || !LoadedSplash || Now < NextSplashTime)
	{
		return;
	}
	FVector Where = FVector::ZeroVector;
	FRotator Facing = FRotator::ZeroRotator;
	float Strength = 0.f;
	if (SplashMinWatts > 0.f && PendingSlamWatts >= SplashMinWatts)
	{
		Where = PendingSlamLocation;
		Strength = FMath::Sqrt(PendingSlamWatts / SplashMinWatts);
	}
	else if (BowSprayMinPushCmS > 0.f)
	{
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
			Where = Hardest->Position + Hardest->Outward * 10.0;
			Facing = Hardest->Outward.Rotation();
			Strength = Hardest->Push / BowSprayMinPushCmS;
		}
	}
	if (Strength > 0.f)
	{
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(World, LoadedSplash, Where, Facing, FVector(SplashScale * FMath::Clamp(Strength, 0.6f, 2.5f)),
			true, true, ENCPoolMethod::AutoRelease);
		NextSplashTime = Now + SplashInterval;
	}
}

void UFluidNinjaBuoyancyComponent::UpdateSound(UPrimitiveComponent& Primitive, float DeltaTime)
{
	UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	// Slaps: louder with the slam's power.
	if (LoadedSlap && SlapMinWatts > 0.f && PendingSlamWatts >= SlapMinWatts && Now >= NextSlapTime)
	{
		const float Loudness = FMath::Clamp(0.35f + 0.35f * FMath::LogX(10.f, PendingSlamWatts / SlapMinWatts), 0.2f, 1.5f) * SoundVolume;
		if (UAudioComponent* Slap = UGameplayStatics::SpawnSoundAtLocation(this, LoadedSlap, PendingSlamLocation, FRotator::ZeroRotator, Loudness, 1.f, 0.f, LoadedAttenuation))
		{
			Slap->SetFloatParameter(TEXT("Pitch Shift"), FMath::FRandRange(-1.5f, 1.5f));
		}
		NextSlapTime = Now + 0.08;
	}
	// Creaks: when the hull is thrown about.
	const FVector Spin = Primitive.GetPhysicsAngularVelocityInRadians();
	const float SpinChange = static_cast<float>((Spin - LastAngularVelocity).Size()) / FMath::Max(DeltaTime, 1e-3f);
	LastAngularVelocity = Spin;
	if (LoadedCreak && Readout.bInWater && Now >= NextCreakTime && CreakMinAngularAcceleration > 0.f
		&& (SpinChange >= CreakMinAngularAcceleration || (SlapMinWatts > 0.f && PendingSlamWatts >= 4.f * SlapMinWatts)))
	{
		const float Loudness = FMath::Clamp(0.4f + 0.2f * SpinChange / CreakMinAngularAcceleration, 0.4f, 1.f) * SoundVolume;
		if (UAudioComponent* Creak = UGameplayStatics::SpawnSoundAtLocation(this, LoadedCreak, Primitive.Bounds.Origin, FRotator::ZeroRotator, Loudness, 1.f, 0.f, LoadedAttenuation))
		{
			Creak->SetFloatParameter(TEXT("Pitch Shift"), FMath::FRandRange(-2.f, 1.f));
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
			}
			WashAudio->SetVolumeMultiplier(WashGain);
			WashAudio->SetFloatParameter(TEXT("Pitch Shift"), FMath::Lerp(-3.f, 2.f, Speed));
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
	SetUp(Analysis);
	if (GetWorld() && !GetWorld()->IsGameWorld())
	{
		Hulls = Kept;   // the editor only reports
	}
	UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy analysis %s"), *Analysis);
	Report = Analysis;
}

void UFluidNinjaBuoyancyComponent::ReceiveReadout(const FFluidNinjaBuoyancyReadout& NewReadout, float SlamWatts, const FVector& SlamLocation)
{
	Readout = NewReadout;
	if (SlamWatts > PendingSlamWatts)
	{
		PendingSlamWatts = SlamWatts;
		PendingSlamLocation = SlamLocation;
	}
	if (SlamWatts >= SlamEventWatts && SlamWatts > 0.f)
	{
		OnSlam.Broadcast(SlamWatts, SlamLocation);
	}
	if (Readout.bInWater != bWasInWater)
	{
		bWasInWater = Readout.bInWater;
		OnWaterContactChanged.Broadcast(bWasInWater);
	}
}

FString UFluidNinjaBuoyancyComponent::GetStatus() const
{
	return FString::Printf(TEXT("%s: %s, hull level %d, %.0f L displaced, %.2f m2 wet, force (%.0f, %.0f, %.0f) N, mass %.0f kg, water %s, current (%.0f, %.0f) cm/s%s"),
		*GetNameSafe(GetOwner()), Readout.bDormant ? TEXT("asleep") : Readout.bInWater ? TEXT("in water") : TEXT("dry"), Readout.HullLevel, Readout.DisplacedLitres, Readout.WettedAreaM2,
		Readout.ForceN.X, Readout.ForceN.Y, Readout.ForceN.Z, Readout.MassKg,
		Readout.WaterAgeSeconds >= 0.f ? *FString::Printf(TEXT("%.0f ms old"), Readout.WaterAgeSeconds * 1000.f) : TEXT("far field (water volume)"),
		Readout.FlowCmS.X, Readout.FlowCmS.Y,
		bPropulsion ? *FString::Printf(TEXT(", throttle %.2f rudder %.2f"), Throttle, Rudder) : TEXT(""));
}
