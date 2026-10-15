#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FluidNinjaBuoyancyComponent.generated.h"

class UAudioComponent;
class UNiagaraSystem;
class UPrimitiveComponent;
class USoundAttenuation;
class USoundBase;
namespace FluidNinjaBuoyancy { struct FHull; struct FParams; }

/** A piece of the hull's water line, as the physics thread last saw it (world). */
struct FFluidNinjaWaterlineContact
{
	FVector Position = FVector::ZeroVector;
	FVector HullVelocity = FVector::ZeroVector;
	FVector Outward = FVector::ZeroVector;
	/** How fast the hull drives into the water there (cm/s, relative to the water); negative where it pulls away. */
	float Push = 0.f;
	/** How fast the hull sinks into the water there (cm/s, 0 when it rises): the water it displaces runs outward from the water line
	 * at about this speed (a dropped or dunked body makes a ring). */
	float Plunge = 0.f;
};

/** How a floating body stirs the FluidNinja simulation. */
UENUM(BlueprintType)
enum class EFluidNinjaBuoyancyInteraction : uint8
{
	/** The plugin's wakes: water-line points in FluidNinja's data channel, written only where the hull moves through the water. */
	Wakes,
	/** FluidNinja's own tracking of the body's pivot, relative to the water: the body generates overlap events, carries the
	 * NinjaLive's tracking tag and writes no wakes. This needs the modified FluidNinja: every NinjaLive turning the Relative Tracking
	 * Switch on and setting its Tracked Density Switch (FluidNinja's Selectively Kill Brush Density Keep Velocity) to 1, 4 or 5, so
	 * tracked bodies paint no density. Without both the body is not tracked and does not stir the water, and the log says so. */
	FluidNinjaTracking UMETA(DisplayName = "FluidNinja Tracking"),
	/** FluidNinja's tracking (same two requirements) and the plugin's wakes as well. */
	Both UMETA(DisplayName = "FluidNinja Tracking and Wakes"),
};

UENUM(BlueprintType)
enum class EFluidNinjaBuoyancyMass : uint8
{
	/** The body weighs what its hull displaces at Rest Draft (the mass is set at Begin Play). */
	RestDraft,
	/** The body keeps its own mass (Physics → Mass); it floats wherever that puts it, or sinks. */
	BodyMass,
};

/** How the body meets the water. */
UENUM(BlueprintType)
enum class EFluidNinjaHullModel : uint8
{
	/** The hull surface, cut at the water line it meets: waves along the hull, bow and stern drag, slamming. Boats, rafts. */
	Triangles,
	/** Cubes filling the hull, each wet by how deep it sits. Logs, crates, barrels, debris. */
	Voxels,
	/** Spheres (pontoons), each lifting its cap volume under the water: the cheapest model. Balls, small or numerous props. */
	Spheres,
};

/** A sphere standing in for part of the hull (body frame, cm): a pontoon. */
USTRUCT(BlueprintType)
struct FFluidNinjaPontoon
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pontoon", meta = (Units = "cm"))
	FVector Location = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pontoon", meta = (ClampMin = "1", Units = "cm"))
	float Radius = 25.f;

	/** Scales its lift. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pontoon", meta = (ClampMin = "0"))
	float LiftWeight = 1.f;

	/** Scales its drag. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pontoon", meta = (ClampMin = "0"))
	float DragWeight = 1.f;
};

/** What the water is doing to a body, as of the last physics step (read on the game thread). */
USTRUCT(BlueprintType)
struct FFluidNinjaBuoyancyReadout
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	bool bInWater = false;

	/** Litres of water the hull pushes aside (its lift ÷ ρg). */
	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	float DisplacedLitres = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	float WettedAreaM2 = 0.f;

	/** Water force on the body, newtons. */
	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	FVector ForceN = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	float MassKg = 0.f;

	/** Largest slamming power in the last frame, watts. */
	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	float SlamWatts = 0.f;

	/** How old the water the body floats on is (seconds since it was drawn); -1 on far-field water (a water volume). */
	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	float WaterAgeSeconds = -1.f;

	/** The current under the body, cm/s. */
	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	FVector2D FlowCmS = FVector2D::ZeroVector;

	/** The hull's level of detail in use (0 finest). */
	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	int32 HullLevel = 0;

	/** Settled out of sight and asleep: the water leaves it alone until it is seen, comes near or is hit. */
	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	bool bDormant = false;

	/** The wind's part of ForceN (the air on the dry hull), newtons. */
	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	FVector WindForceN = FVector::ZeroVector;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFluidNinjaBuoyancySlamSignature, float, Watts, FVector, Location);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFluidNinjaBuoyancyWaterSignature, bool, bInWater);

/**
 * Makes a physics body float on FluidNinja water as drawn: the simulation's real waves (pressure and tile displacement), its current,
 * and water volumes beyond the simulation. Every physics step each hull triangle (from the body's simple collision) is cut at the
 * water line it meets and gets pressure, drag, friction and slamming forces.
 *
 * Add it to an actor whose root (or named) primitive has simple collision; it turns physics simulation on. Guide: Docs/UserGuide.md
 * in the plugin folder. Console: fnb.Buoyancy 0/1/2, fnb.Boat.Throttle / Rudder.
 */
UCLASS(ClassGroup = (FluidNinja), meta = (BlueprintSpawnableComponent, DisplayName = "FluidNinja Buoyancy"), HideCategories = (Activation, Cooking, AssetUserData, Collision, Tags))
class FLUIDNINJABUOYANCY_API UFluidNinjaBuoyancyComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFluidNinjaBuoyancyComponent();

	/** The floating primitive; None uses the actor's root. */
	UPROPERTY(EditAnywhere, Category = "Hull")
	FName PrimitiveName;

	/** How the body meets the water: the hull surface (boats), voxels (logs, crates) or spheres (pontoons: balls, small props). */
	UPROPERTY(EditAnywhere, Category = "Hull")
	EFluidNinjaHullModel HullModel = EFluidNinjaHullModel::Triangles;

	/** The hull is rebuilt from the simple collision as an even shell of triangles about this size, so the water line is followed
	 * across big faces (coarser if Max Hull Triangles is reached). */
	UPROPERTY(EditAnywhere, Category = "Hull", meta = (ClampMin = "2", Units = "cm"))
	float HullResolutionCm = 40.f;

	UPROPERTY(EditAnywhere, Category = "Hull", meta = (ClampMin = "12"))
	int32 MaxHullTriangles = 1000;

	/** Triangles model: beyond the first distance from the camera the hull is twice as coarse, beyond the second four times. */
	UPROPERTY(EditAnywhere, Category = "Hull", meta = (EditCondition = "HullModel == EFluidNinjaHullModel::Triangles", Units = "cm"))
	FVector2D LodDistancesCm = FVector2D(3000.0, 8000.0);

	/** Voxels model: cube size (0: about Voxel Count of them fill the hull). */
	UPROPERTY(EditAnywhere, Category = "Hull", meta = (EditCondition = "HullModel == EFluidNinjaHullModel::Voxels", ClampMin = "0", Units = "cm"))
	float VoxelSizeCm = 0.f;

	UPROPERTY(EditAnywhere, Category = "Hull", meta = (EditCondition = "HullModel == EFluidNinjaHullModel::Voxels", ClampMin = "1", ClampMax = "512"))
	int32 VoxelCount = 48;

	/** Spheres model: the pontoons (body frame). Empty: two rows along the hull's bottom, lifting what the whole hull would. */
	UPROPERTY(EditAnywhere, Category = "Hull", meta = (EditCondition = "HullModel == EFluidNinjaHullModel::Spheres"))
	TArray<FFluidNinjaPontoon> Pontoons;

	/** Once settled (barely moving for two seconds), out of sight and farther than Sleep Distance from the camera, the body sleeps and
	 * the water leaves it alone until it is seen, comes near, or something hits it. */
	UPROPERTY(EditAnywhere, Category = "Hull")
	bool bSleepWhenSettled = true;

	UPROPERTY(EditAnywhere, Category = "Hull", meta = (EditCondition = "bSleepWhenSettled", ClampMin = "0", Units = "cm"))
	float SleepDistanceCm = 6000.f;

	UPROPERTY(EditAnywhere, Category = "Floating")
	EFluidNinjaBuoyancyMass MassMode = EFluidNinjaBuoyancyMass::RestDraft;

	/** How deep the hull's lowest point sits in calm water, at rest. */
	UPROPERTY(EditAnywhere, Category = "Floating", meta = (EditCondition = "MassMode == EFluidNinjaBuoyancyMass::RestDraft", ClampMin = "0.5", Units = "cm"))
	float RestDraftCm = 15.f;

	/** Held still at Begin Play until its water has been read, then set level at its rest draft on that water and released: no
	 * drop or slam at the start. */
	UPROPERTY(EditAnywhere, Category = "Floating")
	bool bSettleOnStart = true;

	/** How the body floats relative to its own frame: zero for a mesh modelled the way up it floats (a boat); a log or barrel
	 * modelled standing up floats lying down with Pitch 90. Rest Draft, the stability check and Settle On Start use it. */
	UPROPERTY(EditAnywhere, Category = "Floating")
	FRotator RestAttitude = FRotator::ZeroRotator;

	/** How fast physics may push the body out of something it starts in or is pressed into (rocks under the water): low, so a bad
	 * placement eases out instead of launching it. 0 leaves the project setting. */
	UPROPERTY(EditAnywhere, Category = "Floating", meta = (ClampMin = "0", Units = "cm/s"))
	float MaxDepenetrationSpeed = 100.f;

	/** Moves the centre of mass (body frame): lower is more stable. The hull analysis reports the metacentric height. */
	UPROPERTY(EditAnywhere, Category = "Floating", meta = (Units = "cm"))
	FVector CenterOfMassOffset = FVector::ZeroVector;

	/** Scales the body's inertia (a hollow hull turns more slowly than a solid one of the same shape). */
	UPROPERTY(EditAnywhere, Category = "Floating")
	FVector InertiaScale = FVector::OneVector;

	/** Art control on the lift. 1 is Archimedes. */
	UPROPERTY(EditAnywhere, Category = "Floating", meta = (ClampMin = "0"))
	float BuoyancyMultiplier = 1.f;

	/** Pressure drag where a face pushes into the water: linear and quadratic in speed / 1 m/s, and how it falls off with the face's
	 * angle to the flow (Kerner's Cpd1, Cpd2, fp). */
	UPROPERTY(EditAnywhere, Category = "Hydrodynamics", meta = (ClampMin = "0"))
	float PressureDragLinear = 0.3f;

	UPROPERTY(EditAnywhere, Category = "Hydrodynamics", meta = (ClampMin = "0"))
	float PressureDragQuadratic = 1.f;

	UPROPERTY(EditAnywhere, Category = "Hydrodynamics", meta = (ClampMin = "0.01"))
	float PressureFalloff = 0.5f;

	/** Suction where a face pulls away from the water (Csd1, Csd2, fs). */
	UPROPERTY(EditAnywhere, Category = "Hydrodynamics", meta = (ClampMin = "0"))
	float SuctionDragLinear = 0.3f;

	UPROPERTY(EditAnywhere, Category = "Hydrodynamics", meta = (ClampMin = "0"))
	float SuctionDragQuadratic = 0.5f;

	UPROPERTY(EditAnywhere, Category = "Hydrodynamics", meta = (ClampMin = "0.01"))
	float SuctionFalloff = 0.5f;

	/** Skin friction (ITTC 1957 line). */
	UPROPERTY(EditAnywhere, Category = "Hydrodynamics", meta = (ClampMin = "0"))
	float ViscousMultiplier = 1.f;

	/** Slamming when faces drive into the water (bow slap, landing from a drop). */
	UPROPERTY(EditAnywhere, Category = "Hydrodynamics", meta = (ClampMin = "0"))
	float SlamMultiplier = 0.5f;

	/** How much the current carries the body (0 ignores it, 1 is physical). */
	UPROPERTY(EditAnywhere, Category = "Hydrodynamics", meta = (ClampMin = "0", ClampMax = "2"))
	float FlowCoupling = 1.f;

	/** Drag on the dry part from the air and the wind. */
	UPROPERTY(EditAnywhere, Category = "Hydrodynamics", meta = (ClampMin = "0"))
	float AirDragCoefficient = 0.8f;

	/** Extra damping about the body's roll, pitch and yaw axes while wet (1/s). Zero leaves it to the water. */
	UPROPERTY(EditAnywhere, Category = "Hydrodynamics", meta = (ClampMin = "0"))
	FVector AngularDamping = FVector::ZeroVector;

	/** A propeller and rudder (body frame). Throttle and Rudder are set from Blueprint, or the console for testing
	 * (fnb.Boat.Throttle, fnb.Boat.Rudder) when Console Control is on. */
	UPROPERTY(EditAnywhere, Category = "Propulsion")
	bool bPropulsion = false;

	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion"))
	bool bConsoleControl = true;

	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion", Units = "cm"))
	FVector ThrustLocation = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion"))
	FVector ThrustDirection = FVector::ForwardVector;

	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion", ClampMin = "0"))
	float MaxThrustNewtons = 1500.f;

	/** The propeller's wash over the rudder at full throttle, so the boat steers when slow. */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion", ClampMin = "0", Units = "cm/s"))
	float PropWashSpeed = 300.f;

	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion", Units = "cm"))
	FVector RudderLocation = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion", ClampMin = "0"))
	float RudderAreaCm2 = 900.f;

	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion", ClampMin = "0", ClampMax = "60", Units = "deg"))
	float MaxRudderAngle = 35.f;

	/** -1 (full astern) to 1 (full ahead). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Propulsion", meta = (ClampMin = "-1", ClampMax = "1"))
	float Throttle = 0.f;

	/** -1 (hard left) to 1 (hard right). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Propulsion", meta = (ClampMin = "-1", ClampMax = "1"))
	float Rudder = 0.f;

	/** Pushes the water: the pieces of water line driving into it (sideways, or outward as the hull sinks in: a drop makes a ring)
	 * are written into FluidNinja's data channel as moving brush points, so the simulation makes the bow wave, the wake and foam
	 * (Project Settings → FluidNinja Buoyancy → Wake; console fnb.Buoyancy.Wake). This works without tags or overlap events. It is
	 * separate from FluidNinja's own tracking (Generate Overlap Events on the floating primitive), which paints the body's pivot
	 * point; the two can run together. */
	UPROPERTY(EditAnywhere, Category = "Wake")
	bool bWake = true;

	/** How the body stirs the simulation: FluidNinja's own tracking relative to the water (the default on this branch, which needs the
	 * modified FluidNinja: Project Settings → FluidNinja Buoyancy → FluidNinja Tracking; console fnb.Buoyancy.Tracking), or the
	 * plugin's wakes. FluidNinja's tracking as it is today paints a body's world velocity, and a floater carried by a current then
	 * speeds that current up, so a body is handed to it only when every NinjaLive says it tracks relative to the water and paints no
	 * density for tracked bodies (Tracked Density Switch 1, 4 or 5). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake")
	EFluidNinjaBuoyancyInteraction Interaction = EFluidNinjaBuoyancyInteraction::FluidNinjaTracking;

	/** Brush size of each wake point at a push of 2 m/s (scaled 0.4–2x with the push). */
	UPROPERTY(EditAnywhere, Category = "Wake", meta = (EditCondition = "bWake", ClampMin = "1", Units = "cm"))
	float WakeBrushSizeCm = 60.f;

	/** Pieces of water line pushing into the water slower than this leave no wake. */
	UPROPERTY(EditAnywhere, Category = "Wake", meta = (EditCondition = "bWake", ClampMin = "0", Units = "cm/s"))
	float WakeMinPushCmS = 20.f;

	/** Each frame a wake point gives the water this share of what it lacks: the hull's outward speed there minus the water's own.
	 * FluidNinja adds brush velocity every frame, so the water near the hull approaches the hull's speed and never overshoots it.
	 * A body that makes wakes, or that FluidNinja tracks itself (Generate Overlap Events on), takes its current from the water around
	 * it, not from the water it has just pushed. */
	UPROPERTY(EditAnywhere, Category = "Wake", meta = (EditCondition = "bWake", ClampMin = "0", ClampMax = "1"))
	float WakeGain = 0.15f;

	/** How much of the hull's speed through the water (relative to the current around it) the water at the hull takes on. A hull
	 * stirs only the water near the surface, not the whole depth FluidNinja's flat simulation stands for: 1 would make it a wall
	 * through the water (the current then races round it), 0 leaves the water alone. A body drifting with the current makes none. */
	UPROPERTY(EditAnywhere, Category = "Wake", meta = (EditCondition = "bWake", ClampMin = "0", ClampMax = "1"))
	float WakeStrength = 0.35f;

	/** Darkens and glosses the hull below the water line and where it was wet a moment ago, drying at Drying Speed. The hull's
	 * materials read it as Custom Primitive Data (Wet Line Data Index … +3: the water line plane in the mesh's local space, and how far
	 * above it the hull is still wet). Add the material function MF_FNB_HullWetness to the hull's material to show it. */
	UPROPERTY(EditAnywhere, Category = "Wet Line")
	bool bWetLine = true;

	UPROPERTY(EditAnywhere, Category = "Wet Line", meta = (EditCondition = "bWetLine", ClampMin = "0", Units = "cm/s"))
	float DryingSpeedCmS = 1.5f;

	UPROPERTY(EditAnywhere, Category = "Wet Line", meta = (EditCondition = "bWetLine", ClampMin = "0"))
	int32 WetLineDataIndex = 0;

	/** Spray where the hull slams into the water and at the bow when it drives hard into it. None: the project default (FluidNinja Buoyancy →
	 * Effects). */
	UPROPERTY(EditAnywhere, Category = "Spray")
	TSoftObjectPtr<UNiagaraSystem> SplashSystem;

	UPROPERTY(EditAnywhere, Category = "Spray", meta = (ClampMin = "0"))
	float SplashMinWatts = 300.f;

	UPROPERTY(EditAnywhere, Category = "Spray", meta = (ClampMin = "0", Units = "cm/s"))
	float BowSprayMinPushCmS = 150.f;

	UPROPERTY(EditAnywhere, Category = "Spray", meta = (ClampMin = "0"))
	float SplashScale = 1.f;

	UPROPERTY(EditAnywhere, Category = "Spray", meta = (ClampMin = "0.02", Units = "s"))
	float SplashInterval = 0.12f;

	/** Hull sounds: a slap (one-shot) when the hull slams into the water, the wash of the water along the hull (a loop), a creak
	 * (one-shot) when it is thrown about. None: the project defaults (Project Settings → FluidNinja Buoyancy → Effects; no sounds ship
	 * with the plugin). Their volume follows the water, and a float input "Pitch Shift" (semitones) is set on each, which a MetaSound
	 * can read. */
	UPROPERTY(EditAnywhere, Category = "Sound")
	TSoftObjectPtr<USoundBase> SlapSound;

	UPROPERTY(EditAnywhere, Category = "Sound")
	TSoftObjectPtr<USoundBase> WashSound;

	UPROPERTY(EditAnywhere, Category = "Sound")
	TSoftObjectPtr<USoundBase> CreakSound;

	UPROPERTY(EditAnywhere, Category = "Sound")
	TSoftObjectPtr<USoundAttenuation> Attenuation;

	UPROPERTY(EditAnywhere, Category = "Sound", meta = (ClampMin = "0"))
	float SoundVolume = 1.f;

	UPROPERTY(EditAnywhere, Category = "Sound", meta = (ClampMin = "0"))
	float SlapMinWatts = 150.f;

	/** The wash is at full volume when the water runs past the water line this fast. */
	UPROPERTY(EditAnywhere, Category = "Sound", meta = (ClampMin = "1", Units = "cm/s"))
	float WashFullSpeedCmS = 250.f;

	/** Creaks when the hull's spin changes faster than this (rad/s²), or on a hard slam. */
	UPROPERTY(EditAnywhere, Category = "Sound", meta = (ClampMin = "0"))
	float CreakMinAngularAcceleration = 2.f;

	/** Slams weaker than this (watts) raise no event. */
	UPROPERTY(EditAnywhere, Category = "Events", meta = (ClampMin = "0"))
	float SlamEventWatts = 200.f;

	UPROPERTY(BlueprintAssignable, Category = "Events")
	FFluidNinjaBuoyancySlamSignature OnSlam;

	UPROPERTY(BlueprintAssignable, Category = "Events")
	FFluidNinjaBuoyancyWaterSignature OnWaterContactChanged;

	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Buoyancy")
	FFluidNinjaBuoyancyReadout GetReadout() const { return Readout; }

	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Buoyancy")
	FString GetStatus() const;

	/** Builds the hull and logs its volume, the mass for the rest draft, and the metacentric heights (roll, pitch). */
	UFUNCTION(CallInEditor, Category = "Hull")
	void AnalyzeHull();

	// Used by UFluidNinjaBuoyancySubsystem.
	UPrimitiveComponent* GetSimulatedPrimitive() const;
	/** The hull at a level of detail (0 finest; clamped to the levels built). */
	TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe> GetHull(int32 Level = 0) const { return Hulls.IsEmpty() ? nullptr : Hulls[FMath::Clamp(Level, 0, Hulls.Num() - 1)]; }
	int32 GetHullLevels() const { return Hulls.Num(); }
	void FillParams(FluidNinjaBuoyancy::FParams& Out) const;
	void ReceiveReadout(const FFluidNinjaBuoyancyReadout& NewReadout, float SlamWatts, const FVector& SlamLocation);
	void ReceiveContacts(TArrayView<const FFluidNinjaWaterlineContact> NewContacts, float Wash, float WaterlineLength);
	const TArray<FFluidNinjaWaterlineContact>& GetContacts() const { return Contacts; }
	float GetBottomZ() const;
	bool IsAwaitingWater() const { return bAwaitingWater; }
	double GetAwaitingSince() const { return AwaitingSince; }
	/** Sets the body level at its rest draft on water at this height, and lets it go. */
	void SettleOnWater(float WaterZ);
	void ReleaseHold();

	/* Testing (the FluidNinja Buoyancy test bench). */
	/** Where the body was when play began. */
	const FTransform& GetStartTransform() const { return StartTransform; }
	/** Puts the body at Transform, still, and settles it on its water again as at the start (Settle On Start), with a fresh start
	 * for everything FluidNinja Buoyancy keeps about it. */
	void RestartAt(const FTransform& Transform);
	/** The world height of the hull's lowest point now. */
	float GetLowestPointZ() const;
	/** The draft it rests at in still water (cm), and its metacentric heights in its rest attitude (cm; negative capsizes). */
	float GetRestingDraft() const { return RestingDraft; }
	float GetRollStability() const { return RollGM; }
	float GetPitchStability() const { return PitchGM; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	bool SetUp(FString& OutReport);

	void UpdateWetLine(UPrimitiveComponent& Primitive, float DeltaTime);
	void UpdateSpray(float DeltaTime);
	void UpdateSound(UPrimitiveComponent& Primitive, float DeltaTime);

	TArray<TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe>> Hulls;
	FFluidNinjaBuoyancyReadout Readout;
	TArray<FFluidNinjaWaterlineContact> Contacts;
	float ContactWash = 0.f;
	float ContactLength = 0.f;
	float PendingSlamWatts = 0.f;
	FVector PendingSlamLocation = FVector::ZeroVector;

	// Wet line: the water line plane (local z = A + B x + C y) and how high the hull is still wet above it.
	FVector WetPlane = FVector::ZeroVector;
	float WetTop = -1e9f;
	bool bHasWetPlane = false;

	bool bAwaitingWater = false;
	double AwaitingSince = 0.0;
	float RestingDraft = 0.f;
	/** The finest hull's lowest and highest point in its rest attitude (cm, about the body's origin). */
	float RestedBottom = 0.f;
	float RestedTop = 0.f;
	float RollGM = 0.f;
	float PitchGM = 0.f;
	FTransform StartTransform = FTransform::Identity;

	double NextSplashTime = 0.0;
	double NextSlapTime = 0.0;
	double NextCreakTime = 0.0;
	FVector LastAngularVelocity = FVector::ZeroVector;
	float WashGain = 0.f;

	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> WashAudio;

	UPROPERTY(Transient)
	TObjectPtr<UNiagaraSystem> LoadedSplash;

	UPROPERTY(Transient)
	TObjectPtr<USoundBase> LoadedSlap;

	UPROPERTY(Transient)
	TObjectPtr<USoundBase> LoadedCreak;

	UPROPERTY(Transient)
	TObjectPtr<USoundAttenuation> LoadedAttenuation;
	bool bWasInWater = false;
	FString Report;
};
