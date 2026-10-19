#pragma once

#include "CoreMinimal.h"
#include "FluidNinjaBuoyancySwimmerProfile.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UObject/Interface.h"
#include "FluidNinjaBuoyancySwimmerProvider.generated.h"

class APawn;
class USkeletalMeshComponent;

/** One body part of a swimmer this frame: where it is, and what the profile says about it (FFluidNinjaSwimmerPartSpec). */
USTRUCT(BlueprintType)
struct FLUIDNINJABUOYANCY_API FFluidNinjaSwimmerPart
{
	GENERATED_BODY()

	/** The part's bone. The contacts keep each part's state by its index in the list, and start over when the bones in the list change. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	FName Bone;

	/** What the part is (hand, foot, head...). Reserved: the plugin does not read it (its contacts go by Bone, Kick and Whole Body Only);
	 * it is carried for a project's own swim code. Default Other. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	EFluidNinjaSwimmerPartKind Kind = EFluidNinjaSwimmerPartKind::Other;

	/** Where the part is, world cm (the bone's location in the default pose). It is in the water while below the surface; a move of more
	 * than 200 cm, or faster than 2000 cm/s, in a frame counts as a teleport and makes no contact. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	FVector Location = FVector::ZeroVector;

	/** The part as a sphere: its entry splash power is added mass × speed³ / (2 × radius), so smaller makes a bigger splash. In cm.
	 * Default 6. See FFluidNinjaSwimmerPartSpec.
	  * More than 0 (the built-in Manny profile uses 5-6 cm). */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer", meta = (Units = "cm"))
	float RadiusCm = 6.f;

	/** The water moving with the part going in: its ring impulse is this times its entry speed; 0 makes no splash or ring. In kg.
	 * Default 0.45.
	  * 0 or more (the built-in Manny profile uses 0.45-0.6 kg). */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer", meta = (Units = "kg"))
	float AddedMassKg = 0.45f;

	/** Its footprint seen from above: the area its ring impulse spreads over. In cm². Default 150.
	 * More than 0 (the built-in Manny profile uses 150-200 cm2); a small area concentrates the impulse into a narrow, tall ring. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	float AreaCm2 = 150.f;

	/** It churns the water moving up and down just under the surface (within Swimmer Kick Depth Cm). Default off. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	bool bKick = false;

	/** Left out unless the swimmer was registered with Whole Body (always left out for the player's pawn). Default off. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	bool bWholeBodyOnly = false;

	/** The part's density, kg/m³. Reserved: the plugin does not read it (the dive's mass share uses 985 kg/m³ for the whole body); it is
	 * carried for a project's own body physics. Default 985. Any positive value. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	float DensityKgM3 = 985.f;
};

/**
 * A swimmer as FluidNinja Buoyancy takes it each frame, from whatever moves it (IFluidNinjaSwimmerProvider): the body's place and
 * velocity, whether it swims, wades or is in the air over the water, its head, chest and hips as spheres (they push the water aside and
 * the live water is read at the hips), its parts (they splash and press rings where they go in), and what its locomotion knows: how much
 * of it is under, the stroke's phase and the stroke's push on the water. Every position is world cm, every velocity cm/s.
 */
USTRUCT(BlueprintType)
struct FLUIDNINJABUOYANCY_API FFluidNinjaSwimmerPose
{
	GENERATED_BODY()

	/** The pose was filled. A provider that returns false or leaves this false leaves the swimmer alone this frame (out of the water as far
	 * as the waves know; its parts start over when it is back). */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	bool bValid = false;

	/** The body's skeletal mesh (its bones gave the parts; the registration and the stats are keyed by it). Left empty by a registered
	 * swimmer's provider: the registered mesh is used; the player's pawn's pose needs it set. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	TObjectPtr<USkeletalMeshComponent> Mesh = nullptr;

	/** The body's reference point, world cm: the capsule's centre for a pawn, the hips for a mesh moved by its animation. Used where the pose
	 * has no hips sphere: the water is then sampled here and the strokes are judged from here. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	FVector Location = FVector::ZeroVector;

	/** How the body moves through the world, cm/s (the default pose: the pawn's velocity; zero for a mesh on its own). A registered
	 * swimmer's wake runs on it: the wake's speed through the water and, going down, its plunge. Zero (or faster than 2000 cm/s, a
	 * teleport): the mesh's own motion frame to frame stands in. The unregistered player's wake takes its pawn's velocity directly. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	FVector Velocity = FVector::ZeroVector;

	/** Where it faces, unit, horizontal (zero: unknown). Reserved: the plugin does not read it (a wake goes along the swimmer's motion
	 * through the water, not its facing); it is carried for a project's code. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	FVector Facing = FVector::ZeroVector;

	/** Its capsule, when it has one: half the height with the caps (0: none; the spheres stand for the body). In cm. Reserved: the
	 * plugin's swimmer path does not read it (a registered swimmer's top is its head sphere; FluidNinja Waves reads the player's capsule
	 * itself); it is carried for a project's code. 0 or more. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer", meta = (Units = "cm"))
	float HalfHeightCm = 0.f;

	/** Its capsule's radius (0: none). In cm. Reserved: the plugin's swimmer path does not read it (the spheres stand for the body); it is
	 * carried for a project's code. 0 or more. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer", meta = (Units = "cm"))
	float RadiusCm = 0.f;

	/** Its locomotion swims (Character Movement's swimming, Mover's IsSwimming, a swim state of its own). The player's pawn's pose is
	 * followed only while this or Airborne is true (and a water volume is under it); a registered swimmer's is not read. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	bool bSwimming = false;

	/** Its locomotion is on foot in water. Reserved: the plugin does not read it (the player's wading is worked out from the water at its
	 * capsule's feet, 20 cm deep or more; a registered swimmer's wake is judged from its hips sphere); it is carried for a project's code. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	bool bWading = false;

	/** In the air (falling, jumping in). The player's pawn's pose is followed while this or Swimming is true, so its parts are followed into
	 * the water; a registered swimmer's is not read. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	bool bAirborne = false;

	/** The three spheres, head, chest and hips, are filled (at least one of their bones was found). False: no displacement spheres and no
	 * dive, and the water is sampled at Location. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	bool bHasSpheres = false;

	/** The head sphere's centre, world cm. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	FVector Head = FVector::ZeroVector;

	/** The head sphere's radius: the water it pushes aside. In cm. 0: no head sphere.
	 * 0 or more. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer", meta = (Units = "cm"))
	float HeadRadiusCm = 0.f;

	/** The chest sphere's centre, world cm: the dive is its going under, and a stroke's point is placed above it. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	FVector Chest = FVector::ZeroVector;

	/** The chest sphere's radius: the water it pushes aside, and the depth a stroke's push fades over. In cm. 0: no chest sphere, so no
	 * dive.
	  * 0 or more. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer", meta = (Units = "cm"))
	float ChestRadiusCm = 0.f;

	/** The hips sphere's centre, world cm: the swimmer's water is sampled here. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	FVector Hips = FVector::ZeroVector;

	/** The hips sphere's radius: the water they push aside, and how high above the still water a stroke still counts. In cm. 0: no hips
	 * sphere (the water is then sampled at Location).
	  * 0 or more. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer", meta = (Units = "cm"))
	float HipsRadiusCm = 0.f;

	/** The parts that meet the water, in the profile's order (the same order every frame: their contacts keep state by index). */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	TArray<FFluidNinjaSwimmerPart> Parts;

	/** How much of the body is under the water, 0 to 1 (below 0, the default -1: unknown). Reserved: the plugin does not read it (it works
	 * the body under the water out from the spheres); it is carried for a project's code. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	float SubmergedFraction = -1.f;

	/** The stroke cycle's phase, 0 to 1, when the locomotion knows it (below 0, the default -1: unknown; the FluidNinja Swimmer component's
	 * Set Stroke Phase). Reserved: the plugin does not read it (a stroke's push comes from Stroke Impulse); it is carried on the pose for a
	 * project that times effects to the stroke. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	float StrokePhase = -1.f;

	/** The push the stroke gives the water this frame, kg·cm/s, world (the momentum the arms and legs hand the water: the body's thrust
	 * reversed; from the animation's hand speed, the locomotion's acceleration, or a curve). Its horizontal part becomes a wake point
	 * above the chest (FluidNinja's point input: the impulse over the profile's Stroke Water Kg, written only with Swimmer Wakes on and
	 * at Swimmer Wake Min Speed and fnb.Wake.MinWriteCmS, default 40 cm/s, or more), its vertical part a small push on the waves, both scaled by Swimmer Stroke Scale and fading with the
	 * chest's depth. It counts only while the hips are no higher than their radius above the still water. Zero: no stroke this frame. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	FVector StrokeImpulse = FVector::ZeroVector;

	/** The body's mass: it caps the dive's mass share. In kg. 0: the profile's Mass Kg (the default pose fills it from the profile).
	 * 0 or more. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer", meta = (Units = "kg"))
	float MassKg = 0.f;

	/** The provider knows the still water at the swimmer (Water Z holds it). For a registered swimmer not on the water query, Water Z then
	 * stands for its water (its parts, dive, spheres, strokes and wake) in place of its registration's Water Z; on the water query the
	 * sample at its hips wins. The FluidNinja Swimmer component sets it with Water From Query off (its Still Water Z Cm). The unregistered
	 * player's pawn does not read it (its water is the surface under it). */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer")
	bool bHasWaterZ = false;

	/** The still water's height at the swimmer, world cm, read when Has Water Z is set (see Has Water Z). Any world height. */
	UPROPERTY(BlueprintReadWrite, Category = "Swimmer", meta = (Units = "cm"))
	float WaterZ = 0.f;
};

/** A swimmer's own wake tuning in place of the project's Swimmers settings (a project that publishes them per swimmer or per level),
 * returned by IFluidNinjaSwimmerProvider::GetSwimmerWakeTuning (the FluidNinja Swimmer component's Wake Tuning). With bOverride on, a
 * registered swimmer's wake (WriteSwimmerWakes) and its strokes' wake points (FFluidNinjaBuoyancySwimmers::TickStroke) run on these
 * values; off, on the project's. The unregistered player's pawn always runs on the project's. */
USTRUCT(BlueprintType)
struct FLUIDNINJABUOYANCY_API FFluidNinjaSwimmerWakeTuning
{
	GENERATED_BODY()

	/** Use the values below for this swimmer's wake and strokes in place of the project's Swimmers settings (Swimmer Wake Strength, Gain,
	 * Size Cm, Min Speed Cm S, Current Ring Inner / Outer Cm). Turn it on to give one swimmer (a big creature, a child) a wake of its own.
	 * Off (the default): the project's values. Needs Swimmer Wakes on, and a registered swimmer (the FluidNinja Swimmer component, or
	 * Register Swimmer with a provider); the unregistered player's pawn always uses the project's. Values a Blueprint sets outside the
	 * ranges below are held to them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake")
	bool bOverride = false;

	/** Stands for Swimmer Wake Strength: the wake's target is the current plus this share of the swimmer's speed through the water.
	 * Higher pushes the water harder. Range 0-1. Default 0.35, as the project's. Used only with Override on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (ClampMin = "0", ClampMax = "1"))
	float Strength = 0.35f;

	/** Stands for Swimmer Wake Gain: the share of what the water at the swimmer still lacks of the target that the wake writes each frame.
	 * FluidNinja ignores points under about 40 cm/s. Measured in the demo (as the project setting): at 0.2 a 3 m/s swimmer leaves no wake
	 * in still water; at 0.6 still water shows one, but a river speeds up around a swimmer that only holds its place. Range 0-1. Default
	 * 0.2, as the project's. Used only with Override on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (ClampMin = "0", ClampMax = "1"))
	float Gain = 0.2f;

	/** Stands for Swimmer Wake Size Cm: the brush size of the swimmer's wake point (it grows a little with speed; a wading foot's is two
	 * thirds of it, a plunge ring's 1.2 times, a stroke's point the same as swimming). Larger spreads the push over more water. In cm.
	 * Default 70, as the project's; at least 1. Used only with Override on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (ClampMin = "1", Units = "cm"))
	float SizeCm = 70.f;

	/** Stands for Swimmer Wake Min Speed Cm S: a swimmer moving slower than this through the water leaves no wake, and a stroke whose push
	 * is slower writes no point (fnb.Wake.MinWriteCmS, default 40, is a second floor). In cm/s. Default 15, as the project's; at least 0.
	 * Used only with Override on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (ClampMin = "0", Units = "cm/s"))
	float MinSpeedCmS = 15.f;

	/** Stands for Swimmer Current Ring Inner Cm: the current a swimmer's wake pushes against is read on a ring around it, not under it
	 * where its own wake is, from this far... Raise it for a big swimmer whose wake reaches further. In cm. Default 70, as the project's;
	 * at least 0 (0 reads the whole disc). Used only with Override on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (ClampMin = "0", Units = "cm"))
	float RingInnerCm = 70.f;

	/** ...to this far (Swimmer Current Ring Outer Cm). Wider averages the current over more water. In cm. Default 160, as the project's;
	 * at least 10, and at least 10 cm past the inner radius (a smaller value is moved out to that). Used only with Override on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (ClampMin = "10", Units = "cm"))
	float RingOuterCm = 160.f;
};

UINTERFACE(BlueprintType, Blueprintable)
class FLUIDNINJABUOYANCY_API UFluidNinjaSwimmerProvider : public UInterface
{
	GENERATED_BODY()
};

/**
 * What moves a swimmer tells FluidNinja Buoyancy how the body stands each frame. Implement it on the pawn, on a component of it (the
 * plugin's UFluidNinjaSwimmerComponent is the default: a skeletal mesh, a profile and, on a Character, its Character Movement), or in
 * Blueprint. The subsystem asks the player's pawn for one every frame (the pawn itself, then its components), and a registered swimmer
 * for the one it was registered with; without one, the default pose is built from the mesh and the profile fitting it
 * (UFluidNinjaSwimmerProviderLibrary::BuildDefaultPose). A project on another movement system (Mover, say) implements GetSwimmerPose
 * once and the plugin's swimmer path (contacts, displacement, wakes, the live water) runs on it unchanged.
 */
class FLUIDNINJABUOYANCY_API IFluidNinjaSwimmerProvider
{
	GENERATED_BODY()

public:
	/** The body this frame. False: not a swimmer now (the plugin leaves it alone this frame). */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "FluidNinja|Swimmer")
	bool GetSwimmerPose(FFluidNinjaSwimmerPose& OutPose);
	virtual bool GetSwimmerPose_Implementation(FFluidNinjaSwimmerPose& OutPose) { return false; }

	/** The swimmer's own wake tuning: with bOverride on, its wake and its strokes' wake points use these values in place of the project's
	 * Swimmers settings (FFluidNinjaBuoyancySwimmers::GetWakeTuning reads it each frame for a registered swimmer). The default returns
	 * bOverride off: the project's. */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "FluidNinja|Swimmer")
	FFluidNinjaSwimmerWakeTuning GetSwimmerWakeTuning();
	virtual FFluidNinjaSwimmerWakeTuning GetSwimmerWakeTuning_Implementation() { return FFluidNinjaSwimmerWakeTuning(); }

	/** The swimmer's profile (null: the preset fitting its mesh). */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "FluidNinja|Swimmer")
	UFluidNinjaSwimmerProfile* GetSwimmerProfile();
	virtual UFluidNinjaSwimmerProfile* GetSwimmerProfile_Implementation() { return nullptr; }
};

/** Finding providers and building the default pose, for the subsystem and for projects. */
UCLASS()
class FLUIDNINJABUOYANCY_API UFluidNinjaSwimmerProviderLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** The object implementing IFluidNinjaSwimmerProvider on Actor: the actor itself, else its first component that does; null for none. */
	UFUNCTION(BlueprintPure, Category = "FluidNinja|Swimmer")
	static UObject* FindSwimmerProvider(AActor* Actor);

	/** Actor's body mesh for swimming: the first skeletal mesh (with a mesh asset) that has Profile's hips bone, or, without a profile, the
	 * first with a hand_r bone (the plugin's rule for the player's pawn), else the first skeletal mesh with a mesh asset. */
	UFUNCTION(BlueprintPure, Category = "FluidNinja|Swimmer")
	static USkeletalMeshComponent* FindSwimmerMesh(AActor* Actor, const UFluidNinjaSwimmerProfile* Profile);

	/**
	 * The default pose: Mesh's bones through Profile (null: the preset fitting the mesh) for the spheres and the parts; Pawn (optional)
	 * for the body: its capsule's centre and size, its velocity, its Character Movement's swimming and falling (any pawn without one is
	 * neither, its mesh's motion gives its velocity). False when no profile fits the mesh.
	 */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Swimmer")
	static bool BuildDefaultPose(USkeletalMeshComponent* Mesh, const UFluidNinjaSwimmerProfile* Profile, APawn* Pawn, FFluidNinjaSwimmerPose& OutPose);

	/**
	 * The pose of Actor this frame: its provider's (FindSwimmerProvider), else the default from its mesh (InOutMesh: the mesh found last
	 * time, looked for again when it is gone or belongs to another actor) and the profile fitting it. False when it is no swimmer.
	 */
	static bool GetPoseFor(AActor* Actor, TWeakObjectPtr<USkeletalMeshComponent>& InOutMesh, FFluidNinjaSwimmerPose& OutPose);

	/** The pose's parts for a swimmer that is (or is not) a whole-body one: the whole-body-only parts dropped unless bWholeBody. */
	static void FilterParts(FFluidNinjaSwimmerPose& Pose, bool bWholeBody);
};
