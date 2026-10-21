#include "FluidNinjaBuoyancySwimmers.h"

#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "FluidNinjaBuoyancyLog.h"
#include "FluidNinjaBuoyancySettings.h"
#include "FluidNinjaBuoyancySwimmerBody.h"
#include "GameFramework/Pawn.h"
#include "NiagaraDataChannelAccessor.h"
#include "NiagaraDataChannelAsset.h"
#include "NiagaraDataChannelFunctionLibrary.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"

namespace FluidNinjaBuoyancyCVars
{
	// fnb.Wake.MinWriteCmS (FluidNinjaBuoyancyCurrent.cpp): FluidNinja ignores points slower than this; a stroke point under it is not written.
	extern float WakeMinWriteCmS;
}

namespace FluidNinjaBuoyancySwimmerParts
{
	/** A part moving faster than this was put somewhere (a teleport, a looping clip jumping back to its start): no contact from it. No
	 * swimmer's or diver's limb goes faster (a 10 m dive goes in at 14 m/s). */
	constexpr double MaxPartSpeedCmS = 2000.0;
	/** ...or further than this in a frame, whatever the frame rate. */
	constexpr double TeleportCm = 200.0;
	/** Splashes weaker than this are not drawn. */
	constexpr float SmallestSplash = 0.2f;
	/** A kick's push acts over about this long (s): the impulse a frame is m·v·Δt / this. */
	constexpr double KickSeconds = 0.1;
	constexpr double KickMinSpeedCmS = 40.0;
	/** Out of the water this long, the next entry starts a new burst (s). */
	constexpr double BurstGapSeconds = 0.5;
	/** The window entries a minute are counted over (s). */
	constexpr double RateWindowSeconds = 20.0;
	/** The spheres, in the order FFluidNinjaSwimmerSphere::Part counts them (FluidNinja Waves keys a puppet by mesh and part). */
	constexpr int32 NumSpheres = 3;
	/** A registered swimmer's hips are in the water once they are no higher than this over it (about their radius), for the strokes. */
	constexpr double HipsRadiusCm = 15.0;
	/** The drift current a swim mode takes: the reading low-passed over this long and dead-banded under this speed
	 * (UFluidNinjaBuoyancyWaterLibrary::FilterDriftCurrent). */
	constexpr float DriftSmoothSeconds = 0.5f;
	constexpr float DriftDeadBandCmS = 10.f;
	/** The human body's mean density over water's: the mass under the surface from the spheres' volume below it. */
	constexpr double BodyDensity = 0.985;
	/** A swimmer on the water query with no sample yet: no water (far below anything), rather than a misleading height. */
	constexpr float NoWaterZ = -1.0e9f;

	/** The volume of a sphere of radius R below water WaterZ, centre at Z (cm³). */
	double SphereVolumeBelow(double Z, double R, double WaterZ)
	{
		const double H = FMath::Clamp(WaterZ - (Z - R), 0.0, 2.0 * R);
		return UE_DOUBLE_PI * H * H * (3.0 * R - H) / 3.0;
	}

	/** The swimmers of each world (FFluidNinjaBuoyancySwimmers::Find). */
	TMap<const UWorld*, FFluidNinjaBuoyancySwimmers*>& Registry()
	{
		static TMap<const UWorld*, FFluidNinjaBuoyancySwimmers*> Map;
		return Map;
	}
}

FFluidNinjaBuoyancySwimmers::FEntryEvent& FFluidNinjaBuoyancySwimmers::OnAnyEntry()
{
	static FEntryEvent Event;
	return Event;
}

FFluidNinjaBuoyancySwimmers::FFluidNinjaBuoyancySwimmers(UFluidNinjaBuoyancySubsystem& InOwner)
	: Owner(&InOwner)
	, World(InOwner.GetWorld())
{
	Player.bPlayer = true;
	if (const UWorld* Key = World.Get())
	{
		FluidNinjaBuoyancySwimmerParts::Registry().Add(Key, this);
	}
}

FFluidNinjaBuoyancySwimmers::~FFluidNinjaBuoyancySwimmers()
{
	// Only this one's entry: a later world at the same address may have registered its own by now.
	TMap<const UWorld*, FFluidNinjaBuoyancySwimmers*>& Map = FluidNinjaBuoyancySwimmerParts::Registry();
	for (auto It = Map.CreateIterator(); It; ++It)
	{
		if (It.Value() == this)
		{
			It.RemoveCurrent();
		}
	}
}

FFluidNinjaBuoyancySwimmers* FFluidNinjaBuoyancySwimmers::Find(const UWorld* InWorld)
{
	FFluidNinjaBuoyancySwimmers* const* Found = InWorld ? FluidNinjaBuoyancySwimmerParts::Registry().Find(InWorld) : nullptr;
	return Found ? *Found : nullptr;
}

void FFluidNinjaBuoyancySwimmers::Register(USkeletalMeshComponent* Mesh, const FFluidNinjaSwimmerSource& Source)
{
	// A mesh registered with a provider keeps it: this only changes the source (UFluidNinjaBuoyancySubsystem::RegisterSwimmer's contract).
	for (FSwimmer& Swimmer : Registered)
	{
		if (Mesh && Swimmer.Mesh.Get() == Mesh)
		{
			Swimmer.Source = Source;
			return;
		}
	}
	Register(Mesh, Source, nullptr, nullptr, false);
}

void FFluidNinjaBuoyancySwimmers::Register(USkeletalMeshComponent* Mesh, const FFluidNinjaSwimmerSource& Source, UObject* Provider, const UFluidNinjaSwimmerProfile* Profile,
	bool bWaterFromQuery)
{
	if (!Mesh)
	{
		return;
	}
	if (Provider && !Provider->GetClass()->ImplementsInterface(UFluidNinjaSwimmerProvider::StaticClass()))
	{
		UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("FluidNinja Buoyancy: %s registered as a swimmer with a provider (%s) that does not implement FluidNinjaSwimmerProvider; the default pose is used."),
			*Mesh->GetPathName(), *Provider->GetPathName());
		Provider = nullptr;
	}
	for (FSwimmer& Swimmer : Registered)
	{
		if (Swimmer.Mesh.Get() == Mesh)
		{
			Swimmer.Source = Source;
			Swimmer.Provider = Provider;
			Swimmer.Profile = Profile;
			// Off the water query now: no sample to hand out (GetWater false), as for a swimmer registered without it.
			if (Swimmer.bWaterFromQuery && !bWaterFromQuery)
			{
				Swimmer.bHasWater = false;
				Swimmer.Water = FFluidNinjaWaterSample();
				Swimmer.Drift = FVector::ZeroVector;
			}
			Swimmer.bWaterFromQuery = bWaterFromQuery;
			return;
		}
	}
	FSwimmer& Swimmer = Registered.AddDefaulted_GetRef();
	Swimmer.Mesh = Mesh;
	Swimmer.Source = Source;
	Swimmer.Provider = Provider;
	Swimmer.Profile = Profile;
	Swimmer.bWaterFromQuery = bWaterFromQuery;
}

void FFluidNinjaBuoyancySwimmers::Unregister(USkeletalMeshComponent* Mesh)
{
	Registered.RemoveAll([Mesh](const FSwimmer& Swimmer) { return !Swimmer.Mesh.IsValid() || Swimmer.Mesh.Get() == Mesh; });
}

bool FFluidNinjaBuoyancySwimmers::IsRegistered(const USkeletalMeshComponent* Mesh) const
{
	return Mesh && Registered.ContainsByPredicate([Mesh](const FSwimmer& Swimmer) { return Swimmer.Mesh.Get() == Mesh; });
}

const FFluidNinjaBuoyancySwimmers::FSwimmer* FFluidNinjaBuoyancySwimmers::FindSwimmer(const USkeletalMeshComponent* Mesh) const
{
	if (!Mesh)
	{
		return nullptr;
	}
	if (Player.Mesh.Get() == Mesh)
	{
		return &Player;
	}
	return Registered.FindByPredicate([Mesh](const FSwimmer& Swimmer) { return Swimmer.Mesh.Get() == Mesh; });
}

void FFluidNinjaBuoyancySwimmers::GetRegistered(TArray<TPair<USkeletalMeshComponent*, FFluidNinjaSwimmerSource>>& Out) const
{
	Out.Reset();
	for (const FSwimmer& Swimmer : Registered)
	{
		USkeletalMeshComponent* Mesh = Swimmer.Mesh.Get();
		if (Mesh && Mesh->IsVisible())
		{
			FFluidNinjaSwimmerSource Source = Swimmer.Source;
			// On the water query, the still water the wakes judge the hips against is the live one (none sampled yet: no water); off it, the
			// pose's Water Z when its provider gave one, else the registration's (the same water its parts and spheres use).
			Source.WaterZ = Swimmer.bWaterFromQuery
				? (Swimmer.bHasWater && Swimmer.Water.bWater ? Swimmer.Water.StillZ : FluidNinjaBuoyancySwimmerParts::NoWaterZ)
				: FixedWaterZ(Swimmer);
			Out.Emplace(Mesh, Source);
		}
	}
}

bool FFluidNinjaBuoyancySwimmers::GetStats(const USkeletalMeshComponent* Mesh, FFluidNinjaSwimmerStats& Out) const
{
	if (const FSwimmer* Swimmer = FindSwimmer(Mesh))
	{
		Out = Swimmer->Stats;
		return true;
	}
	return false;
}

bool FFluidNinjaBuoyancySwimmers::GetWater(const USkeletalMeshComponent* Mesh, FFluidNinjaWaterSample& Out, FVector& OutDriftCurrent) const
{
	const FSwimmer* Swimmer = FindSwimmer(Mesh);
	if (!Swimmer || !Swimmer->bHasWater)
	{
		Out = FFluidNinjaWaterSample();
		OutDriftCurrent = FVector::ZeroVector;
		return false;
	}
	Out = Swimmer->Water;
	OutDriftCurrent = Swimmer->Drift;
	return true;
}

bool FFluidNinjaBuoyancySwimmers::GetPose(const USkeletalMeshComponent* Mesh, FFluidNinjaSwimmerPose& Out) const
{
	const FSwimmer* Swimmer = FindSwimmer(Mesh);
	if (!Swimmer || !Swimmer->Pose.bValid)
	{
		Out = FFluidNinjaSwimmerPose();
		return false;
	}
	Out = Swimmer->Pose;
	return true;
}

FFluidNinjaSwimmerWakeTuning FFluidNinjaBuoyancySwimmers::ProjectWakeTuning()
{
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	FFluidNinjaSwimmerWakeTuning Tuning;
	Tuning.bOverride = false;
	Tuning.Strength = Settings.SwimmerWakeStrength;
	Tuning.Gain = Settings.SwimmerWakeGain;
	Tuning.SizeCm = Settings.SwimmerWakeSizeCm;
	Tuning.MinSpeedCmS = Settings.SwimmerWakeMinSpeedCmS;
	Tuning.RingInnerCm = Settings.SwimmerCurrentRingInnerCm;
	Tuning.RingOuterCm = Settings.SwimmerCurrentRingOuterCm;
	return Tuning;
}

FFluidNinjaSwimmerWakeTuning FFluidNinjaBuoyancySwimmers::GetWakeTuning(const USkeletalMeshComponent* Mesh) const
{
	const FSwimmer* Swimmer = FindSwimmer(Mesh);
	if (!Swimmer || !Swimmer->Provider.IsValid())
	{
		return ProjectWakeTuning();
	}
	FFluidNinjaSwimmerWakeTuning Tuning = IFluidNinjaSwimmerProvider::Execute_GetSwimmerWakeTuning(Swimmer->Provider.Get());
	if (!Tuning.bOverride)
	{
		return ProjectWakeTuning();
	}
	// A Blueprint may write any value (the struct's ClampMin holds only in the Details panel): held to the ranges the project's settings
	// keep, and a value that is no number takes the project's.
	const FFluidNinjaSwimmerWakeTuning Project = ProjectWakeTuning();
	auto Held = [](float Value, float Fallback, float Min, float Max) { return FMath::IsFinite(Value) ? FMath::Clamp(Value, Min, Max) : Fallback; };
	Tuning.Strength = Held(Tuning.Strength, Project.Strength, 0.f, 1.f);
	Tuning.Gain = Held(Tuning.Gain, Project.Gain, 0.f, 1.f);
	Tuning.SizeCm = Held(Tuning.SizeCm, Project.SizeCm, 1.f, TNumericLimits<float>::Max());
	Tuning.MinSpeedCmS = Held(Tuning.MinSpeedCmS, Project.MinSpeedCmS, 0.f, TNumericLimits<float>::Max());
	Tuning.RingInnerCm = Held(Tuning.RingInnerCm, Project.RingInnerCm, 0.f, TNumericLimits<float>::Max());
	Tuning.RingOuterCm = Held(Tuning.RingOuterCm, Project.RingOuterCm, Tuning.RingInnerCm + 10.f, TNumericLimits<float>::Max());
	return Tuning;
}

float FFluidNinjaBuoyancySwimmers::FixedWaterZ(const FSwimmer& Swimmer)
{
	return Swimmer.Pose.bValid && Swimmer.Pose.bHasWaterZ && FMath::IsFinite(Swimmer.Pose.WaterZ) ? Swimmer.Pose.WaterZ : Swimmer.Source.WaterZ;
}

const UFluidNinjaSwimmerProfile* FFluidNinjaBuoyancySwimmers::ProfileOf(const FSwimmer& Swimmer, const USkeletalMeshComponent* Mesh) const
{
	const UFluidNinjaSwimmerProfile* Profile = Swimmer.Profile.Get();
	if (!Profile && Swimmer.Provider.IsValid())
	{
		Profile = IFluidNinjaSwimmerProvider::Execute_GetSwimmerProfile(Swimmer.Provider.Get());
	}
	return UFluidNinjaSwimmerProfile::Resolve(Profile, Mesh);
}

bool FFluidNinjaBuoyancySwimmers::PoseFor(FSwimmer& Swimmer, USkeletalMeshComponent& Mesh, FFluidNinjaSwimmerPose& Out) const
{
	if (UObject* Provider = Swimmer.Provider.Get())
	{
		Out = FFluidNinjaSwimmerPose();
		if (!IFluidNinjaSwimmerProvider::Execute_GetSwimmerPose(Provider, Out) || !Out.bValid)
		{
			return false;
		}
		if (!Out.Mesh)
		{
			Out.Mesh = &Mesh;
		}
		return true;
	}
	// The default: the mesh's own bones through its profile; no pawn, so its velocity is its mesh's motion frame to frame (the wakes' rule
	// for a registered swimmer).
	return UFluidNinjaSwimmerProviderLibrary::BuildDefaultPose(&Mesh, ProfileOf(Swimmer, &Mesh), nullptr, Out);
}

void FFluidNinjaBuoyancySwimmers::Splash(const FVector& Where, float Strength)
{
	if (Strength < FluidNinjaBuoyancySwimmerParts::SmallestSplash)
	{
		return;
	}
	if (!bSplashLoaded)
	{
		bSplashLoaded = true;
		SplashSystem.Reset(Cast<UNiagaraSystem>(UFluidNinjaBuoyancySettings::Get().SplashSystem.TryLoad()));
	}
	UFluidNinjaBuoyancySubsystem* System = Owner.Get();
	if (SplashSystem && System && System->GetWorld())
	{
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(System->GetWorld(), SplashSystem.Get(), Where, FRotator::ZeroRotator, FVector(Strength), true, true,
			ENCPoolMethod::AutoRelease);
	}
}

void FFluidNinjaBuoyancySwimmers::Tick(float DeltaSeconds, USkeletalMeshComponent* PlayerMesh, float PlayerWaterZ)
{
	FFluidNinjaSwimmerPose Pose;
	const FFluidNinjaSwimmerPose* Given = nullptr;
	if (PlayerMesh && UFluidNinjaSwimmerProviderLibrary::BuildDefaultPose(PlayerMesh, nullptr, Cast<APawn>(PlayerMesh->GetOwner()), Pose))
	{
		Given = &Pose;
	}
	Tick(DeltaSeconds, Given, PlayerWaterZ);
}

void FFluidNinjaBuoyancySwimmers::WaterFor(FSwimmer& Swimmer, const FFluidNinjaSwimmerPose& Pose, float DeltaSeconds, float& OutSurfaceZ, float& OutStillZ)
{
	using namespace FluidNinjaBuoyancySwimmerParts;
	UFluidNinjaBuoyancySubsystem* System = Owner.Get();
	if (Swimmer.bWaterFromQuery && System)
	{
		// At the hips (the body's reference point): the still water, the waves and the current around it, one reading for everything the
		// swimmer does this frame. A frame without water keeps the last sample for the parts (a reading briefly missing at a bank) but says so.
		FFluidNinjaWaterSample Sample;
		const FVector At = Pose.bHasSpheres && Pose.HipsRadiusCm > 0.f ? Pose.Hips : Pose.Location;
		if (System->SampleWater(At, Sample))
		{
			Swimmer.Water = Sample;
			Swimmer.bHasWater = true;
		}
		else if (Swimmer.bHasWater)
		{
			Swimmer.Water.bHasCurrent = false;
			Swimmer.Water.bHasWaves = false;
			Swimmer.Water.AgeSeconds += DeltaSeconds;
		}
		Swimmer.Drift = UFluidNinjaBuoyancyWaterLibrary::FilterDriftCurrent(Swimmer.Drift, Swimmer.Water.Current, Swimmer.bHasWater && Swimmer.Water.bHasCurrent, DeltaSeconds,
			DriftSmoothSeconds, DriftDeadBandCmS);
		if (Swimmer.bHasWater && Swimmer.Water.bWater)
		{
			OutSurfaceZ = Swimmer.Water.SurfaceZ;
			OutStillZ = Swimmer.Water.StillZ;
			return;
		}
		// On the query with nothing sampled yet: no water, rather than Source.WaterZ's default standing in as a surface.
		OutSurfaceZ = OutStillZ = NoWaterZ;
		return;
	}
	// Off the query: the still water its provider gives (the pose's Water Z), else its registration's.
	OutSurfaceZ = OutStillZ = Pose.bHasWaterZ && FMath::IsFinite(Pose.WaterZ) ? Pose.WaterZ : Swimmer.Source.WaterZ;
}

void FFluidNinjaBuoyancySwimmers::Tick(float DeltaSeconds, const FFluidNinjaSwimmerPose* PlayerPose, float PlayerWaterZ)
{
	using namespace FluidNinjaBuoyancySwimmerParts;
	Spheres.Reset();
	StrokePoints.Reset();
	LastStrokePoints = 0;
	UFluidNinjaBuoyancySubsystem* System = Owner.Get();
	const UWorld* ThisWorld = System ? System->GetWorld() : nullptr;
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	if (!ThisWorld || DeltaSeconds <= 0.f)
	{
		return;
	}
	const double Now = ThisWorld->GetTimeSeconds();
	Registered.RemoveAll([](const FSwimmer& Swimmer) { return !Swimmer.Mesh.IsValid(); });
	for (FSwimmer& Swimmer : Registered)
	{
		Swimmer.Pose.bValid = false;
	}

	// The player's pawn: followed while it is in the water (its parts start over each time it comes back). Its mesh registered as a swimmer
	// (a FluidNinja Swimmer component on the pawn): that registered swimmer makes its contacts, with its own provider, not this path too.
	USkeletalMeshComponent* Shown = Settings.bSwimmerContacts && Settings.bPlayerContacts && PlayerPose && PlayerPose->bValid ? PlayerPose->Mesh.Get() : nullptr;
	if (Shown && IsRegistered(Shown))
	{
		Shown = nullptr;
	}
	Player.Pose.bValid = false;
	Player.Stats.DisplacedLitres = 0.f;
	Player.Stats.bUsesPawnBody = false;
	Player.Stats.ContactPartCount = 0;
	if (Player.Mesh.Get() != Shown || !Shown)
	{
		if (Shown && Player.Mesh.Get() != Shown)
		{
			Player.Stats = FFluidNinjaSwimmerStats();
			Player.EntryTimes.Reset();
			Player.Recent.Reset();
			Player.Parts.Reset();
			Player.PartBones.Reset();
			Player.bChestKnown = false;
			Player.bHasWater = false;
			Player.Drift = FVector::ZeroVector;
		}
		for (FPart& Part : Player.Parts)
		{
			Part.bHasPrevious = false;
		}
		Player.bChestKnown = false;
		Player.Mesh = Shown;
	}
	if (Shown)
	{
		// The player's pawn: hands, elbows, feet and head only (its chest and hips ride its capsule).
		Player.Pose = *PlayerPose;
		UFluidNinjaSwimmerProviderLibrary::FilterParts(Player.Pose, false);
		Player.Stats.ContactPartCount = Player.Pose.Parts.Num();
		// Its live water, for GetWater (the caller's PlayerWaterZ stays the surface its parts meet).
		FFluidNinjaWaterSample Sample;
		if (System->SampleWater(Player.Pose.bHasSpheres && Player.Pose.HipsRadiusCm > 0.f ? Player.Pose.Hips : Player.Pose.Location, Sample))
		{
			Player.Water = Sample;
			Player.bHasWater = true;
		}
		Player.Drift = UFluidNinjaBuoyancyWaterLibrary::FilterDriftCurrent(Player.Drift, Player.Water.Current, Player.bHasWater && Player.Water.bHasCurrent, DeltaSeconds,
			DriftSmoothSeconds, DriftDeadBandCmS);
		TickSwimmer(Player, Player.Pose, PlayerWaterZ, DeltaSeconds, Now);
		TickDive(Player, Player.Pose, PlayerWaterZ, DeltaSeconds);
		TickStroke(Player, Player.Pose, Player.bHasWater && Player.Water.bWater ? Player.Water.StillZ : PlayerWaterZ, DeltaSeconds);
		const APawn* Pawn = Cast<APawn>(Shown->GetOwner());
		const UCapsuleComponent* Capsule = Pawn ? Cast<UCapsuleComponent>(Pawn->GetRootComponent()) : nullptr;
		if (Capsule && Capsule->IsCollisionEnabled())
		{
			Player.Stats.bUsesPawnBody = true;
			Player.Stats.DisplacedLitres = FFluidNinjaSwimmerBodyShape::Make(Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleHalfHeight(),
				Capsule->GetComponentLocation().Z, PlayerWaterZ, Settings.SwimmerVolumeLitres).DisplacedLitres;
		}
	}

	for (FSwimmer& Swimmer : Registered)
	{
		USkeletalMeshComponent* Mesh = Swimmer.Mesh.Get();
		if (!Mesh)
		{
			continue;
		}
		// Hidden (a diver climbing out), or its provider says it is no swimmer now: out of the water as far as the waves know; its parts start
		// over when it is back.
		if (!Mesh->IsVisible() || !PoseFor(Swimmer, *Mesh, Swimmer.Pose))
		{
			for (FPart& Part : Swimmer.Parts)
			{
				Part.bHasPrevious = false;
			}
			Swimmer.bHasSpheres = false;
			Swimmer.bChestKnown = false;
			Swimmer.Pose.bValid = false;
			Swimmer.Stats.DisplacedLitres = 0.f;
			Swimmer.Stats.bUsesPawnBody = false;
			Swimmer.Stats.ContactPartCount = 0;
			continue;
		}
		UFluidNinjaSwimmerProviderLibrary::FilterParts(Swimmer.Pose, Swimmer.Source.bWholeBody);
		Swimmer.Stats.ContactPartCount = Swimmer.Source.bPartContacts && Settings.bSwimmerContacts ? Swimmer.Pose.Parts.Num() : 0;
		float WaterZ = 0.f;
		float StillZ = 0.f;
		WaterFor(Swimmer, Swimmer.Pose, DeltaSeconds, WaterZ, StillZ);
		if (Swimmer.Source.bPartContacts && Settings.bSwimmerContacts)
		{
			TickSwimmer(Swimmer, Swimmer.Pose, WaterZ, DeltaSeconds, Now);
		}
		TickDive(Swimmer, Swimmer.Pose, WaterZ, DeltaSeconds);
		TickStroke(Swimmer, Swimmer.Pose, StillZ, DeltaSeconds);

		// Its head, chest and hips pushing the water aside, with their velocity (a frame's move further than a teleport: still). They stand
		// in the still water: against the waves the head would chase its own hollow.
		double Below = 0.0;
		Swimmer.SphereWas.SetNum(NumSpheres);
		const FVector Centres[NumSpheres] = {Swimmer.Pose.Head, Swimmer.Pose.Chest, Swimmer.Pose.Hips};
		const float Radii[NumSpheres] = {Swimmer.Pose.HeadRadiusCm, Swimmer.Pose.ChestRadiusCm, Swimmer.Pose.HipsRadiusCm};
		for (int32 Index = 0; Index < NumSpheres; ++Index)
		{
			if (!Swimmer.Pose.bHasSpheres || Radii[Index] <= 0.f)
			{
				continue;
			}
			const FVector Centre = Centres[Index];
			FVector Velocity = FVector::ZeroVector;
			const double Moved = Swimmer.bHasSpheres ? FVector::Dist(Centre, Swimmer.SphereWas[Index]) : TNumericLimits<double>::Max();
			if (Moved < TeleportCm && Moved < MaxPartSpeedCmS * DeltaSeconds + 1.0)
			{
				Velocity = (Centre - Swimmer.SphereWas[Index]) / DeltaSeconds;
			}
			Swimmer.SphereWas[Index] = Centre;
			Below += SphereVolumeBelow(Centre.Z, Radii[Index], StillZ);
			if (Swimmer.Source.bDisplacement)
			{
				FFluidNinjaSwimmerSphere& Sphere = Spheres.AddDefaulted_GetRef();
				Sphere.Mesh = Mesh;
				Sphere.Part = Index;
				Sphere.Centre = Centre;
				Sphere.Radius = Radii[Index];
				Sphere.Velocity = Velocity;
				Sphere.WaterZ = StillZ;
			}
		}
		Swimmer.bHasSpheres = true;
		Swimmer.Stats.DisplacedLitres = Swimmer.Source.bDisplacement ? static_cast<float>(Below / 1000.0) : 0.f;
		const APawn* Pawn = Swimmer.Source.bUsePawnBody ? Cast<APawn>(Mesh->GetOwner()) : nullptr;
		const UCapsuleComponent* Capsule = Pawn ? Cast<UCapsuleComponent>(Pawn->GetRootComponent()) : nullptr;
		Swimmer.Stats.bUsesPawnBody = Capsule != nullptr;
		if (Capsule)
		{
			Swimmer.Stats.DisplacedLitres = Swimmer.Source.bDisplacement && Capsule->IsCollisionEnabled()
				? FFluidNinjaSwimmerBodyShape::Make(Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleHalfHeight(),
					Capsule->GetComponentLocation().Z, StillZ, Settings.SwimmerVolumeLitres).DisplacedLitres : 0.f;
		}
	}

	WriteStrokePoints();
}

void FFluidNinjaBuoyancySwimmers::TickSwimmer(FSwimmer& Swimmer, const FFluidNinjaSwimmerPose& Pose, float WaterZ, float DeltaSeconds, double Now)
{
	using namespace FluidNinjaBuoyancySwimmerParts;
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	UFluidNinjaBuoyancySubsystem* System = Owner.Get();
	if (!System)
	{
		return;
	}
	// The parts' state follows the pose's parts by index; another set of bones (another mesh, a profile change) starts over.
	bool bSameParts = Swimmer.Parts.Num() == Pose.Parts.Num() && Swimmer.PartBones.Num() == Pose.Parts.Num();
	for (int32 Index = 0; bSameParts && Index < Pose.Parts.Num(); ++Index)
	{
		bSameParts = Swimmer.PartBones[Index] == Pose.Parts[Index].Bone;
	}
	if (!bSameParts)
	{
		Swimmer.Parts.Reset();
		Swimmer.PartBones.Reset();
		for (const FFluidNinjaSwimmerPart& Part : Pose.Parts)
		{
			Swimmer.Parts.AddDefaulted();
			Swimmer.PartBones.Add(Part.Bone);
		}
	}
	const UFluidNinjaSwimmerProfile* Profile = ProfileOf(Swimmer, Pose.Mesh);
	const double MinEntry = FMath::Max(Profile && Profile->EntryMinCmS >= 0.f ? Profile->EntryMinCmS : Settings.SwimmerEntryMinCmS, 1.f);
	const double ExitFactor = Profile ? FMath::Max(Profile->ExitSpeedFactor, 1.0) : 1.3;
	const double ImpulseScale = FMath::Max(Settings.SwimmerImpulseScale, 0.f) * (Profile ? FMath::Max(Profile->ImpulseScale, 0.f) : 1.0);
	const double SplashScale = FMath::Max(Settings.SwimmerSplashScale, 0.f) * (Profile ? FMath::Max(Profile->SplashScale, 0.f) : 1.0);
	const double KickDepth = FMath::Max(Settings.SwimmerKickDepthCm, 0.f);
	Swimmer.Recent.RemoveAll([Now, &Settings](const FEntry& Entry) { return Now - Entry.Time > Settings.SwimmerCavitySeconds; });
	bool bAnyUnder = false;
	for (int32 Index = 0; Index < Pose.Parts.Num(); ++Index)
	{
		const FFluidNinjaSwimmerPart& Spec = Pose.Parts[Index];
		FPart& Part = Swimmer.Parts[Index];
		const FVector Location = Spec.Location;
		const bool bUnder = Location.Z < WaterZ;
		bAnyUnder |= bUnder;
		const double Moved = FVector::Dist(Location, Part.Previous);
		if (!Part.bHasPrevious || Moved > TeleportCm || Moved > MaxPartSpeedCmS * DeltaSeconds + 1.0)
		{
			Part.Previous = Location;
			Part.bHasPrevious = true;
			continue;
		}
		const FVector Velocity = (Location - Part.Previous) / DeltaSeconds;
		const bool bWasUnder = Part.Previous.Z < WaterZ;
		Part.Previous = Location;
		const FVector Surface(Location.X, Location.Y, WaterZ);
		const double Mass = Spec.AddedMassKg;
		const double RadiusM = Spec.RadiusCm * 0.01;

		if (!bWasUnder && bUnder && -Velocity.Z >= MinEntry)
		{
			// In: its slam power (W) on the project's splash scale, and its momentum into the waves.
			const double Speed = -Velocity.Z;
			const double SpeedMs = Speed * 0.01;
			const double Watts = Mass * SpeedMs * SpeedMs * SpeedMs / (2.0 * RadiusM);
			const FVector2D Where(Location);
			// Into the pocket an earlier part opened where this one goes in; parts going in in the same frame went in together, side by
			// side (a tuck's hips and feet, 40 cm apart, are too far apart for one pocket anyway), whatever the frame rate.
			const bool bCavity = Swimmer.Recent.ContainsByPredicate([&Where, &Settings, Now](const FEntry& Entry)
			{
				return Entry.Time < Now && FVector2D::Distance(Entry.Where, Where) < Settings.SwimmerCavityCm;
			});
			const float Strength = static_cast<float>(Settings.SplashStrength(static_cast<float>(Watts)) * SplashScale * (bCavity ? 1.0 / 3.0 : 1.0));
			const double Impulse = Mass * Speed * ImpulseScale * (bCavity ? 0.5 : 1.0);
			Splash(Surface, Strength);
			System->AddWaterImpulse(Surface, static_cast<float>(Impulse), Spec.AreaCm2);
			Swimmer.Recent.Add({Where, Now});

			FFluidNinjaSwimmerStats& Stats = Swimmer.Stats;
			if (Swimmer.bNewBurst)
			{
				Swimmer.bNewBurst = false;
				Stats.BurstParts = 0;
				Stats.BurstLargestSplash = 0.f;
				Stats.BurstLargestPart = NAME_None;
				Stats.BurstImpulseNs = 0.f;
			}
			++Stats.Entries;
			Stats.LastPart = Spec.Bone;
			Stats.LastEntrySpeedCmS = static_cast<float>(Speed);
			Stats.LastImpulseNs = static_cast<float>(Impulse * 0.01);   // kg·cm/s to N·s
			Stats.LastSplash = Strength >= FluidNinjaBuoyancySwimmerParts::SmallestSplash ? Strength : 0.f;
			Stats.LastEntryTime = Now;
			++Stats.BurstParts;
			Stats.BurstImpulseNs += static_cast<float>(Impulse * 0.01);
			if (Strength > Stats.BurstLargestSplash)
			{
				Stats.BurstLargestSplash = Strength;
				Stats.BurstLargestPart = Spec.Bone;
			}
			Swimmer.EntryTimes.Add(Now);
		}
		else if (bWasUnder && !bUnder && Velocity.Z >= MinEntry * ExitFactor)
		{
			// Out: a small splash, and the water it draws up with it.
			const double SpeedMs = Velocity.Z * 0.01;
			const double Watts = Mass * SpeedMs * SpeedMs * SpeedMs / (2.0 * RadiusM);
			Splash(Surface, static_cast<float>(0.4 * Settings.SplashStrength(static_cast<float>(Watts)) * SplashScale));
			System->AddWaterImpulse(Surface, static_cast<float>(-Mass * Velocity.Z * ImpulseScale / 3.0), Spec.AreaCm2);
		}
		else if (Spec.bKick && bUnder && KickDepth > 0.0 && FMath::Abs(Velocity.Z) >= KickMinSpeedCmS)
		{
			// A kick just under the surface: going up it pushes the water above up, going down it draws it down.
			const double Depth = WaterZ - Location.Z;
			if (Depth < KickDepth)
			{
				const double Impulse = -Mass * Velocity.Z * (DeltaSeconds / KickSeconds) * (1.0 - Depth / KickDepth) * ImpulseScale;
				System->AddWaterImpulse(Surface, static_cast<float>(Impulse), Spec.AreaCm2);
			}
		}
	}

	// Bursts: half a second with every part out of the water, and the next entry starts a new one.
	if (bAnyUnder)
	{
		Swimmer.OutSince = -1.0;
	}
	else if (Swimmer.OutSince < 0.0)
	{
		Swimmer.OutSince = Now;
	}
	else if (Now - Swimmer.OutSince >= BurstGapSeconds)
	{
		Swimmer.bNewBurst = true;
	}
	Swimmer.EntryTimes.RemoveAll([Now](double Time) { return Now - Time > RateWindowSeconds; });
	Swimmer.Stats.EntriesPerMinute = static_cast<float>(Swimmer.EntryTimes.Num() * 60.0 / RateWindowSeconds);
}

void FFluidNinjaBuoyancySwimmers::TickDive(FSwimmer& Swimmer, const FFluidNinjaSwimmerPose& Pose, float WaterZ, float DeltaSeconds)
{
	using namespace FluidNinjaBuoyancySwimmerParts;
	// The chest going under at the profile's dive speed: the dive, once. The chest is the sphere in the middle: a feet-first jump and a
	// head-first dive both put it under on the way in, after the limbs' own contacts.
	if (!Pose.bHasSpheres || Pose.ChestRadiusCm <= 0.f)
	{
		Swimmer.bChestKnown = false;
		return;
	}
	const float ChestZ = static_cast<float>(Pose.Chest.Z);
	const bool bUnder = ChestZ < WaterZ;
	const bool bKnown = Swimmer.bChestKnown && FMath::Abs(ChestZ - Swimmer.ChestWasZ) < TeleportCm && FMath::Abs(ChestZ - Swimmer.ChestWasZ) < MaxPartSpeedCmS * DeltaSeconds + 1.0;
	const bool bWasUnder = Swimmer.bChestWasUnder;
	const float Fall = bKnown ? (Swimmer.ChestWasZ - ChestZ) / DeltaSeconds : 0.f;
	Swimmer.bChestKnown = true;
	Swimmer.bChestWasUnder = bUnder;
	Swimmer.ChestWasZ = ChestZ;
	const UFluidNinjaSwimmerProfile* Profile = ProfileOf(Swimmer, Pose.Mesh);
	const float DiveMin = Profile ? FMath::Max(Profile->DiveMinCmS, 0.f) : 150.f;
	if (!bKnown || bWasUnder || !bUnder || Fall < DiveMin)
	{
		return;
	}
	// The mass under the surface now: the spheres' volume below it at the body's density, at most the body's mass.
	double BelowCm3 = 0.0;
	const FVector Centres[NumSpheres] = {Pose.Head, Pose.Chest, Pose.Hips};
	const float Radii[NumSpheres] = {Pose.HeadRadiusCm, Pose.ChestRadiusCm, Pose.HipsRadiusCm};
	for (int32 Index = 0; Index < NumSpheres; ++Index)
	{
		if (Radii[Index] > 0.f)
		{
			BelowCm3 += SphereVolumeBelow(Centres[Index].Z, Radii[Index], WaterZ);
		}
	}
	const double BodyMass = Pose.MassKg > 0.f ? Pose.MassKg : Profile ? Profile->MassKg : 75.0;
	const float MassShare = static_cast<float>(FMath::Min(BelowCm3 * 0.001 * BodyDensity, BodyMass));
	const FVector Where(Pose.Chest.X, Pose.Chest.Y, WaterZ);
	const USkeletalMeshComponent* Mesh = Pose.Mesh ? Pose.Mesh.Get() : Swimmer.Mesh.Get();
	UE_LOG(LogFluidNinjaBuoyancy, Log, TEXT("FluidNinja Buoyancy: swimmer %s dived at (%.0f, %.0f), %.0f cm/s, %.1f kg under"), Mesh ? *Mesh->GetName() : TEXT("?"),
		Where.X, Where.Y, Fall, MassShare);
	OnEntry.Broadcast(Mesh, Where, Fall, MassShare);
	OnAnyEntry().Broadcast(Mesh, Where, Fall, MassShare);
}

void FFluidNinjaBuoyancySwimmers::TickStroke(FSwimmer& Swimmer, const FFluidNinjaSwimmerPose& Pose, float StillZ, float DeltaSeconds)
{
	using namespace FluidNinjaBuoyancySwimmerParts;
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	UFluidNinjaBuoyancySubsystem* System = Owner.Get();
	if (!System || Pose.StrokeImpulse.IsNearlyZero() || !FMath::IsFinite(Pose.StrokeImpulse.X) || !FMath::IsFinite(Pose.StrokeImpulse.Y) || !FMath::IsFinite(Pose.StrokeImpulse.Z))
	{
		return;
	}
	const double Scale = FMath::Max(Settings.SwimmerStrokeScale, 0.f);
	if (Scale <= 0.0)
	{
		return;
	}
	// In the water: the hips no higher than their radius over the still water (the wakes' rule), else the body's reference point.
	const FVector Reference = Pose.bHasSpheres && Pose.HipsRadiusCm > 0.f ? Pose.Hips : Pose.Location;
	const double Reach = Pose.bHasSpheres && Pose.HipsRadiusCm > 0.f ? Pose.HipsRadiusCm : HipsRadiusCm;
	if (StillZ - Reference.Z <= -Reach)
	{
		return;
	}
	const UFluidNinjaSwimmerProfile* Profile = ProfileOf(Swimmer, Pose.Mesh);
	const double StrokeWater = FMath::Max(Profile ? Profile->StrokeWaterKg : 2.f, 0.1f);
	const double ImpulseScale = Profile ? FMath::Max(Profile->ImpulseScale, 0.f) : 1.0;
	const FVector2D Surface(Pose.bHasSpheres && Pose.ChestRadiusCm > 0.f ? FVector2D(Pose.Chest) : FVector2D(Reference));
	// A stroke reaches the surface only from near it: its push fades as exp(-depth / reach), the depth of the chest's top under the still
	// water and the reach the chest's radius, the rule every other wave source keeps (a hull piece's drag fades over the hull's half-extent,
	// FluidNinjaBuoyancyCore RippleReach). Pressed into the surface from any depth, an underwater breaststroke punched a hand-sized impulse
	// into it every stroke, a metre down, and the surface stood up in glass spikes over the swimmer.
	const bool bChest = Pose.bHasSpheres && Pose.ChestRadiusCm > 0.f;
	const double ChestTopZ = bChest ? Pose.Chest.Z + Pose.ChestRadiusCm : Reference.Z + Reach;
	const double StrokeReach = FMath::Max(bChest ? static_cast<double>(Pose.ChestRadiusCm) : Reach, 1.0);
	const double DepthFade = FMath::Exp(-FMath::Max(StillZ - ChestTopZ, 0.0) / StrokeReach);

	// The horizontal push: the water behind the stroke moves at the impulse over the water a stroke sweeps, as a point into FluidNinja's
	// input (the swimmers' wakes' rule: nothing under the cut-off FluidNinja ignores, nothing when wakes are off or the water is controlled).
	// The swimmer's own tuning (its provider's Wake Tuning with Override on), else the project's: Min Speed and Size.
	const FFluidNinjaSwimmerWakeTuning Tuning = GetWakeTuning(Swimmer.Mesh.Get());
	const FVector2D Along(Pose.StrokeImpulse.X, Pose.StrokeImpulse.Y);
	const double Speed = Along.Size() / StrokeWater * Scale * DepthFade;
	const double MinWrite = FMath::Max(FluidNinjaBuoyancyCVars::WakeMinWriteCmS, 0.f);
	if (Speed >= FMath::Max(static_cast<double>(Tuning.MinSpeedCmS), MinWrite) && Settings.SwimmerWakesEnabled() && !System->IsWaterControlled())
	{
		const FVector2D Way = Along.GetSafeNormal();
		FFluidNinjaSwimmerStrokePoint& Point = StrokePoints.AddDefaulted_GetRef();
		Point.Position = FVector(Surface.X, Surface.Y, StillZ);
		Point.Previous = Point.Position - FVector(Way.X, Way.Y, 0.0) * (Speed * DeltaSeconds);
		Point.VelocityAndSize = FVector4(Way.X * Speed, Way.Y * Speed, 0.0, Tuning.SizeCm * FMath::Clamp(Speed / 100.0, 0.6, 1.5));
	}
	// The vertical push: a small impulse on the waves where the stroke pressed (down positive, as the parts' entries), over a hand's area.
	const double Down = -Pose.StrokeImpulse.Z * Scale * ImpulseScale * DepthFade;
	if (FMath::Abs(Down) > 1.0)
	{
		System->AddWaterImpulse(FVector(Surface.X, Surface.Y, StillZ), static_cast<float>(Down), 150.f);
	}
}

void FFluidNinjaBuoyancySwimmers::WriteStrokePoints()
{
	if (StrokePoints.IsEmpty())
	{
		return;
	}
	UFluidNinjaBuoyancySubsystem* System = Owner.Get();
	UWorld* ThisWorld = System ? System->GetWorld() : nullptr;
	if (!ThisWorld)
	{
		return;
	}
	if (!WakeChannel.IsValid())
	{
		WakeChannel = Cast<UNiagaraDataChannelAsset>(UFluidNinjaBuoyancySettings::Get().WakeDataChannel.TryLoad());
		if (!bWakeChannelLoaded && !WakeChannel.IsValid())
		{
			UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("FluidNinja Buoyancy: no Wake Data Channel; swimmers' strokes write no wake points."));
		}
		bWakeChannelLoaded = true;
	}
	UNiagaraDataChannelAsset* Channel = WakeChannel.Get();
	if (!Channel)
	{
		return;
	}
	if (UNiagaraDataChannelWriter* Writer = UNiagaraDataChannelLibrary::WriteToNiagaraDataChannel(ThisWorld, Channel, FNiagaraDataChannelSearchParameters(),
		StrokePoints.Num(), false, true, true, TEXT("FluidNinja Buoyancy strokes")))
	{
		static const FName PositionName(TEXT("Position"));
		static const FName PreviousName(TEXT("PositionPrevious"));
		static const FName VelocityName(TEXT("VelocityAndSize"));
		for (int32 Index = 0; Index < StrokePoints.Num(); ++Index)
		{
			Writer->WritePosition(PositionName, Index, StrokePoints[Index].Position);
			Writer->WritePosition(PreviousName, Index, StrokePoints[Index].Previous);
			Writer->WriteVector4(VelocityName, Index, StrokePoints[Index].VelocityAndSize);
		}
		LastStrokePoints = StrokePoints.Num();
	}
}
