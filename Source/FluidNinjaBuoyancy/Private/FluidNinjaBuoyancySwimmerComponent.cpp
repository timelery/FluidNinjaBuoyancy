#include "FluidNinjaBuoyancySwimmerComponent.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "FluidNinjaBuoyancyLog.h"
#include "FluidNinjaBuoyancySwimmers.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"

UFluidNinjaSwimmerComponent::UFluidNinjaSwimmerComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UFluidNinjaSwimmerComponent::BeginPlay()
{
	Super::BeginPlay();
	RegisterWith(UFluidNinjaSwimmerProviderLibrary::FindSwimmerMesh(GetOwner(), Profile));
}

void UFluidNinjaSwimmerComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterNow();
	Super::EndPlay(EndPlayReason);
}

void UFluidNinjaSwimmerComponent::SetMesh(USkeletalMeshComponent* InMesh)
{
	if (InMesh != Mesh.Get())
	{
		UnregisterNow();
		RegisterWith(InMesh);
	}
}

void UFluidNinjaSwimmerComponent::SetDisplacement(bool bInDisplacement)
{
	bDisplacement = bInDisplacement;
	RefreshRegistration();
}

void UFluidNinjaSwimmerComponent::SetPartContacts(bool bInPartContacts)
{
	bPartContacts = bInPartContacts;
	RefreshRegistration();
}

void UFluidNinjaSwimmerComponent::SetWholeBody(bool bInWholeBody)
{
	bWholeBody = bInWholeBody;
	RefreshRegistration();
}

void UFluidNinjaSwimmerComponent::SetWaterFromQuery(bool bInWaterFromQuery)
{
	bWaterFromQuery = bInWaterFromQuery;
	RefreshRegistration();
}

void UFluidNinjaSwimmerComponent::SetStillWaterZCm(float InStillWaterZCm)
{
	StillWaterZCm = InStillWaterZCm;
	RefreshRegistration();
}

#if WITH_EDITOR
void UFluidNinjaSwimmerComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	RefreshRegistration();
}
#endif

void UFluidNinjaSwimmerComponent::RefreshRegistration()
{
	// Only a registered swimmer (after Begin Play): the same mesh registered again updates its source, provider, profile and water in place.
	if (!HasBegunPlay() || !Mesh.IsValid())
	{
		return;
	}
	if (FFluidNinjaBuoyancySwimmers* Swimmers = FFluidNinjaBuoyancySwimmers::Find(GetWorld()))
	{
		FFluidNinjaSwimmerSource Source;
		Source.WaterZ = StillWaterZCm;
		Source.bDisplacement = bDisplacement;
		Source.bPartContacts = bPartContacts;
		Source.bWholeBody = bWholeBody;
		Swimmers->Register(Mesh.Get(), Source, this, Profile, bWaterFromQuery);
	}
}

void UFluidNinjaSwimmerComponent::RegisterWith(USkeletalMeshComponent* InMesh)
{
	Mesh = InMesh;
	UWorld* World = GetWorld();
	UFluidNinjaBuoyancySubsystem* Subsystem = World ? World->GetSubsystem<UFluidNinjaBuoyancySubsystem>() : nullptr;
	if (!Subsystem)
	{
		return;
	}
	if (!InMesh)
	{
		UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("FluidNinja Swimmer on %s: no skeletal mesh fits the profile; the owner is no swimmer."),
			*GetOwner()->GetName());
		return;
	}
	FFluidNinjaSwimmerSource Source;
	Source.WaterZ = StillWaterZCm;
	Source.bDisplacement = bDisplacement;
	Source.bPartContacts = bPartContacts;
	Source.bWholeBody = bWholeBody;
	// Through the subsystem first (it creates the swimmers container), then the container's extended registration: this component is the
	// provider, so the pose, the wake tuning and the profile are read from it each frame.
	Subsystem->RegisterSwimmer(InMesh, Source);
	if (FFluidNinjaBuoyancySwimmers* Swimmers = FFluidNinjaBuoyancySwimmers::Find(World))
	{
		Swimmers->Register(InMesh, Source, this, Profile, bWaterFromQuery);
		if (!EntryHandle.IsValid())
		{
			EntryHandle = Swimmers->OnEntry.AddUObject(this, &UFluidNinjaSwimmerComponent::HandleEntry);
		}
	}
}

void UFluidNinjaSwimmerComponent::UnregisterNow()
{
	UWorld* World = GetWorld();
	if (FFluidNinjaBuoyancySwimmers* Swimmers = FFluidNinjaBuoyancySwimmers::Find(World))
	{
		if (EntryHandle.IsValid())
		{
			Swimmers->OnEntry.Remove(EntryHandle);
			EntryHandle.Reset();
		}
		if (USkeletalMeshComponent* Registered = Mesh.Get())
		{
			Swimmers->Unregister(Registered);
		}
	}
	Mesh.Reset();
}

void UFluidNinjaSwimmerComponent::HandleEntry(const USkeletalMeshComponent* EntryMesh, const FVector& Where, float SpeedCmS, float MassShareKg)
{
	if (EntryMesh && EntryMesh == Mesh.Get())
	{
		const UFluidNinjaSwimmerProfile* Resolved = UFluidNinjaSwimmerProfile::Resolve(Profile, EntryMesh);
		OnEntry.Broadcast(Where, SpeedCmS, MassShareKg, Resolved ? Resolved->SoundScale : 1.f);
	}
}

bool UFluidNinjaSwimmerComponent::IsSwimmingNow() const
{
	const AActor* Owner = GetOwner();
	const UCharacterMovementComponent* Movement = Owner ? Owner->FindComponentByClass<UCharacterMovementComponent>() : nullptr;
	return Movement && Movement->IsSwimming();
}

bool UFluidNinjaSwimmerComponent::IsAirborneNow() const
{
	const AActor* Owner = GetOwner();
	const UCharacterMovementComponent* Movement = Owner ? Owner->FindComponentByClass<UCharacterMovementComponent>() : nullptr;
	return Movement && Movement->IsFalling();
}

bool UFluidNinjaSwimmerComponent::GetSwimmerPose_Implementation(FFluidNinjaSwimmerPose& OutPose)
{
	USkeletalMeshComponent* Body = Mesh.Get();
	if (!UFluidNinjaSwimmerProviderLibrary::BuildDefaultPose(Body, Profile, Cast<APawn>(GetOwner()), OutPose))
	{
		return false;
	}
	// The locomotion's state through the overridables (a Character's by default), and what the animation fed in since last frame.
	OutPose.bSwimming = IsSwimmingNow();
	OutPose.bAirborne = IsAirborneNow();
	OutPose.StrokeImpulse = PendingStroke;
	OutPose.StrokePhase = StrokePhase;
	PendingStroke = FVector::ZeroVector;
	if (!bWaterFromQuery)
	{
		OutPose.bHasWaterZ = true;
		OutPose.WaterZ = StillWaterZCm;
	}
	return true;
}

bool UFluidNinjaSwimmerComponent::GetWater(FFluidNinjaWaterSample& OutSample, FVector& OutDriftCurrent) const
{
	const FFluidNinjaBuoyancySwimmers* Swimmers = FFluidNinjaBuoyancySwimmers::Find(GetWorld());
	if (!Swimmers || !Swimmers->GetWater(Mesh.Get(), OutSample, OutDriftCurrent))
	{
		OutSample = FFluidNinjaWaterSample();
		OutDriftCurrent = FVector::ZeroVector;
		return false;
	}
	return true;
}

FVector UFluidNinjaSwimmerComponent::GetDriftCurrent() const
{
	FFluidNinjaWaterSample Sample;
	FVector Drift = FVector::ZeroVector;
	GetWater(Sample, Drift);
	return Drift;
}

bool UFluidNinjaSwimmerComponent::GetSwimmerStats(FFluidNinjaSwimmerStats& OutStats) const
{
	const FFluidNinjaBuoyancySwimmers* Swimmers = FFluidNinjaBuoyancySwimmers::Find(GetWorld());
	if (!Swimmers || !Swimmers->GetStats(Mesh.Get(), OutStats))
	{
		OutStats = FFluidNinjaSwimmerStats();
		return false;
	}
	return true;
}

void UFluidNinjaSwimmerComponent::AddStrokeImpulse(FVector ImpulseKgCmS)
{
	if (!ImpulseKgCmS.ContainsNaN())
	{
		PendingStroke += ImpulseKgCmS;
	}
}
