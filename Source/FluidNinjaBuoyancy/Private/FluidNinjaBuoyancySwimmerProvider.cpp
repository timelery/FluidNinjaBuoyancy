#include "FluidNinjaBuoyancySwimmerProvider.h"

#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"

UObject* UFluidNinjaSwimmerProviderLibrary::FindSwimmerProvider(AActor* Actor)
{
	if (!Actor)
	{
		return nullptr;
	}
	if (Actor->GetClass()->ImplementsInterface(UFluidNinjaSwimmerProvider::StaticClass()))
	{
		return Actor;
	}
	for (UActorComponent* Component : Actor->GetComponents())
	{
		if (Component && Component->GetClass()->ImplementsInterface(UFluidNinjaSwimmerProvider::StaticClass()))
		{
			return Component;
		}
	}
	return nullptr;
}

USkeletalMeshComponent* UFluidNinjaSwimmerProviderLibrary::FindSwimmerMesh(AActor* Actor, const UFluidNinjaSwimmerProfile* Profile)
{
	if (!Actor)
	{
		return nullptr;
	}
	TInlineComponentArray<USkeletalMeshComponent*> Meshes(Actor);
	USkeletalMeshComponent* Any = nullptr;
	for (USkeletalMeshComponent* Mesh : Meshes)
	{
		if (!Mesh->GetSkeletalMeshAsset())
		{
			continue;
		}
		if (Profile ? Mesh->GetBoneIndex(Profile->HipsBone) != INDEX_NONE : Mesh->GetBoneIndex(TEXT("hand_r")) != INDEX_NONE)
		{
			return Mesh;
		}
		if (!Any)
		{
			Any = Mesh;
		}
	}
	return Any;
}

bool UFluidNinjaSwimmerProviderLibrary::BuildDefaultPose(USkeletalMeshComponent* Mesh, const UFluidNinjaSwimmerProfile* Profile, APawn* Pawn, FFluidNinjaSwimmerPose& OutPose)
{
	OutPose = FFluidNinjaSwimmerPose();
	Profile = UFluidNinjaSwimmerProfile::Resolve(Profile, Mesh);
	if (!Mesh || !Profile)
	{
		return false;
	}
	OutPose.Mesh = Mesh;
	OutPose.MassKg = Profile->MassKg;

	// The spheres, where the mesh has their bones (a mesh without a head still floats on its chest and hips).
	auto Sphere = [&](FName Bone, float Radius, FVector& OutCentre, float& OutRadius)
	{
		if (Mesh->GetBoneIndex(Bone) != INDEX_NONE)
		{
			OutCentre = Mesh->GetBoneLocation(Bone);
			OutRadius = Radius;
			return true;
		}
		return false;
	};
	const bool bHead = Sphere(Profile->HeadBone, Profile->HeadRadiusCm, OutPose.Head, OutPose.HeadRadiusCm);
	const bool bChest = Sphere(Profile->ChestBone, Profile->ChestRadiusCm, OutPose.Chest, OutPose.ChestRadiusCm);
	const bool bHips = Sphere(Profile->HipsBone, Profile->HipsRadiusCm, OutPose.Hips, OutPose.HipsRadiusCm);
	OutPose.bHasSpheres = bHead || bChest || bHips;

	// The parts, in the profile's order, those the mesh has.
	OutPose.Parts.Reserve(Profile->Parts.Num());
	for (const FFluidNinjaSwimmerPartSpec& Spec : Profile->Parts)
	{
		if (Mesh->GetBoneIndex(Spec.Bone) == INDEX_NONE)
		{
			continue;
		}
		FFluidNinjaSwimmerPart& Part = OutPose.Parts.AddDefaulted_GetRef();
		Part.Bone = Spec.Bone;
		Part.Kind = Spec.Kind;
		Part.Location = Mesh->GetBoneLocation(Spec.Bone);
		Part.RadiusCm = Spec.RadiusCm;
		Part.AddedMassKg = Spec.AddedMassKg;
		Part.AreaCm2 = Spec.AreaCm2;
		Part.bKick = Spec.bKick;
		Part.bWholeBodyOnly = Spec.bWholeBodyOnly;
		Part.DensityKgM3 = Spec.DensityKgM3;
	}

	// The body: a pawn's capsule and movement (the plugin's rule for the player: its actor location, its capsule's half height, its
	// Character Movement's swimming and falling); a mesh on its own stands on its hips, with no velocity of its own (the subsystem takes
	// the mesh's motion frame to frame).
	if (Pawn)
	{
		OutPose.Location = Pawn->GetActorLocation();
		OutPose.Velocity = Pawn->GetVelocity();
		OutPose.Facing = FVector(Pawn->GetActorForwardVector().X, Pawn->GetActorForwardVector().Y, 0.0).GetSafeNormal();
		if (const UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(Pawn->GetRootComponent()))
		{
			OutPose.HalfHeightCm = Capsule->GetScaledCapsuleHalfHeight();
			OutPose.RadiusCm = Capsule->GetScaledCapsuleRadius();
		}
		else
		{
			OutPose.HalfHeightCm = Pawn->GetSimpleCollisionHalfHeight();
			OutPose.RadiusCm = Pawn->GetSimpleCollisionRadius();
		}
		if (const UCharacterMovementComponent* Movement = Pawn->FindComponentByClass<UCharacterMovementComponent>())
		{
			OutPose.bSwimming = Movement->IsSwimming();
			OutPose.bAirborne = Movement->IsFalling();
		}
	}
	else
	{
		OutPose.Location = bHips ? OutPose.Hips : Mesh->GetComponentLocation();
		OutPose.Facing = FVector(Mesh->GetForwardVector().X, Mesh->GetForwardVector().Y, 0.0).GetSafeNormal();
	}
	OutPose.bValid = true;
	return true;
}

bool UFluidNinjaSwimmerProviderLibrary::GetPoseFor(AActor* Actor, TWeakObjectPtr<USkeletalMeshComponent>& InOutMesh, FFluidNinjaSwimmerPose& OutPose)
{
	OutPose = FFluidNinjaSwimmerPose();
	if (!Actor)
	{
		return false;
	}
	if (UObject* Provider = FindSwimmerProvider(Actor))
	{
		if (IFluidNinjaSwimmerProvider::Execute_GetSwimmerPose(Provider, OutPose) && OutPose.bValid)
		{
			if (OutPose.Mesh)
			{
				InOutMesh = OutPose.Mesh;
			}
			return true;
		}
		return false;
	}
	USkeletalMeshComponent* Mesh = InOutMesh.IsValid() && InOutMesh->GetOwner() == Actor ? InOutMesh.Get() : nullptr;
	if (!Mesh)
	{
		Mesh = FindSwimmerMesh(Actor, nullptr);
		InOutMesh = Mesh;
	}
	return BuildDefaultPose(Mesh, nullptr, Cast<APawn>(Actor), OutPose);
}

void UFluidNinjaSwimmerProviderLibrary::FilterParts(FFluidNinjaSwimmerPose& Pose, bool bWholeBody)
{
	if (!bWholeBody)
	{
		Pose.Parts.RemoveAll([](const FFluidNinjaSwimmerPart& Part) { return Part.bWholeBodyOnly; });
	}
}
