#include "FluidNinjaBuoyancyWaterQuery.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "FluidNinjaBuoyancySettings.h"
#include "FluidNinjaBuoyancySubsystem.h"
#include "FluidNinjaBuoyancySwimmers.h"

UFluidNinjaBuoyancySubsystem* UFluidNinjaBuoyancyWaterLibrary::FindSubsystem(const UObject* WorldContextObject)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	// GetSubsystem<Base>() returns the project's derived subsystem too (the base steps aside when one exists).
	return World ? World->GetSubsystem<UFluidNinjaBuoyancySubsystem>() : nullptr;
}

bool UFluidNinjaBuoyancyWaterLibrary::SampleWater(UFluidNinjaBuoyancySubsystem& Subsystem, const FVector& Location, FFluidNinjaWaterSample& OutSample)
{
	return Subsystem.SampleWater(Location, OutSample);
}

bool UFluidNinjaBuoyancyWaterLibrary::GetWaterAtLocation(const UObject* WorldContextObject, FVector Location, FFluidNinjaWaterSample& OutSample)
{
	OutSample = FFluidNinjaWaterSample();
	UFluidNinjaBuoyancySubsystem* Subsystem = FindSubsystem(WorldContextObject);
	return Subsystem && Subsystem->SampleWater(Location, OutSample);
}

FVector UFluidNinjaBuoyancyWaterLibrary::FilterDriftCurrent(FVector Previous, FVector Measured, bool bMeasured, float DeltaSeconds, float SmoothSeconds, float DeadBandCmS)
{
	// The target: the reading, dead-banded (under the band it is still water; above it the band is taken off, so the drift starts from 0
	// rather than jumping to the band), horizontal. No reading: still water, eased to over the same time.
	FVector Target = FVector::ZeroVector;
	if (bMeasured && !Measured.ContainsNaN())
	{
		const FVector Flat(Measured.X, Measured.Y, 0.0);
		const double Speed = Flat.Size();
		const double Band = FMath::Max(DeadBandCmS, 0.f);
		if (Speed > Band)
		{
			Target = Flat * ((Speed - Band) / Speed);
		}
	}
	if (Previous.ContainsNaN())
	{
		Previous = FVector::ZeroVector;
	}
	if (SmoothSeconds <= 0.f || DeltaSeconds <= 0.f)
	{
		return Target;
	}
	// An exponential ease: the fraction of the gap closed in a frame, the same for any frame rate.
	const float Ease = 1.f - FMath::Exp(-DeltaSeconds / SmoothSeconds);
	return FMath::Lerp(Previous, Target, Ease);
}

bool UFluidNinjaBuoyancyWaterLibrary::GetSwimmerWater(const UObject* WorldContextObject, const USkeletalMeshComponent* Mesh, FFluidNinjaWaterSample& OutSample, FVector& OutDriftCurrent)
{
	OutSample = FFluidNinjaWaterSample();
	OutDriftCurrent = FVector::ZeroVector;
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	const FFluidNinjaBuoyancySwimmers* Swimmers = FFluidNinjaBuoyancySwimmers::Find(World);
	return Swimmers && Swimmers->GetWater(Mesh, OutSample, OutDriftCurrent);
}

bool UFluidNinjaBuoyancyWaterLibrary::PokeAt(const UObject* WorldContextObject, FVector Location, float RadiusCm, float StrengthNs)
{
	UFluidNinjaBuoyancySubsystem* Subsystem = FindSubsystem(WorldContextObject);
	if (!Subsystem || Location.ContainsNaN() || !FMath::IsFinite(StrengthNs) || StrengthNs == 0.f)
	{
		return false;
	}
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	// Newton-seconds to kg·cm/s (AddWaterImpulse's unit), on the swimmers' impulse scale; over the disc's area (the ring's footprint).
	const double Radius = FMath::Max(RadiusCm, 1.f);
	const double Impulse = static_cast<double>(StrengthNs) * 100.0 * FMath::Max(Settings.SwimmerImpulseScale, 0.f);
	Subsystem->AddWaterImpulse(Location, static_cast<float>(Impulse), static_cast<float>(UE_DOUBLE_PI * Radius * Radius));
	return Subsystem->GetWaves() != nullptr;
}
