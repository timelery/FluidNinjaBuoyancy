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
	/** The area of the piece of hull this contact stands for (cm²), when the physics measured one; 0 when it did not, and bow spray then
	 * assumes a hand-sized piece (UpdateSpray). */
	float AreaCm2 = 0.f;
};

/** How a floating body stirs the FluidNinja simulation (the component's Interaction; default FluidNinja Tracking). */
UENUM(BlueprintType)
enum class EFluidNinjaBuoyancyInteraction : uint8
{
	/** The plugin's wakes: brush points along the hull's water line where it pushes into the water, shaped by the hull (bow wave,
	 * side wake, a ring when dropped in). Works with stock FluidNinja. Best for hulls and props: boats, crates, barrels, anything
	 * dropped or thrown in. Needs Wake on. */
	Wakes,
	/** FluidNinja's own tracking: one brush at the body's pivot, moving at its speed relative to the water, sized by the NinjaLive's
	 * brush settings rather than the hull; no plugin wakes, so no hull-shaped impact ring. Stirs the water the way FluidNinja does
	 * for characters. Needs the modified FluidNinja: every NinjaLive with its Relative Tracking Switch on and its Tracked Density
	 * Switch (Selectively Kill Brush Density Keep Velocity) at 1, 4 or 5. Without them the body does not stir the water, and the
	 * log says so. */
	FluidNinjaTracking UMETA(DisplayName = "FluidNinja Tracking"),
	/** Both: FluidNinja's pivot brush (same requirements) and the plugin's hull wakes and impact rings. The pushes add up: lower
	 * Wake Strength or Wake Gain if the stir is too strong. Needs Wake on. */
	Both UMETA(DisplayName = "FluidNinja Tracking and Wakes"),
};

/** Where a floating body's mass comes from (the component's Mass Mode; default Rest Draft). */
UENUM(BlueprintType)
enum class EFluidNinjaBuoyancyMass : uint8
{
	/** The body weighs what its hull displaces at Rest Draft, in its Rest Attitude: at Begin Play the plugin sets that as a mass
	 * override on the body (Physics → Mass is overwritten), worked out at Buoyancy Multiplier 1. Use it when you know how deep the body
	 * should float (a boat, a barrel). */
	RestDraft,
	/** The body keeps its own mass (Physics → Mass, or its mass override); it floats wherever that puts it, or sinks when it weighs
	 * more than its whole hull's water (the hull report in the log says "SINKS"). Use it when the body's weight is known; the density
	 * presets set it this way. */
	BodyMass,
};

/** How the body meets the water (the component's Hull Model; default Triangles). */
UENUM(BlueprintType)
enum class EFluidNinjaHullModel : uint8
{
	/**
	 * The hull surface: the simple collision rebuilt as an even shell of triangles, cut exactly at the waterline every step. The most
	 * accurate and the most costly. Gives true bow and stern drag, slamming on flat faces, heeling and righting, and believable motion in
	 * waves. Choose it for anything with a real hull shape, or that is steered or ridden: boats, rafts. Tune Hull Resolution Cm (smaller
	 * follows the shape more closely) and Max Hull Triangles (the budget).
	 */
	Triangles,
	/**
	 * Cubes filling the hull, each lifting by how deep it sits. Cheaper than Triangles; the exact waterline does not matter. Choose it for
	 * solid bodies that bob, drift and tumble: logs, crates, barrels, debris. Tune Voxel Count (more follows the shape better and costs
	 * more) or a fixed Voxel Size Cm.
	 */
	Voxels,
	/**
	 * Spheres (pontoons), each lifting the cap of itself under the water. The cheapest model, and the only good one for very thin or tiny
	 * bodies the other two cannot resolve. Choose it for balls, buoys, many small props, or anything under a few centimetres thick (an
	 * arrow). Place the Pontoons yourself (location, radius, lift and drag weights); left empty, two rows along the bottom lift the whole
	 * hull, fine for a ball, wrong for odd shapes.
	 */
	Spheres,
};

/** A sphere standing in for part of the hull (body frame, cm): a pontoon. Used by the Spheres hull model (the component's Pontoons). */
USTRUCT(BlueprintType)
struct FFluidNinjaPontoon
{
	GENERATED_BODY()

	/** The sphere's centre, in the body's frame: cm from the floating primitive's origin along its axes, at world size (the
	 * component's scale is not applied to it). Put the spheres where the hull holds its volume; a sphere above the water lifts nothing.
	 * Default 0 (the origin). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pontoon", meta = (Units = "cm"))
	FVector Location = FVector::ZeroVector;

	/** The sphere's radius: it lifts the volume of its cap under the water, and its drag acts on the wet part of its cross-section.
	 * Bigger lifts more (a body of the same mass floats higher) and drags more. In cm, at world size (the component's scale is not
	 * applied). Default 25. At least 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pontoon", meta = (ClampMin = "1", Units = "cm"))
	float Radius = 25.f;

	/** Scales this sphere's lift and its share of the waterplane stiffness: above 1 it lifts more than its own cap, below 1 less, 0
	 * nothing. Under Mass Mode Rest Draft the mass is worked out with these weights, so the body still rests at Rest Draft and only the
	 * balance between spheres changes; under Body Mass a higher weight floats the body higher. Default 1. At least 0. Use it to trim a
	 * body that lists, or to let a small sphere stand in for more volume. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pontoon", meta = (ClampMin = "0"))
	float LiftWeight = 1.f;

	/** Scales this sphere's water drag: its pressure drag, and the skin friction on its wet cap that slows its spin. Higher ties it more
	 * closely to the water's motion and stops it sooner; 0 gives it no water drag. The air drag is not scaled. Default 1. At least 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pontoon", meta = (ClampMin = "0"))
	float DragWeight = 1.f;
};

/** What the water is doing to a body, as of the last physics step (read on the game thread). */
USTRUCT(BlueprintType)
struct FFluidNinjaBuoyancyReadout
{
	GENERATED_BODY()

	/** Some of the hull touches the water (its wetted area is above 0). */
	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	bool bInWater = false;

	/** Litres of water the hull pushes aside: its upward lift ÷ (ρg × Buoyancy Multiplier), so the multiplier does not inflate it. 0 when
	 * dry.
	  * Read-only readout: 0 or more. */
	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	float DisplacedLitres = 0.f;

	/** Hull area under the water, square metres. 0 when dry.
	 * Read-only readout: 0 or more. */
	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	float WettedAreaM2 = 0.f;

	/** The plugin's whole force on the body (the water's lift, drag and slamming, the air, the propeller and rudder), world frame,
	 * newtons. */
	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	FVector ForceN = FVector::ZeroVector;

	/** The body's mass as the physics step used it, kilograms.
	 * Read-only readout: more than 0 while the body simulates. */
	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	float MassKg = 0.f;

	/** The body's slamming power in the last frame, watts: one slam for the whole body (its hull pieces driving in at once, summed), so
	 * it is the same at every Hull Resolution, level of detail and Voxel Size.
	  * Read-only readout: 0 or more; 0 when nothing slams. */
	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	float SlamWatts = 0.f;

	/** The body's entry power, watts, on the frame it goes into the water after being clear of it (0 on every other frame): its
	 * weight × its sinking speed³ / (2 × its height). It sizes that entry's splash (Spray).
	  * Read-only readout: 0 or more. */
	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	float EntryWatts = 0.f;

	/** How old the water the body floats on is (seconds since the Surface Mirror drew it); -1 whenever the body is outside the capture
	 * (far-field water from a water volume, or no water read).
	  * Read-only readout: -1, or 0 and more. */
	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	float WaterAgeSeconds = -1.f;

	/** The current under the body's centre of mass, world X and Y, cm/s; 0 where the plugin reads no water around the body. */
	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	FVector2D FlowCmS = FVector2D::ZeroVector;

	/** The hull's level of detail in use: 0 finest, 1 and 2 coarser (Lod Distances Cm, plus fnb.Lod.Bias). Only the Triangles model has
	 * levels 1 and 2; Voxels and Spheres stay at 0.
	  * Read-only readout: 0, 1 or 2. */
	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	int32 HullLevel = 0;

	/** Settled out of sight and asleep: the water leaves it alone until it is seen, comes near or is hit. */
	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	bool bDormant = false;

	/** The air's part of ForceN (drag on the dry hull from its own motion and the wind), world frame, newtons. */
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

	/** The floating primitive, by its component name. None (the default), or a name no component of the actor has, uses the actor's root.
	 * It must be a primitive with simple collision; the plugin turns its physics simulation on. Set it when the hull is not the root (an
	 * actor whose root is a scene component). */
	UPROPERTY(EditAnywhere, Category = "Hull")
	FName PrimitiveName;

	/**
	 * How the physics measures the part of the body under water: its lift, drag, rocking and settling. A trade of accuracy against cost.
	 * - Triangles: the hull surface cut at the waterline. Most accurate, most costly. Anything steered or ridden, or that should rock and
	 *   heel convincingly: boats, rafts.
	 * - Voxels: cubes filling the hull. Solid props that bob, drift and tumble: logs, crates, barrels, debris.
	 * - Spheres: hand-placed pontoons. Cheapest. Round things, many small things, or anything under a few centimetres thick.
	 * Check the hull report in the log at Play ("hull from ...; volume ... L; rests ... cm deep") to see what the model measured. Whatever
	 * the model, the waves build their own shape of the body from the collision at Hull Resolution Cm: keep it well below the size of a
	 * very thin body, or its wave shape degenerates. Default Triangles.
	 */
	UPROPERTY(EditAnywhere, Category = "Hull")
	EFluidNinjaHullModel HullModel = EFluidNinjaHullModel::Triangles;

	/** The hull is rebuilt from the simple collision as an even shell of triangles about this size, so the water line is followed
	 * across big faces. Smaller follows the shape and the water line more closely and costs more triangles; larger is cheaper. If the
	 * shell would pass Max Hull Triangles it is made coarser in steps of 1.25x. The farther levels of detail are built at 2 and 4 times
	 * it. Every hull model builds its surface with it (Voxels fill that surface; FluidNinja Waves measure it). In cm. Default 40; the
	 * presets use 15 to 30. At least 2. */
	UPROPERTY(EditAnywhere, Category = "Hull", meta = (ClampMin = "2", UIMin = "15", UIMax = "40", Units = "cm"))
	float HullResolutionCm = 40.f;

	/** The most triangles the hull may have; past it the shell is made coarser than Hull Resolution, in steps of 1.25x until it fits.
	 * More follows the water line more closely and costs more every physics step. The farther levels of detail get a quarter and a
	 * sixteenth of it (at least 12 each), and a farther level is kept only if it has fewer triangles than the one before. Default 1000.
	 * At least 12. */
	UPROPERTY(EditAnywhere, Category = "Hull", meta = (ClampMin = "12"))
	int32 MaxHullTriangles = 1000;

	/** Distances from the camera (X, then Y). Triangles model: beyond the first the hull is twice as coarse, beyond the second four times,
	 * with a 10% band at each so it does not flicker. Every model: the water read around the body (the gather) is spaced 2 and 4 times
	 * wider beyond them and taken every fnb.Gather.FarEveryNFrames frames, then twice that. Every model: beyond the first a slam throws
	 * only its main splash (no crown and no plunge splashes) and only while the frame's splash budget is less than half spent
	 * (fnb.Effects.SplashBudgetPerFrame); beyond the second, or when the body has not been drawn for a second, no splashes or spray at
	 * all. In cm. Default 3000, 8000 (30 m, 80 m). Keep X below Y: above it the middle level is skipped. fnb.Lod.DistanceScale scales
	 * them for the hull and the gather, fnb.Lod.Bias makes every hull up to two levels coarser, fnb.Effects.ReachScale scales them for
	 * the effects (all set by sg.WaterQuality). */
	UPROPERTY(EditAnywhere, Category = "Hull", meta = (Units = "cm"))
	FVector2D LodDistancesCm = FVector2D(3000.0, 8000.0);

	/** Voxels model: the cube size. 0 (the default): about Voxel Count cubes fill the hull (the size is the cube root of the hull's volume
	 * ÷ Voxel Count). Smaller follows the shape more closely and costs more every physics step. If no cube fits inside the hull, the size
	 * shrinks by 0.7 up to five times; then the setup fails and the log says the hull is too thin. In cm. At least 0. */
	UPROPERTY(EditAnywhere, Category = "Hull", meta = (EditCondition = "HullModel == EFluidNinjaHullModel::Voxels", ClampMin = "0", Units = "cm"))
	float VoxelSizeCm = 0.f;

	/** Voxels model: about how many cubes fill the hull when Voxel Size is 0 (the real count follows the hull's shape; the hull report in
	 * the log gives it). More follows the shape more closely and costs more every physics step. Default 48; the presets use 64 and 216.
	 * Range 1-512. */
	UPROPERTY(EditAnywhere, Category = "Hull", meta = (EditCondition = "HullModel == EFluidNinjaHullModel::Voxels", ClampMin = "1", ClampMax = "512"))
	int32 VoxelCount = 48;

	/** Spheres model: the pontoons (body frame; see each one's Location, Radius, Lift Weight and Drag Weight). Empty (the default): two
	 * rows along the hull's bottom, two to four spheres per side, each with a radius of a quarter of the hull's width or half its height
	 * (the smaller), lifting together what the whole hull would: fine for a ball or a box, wrong for odd shapes. A ball is one pontoon at its
	 * centre. */
	UPROPERTY(EditAnywhere, Category = "Hull", meta = (EditCondition = "HullModel == EFluidNinjaHullModel::Spheres"))
	TArray<FFluidNinjaPontoon> Pontoons;

	/** Once settled (under 3 cm/s and 0.03 rad/s for two seconds), out of sight (not drawn for half a second) and farther than Sleep
	 * Distance from the camera, the body sleeps and the water leaves it alone until it is seen, comes near, or something hits it.
	 * Default on. It saves the cost of bodies nobody watches; turn it off for a body that must answer every wave (the beach ball and
	 * arrow presets do). */
	UPROPERTY(EditAnywhere, Category = "Hull")
	bool bSleepWhenSettled = true;

	/** A settled body sleeps only when at least this far from the camera (and out of sight); it wakes when it comes nearer. Lower lets
	 * bodies sleep closer to the player (cheaper); higher keeps more of them awake. In cm. Default 6000 (60 m). At least 0; 0 lets any
	 * settled body out of sight sleep. fnb.Lod.SleepDistanceScale scales it (set by sg.WaterQuality). */
	UPROPERTY(EditAnywhere, Category = "Hull", meta = (EditCondition = "bSleepWhenSettled", ClampMin = "0", Units = "cm"))
	float SleepDistanceCm = 6000.f;

	/** Where the body's mass comes from: Rest Draft sets it at Begin Play (as a mass override on the body) to what the hull displaces at
	 * Rest Draft; Body Mass keeps its own (Physics → Mass), and it floats wherever that puts it, or sinks. Default Rest Draft. */
	UPROPERTY(EditAnywhere, Category = "Floating")
	EFluidNinjaBuoyancyMass MassMode = EFluidNinjaBuoyancyMass::RestDraft;

	/** Mass Mode Rest Draft: how deep the hull's lowest point sits in calm water, at rest. The body's mass is set to the water the hull
	 * displaces that deep, in its Rest Attitude and at Buoyancy Multiplier 1. Deeper makes it heavier and float lower; shallower, lighter.
	 * In cm. Default 15; the presets use 1.2 (air mattress) to 25 (barrel). At least 0.5. */
	UPROPERTY(EditAnywhere, Category = "Floating", meta = (EditCondition = "MassMode == EFluidNinjaBuoyancyMass::RestDraft", ClampMin = "0.5", UIMin = "0.5", UIMax = "25", Units = "cm"))
	float RestDraftCm = 15.f;

	/** Held still at Begin Play until its water has been read (at least a second, while FluidNinja's surface starts up), then set level
	 * at its rest draft on that water and released: no drop or slam at the start. With only a water volume there it is set on the
	 * volume's top; with no water read at all it is let go after 4 seconds. Default on. Turn it off for a body that should start moving
	 * from where it is placed (the arrow preset does). */
	UPROPERTY(EditAnywhere, Category = "Floating")
	bool bSettleOnStart = true;

	/** How the body floats relative to its own frame: zero (the default) for a mesh modelled the way up it floats (a boat); a log or
	 * barrel modelled standing up floats lying down with Pitch 90. Rest Draft, the stability check, Settle On Start and the body's
	 * FluidNinja Waves footprint use it. */
	UPROPERTY(EditAnywhere, Category = "Floating")
	FRotator RestAttitude = FRotator::ZeroRotator;

	/** How fast physics may push the body out of something it starts in or is pressed into (rocks under the water): low, so a bad
	 * placement eases out instead of launching it. Higher frees it faster but more violently. In cm/s. Default 100. At least 0; 0 leaves
	 * the project setting. */
	UPROPERTY(EditAnywhere, Category = "Floating", meta = (ClampMin = "0", Units = "cm/s"))
	float MaxDepenetrationSpeed = 100.f;

	/** Moves the centre of mass from where the collision puts it (body frame): lower is more stable, off-centre makes the body list or
	 * trim. It is the body's centre of mass nudge, which the engine multiplies by the component's scale. In cm. Default 0. The hull report
	 * in the log (and Analyze Hull) gives the metacentric heights; zero or below says UNSTABLE: it will capsize. */
	UPROPERTY(EditAnywhere, Category = "Floating", meta = (Units = "cm"))
	FVector CenterOfMassOffset = FVector::ZeroVector;

	/** Scales the body's inertia about its X, Y and Z axes (the engine's inertia tensor scale). Above 1 it turns, rolls and pitches more
	 * slowly (a hollow hull, its mass at the outside, turns more slowly than a solid one of the same shape); below 1 more quickly.
	 * Default 1 (a solid body). */
	UPROPERTY(EditAnywhere, Category = "Floating")
	FVector InertiaScale = FVector::OneVector;

	/** Art control on the lift: the water's push up (and the waterplane's stiffness) times this. Above 1 floats the body higher and
	 * stiffer, below 1 lower and softer, 0 lifts nothing. 1 is Archimedes. Default 1. At least 0. Rest Draft's mass, Settle On Start and
	 * the stability report are worked out at 1, so above 1 the body rides higher than Rest Draft. Prefer Rest Draft or the body's mass to
	 * set how deep it floats. */
	UPROPERTY(EditAnywhere, Category = "Floating", meta = (ClampMin = "0"))
	float BuoyancyMultiplier = 1.f;

	/** Pressure drag where a face pushes into the water: the part that grows with its speed through the water (speed / 1 m/s;
	 * Kerner's Cpd1). The force is ½ρ(1 m/s)² × area × (Linear × s + Quadratic × s²), s = speed ÷ 1 m/s. Higher slows the body sooner
	 * through the water (and ties it more closely to the current); 0 drops this part. Triangles: per wet face. Voxels and Spheres: per
	 * point over the wet part of its section (times a pontoon's Drag Weight). Default 0.3; the arrow preset uses 0.05. At least 0. */
	UPROPERTY(EditAnywhere, Category = "Hydrodynamics", meta = (ClampMin = "0"))
	float PressureDragLinear = 0.3f;

	/** Pressure drag where a face pushes into the water: the part that grows with the square of its speed through the water
	 * ((speed / 1 m/s)²; Kerner's Cpd2), the one that dominates at speed. Higher makes fast motion through the water stop harder; 0
	 * drops this part. Every hull model uses it (as Pressure Drag Linear). Default 1; the arrow preset uses 0.08. At least 0. */
	UPROPERTY(EditAnywhere, Category = "Hydrodynamics", meta = (ClampMin = "0"))
	float PressureDragQuadratic = 1.f;

	/** Triangles model only: how the pressure drag falls off as a face turns from meeting the flow head-on: it is scaled by cos^Falloff
	 * of the angle between the face's normal and the flow (Kerner's fp). Lower keeps more drag on slanted faces; higher leaves it to the
	 * faces meeting the flow squarely. Default 0.5 (the square root). At least 0.01. */
	UPROPERTY(EditAnywhere, Category = "Hydrodynamics", meta = (ClampMin = "0.01"))
	float PressureFalloff = 0.5f;

	/** Triangles model only: suction where a face pulls away from the water (behind the hull): the part that grows with its speed through
	 * the water (speed / 1 m/s; Kerner's Csd1). Higher holds the stern back more; 0 drops this part. Voxels and Spheres have no suction.
	 * Default 0.3; the arrow preset uses 0.05. At least 0. */
	UPROPERTY(EditAnywhere, Category = "Hydrodynamics", meta = (ClampMin = "0"))
	float SuctionDragLinear = 0.3f;

	/** Triangles model only: suction where a face pulls away from the water: the part that grows with the square of its speed through
	 * the water ((speed / 1 m/s)²; Kerner's Csd2). Higher holds the stern back more at speed; 0 drops this part. Default 0.5; the arrow
	 * preset uses 0.05. At least 0. */
	UPROPERTY(EditAnywhere, Category = "Hydrodynamics", meta = (ClampMin = "0"))
	float SuctionDragQuadratic = 0.5f;

	/** Triangles model only: how the suction falls off as a face turns from facing straight away from the flow: it is scaled by
	 * cos^Falloff of that angle (Kerner's fs). Lower keeps more suction on slanted faces. Default 0.5 (the square root). At least 0.01. */
	UPROPERTY(EditAnywhere, Category = "Hydrodynamics", meta = (ClampMin = "0.01"))
	float SuctionFalloff = 0.5f;

	/** Scales skin friction, the water's drag along the wet hull (ITTC 1957 line, from the hull's Reynolds number). Triangles: along
	 * every wet face. Spheres: on each wet cap, which also slows the sphere's spin, so a ball rolls along the water instead of spinning in
	 * place. Voxels: none. Higher slows a gliding hull more; 0 turns it off. Default 1 (physical). At least 0. */
	UPROPERTY(EditAnywhere, Category = "Hydrodynamics", meta = (ClampMin = "0"))
	float ViscousMultiplier = 1.f;

	/** Slamming when faces drive into the water (bow slap, landing from a drop): a force that grows with the water's density, how fast
	 * the wetted area grows and how fast the piece drives in (pieces slower than 50 cm/s do not slam), capped at what would stop the body
	 * in one step. Higher makes landings and wave impacts harder, and their slam splashes, slaps, On Slam events and wave impulses
	 * bigger; 0 turns slamming off (none of those; the splash when the body goes in after being clear of the water still follows its
	 * weight and speed). Default 0.5. At least 0. */
	UPROPERTY(EditAnywhere, Category = "Hydrodynamics", meta = (ClampMin = "0"))
	float SlamMultiplier = 0.5f;

	/** How much the current carries the body: the water's horizontal velocity is scaled by this in every drag term and over the rudder.
	 * 0 ignores the current (the body drags as if the water stood still), 1 is physical, above 1 exaggerates. Range 0-2. Default 1. */
	UPROPERTY(EditAnywhere, Category = "Hydrodynamics", meta = (ClampMin = "0", ClampMax = "2"))
	float FlowCoupling = 1.f;

	/** Drag on the dry part from the air and the wind (Project Settings → FluidNinja Buoyancy → Wind, Air Density): ½ρ_air × this ×
	 * the dry area × the speed through the air². Higher lets the wind push a high-sided body more and slows it more through the air; 0
	 * turns air drag off. Default 0.8; the arrow preset uses 0.02 (a streamlined shaft). At least 0. */
	UPROPERTY(EditAnywhere, Category = "Hydrodynamics", meta = (ClampMin = "0"))
	float AirDragCoefficient = 0.8f;

	/** Extra damping about the body's own roll (X), pitch (Y) and yaw (Z) axes while wet, scaled by the share of the hull that is wet: the
	 * spin about each axis loses about this much of itself per second. Higher calms rocking and spinning; 0 (the default) leaves it to the
	 * water. In 1/s. At least 0. An art control for a hull that rocks or spins longer than wanted. */
	UPROPERTY(EditAnywhere, Category = "Hydrodynamics", meta = (ClampMin = "0"))
	FVector AngularDamping = FVector::ZeroVector;

	/** A propeller and rudder (body frame, the bow along +X). Throttle and Rudder are set from Blueprint, or the console for testing
	 * (fnb.Boat.Throttle, fnb.Boat.Rudder) when Console Control is on. Default off. */
	UPROPERTY(EditAnywhere, Category = "Propulsion")
	bool bPropulsion = false;

	/** For testing: a change of fnb.Boat.Throttle or fnb.Boat.Rudder sets this body's Throttle and Rudder. Off: only Blueprint sets
	 * them. Default on. */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion"))
	bool bConsoleControl = true;

	/** Where the propeller pushes, in the body's frame: cm from the floating primitive's origin along its axes, at world size (the
	 * component's scale is not applied). It pushes only while this point is under the water. Default 0 (the origin). */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion", Units = "cm"))
	FVector ThrustLocation = FVector::ZeroVector;

	/** Which way the propeller pushes at full ahead, in the body's frame (zero: forward, +X); astern pushes the other way. Only its
	 * direction counts (it is normalised). Default +X. */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion"))
	FVector ThrustDirection = FVector::ForwardVector;

	/** The propeller's push at Throttle 1 (and astern at -1), newtons. More drives the boat faster and accelerates it harder. Default
	 * 1500. At least 0; 0 gives no thrust. */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion", ClampMin = "0"))
	float MaxThrustNewtons = 1500.f;

	/** The propeller's wash over the rudder at full throttle, so the boat steers when slow: added to the water's speed past the rudder,
	 * times Throttle (astern takes it away). Higher steers harder at low speed; 0 leaves the rudder to the boat's own speed. In cm/s.
	 * Default 300. At least 0. */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion", ClampMin = "0", Units = "cm/s"))
	float PropWashSpeed = 300.f;

	/** Where the rudder sits, in the body's frame: cm from the floating primitive's origin along its axes, at world size (the component's
	 * scale is not applied). It steers only while this point is under the water. Put it aft of the centre of mass (at the stern): its side
	 * force turns the boat by its lever arm, and at the default 0 (the origin, near the centre of mass) it pushes the boat sideways
	 * rather than turning it. */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion", Units = "cm"))
	FVector RudderLocation = FVector::ZeroVector;

	/** The rudder blade's area, cm²: its side force grows with it and with the square of the water's speed past the rudder (the boat's
	 * speed through the water plus the propeller's wash). Bigger turns the boat harder. Default 900 (a 30 x 30 cm blade). At least 0; 0:
	 * no rudder. */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion", ClampMin = "0"))
	float RudderAreaCm2 = 900.f;

	/** The rudder's angle at Rudder -1 or 1. Its side force grows with the angle only up to about 11 degrees (lift coefficient 2π × the
	 * angle in radians, capped at 1.2), so a larger angle reaches that force at a smaller Rudder input and adds none beyond it. In
	 * degrees. Default 35. Range 0-60. */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion", ClampMin = "0", ClampMax = "60", Units = "deg"))
	float MaxRudderAngle = 35.f;

	/** The propeller's setting: -1 (full astern) to 1 (full ahead), a share of Max Thrust Newtons; 0 (the default) stops it. Clamped to
	 * that range. Set it from Blueprint, or with fnb.Boat.Throttle when Console Control is on. Needs Propulsion on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Propulsion", meta = (ClampMin = "-1", ClampMax = "1"))
	float Throttle = 0.f;

	/** The rudder's setting: -1 (hard left) to 1 (hard right), a share of Max Rudder Angle; positive turns the bow toward the body's +Y
	 * when Rudder Location is aft of the centre of mass. 0 (the default) is straight ahead. Clamped to that range. Set it from Blueprint, or with fnb.Boat.Rudder when Console Control is on.
	 * Needs Propulsion on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Propulsion", meta = (ClampMin = "-1", ClampMax = "1"))
	float Rudder = 0.f;

	/** The plugin's wakes for this body: the pieces of water line driving into the water (sideways, or outward as the hull sinks in:
	 * a drop makes a ring) are written into FluidNinja's data channel as moving brush points, so the simulation makes the bow wave,
	 * the wake, the impact ring and foam. Used only when Interaction is Wakes or FluidNinja Tracking and Wakes; under FluidNinja
	 * Tracking alone it does nothing. Off: the body floats but writes nothing into FluidNinja's flow (it still makes waves if Waves
	 * is on). Keep it off for light floaters that should ride the water others push (balls, ring buoys, mattresses). Needs Project
	 * Settings → FluidNinja Buoyancy → Wake on (console fnb.Buoyancy.Wake) and DataChannelPointReaderEnabled on the NinjaLive
	 * component. No tags or overlap events. Default on. */
	UPROPERTY(EditAnywhere, Category = "Wake")
	bool bWake = true;

	/** How the body stirs FluidNinja's flow (the current, dye and foam; FluidNinja Waves are separate, under Waves).
	 *
	 * Wakes: the plugin writes brush points along the hull's water line where it pushes into the water, sized and aimed by the
	 * hull's own shape and speed through the water. A drop makes a ring round the hull, a boat a bow wave and side wake; a body
	 * drifting with the current writes nothing. Works with stock FluidNinja. Use it for hulls and props: boats, crates, barrels,
	 * logs, anything dropped or thrown in. Tune it with the Wake settings below.
	 *
	 * FluidNinja Tracking: FluidNinja's own tracking paints one brush at the body's pivot (its size and strength are the NinjaLive's
	 * brush settings, not the hull's shape), moving at the body's speed relative to the water. The plugin turns Generate Overlap
	 * Events on and adds the NinjaLive's tracking tag; the plugin's wakes are off, so a drop gives no hull-shaped ring. Use it when
	 * bodies should stir the water the way FluidNinja stirs it for characters, set up in one place on the NinjaLive. Needs the
	 * modified FluidNinja: Project Settings → FluidNinja Buoyancy → FluidNinja Tracking (console fnb.Buoyancy.Tracking), and every
	 * NinjaLive turning its Relative Tracking Switch on and setting its Tracked Density Switch to 1, 4 or 5. The body must also be
	 * inside a NinjaLive's Interaction Volume that selects it, by object type (its Overlap Filter Inclusive Obj Type) or by tag; a
	 * body outside every volume is handed over once it enters one. Otherwise the body is
	 * not handed over, does not stir the water at all and the log says why (stock FluidNinja paints a body's world velocity, so a
	 * floater carried by a current would speed that current up).
	 *
	 * FluidNinja Tracking and Wakes: both at once (same requirements). Their pushes add up, so lower Wake Strength or Wake Gain if
	 * the stir is too strong. Use it for a body that needs FluidNinja's pivot brush and the plugin's hull wake and impact ring.
	 *
	 * Default FluidNinja Tracking. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake")
	EFluidNinjaBuoyancyInteraction Interaction = EFluidNinjaBuoyancyInteraction::FluidNinjaTracking;

	/** Brush size of each wake point at a push of 2 m/s (scaled 0.4–2x with the push, so 24 to 120 cm at the default); the impact
	 * burst's points use it too. Bigger stirs a wider band of water around the hull. In cm. Default 60. At least 1. */
	UPROPERTY(EditAnywhere, Category = "Wake", meta = (EditCondition = "bWake", ClampMin = "1", Units = "cm"))
	float WakeBrushSizeCm = 60.f;

	/** Pieces of water line pushing into the water slower than this (sideways and sinking speed together) leave no wake, and pieces
	 * sinking slower than it throw no impact burst, so a body bobbing gently at rest stays quiet. Higher keeps slow motion quiet; lower
	 * lets gentle motion stir. In cm/s. Default 20. At least 0. */
	UPROPERTY(EditAnywhere, Category = "Wake", meta = (EditCondition = "bWake", ClampMin = "0", Units = "cm/s"))
	float WakeMinPushCmS = 20.f;

	/** Each frame a wake point gives the water this share of what it lacks: the hull's outward speed there minus the water's own.
	 * FluidNinja adds brush velocity every frame, so the water near the hull approaches the hull's speed and never overshoots it.
	 * A body that makes wakes, or that FluidNinja tracks itself (Generate Overlap Events on), takes its current from the water around
	 * it, not from the water it has just pushed. Higher builds the wake sooner. Default 0.15. Range 0-1. */
	UPROPERTY(EditAnywhere, Category = "Wake", meta = (EditCondition = "bWake", ClampMin = "0", ClampMax = "1"))
	float WakeGain = 0.15f;

	/** How much of the hull's speed through the water (relative to the current around it) the water at the hull takes on. A hull
	 * stirs only the water near the surface, not the whole depth FluidNinja's flat simulation stands for: 1 would make it a wall
	 * through the water (the current then races round it), 0 leaves the water alone. A body drifting with the current makes none.
	 * Default 0.35. Range 0-1. */
	UPROPERTY(EditAnywhere, Category = "Wake", meta = (EditCondition = "bWake", ClampMin = "0", ClampMax = "1"))
	float WakeStrength = 0.35f;

	/** The impact burst: where the hull sinks into the water (a drop, a dunk, a flat bottom slapping down), the water it displaces runs
	 * outward across the water line at about the sinking speed. This scales that outward speed: 1 is physical, 0 leaves only the side
	 * wake, above 1 exaggerates. A radial burst carries no net momentum, so it cannot build a current. Default 1. At least 0. */
	UPROPERTY(EditAnywhere, Category = "Wake", meta = (EditCondition = "bWake", ClampMin = "0"))
	float PlungeGain = 1.f;

	/** Each frame a burst point gives the water this share of what it lacks of the burst's outward speed (as Wake Gain does for the side
	 * wake): higher makes the ring start faster and harder. Default 0.5. Range 0-1. */
	UPROPERTY(EditAnywhere, Category = "Wake", meta = (EditCondition = "bWake", ClampMin = "0", ClampMax = "1"))
	float PlungeResponse = 0.5f;

	/** The body takes part in FluidNinja Waves (Project Settings → FluidNinja Buoyancy → Waves): its static head (how deep its hull sits below the
	 * wave-free surface) and the impulse of the slam and drag that stop it make real water waves in FluidNinja's simulation, which spread,
	 * bounce off banks and rock other floaters; and it feels the others' waves (its own taken out, its own waves' drag put back as radiation
	 * damping). Off: it floats on FluidNinja's own surface and leaves the waves alone. Needs the modified FluidNinja core (FluidNinja
	 * Waves) and Waves on in the project settings. Default on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Waves")
	bool bWaves = true;

	/** Scales how much of the waves this body feels (κ, with Project Settings → FluidNinja Buoyancy → Waves → Wave Feel Scale): 1 is physical, 0
	 * floats it on FluidNinja's own surface while it still makes waves, above 1 exaggerates. Default 1. Range 0-2. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Waves", meta = (EditCondition = "bWaves", ClampMin = "0", ClampMax = "2"))
	float WaveFeelScale = 1.f;

	/** Darkens and glosses the hull below the water line and where it was wet a moment ago, drying at Drying Speed. The hull's
	 * materials read it as Custom Primitive Data (Wet Line Data Index … +3: the water line plane in the mesh's local space, and how far
	 * above it the hull is still wet). Add the material function MF_FNB_HullWetness to the hull's material to show it. Default on.
	 * fnb.Effects.WetLine 0 (set by sg.WaterQuality) stops it for every body, writing the hulls dry once. */
	UPROPERTY(EditAnywhere, Category = "Wet Line")
	bool bWetLine = true;

	/** How fast the wet band above the water line dries, from the top down (and the whole hull once it is out of the water). Higher
	 * dries faster. In cm/s. Default 1.5. At least 0; 0 never dries. */
	UPROPERTY(EditAnywhere, Category = "Wet Line", meta = (EditCondition = "bWetLine", ClampMin = "0", Units = "cm/s"))
	float DryingSpeedCmS = 1.5f;

	/** The first of the four Custom Primitive Data slots the wet line writes on the floating primitive (this index … +3); move it when
	 * the hull's materials use those slots for something else. MF_FNB_HullWetness reads slots 0-3: after moving it, change the Primitive
	 * Data Index of its HullWaterPlane parameter to match, in a copy of the function. Default 0. At least 0. */
	UPROPERTY(EditAnywhere, Category = "Wet Line", meta = (EditCondition = "bWetLine", ClampMin = "0"))
	int32 WetLineDataIndex = 0;

	/** Spray where the hull slams into the water and at the bow when it drives hard into it. None (the default): the project default
	 * (FluidNinja Buoyancy → Effects → Splash System; NS_FNB_HullSplash as the plugin ships), and with that empty too, no spray. Splashes
	 * are drawn only within Lod Distances Y and while the frame's splash budget allows (fnb.Effects.SplashBudgetPerFrame). */
	UPROPERTY(EditAnywhere, Category = "Spray")
	TSoftObjectPtr<UNiagaraSystem> SplashSystem;

	/** Slams at least this strong (watts) throw a splash where they hit (the noise floor: a bobbing body's small slams make none). Its
	 * size is on the project's splash scale (FluidNinja Buoyancy → Effects → Splash Reference Watts), the same for every body. The body's
	 * entry (going in after being clear of the water) does not use this floor: it splashes whenever its own entry power makes a splash of
	 * at least 0.2 on that scale. Lower lets gentler slams splash. Default 300; the presets use 2 (the arrow), 20 (props) and 300 (the
	 * boat). At least 0; 0: no slam splashes (entries and bow spray still run). */
	UPROPERTY(EditAnywhere, Category = "Spray", meta = (ClampMin = "0", UIMin = "0", UIMax = "300"))
	float SplashMinWatts = 300.f;

	/** The largest a splash from this body gets: its size is the cube root of the slam's (or entry's, or bow spray's) power over Splash
	 * Reference Watts (eight times the power, twice the splash), up to this many times the splash system's own size (before Splash Scale)
	 * and never past the project's Splash Strength Ceiling. Splashes under 0.2 are not drawn. Up to three more splashes go where the water
	 * line plunges in hardest, smaller by how much slower it plunges there. Lower caps a body's biggest splashes. Default 12. At least
	 * 0.2. */
	UPROPERTY(EditAnywhere, Category = "Spray", meta = (ClampMin = "0.2"))
	float MaxSplashStrength = 12.f;

	/** When nothing slams, spray is thrown where the water line drives into the water at least this fast (the bow), on the project's
	 * splash scale: the power of the water that piece of water line drives in (its area × the cube of its speed), so twice the speed is
	 * twice the spray. Higher keeps slower hulls from spraying. In cm/s. Default 150. At least 0; 0: no bow spray. */
	UPROPERTY(EditAnywhere, Category = "Spray", meta = (ClampMin = "0", Units = "cm/s"))
	float BowSprayMinPushCmS = 150.f;

	/** Scales the size of every splash and spray from this body (the Niagara system's scale): above 1 bigger, below 1 smaller. It changes
	 * no threshold and no budget: which splashes are thrown stays the same. Default 1. At least 0. */
	UPROPERTY(EditAnywhere, Category = "Spray", meta = (ClampMin = "0"))
	float SplashScale = 1.f;

	/** The least time between two slam splashes from this body, and between two bow sprays. A slam does not wait for the bow spray's
	 * interval (it cuts in), and a slam that could not splash yet keeps its hardest value for a quarter of a second, so one-frame slams
	 * on a spraying bow are not lost. Higher thins out the splashes of a body that slams often. In s. Default 0.12. At least 0.02. */
	UPROPERTY(EditAnywhere, Category = "Spray", meta = (ClampMin = "0.02", Units = "s"))
	float SplashInterval = 0.12f;

	/** A one-shot played where the hull slams into the water (Slap Min Watts), louder the harder the slam. Float inputs a MetaSound can
	 * read are set on it: "Pitch Shift" (semitones), "Impact" (the slam on the project's splash scale, Splash Reference Watts), "Speed"
	 * (the water running past the water line, cm/s), "Length" (the water line, cm) and "Mass" (the body, kg). At most one every 0.08 s;
	 * its volume is 0.35 + 0.35 × log10(slam ÷ Slap Min Watts), held to 0.2-2, times Sound Volume. None (the default): the project
	 * default (Project Settings → FluidNinja Buoyancy → Effects; no sounds ship with the plugin). */
	UPROPERTY(EditAnywhere, Category = "Sound")
	TSoftObjectPtr<USoundBase> SlapSound;

	/** A loop played while the hull is in the water: the water running along the hull, louder and higher the faster it runs past the
	 * water line (Wash Full Speed) and the longer the water line; quiet lapping at rest. Its "Pitch Shift" input (semitones) follows the
	 * speed (-3 still to +2 at Wash Full Speed); "Impact", "Speed", "Length" and "Mass" are kept up to date on it as on the slap. Its
	 * volume is full at a 3 m water line and Wash Full Speed, times Sound Volume; it stops while the body is out of the water or asleep.
	 * None (the default): the project default (Project Settings → FluidNinja Buoyancy → Effects; no sounds ship with the plugin). */
	UPROPERTY(EditAnywhere, Category = "Sound")
	TSoftObjectPtr<USoundBase> WashSound;

	/** A one-shot played when the hull is thrown about (Creak Min Angular Acceleration) or slams four times Slap Min Watts; "Pitch
	 * Shift" (semitones), "Impact", "Speed", "Length" and "Mass" are set on it as on the slap. Only while the hull is in the water, at
	 * most one every 0.7-1.3 s, volume 0.4-1 with how hard it was thrown, times Sound Volume. None (the default): the project default
	 * (Project Settings → FluidNinja Buoyancy → Effects; no sounds ship with the plugin). */
	UPROPERTY(EditAnywhere, Category = "Sound")
	TSoftObjectPtr<USoundBase> CreakSound;

	/** A one-shot played once when the hull leaves the water entirely: water dripping off it. Its float input "Impact" is set to how high
	 * the hull is wet above its last water line (cm), so a MetaSound can drip longer from a hull that went in deeper. None (the default):
	 * no drip (there is no project default). */
	UPROPERTY(EditAnywhere, Category = "Sound")
	TSoftObjectPtr<USoundBase> DripSound;

	/** How the hull's sounds fade with distance. None (the default): the project default (Project Settings → FluidNinja Buoyancy →
	 * Effects → Hull Attenuation; SA_FNB_Hull as the plugin ships). */
	UPROPERTY(EditAnywhere, Category = "Sound")
	TSoftObjectPtr<USoundAttenuation> Attenuation;

	/** Scales the volume of all of this body's hull sounds (slap, wash, creak, drip): 1 as the plugin sets them, 0 silent. Default 1.
	 * At least 0. */
	UPROPERTY(EditAnywhere, Category = "Sound", meta = (ClampMin = "0"))
	float SoundVolume = 1.f;

	/** Slams at least this strong (watts) play a slap; four times this also creaks. Lower makes gentler slams slap. Default 150. At
	 * least 0; 0: no slaps, and no creaks from slams. */
	UPROPERTY(EditAnywhere, Category = "Sound", meta = (ClampMin = "0"))
	float SlapMinWatts = 150.f;

	/** The wash is at full volume when the water runs past the water line this fast (with a 3 m water line or longer). Lower reaches full
	 * volume at a slower speed. In cm/s. Default 250. At least 1. */
	UPROPERTY(EditAnywhere, Category = "Sound", meta = (ClampMin = "1", Units = "cm/s"))
	float WashFullSpeedCmS = 250.f;

	/** Creaks when the hull's spin changes faster than this (rad/s²), or on a slam of four times Slap Min Watts. Lower creaks more easily.
	 * Default 2. At least 0; 0 turns creaks off entirely, slam creaks too. */
	UPROPERTY(EditAnywhere, Category = "Sound", meta = (ClampMin = "0"))
	float CreakMinAngularAcceleration = 2.f;

	/** Slams weaker than this (watts) raise no On Slam event. Higher keeps the event for hard impacts. Default 200. At least 0; 0 raises it
	 * for every slam. */
	UPROPERTY(EditAnywhere, Category = "Events", meta = (ClampMin = "0"))
	float SlamEventWatts = 200.f;

	/** The hull slammed into the water at least Slam Event Watts hard: the slam's power (watts) and where it hit. Raised on the game
	 * thread from the last physics step's results; no slams at Slam Multiplier 0. */
	UPROPERTY(BlueprintAssignable, Category = "Events")
	FFluidNinjaBuoyancySlamSignature OnSlam;

	/** The body touched the water (true) or left it entirely (false). */
	UPROPERTY(BlueprintAssignable, Category = "Events")
	FFluidNinjaBuoyancyWaterSignature OnWaterContactChanged;

	/** What the water is doing to the body, as of the last physics step. */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Buoyancy")
	FFluidNinjaBuoyancyReadout GetReadout() const { return Readout; }

	/** One line on how the body floats now: in the water, dry or asleep; hull level, displaced litres, wetted area, force, mass, the
	 * water's age and current, and the throttle and rudder (with propulsion). */
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
	/** The scale on a hull level's hydrostatic lift that makes it displace, at the finest hull's rest waterline and in the rest attitude,
	 * what the finest hull displaces there: V_0 / V_Level, clamped to 0.5..2 (1 for level 0, and for a level that was not built: pass the
	 * level GetHull was asked for after the subsystem's clamp to GetHullLevels). The coarse levels are cruder shells inside the true
	 * surface, so without it the same mass floats deeper on them. The subsystem multiplies the step's FParams::BuoyancyMultiplier by it,
	 * which scales the lift and the waterplane stiffness together: the level then rests at the finest hull's draft, but its heave and tilt
	 * stiffness are this times its own waterplane's and its metacentric heights stay what its coarse geometry gives (GetRollStability and
	 * GetPitchStability are the finest hull's). Follows the mass (RefreshRestingDraft). */
	float GetLevelBuoyancyScale(int32 Level) const { return Level >= 0 && Level < Hulls.Num() && LevelBuoyancyScales.IsValidIndex(Level) ? LevelBuoyancyScales[Level] : 1.f; }
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
	/** The finest triangle surface of the hull (body frame), whatever model floats it: FluidNinja Waves measure how deep it sits in each cell. */
	TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe> GetWaveHull() const { return WaveHull; }
	/** The same surface copied into plain arrays (the body frame's vertices and the triangles indexing them), all another module needs to
	 * measure the hull (the test bench; FluidNinjaBuoyancyCore.h stays private). False while there is none (before Begin Play). */
	bool CopyWaveHullSurface(TArray<FVector3f>& OutVertices, TArray<FIntVector3>& OutTriangles) const;
	/** Changes whenever the body is put somewhere rather than moved there (MarkTeleported). */
	int32 GetTeleportCounter() const { return TeleportCounter; }
	/** FluidNinja Waves' running estimate of how fast the body heaves, rolls and pitches (Hz; 0 before there is one), for the status line. */
	void SetWaveMotion(const FVector& Hz) { WaveMotionHz = Hz; }
	const FVector& GetWaveMotion() const { return WaveMotionHz; }

	/** Call after moving the body by teleporting it (setting its location, a reset, a warp): FluidNinja Waves then clear the water where it
	 * was and where it is now, and start its own field over, so the jump makes no wave. Settle On Start, Restart At and the test bench call
	 * it themselves. */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Buoyancy")
	void MarkTeleported() { ++TeleportCounter; }

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
	/** Recomputes the resting draft and the metacentric heights when the body's mass has moved by over half a percent since they were
	 * computed (cargo, ballast, a mass override): FluidNinja Waves cap a hull's static head at 1.2 times its resting draft, so a stale
	 * draft starves a heavier body of displacement (a 96 kg crate made 320 kg pressed 266 L for its 320). The subsystem calls it every
	 * frame; it costs nothing while the mass stands (DraftForMass bisects the hull when it has moved). */
	void RefreshRestingDraft();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

protected:
	bool SetUp(FString& OutReport);
	/** Each level's buoyancy scale (GetLevelBuoyancyScale) for these hulls with the body resting Draft cm deep on the finest of them (negative:
	 * it sinks, and the whole volumes are compared), into OutScales (one per level, 1 for level 0). Warns once per body and level when a
	 * scale is clamped. */
	void ComputeLevelBuoyancyScales(const TArray<TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe>>& Levels, float Draft,
		const FluidNinjaBuoyancy::FParams& Params, TArray<float>& OutScales) const;

	void UpdateWetLine(UPrimitiveComponent& Primitive, float DeltaTime);
	/** How far the body is from the viewer and whether it was drawn lately, into EffectReach, once a tick (see Lod Distances). */
	void UpdateEffectReach(const UPrimitiveComponent& Primitive);
	void UpdateSpray(float DeltaTime);
	void UpdateSound(UPrimitiveComponent& Primitive, float DeltaTime);
	/** Throws one splash of Strength on the project's scale (times Splash Scale) if it is worth drawing, the body is near enough for it
	 * (bSecondary: a crown or plunge splash, near bodies only) and the frame's splash budget allows it. True when it was thrown. */
	bool SpawnSplash(UWorld& World, const FVector& Where, const FRotator& Facing, float Strength, bool bSecondary);
	/** Sets "Impact", "Speed", "Length" and "Mass" on a sound so a MetaSound can layer on them. */
	void SetSoundLayering(UAudioComponent& Audio, float Impact) const;

	TArray<TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe>> Hulls;
	/** Per entry of Hulls, the scale on its lift that volume-matches it to the finest at the rest waterline (GetLevelBuoyancyScale). */
	TArray<float> LevelBuoyancyScales;
	/** The finest triangle surface, kept for every hull model (the triangles model's finest level itself). */
	TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe> WaveHull;
	int32 TeleportCounter = 0;
	/** FluidNinja Waves' estimate of the body's heave, roll and pitch frequencies (Hz), as last set (SetWaveMotion). */
	FVector WaveMotionHz = FVector::ZeroVector;
	FFluidNinjaBuoyancyReadout Readout;
	TArray<FFluidNinjaWaterlineContact> Contacts;
	float ContactWash = 0.f;
	float ContactLength = 0.f;
	/** The hardest slam reported this frame (watts) and where: the sounds (a slap, a creak) follow it; cleared at the end of the tick. */
	float FrameSlamWatts = 0.f;
	FVector FrameSlamLocation = FVector::ZeroVector;
	/** The hardest slam since the last slam splash (watts), where it hit, and when it started waiting: the splash follows it. It keeps
	 * growing until the splash is thrown, or for a quarter of a second (UpdateSpray), so a slam on a frame the splash interval holds is
	 * not lost. */
	float PendingSlamWatts = 0.f;
	FVector PendingSlamLocation = FVector::ZeroVector;
	double PendingSlamTime = 0.0;
	/** The last entry's power (watts) and when it went in: the entry's splash takes its size from it (UpdateSpray). */
	float PendingEntryWatts = 0.f;
	double PendingEntryTime = 0.0;
	/** How much of the body's effects reach the viewer this tick: 0 every splash; 1 (beyond Lod Distances X) the main slam splash and
	 * bow spray only, and only while the frame's budget is under half spent; 2 (beyond Lod Distances Y, or not drawn for a second) none. */
	int32 EffectReach = 2;

	// Wet line: the water line plane (local z = A + B x + C y) and how high the hull is still wet above it.
	FVector WetPlane = FVector::ZeroVector;
	float WetTop = -1e9f;
	bool bHasWetPlane = false;

	bool bAwaitingWater = false;
	double AwaitingSince = 0.0;
	float RestingDraft = 0.f;
	/** The body mass RestingDraft was computed at (kg; RefreshRestingDraft). */
	float RestingDraftMassKg = 0.f;
	/** The finest hull's lowest and highest point in its rest attitude (cm, about the body's origin). */
	float RestedBottom = 0.f;
	float RestedTop = 0.f;
	float RollGM = 0.f;
	float PitchGM = 0.f;
	FTransform StartTransform = FTransform::Identity;

	/** When the next slam splash, bow spray, slap and creak may come (seconds): a slam's hold does not stop bow spray, nor the bow
	 * spray's hold a slam. */
	double NextSplashTime = 0.0;
	double NextBowSprayTime = 0.0;
	double NextSlapTime = 0.0;
	double NextCreakTime = 0.0;
	FVector LastAngularVelocity = FVector::ZeroVector;
	float WashGain = 0.f;
	/** The layering values last sent to the wash loop (Impact, Speed, Length, Mass): they are sent again only when they change. */
	float WashSent[4] = {-1.f, -1.f, -1.f, -1.f};

	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> WashAudio;

	UPROPERTY(Transient)
	TObjectPtr<UNiagaraSystem> LoadedSplash;

	UPROPERTY(Transient)
	TObjectPtr<USoundBase> LoadedSlap;

	UPROPERTY(Transient)
	TObjectPtr<USoundBase> LoadedCreak;

	UPROPERTY(Transient)
	TObjectPtr<USoundBase> LoadedDrip;

	UPROPERTY(Transient)
	TObjectPtr<USoundAttenuation> LoadedAttenuation;
	bool bWasInWater = false;
	FString Report;
};
