#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "FluidNinjaBuoyancySwimmerProfile.generated.h"

class USkeletalMeshComponent;

/** What a body part is, for a project's swim code that reads a pose. Reserved: a label only, the plugin does not read it (a part kicks
 * by its Kick flag, and the dive is the profile's Chest Bone going under). */
UENUM(BlueprintType)
enum class EFluidNinjaSwimmerPartKind : uint8
{
	/** A hand (Manny: hand_l, hand_r). */
	Hand,
	/** A forearm (Manny: lowerarm_l, lowerarm_r). */
	Forearm,
	/** An upper arm (no shipped preset uses it). */
	UpperArm,
	/** A foot (Manny's feet, and all four of the quadruped's). */
	Foot,
	/** A calf (Manny: calf_l, calf_r, whole body only). */
	Calf,
	/** A thigh (no shipped preset uses it). */
	Thigh,
	/** The head. */
	Head,
	/** The chest (Manny: spine_03, whole body only). */
	Chest,
	/** The hips (Manny: pelvis, whole body only). */
	Hips,
	/** Anything else (the quadruped's neck_01 and spine_01). */
	Other,
};

/** The skeletons the plugin ships profiles for (UFluidNinjaSwimmerProfile::GetPreset). */
UENUM(BlueprintType)
enum class EFluidNinjaSwimmerSkeleton : uint8
{
	/** UE5's mannequins, Manny and Quinn (SK_Mannequin): pelvis, spine_03, head, hand_l... A swimmer without a profile gets this one when
	 * its mesh has these bones and lowerarm_r. */
	Manny,
	/** The MetaHuman body (metahuman_base_skel): the same bone names as Manny, verified against the engine's asset, so Apply Preset fills
	 * Manny's numbers. */
	MetaHuman,
	/** A four-legged creature on a typical quadruped rig (pelvis, spine_01..03, neck_01, head, the four feet). Rename the bones to the
	 * rig's. A swimmer without a profile gets this one when its mesh fits it but has no lowerarm_r. */
	Quadruped,
};

/**
 * A body part that meets the water (FluidNinjaBuoyancySwimmers.cpp): its bone, its size as a sphere, the water that moves with it going in
 * (its added mass: about half a sphere of water its size, ρ·⅔·π·r³ for a hand), the area it meets the water with, and whether it kicks
 * (feet) or only counts for a diver's whole body (Manny: chest, hips, calves).
 */
USTRUCT(BlueprintType)
struct FLUIDNINJABUOYANCY_API FFluidNinjaSwimmerPartSpec
{
	GENERATED_BODY()

	/** The bone the part follows: each frame the part is where this bone is. A bone the mesh lacks leaves the part out. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Part")
	FName Bone;

	/** What the part is (see the values). Reserved: the plugin does not read it; it is carried on the pose for a project's swim code.
	 * Default Other. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Part")
	EFluidNinjaSwimmerPartKind Kind = EFluidNinjaSwimmerPartKind::Other;

	/** The part as a sphere. Its splash power going in is added mass × speed³ / (2 × radius), so a smaller radius makes a bigger splash for
	 * the same entry. In cm. Default 6; at least 0.1. Manny: 5 (forearm) to 17 (chest); the quadruped up to 22. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Part", meta = (ClampMin = "0.1", Units = "cm"))
	float RadiusCm = 6.f;

	/** The water that moves with the part going in (its added mass, about half a sphere of water its size). Its ring impulse is this times
	 * its entry speed and its splash power grows with it; a kick pushes with it too. More makes bigger splashes and rings; 0 makes none.
	 * In kg. Default 0.45; at least 0. Manny: 0.45 (hand) to 10 (chest); the quadruped up to 22. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Part", meta = (ClampMin = "0", Units = "kg"))
	float AddedMassKg = 0.45f;

	/** The area it meets the water with, seen from above: the footprint its ring impulse is spread over (a Gaussian 1.5 times that disc's
	 * radius wide, at least Wave Source Smooth Cm and one wave cell). Bigger spreads the same impulse wider and lower. In cm². Default
	 * 150; at least 1. Manny: 150 (hand) to 900 (chest); the quadruped 120 to 1500. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Part", meta = (ClampMin = "1"))
	float AreaCm2 = 150.f;

	/** Moving up or down at 40 cm/s or more within Swimmer Kick Depth Cm under the surface, it churns the water (a flutter kick): going up
	 * it lifts the water above it, going down it draws it down, more the nearer the surface. The presets set it on the feet. Needs the
	 * swimmer's Part Contacts. Default off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Part")
	bool bKick = false;

	/** Only a swimmer registered with Whole Body (a diver going in) makes contacts with it; the player's pawn and ordinary swimmers leave it
	 * out (their chest and hips ride their capsule). Manny sets it on the chest, hips and calves. Default off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Part")
	bool bWholeBodyOnly = false;

	/** The part's own density, kg/m³ (the human body: about 985; lungs full, less). Reserved: the plugin does not read it (the dive's mass
	 * share uses 985 kg/m³ for the whole body); it is carried on the pose for a project's own body physics. Default 985; at least 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Part", meta = (ClampMin = "1"))
	float DensityKgM3 = 985.f;

	/** Scales the buoyancy a project's own body physics gives this part (a ragdoll or a creature controller may read it from the profile).
	 * Reserved: the plugin does not read it, and it is not carried on the pose (the plugin's swim paths do not move the swimmer). 1: by its
	 * density. Default 1; at least 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Part", meta = (ClampMin = "0"))
	float BuoyancyScale = 1.f;
};

/**
 * A swimmer's skeleton for FluidNinja Buoyancy: which bones are its head, chest and hips (the spheres that push the water aside and the
 * live water height is read at), which bones are the parts that splash and press rings where they go in, and the scales of what they do.
 * The plugin ships presets (GetPreset: Manny / Quinn, MetaHuman, a quadruped example); make a data asset of this class and pick a Preset,
 * or fill it in for a creature's rig. The player's pawn and registered swimmers without a profile get the preset whose bones their mesh has.
 */
UCLASS(BlueprintType)
class FLUIDNINJABUOYANCY_API UFluidNinjaSwimmerProfile : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UFluidNinjaSwimmerProfile();

	/** The skeleton the fields below were filled from (Manny, MetaHuman or Quadruped). Apply Preset fills every field again from it,
	 * overwriting edits; changing it alone changes nothing. Editing the fields afterwards is fine. Default Manny (a new profile starts
	 * filled as Manny). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skeleton")
	EFluidNinjaSwimmerSkeleton Preset = EFluidNinjaSwimmerSkeleton::Manny;

	/** The head: the top sphere pushing the water aside. A mesh without this bone floats on its chest and hips. Default head. (The
	 * registered swimmers' wakes judge "at the surface" by a bone named head, whatever this says.) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skeleton")
	FName HeadBone = TEXT("head");

	/** The head sphere's radius: the water it pushes aside in FluidNinja Waves and its share of the dive's mass under the surface. Bigger
	 * pushes more water. In cm. Default 11; at least 1. Manny 11, the quadruped 12. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skeleton", meta = (ClampMin = "1", Units = "cm"))
	float HeadRadiusCm = 11.f;

	/** The chest: the middle sphere, and the part whose going under faster than Dive Min Cm S is the dive (On Entry). A stroke's wake
	 * point and push are placed above it. Without it there is no dive event. Default spine_03. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skeleton")
	FName ChestBone = TEXT("spine_03");

	/** The chest sphere's radius: the water it pushes aside, its share of the dive's mass, and the depth a stroke still reaches the surface
	 * from (a stroke's push fades as exp(-depth of the chest's top / this radius)). In cm. Default 16; at least 1. Manny 16, the
	 * quadruped 22. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skeleton", meta = (ClampMin = "1", Units = "cm"))
	float ChestRadiusCm = 16.f;

	/** The hips: the bottom sphere and the body's reference point. The swimmer's water is sampled there (Water From Query), and a stroke
	 * counts only while the hips are no higher than their radius above the still water. A mesh must have this bone to fit the profile.
	 * Default pelvis. (The registered swimmers' wakes read a bone named pelvis, whatever this says.) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skeleton")
	FName HipsBone = TEXT("pelvis");

	/** The hips sphere's radius: the water they push aside, their share of the dive's mass, and the height above the still water up to
	 * which a stroke counts. In cm. Default 15; at least 1. Manny 15, the quadruped 20. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skeleton", meta = (ClampMin = "1", Units = "cm"))
	float HipsRadiusCm = 15.f;

	/** The parts that meet the water: hands, forearms, feet, head, and (whole body only) chest, hips and thighs or calves. A part whose bone
	 * the mesh lacks is left out; the mesh must have at least one of them to fit the profile. The order is kept: the contacts' state
	 * follows it frame to frame. Manny has 11 (4 whole body only), the quadruped 9. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Parts")
	TArray<FFluidNinjaSwimmerPartSpec> Parts;

	/** A part going in (straight down) slower than this makes no splash and no ring; coming out it needs this times Exit Speed Factor.
	 * Lower lets gentle strokes splash; higher keeps only hard entries. In cm/s. Below 0 (the default, -1): the project's Swimmer Entry
	 * Min Cm S (default 60). Used as at least 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thresholds", meta = (Units = "cm/s"))
	float EntryMinCmS = -1.f;

	/** A part coming out of the water faster than this times the entry threshold makes its small exit splash (0.4 of an entry's) and draws
	 * the water up with it. Higher keeps exits quieter. Default 1.3 (the plugin's rule); at least 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thresholds", meta = (ClampMin = "1"))
	double ExitSpeedFactor = 1.3;

	/** The chest going under faster than this (straight down) is a dive: On Entry fires once, with the speed and the mass under the
	 * surface, for the entry package (cavity, crown, bubbles, sound), and the plugin throws the dive's entry splash. Lower fires it for
	 * slower entries (a step off the edge); higher only for real dives. In cm/s. Default 150; at least 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thresholds", meta = (ClampMin = "0", Units = "cm/s"))
	float DiveMinCmS = 150.f;

	/** Scales this swimmer's part splashes (entries and exits), on top of the project's Swimmer Splash Scale. 0: no part splashes. The
	 * dive's entry splash uses only the project's Swimmer Splash Scale. Default 1; at least 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scales", meta = (ClampMin = "0"))
	float SplashScale = 1.f;

	/** Scales this swimmer's part impulses into the waves (entries, exits and kicks), on top of the project's Swimmer Impulse Scale, and
	 * the vertical push of its strokes, on top of Swimmer Stroke Scale. 0: no rings from it. The rings need FluidNinja Waves with Wave
	 * Impulse on. Default 1; at least 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scales", meta = (ClampMin = "0"))
	float ImpulseScale = 1.f;

	/** Scales the entry package's sound: carried on the FluidNinja Swimmer component's On Entry for the project's sound to read; the plugin
	 * plays none itself. Default 1; at least 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scales", meta = (ClampMin = "0"))
	float SoundScale = 1.f;

	/** The water a stroke sweeps along, kg: a stroke impulse (FFluidNinjaSwimmerPose::StrokeImpulse, kg·cm/s) moves this much water, so the
	 * wake point it writes runs at the impulse over this (times the project's Swimmer Stroke Scale). More water makes the same stroke a
	 * slower point, and one under FluidNinja's cut-off (about 40 cm/s) is not written. A hand and forearm sweep about 2 kg. Default 2; at
	 * least 0.1. Manny 2, the quadruped 3. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scales", meta = (ClampMin = "0.1", Units = "kg"))
	float StrokeWaterKg = 2.f;

	/** The swimmer's whole mass when the pose does not give one (the default pose takes it from here): it caps the dive's mass share on On
	 * Entry. In kg. Default 75; at least 1. Manny 75, the quadruped 90. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scales", meta = (ClampMin = "1", Units = "kg"))
	float MassKg = 75.f;

	/** Fills every field from Preset. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Skeleton")
	void ApplyPreset();

	/** The plugin's built-in profile for a skeleton (a class default object: never edit it, copy it into a data asset instead). */
	static const UFluidNinjaSwimmerProfile* GetPreset(EFluidNinjaSwimmerSkeleton Skeleton);

	/** The built-in profile whose head, chest and hips bones Mesh has (Manny first, then the quadruped); null when none fits. The MetaHuman
	 * profile has Manny's bones, so a MetaHuman takes Manny's (the same numbers). */
	static const UFluidNinjaSwimmerProfile* FindPresetFor(const USkeletalMeshComponent* Mesh);

	/** Profile when it is set, else the preset fitting Mesh. */
	static const UFluidNinjaSwimmerProfile* Resolve(const UFluidNinjaSwimmerProfile* Profile, const USkeletalMeshComponent* Mesh);

	/** The mesh has the profile's hips bone and at least one of its parts. */
	bool FitsMesh(const USkeletalMeshComponent* Mesh) const;

	/** Fills Profile's fields from Skeleton (the presets' numbers; Manny's are FluidNinjaBuoyancySwimmers.cpp's original table). */
	static void FillPreset(EFluidNinjaSwimmerSkeleton Skeleton, UFluidNinjaSwimmerProfile& Profile);
};

/** The built-in Manny / Quinn profile (UE5's SK_Mannequin bones). GetDefault<UFluidNinjaSwimmerProfileManny>(). */
UCLASS(NotBlueprintable)
class FLUIDNINJABUOYANCY_API UFluidNinjaSwimmerProfileManny : public UFluidNinjaSwimmerProfile
{
	GENERATED_BODY()

public:
	UFluidNinjaSwimmerProfileManny();
};

/** The built-in MetaHuman profile: the MetaHuman body skeleton has Manny's bone names (pelvis, spine_01..05, neck_01..02, head, clavicle,
 * upperarm, lowerarm, hand, thigh, calf, foot, ball; read from the engine's metahuman_base_skel, UE 5.8), so it is Manny's profile under
 * its own name. TODO: check the chest bone's height on a tall MetaHuman in the editor; spine_04 may sit nearer the chest than spine_03. */
UCLASS(NotBlueprintable)
class FLUIDNINJABUOYANCY_API UFluidNinjaSwimmerProfileMetaHuman : public UFluidNinjaSwimmerProfile
{
	GENERATED_BODY()

public:
	UFluidNinjaSwimmerProfileMetaHuman();
};

/** The built-in quadruped example: pelvis, spine_01..03 (the chest is spine_03), neck_01, head, and the four feet (foot_l / foot_r for
 * the hind feet, hand_l / hand_r for the fore feet, as many animal rigs name them). Rename the bones for the creature's rig. */
UCLASS(NotBlueprintable)
class FLUIDNINJABUOYANCY_API UFluidNinjaSwimmerProfileQuadruped : public UFluidNinjaSwimmerProfile
{
	GENERATED_BODY()

public:
	UFluidNinjaSwimmerProfileQuadruped();
};
