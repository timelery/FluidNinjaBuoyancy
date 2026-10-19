#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FluidNinjaBuoyancySwimmerProvider.h"
#include "FluidNinjaBuoyancySubsystem.h"
#include "FluidNinjaBuoyancyWaterQuery.h"
#include "FluidNinjaBuoyancySwimmerComponent.generated.h"

class USkeletalMeshComponent;

/** The owner dived (its chest went under at the profile's Dive Min): where (on the surface), how fast (straight down) and the mass under
 * the surface at that moment. Bind the entry package to it: cavity, crown, bubbles, entry sound. Sound Scale is the profile's. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FFluidNinjaSwimmerEntrySignature, FVector, Location, float, SpeedCmS, float, MassShareKg, float, SoundScale);

/**
 * Makes the owner a FluidNinja Buoyancy swimmer (Add Component → FluidNinja Swimmer): its mesh is registered at Begin Play, so its body
 * parts splash and press rings, its head, chest and hips push the water aside, it writes its wake, and its water (surface with waves,
 * current) is sampled at its hips every frame — one source of truth its swim locomotion reads back with GetWater / GetDriftCurrent.
 *
 * The component is the plugin's default IFluidNinjaSwimmerProvider: the pose comes from the mesh's bones through Profile (none: the
 * preset fitting the mesh) and, on a Character, its Character Movement (swimming, falling). A pawn on another movement system (Mover,
 * say) overrides IsSwimmingNow / IsAirborneNow in Blueprint or a subclass, or implements IFluidNinjaSwimmerProvider on the pawn itself,
 * which then wins over this component's default pose. The animation feeds strokes in through AddStrokeImpulse and SetStrokePhase.
 */
UCLASS(ClassGroup = "FluidNinja", meta = (BlueprintSpawnableComponent, DisplayName = "FluidNinja Swimmer"))
class FLUIDNINJABUOYANCY_API UFluidNinjaSwimmerComponent : public UActorComponent, public IFluidNinjaSwimmerProvider
{
	GENERATED_BODY()

public:
	UFluidNinjaSwimmerComponent();

	/** The skeleton: bones, sphere radii, added masses, thresholds, scales (a FluidNinja Swimmer Profile data asset). None: the built-in
	 * preset whose bones the mesh has (Manny / MetaHuman for a mesh with lowerarm_r, else the quadruped); when none fits, the owner is no
	 * swimmer. It also picks the mesh: the owner's first skeletal mesh with the profile's hips bone (None: the first with hand_r). Set it
	 * before Begin Play: the registration keeps the profile it was given. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Swimmer")
	TObjectPtr<UFluidNinjaSwimmerProfile> Profile;

	/** The head, chest and hips spheres push the water aside in FluidNinja Waves, so the body's bob makes waves. Off: no displacement
	 * waves from this swimmer (Displaced Litres in its stats reads 0). Needs FluidNinja Waves (the modified FluidNinja core). Takes effect
	 * at once, also while playing (Blueprint writes go through Set Displacement). Default on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, BlueprintSetter = SetDisplacement, Category = "Swimmer")
	bool bDisplacement = true;

	/** The profile's parts (hands, forearms, feet, head) splash where they go into the water faster than the entry threshold, press rings
	 * into the waves and make a small splash coming out; kicking feet churn the water just under the surface. Off: none of that (the dive
	 * event still fires). Needs Project Settings → FluidNinja Buoyancy → Swimmer Contacts; the rings need FluidNinja Waves with Wave
	 * Impulse on. Takes effect at once, also while playing (Blueprint writes go through Set Part Contacts). Default on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, BlueprintSetter = SetPartContacts, Category = "Swimmer")
	bool bPartContacts = true;

	/** The profile's whole-body-only parts make contacts too (Manny: the chest, hips and calves): for a diver going in. Off: those parts
	 * are left out, as for the player's pawn, whose chest and hips ride its capsule. Takes effect at once, also while playing (Blueprint
	 * writes go through Set Whole Body; switching it starts the parts' contacts over). Default off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, BlueprintSetter = SetWholeBody, Category = "Swimmer")
	bool bWholeBody = false;

	/** The swimmer's water is sampled at its hips every frame (the still water, the waves and the current: Get Water At Location), and its
	 * contacts, dive, displacement spheres, strokes and wakes are judged against that one sample; Get Water returns it with the filtered
	 * drift current. Off: Still Water Z Cm stands for the water, as a plain Register Swimmer's Water Z does, and Get Water returns false.
	 * Takes effect at once, also while playing (Blueprint writes go through Set Water From Query). Default on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, BlueprintSetter = SetWaterFromQuery, Category = "Swimmer")
	bool bWaterFromQuery = true;

	/** The still water's height while Water From Query is off: the surface the parts, the dive, the spheres, the strokes and the wake are
	 * judged against (set it to the pool's water line; a swimmer whose water rises or falls updates it, or turns Water From Query on).
	 * World Z, in cm. Default 0. Unused while Water From Query is on (where the query finds no water the swimmer has none, not this
	 * height). Takes effect at once, also while playing (Blueprint writes go through Set Still Water Z Cm). Any world height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, BlueprintSetter = SetStillWaterZCm, Category = "Swimmer", meta = (Units = "cm"))
	float StillWaterZCm = 0.f;

	/** This swimmer's own wake tuning: with Override on, its wake and its strokes' wake points use these values (strength, gain, brush
	 * size, minimum speed, the ring its current is read on) in place of the project's Swimmers settings; off (the default), the project's.
	 * Read every frame, so changes take effect at once. Needs Swimmer Wakes on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Swimmer")
	FFluidNinjaSwimmerWakeTuning WakeTuning;

	/** Fires once when the owner dives: its chest goes under faster (straight down) than the profile's Dive Min Cm S. Gives the point on
	 * the surface above the chest, the fall speed (cm/s), the mass under the surface then (kg: the spheres' volume below the water at the
	 * body's density, at most the body's mass) and the profile's Sound Scale. Bind the entry package to it (cavity, crown, bubbles,
	 * sound); the plugin itself adds only the entry splash. */
	UPROPERTY(BlueprintAssignable, Category = "Swimmer")
	FFluidNinjaSwimmerEntrySignature OnEntry;

	/** The mesh registered as the swimmer (found at Begin Play: the first skeletal mesh fitting the profile); SetMesh re-registers. */
	UFUNCTION(BlueprintPure, Category = "Swimmer")
	USkeletalMeshComponent* GetMesh() const { return Mesh.Get(); }

	UFUNCTION(BlueprintCallable, Category = "Swimmer")
	void SetMesh(USkeletalMeshComponent* InMesh);

	/** Sets Displacement and updates the registration at once. */
	UFUNCTION(BlueprintSetter, Category = "Swimmer")
	void SetDisplacement(bool bInDisplacement);

	/** Sets Part Contacts and updates the registration at once. */
	UFUNCTION(BlueprintSetter, Category = "Swimmer")
	void SetPartContacts(bool bInPartContacts);

	/** Sets Whole Body and updates the registration at once (the parts' contacts start over). */
	UFUNCTION(BlueprintSetter, Category = "Swimmer")
	void SetWholeBody(bool bInWholeBody);

	/** Sets Water From Query and updates the registration at once. */
	UFUNCTION(BlueprintSetter, Category = "Swimmer")
	void SetWaterFromQuery(bool bInWaterFromQuery);

	/** Sets Still Water Z Cm and updates the registration at once. */
	UFUNCTION(BlueprintSetter, Category = "Swimmer")
	void SetStillWaterZCm(float InStillWaterZCm);

	/** The swimmer's water as last sampled (Water From Query) and its filtered drift current (low-passed 0.5 s, dead-banded 10 cm/s): what
	 * the swim mode floats on and drifts with. False before the first sample. */
	UFUNCTION(BlueprintCallable, Category = "Swimmer")
	bool GetWater(FFluidNinjaWaterSample& OutSample, FVector& OutDriftCurrent) const;

	/** The filtered drift current alone (zero before any). */
	UFUNCTION(BlueprintPure, Category = "Swimmer")
	FVector GetDriftCurrent() const;

	/** What the swimmer's body parts did to the water. */
	UFUNCTION(BlueprintCallable, Category = "Swimmer")
	bool GetSwimmerStats(FFluidNinjaSwimmerStats& OutStats) const;

	/** The push a stroke gives the water this frame, kg·cm/s, world (the thrust the arms and legs hand the water: the body's push
	 * reversed). Call it from the animation or the locomotion as the stroke pulls; several calls in a frame add up. Its horizontal part
	 * becomes a wake point, its vertical part a push on the waves (Swimmer Stroke Scale, the profile's Stroke Water Kg). */
	UFUNCTION(BlueprintCallable, Category = "Swimmer")
	void AddStrokeImpulse(FVector ImpulseKgCmS);

	/** The stroke cycle's phase, 0 to 1, when the animation knows it (carried on the pose). */
	UFUNCTION(BlueprintCallable, Category = "Swimmer")
	void SetStrokePhase(float Phase) { StrokePhase = FMath::Clamp(Phase, 0.f, 1.f); }

	/** The owner's locomotion swims now. The default reads a Character Movement component; a Mover pawn overrides this (or SwimmingNow in
	 * Blueprint via a subclass). */
	virtual bool IsSwimmingNow() const;

	/** The owner is in the air (jumping in). The default reads a Character Movement component. */
	virtual bool IsAirborneNow() const;

	// IFluidNinjaSwimmerProvider
	virtual bool GetSwimmerPose_Implementation(FFluidNinjaSwimmerPose& OutPose) override;
	virtual FFluidNinjaSwimmerWakeTuning GetSwimmerWakeTuning_Implementation() override { return WakeTuning; }
	virtual UFluidNinjaSwimmerProfile* GetSwimmerProfile_Implementation() override { return Profile; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

#if WITH_EDITOR
	/** A change in the Details panel while playing updates the registration (Set Displacement and the others' rule). */
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** Registers Mesh with the world's swimmers (this component as its provider), and binds the dive event. */
	void RegisterWith(USkeletalMeshComponent* InMesh);
	/** The registered mesh's registration again, with the current Displacement, Part Contacts, Whole Body, Water From Query, Still Water
	 * Z Cm and Profile (its state kept); nothing before Begin Play. */
	void RefreshRegistration();
	void UnregisterNow();
	void HandleEntry(const USkeletalMeshComponent* EntryMesh, const FVector& Where, float SpeedCmS, float MassShareKg);

	TWeakObjectPtr<USkeletalMeshComponent> Mesh;
	/** The strokes' push since the pose was last taken (cleared when the pose consumes it). */
	FVector PendingStroke = FVector::ZeroVector;
	float StrokePhase = -1.f;
	FDelegateHandle EntryHandle;
};
