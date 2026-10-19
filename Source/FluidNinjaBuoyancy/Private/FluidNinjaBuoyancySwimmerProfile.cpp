#include "FluidNinjaBuoyancySwimmerProfile.h"

#include "Components/SkeletalMeshComponent.h"

namespace FluidNinjaBuoyancySwimmerPresets
{
	FFluidNinjaSwimmerPartSpec Part(const TCHAR* Bone, EFluidNinjaSwimmerPartKind Kind, float RadiusCm, float AddedMassKg, float AreaCm2, bool bKick, bool bWholeBodyOnly)
	{
		FFluidNinjaSwimmerPartSpec Spec;
		Spec.Bone = Bone;
		Spec.Kind = Kind;
		Spec.RadiusCm = RadiusCm;
		Spec.AddedMassKg = AddedMassKg;
		Spec.AreaCm2 = AreaCm2;
		Spec.bKick = bKick;
		Spec.bWholeBodyOnly = bWholeBodyOnly;
		return Spec;
	}

	/** Manny / Quinn: the table FluidNinjaBuoyancySwimmers.cpp carried before the profiles, in its order (the contacts' state is kept per
	 * index, and the demo's numbers must not change). */
	void FillManny(UFluidNinjaSwimmerProfile& P)
	{
		using E = EFluidNinjaSwimmerPartKind;
		P.HeadBone = TEXT("head");
		P.HeadRadiusCm = 11.f;
		P.ChestBone = TEXT("spine_03");
		P.ChestRadiusCm = 16.f;
		P.HipsBone = TEXT("pelvis");
		P.HipsRadiusCm = 15.f;
		P.Parts.Reset();
		P.Parts.Add(Part(TEXT("hand_l"), E::Hand, 6.f, 0.45f, 150.f, false, false));
		P.Parts.Add(Part(TEXT("hand_r"), E::Hand, 6.f, 0.45f, 150.f, false, false));
		P.Parts.Add(Part(TEXT("lowerarm_l"), E::Forearm, 5.f, 0.6f, 180.f, false, false));
		P.Parts.Add(Part(TEXT("lowerarm_r"), E::Forearm, 5.f, 0.6f, 180.f, false, false));
		P.Parts.Add(Part(TEXT("foot_l"), E::Foot, 6.f, 0.6f, 200.f, true, false));
		P.Parts.Add(Part(TEXT("foot_r"), E::Foot, 6.f, 0.6f, 200.f, true, false));
		P.Parts.Add(Part(TEXT("head"), E::Head, 11.f, 2.8f, 380.f, false, false));
		P.Parts.Add(Part(TEXT("spine_03"), E::Chest, 17.f, 10.f, 900.f, false, true));
		P.Parts.Add(Part(TEXT("pelvis"), E::Hips, 15.f, 8.f, 700.f, false, true));
		P.Parts.Add(Part(TEXT("calf_l"), E::Calf, 8.f, 2.f, 400.f, false, true));
		P.Parts.Add(Part(TEXT("calf_r"), E::Calf, 8.f, 2.f, 400.f, false, true));
		P.MassKg = 75.f;
		P.StrokeWaterKg = 2.f;
	}

	/** A quadruped (a deer, a dog, a horse scaled by its rig): the chest and hips are the body's two ends, the head is its own sphere, the
	 * four feet kick; nothing is whole-body-only (a creature in the water is all in). */
	void FillQuadruped(UFluidNinjaSwimmerProfile& P)
	{
		using E = EFluidNinjaSwimmerPartKind;
		P.HeadBone = TEXT("head");
		P.HeadRadiusCm = 12.f;
		P.ChestBone = TEXT("spine_03");
		P.ChestRadiusCm = 22.f;
		P.HipsBone = TEXT("pelvis");
		P.HipsRadiusCm = 20.f;
		P.Parts.Reset();
		P.Parts.Add(Part(TEXT("hand_l"), E::Foot, 6.f, 0.6f, 120.f, true, false));
		P.Parts.Add(Part(TEXT("hand_r"), E::Foot, 6.f, 0.6f, 120.f, true, false));
		P.Parts.Add(Part(TEXT("foot_l"), E::Foot, 6.f, 0.6f, 120.f, true, false));
		P.Parts.Add(Part(TEXT("foot_r"), E::Foot, 6.f, 0.6f, 120.f, true, false));
		P.Parts.Add(Part(TEXT("head"), E::Head, 12.f, 3.6f, 450.f, false, false));
		P.Parts.Add(Part(TEXT("neck_01"), E::Other, 9.f, 1.5f, 250.f, false, false));
		P.Parts.Add(Part(TEXT("spine_03"), E::Chest, 22.f, 22.f, 1500.f, false, false));
		P.Parts.Add(Part(TEXT("spine_01"), E::Other, 20.f, 17.f, 1250.f, false, false));
		P.Parts.Add(Part(TEXT("pelvis"), E::Hips, 20.f, 17.f, 1250.f, false, false));
		P.MassKg = 90.f;
		P.StrokeWaterKg = 3.f;
	}
}

UFluidNinjaSwimmerProfile::UFluidNinjaSwimmerProfile()
{
	// A fresh data asset starts as Manny: its fields are filled, so a profile picked for a mannequin works without Apply Preset.
	if (HasAnyFlags(RF_ClassDefaultObject) && GetClass() == StaticClass())
	{
		FillPreset(EFluidNinjaSwimmerSkeleton::Manny, *this);
	}
}

void UFluidNinjaSwimmerProfile::ApplyPreset()
{
	FillPreset(Preset, *this);
}

void UFluidNinjaSwimmerProfile::FillPreset(EFluidNinjaSwimmerSkeleton Skeleton, UFluidNinjaSwimmerProfile& Profile)
{
	Profile.Preset = Skeleton;
	switch (Skeleton)
	{
	case EFluidNinjaSwimmerSkeleton::Quadruped:
		FluidNinjaBuoyancySwimmerPresets::FillQuadruped(Profile);
		break;
	case EFluidNinjaSwimmerSkeleton::MetaHuman:
	case EFluidNinjaSwimmerSkeleton::Manny:
	default:
		FluidNinjaBuoyancySwimmerPresets::FillManny(Profile);
		break;
	}
}

const UFluidNinjaSwimmerProfile* UFluidNinjaSwimmerProfile::GetPreset(EFluidNinjaSwimmerSkeleton Skeleton)
{
	switch (Skeleton)
	{
	case EFluidNinjaSwimmerSkeleton::MetaHuman:
		return GetDefault<UFluidNinjaSwimmerProfileMetaHuman>();
	case EFluidNinjaSwimmerSkeleton::Quadruped:
		return GetDefault<UFluidNinjaSwimmerProfileQuadruped>();
	case EFluidNinjaSwimmerSkeleton::Manny:
	default:
		return GetDefault<UFluidNinjaSwimmerProfileManny>();
	}
}

bool UFluidNinjaSwimmerProfile::FitsMesh(const USkeletalMeshComponent* Mesh) const
{
	if (!Mesh || !Mesh->GetSkeletalMeshAsset() || Mesh->GetBoneIndex(HipsBone) == INDEX_NONE)
	{
		return false;
	}
	for (const FFluidNinjaSwimmerPartSpec& Spec : Parts)
	{
		if (Mesh->GetBoneIndex(Spec.Bone) != INDEX_NONE)
		{
			return true;
		}
	}
	return false;
}

const UFluidNinjaSwimmerProfile* UFluidNinjaSwimmerProfile::FindPresetFor(const USkeletalMeshComponent* Mesh)
{
	// Manny before the quadruped: both name their hips pelvis and their feet foot_l; a mannequin has hands, a quadruped's "hands" are feet
	// and it has no lowerarm. The MetaHuman profile is Manny's under another name, so Manny's answers for it.
	const UFluidNinjaSwimmerProfile* Manny = GetPreset(EFluidNinjaSwimmerSkeleton::Manny);
	if (Manny->FitsMesh(Mesh) && Mesh->GetBoneIndex(TEXT("lowerarm_r")) != INDEX_NONE)
	{
		return Manny;
	}
	const UFluidNinjaSwimmerProfile* Quadruped = GetPreset(EFluidNinjaSwimmerSkeleton::Quadruped);
	if (Quadruped->FitsMesh(Mesh))
	{
		return Quadruped;
	}
	return Manny->FitsMesh(Mesh) ? Manny : nullptr;
}

const UFluidNinjaSwimmerProfile* UFluidNinjaSwimmerProfile::Resolve(const UFluidNinjaSwimmerProfile* Profile, const USkeletalMeshComponent* Mesh)
{
	return Profile ? Profile : FindPresetFor(Mesh);
}

UFluidNinjaSwimmerProfileManny::UFluidNinjaSwimmerProfileManny()
{
	FillPreset(EFluidNinjaSwimmerSkeleton::Manny, *this);
}

UFluidNinjaSwimmerProfileMetaHuman::UFluidNinjaSwimmerProfileMetaHuman()
{
	FillPreset(EFluidNinjaSwimmerSkeleton::MetaHuman, *this);
}

UFluidNinjaSwimmerProfileQuadruped::UFluidNinjaSwimmerProfileQuadruped()
{
	FillPreset(EFluidNinjaSwimmerSkeleton::Quadruped, *this);
}
