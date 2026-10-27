#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Curves/CurveFloat.h"
#include "FluidNinjaBuoyancyWash.h"
#include "UObject/SoftObjectPath.h"
#include "FluidNinjaBuoyancyComponent.generated.h"

class UAudioComponent;
class UNiagaraComponent;
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

/** How the water's dynamic pressure acts on a Triangles hull (the component's Hull Flow; default Displacement). */
UENUM(BlueprintType)
enum class EFluidNinjaHullFlow : uint8
{
	/** Kerner's pressure drag and suction (Pressure Drag, Suction Drag and their falloffs): right for displacement hulls and props, the
	 * plugin's behaviour before Hull Flow existed. */
	Displacement,
	/**
	 * A planing hull (a motorboat, a runabout, a jet ski): the faces meeting the flow take Savitsky's planing pressure k · ½ρV² · cos θ
	 * (Planing Pressure Coefficient k), so the bottom lifts with its trim and the boat climbs out of the water as it speeds up; the
	 * suction behind the hull is capped at the local hydrostatic head (the water lets go of the transom: a ventilated stern); slams take
	 * only the vertical entry speed; the hull pays the wave-making resistance of its own pressure (Havelock: the hump before it planes,
	 * and the peak near the critical speed in shallow water); and the steady part of its pressure makes waves at its full weight. Triangles
	 * hull model only (Voxels and Spheres ignore it); the hull stays at its finest level of detail.
	 */
	Planing,
};

/** How the propeller is driven (the component's Propulsion Model; default Simple). */
UENUM(BlueprintType)
enum class EFluidNinjaPropulsionModel : uint8
{
	/** Throttle × Max Thrust Newtons at Thrust Location while that point is under water; Prop Wash Speed washes the rudder. Rowing,
	 * pushing, scripted propulsion; the plugin's behaviour before the propeller model existed. */
	Simple,
	/**
	 * An engine and a propeller: Engine Power Kw at Engine Max Rpm through Gear Ratio to a propeller of Prop Diameter Cm and Prop Pitch
	 * Ratio, with the open-water thrust and torque curves of a generic four-blade series (or your own Prop Thrust Curve and Prop Torque
	 * Curve). Thrust falls as the boat speeds up (the advance ratio) until it meets the drag: the boat finds its own top speed. The throttle
	 * lags (Engine Response Seconds), its sign shifts ahead, neutral and astern, the disc thrusts only as far as it is under water and
	 * ventilates (loses grip, the engine flares) when it breaks the surface, the propeller's torque leans the hull, and its jet washes the
	 * rudder. Readout: thrust, rpm, power, load, immersion, ventilation and the jet. Max Thrust Newtons and Prop Wash Speed are not used.
	 */
	Propeller,
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

	/** The body's horizontal speed through the water (its velocity less the current under it), cm/s.
	 * Read-only readout: 0 or more. */
	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	float SpeedThroughWaterCmS = 0.f;

	/** How far the bow is raised: the body's forward axis (+X) against the horizontal, degrees (positive bow up). A planing hull trims bow
	 * up at the hump (about 5-8°) and settles to 2-5° on the plane.
	 * Read-only readout: -90 to 90. */
	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	float TrimDeg = 0.f;

	/** Boats (Propulsion on, or a Keel Area above 0): the drift angle between the hull's heading (its +X, level) and its horizontal way
	 * through the water at the centre of mass, degrees. Positive while the hull slides toward its right (+Y): a turn to the right (Rudder
	 * positive) slides it out to its left and reads negative. A boat holding a hard turn drifts a few degrees to about 15-20°; a hull
	 * that reads far more is skating sideways (give it a Keel Area). Near ±180 going astern. 0 below 50 cm/s, out of the water, and on
	 * bodies with neither Propulsion nor a keel.
	 * Read-only readout: -180 to 180. */
	UPROPERTY(BlueprintReadOnly, Category = "Buoyancy")
	float DriftAngleDeg = 0.f;

	/** Keel Area above 0: the keel's side force against the slide this frame, newtons (its size; it pushes the hull back toward its
	 * heading, at Keel Location). 0 without a keel, with the keel out of the water, and running straight.
	 * Read-only readout: 0 or more. */
	UPROPERTY(BlueprintReadOnly, Category = "Keel")
	float KeelForceN = 0.f;

	/** Hull Flow Planing: the wave-making resistance the hull pays this frame, newtons (its size; it acts against the body's way through
	 * the water). It peaks at the hump (a length Froude number of about 0.5-0.7) and falls once the hull planes.
	 * Read-only readout: 0 or more. */
	UPROPERTY(BlueprintReadOnly, Category = "Propulsion")
	float WaveResistanceN = 0.f;

	/** Propulsion: the propeller's push along its axis, newtons (negative astern). Simple: Throttle × Max Thrust Newtons while the thrust
	 * point is under water. */
	UPROPERTY(BlueprintReadOnly, Category = "Propulsion")
	float ThrustN = 0.f;

	/** Propeller model: the engine's speed, rev/min (idle in neutral, the rev limit at full power). 0 under Simple. */
	UPROPERTY(BlueprintReadOnly, Category = "Propulsion")
	float EngineRpm = 0.f;

	/** Propeller model: the propeller shaft's speed, rev/min (negative astern; Engine Rpm ÷ Gear Ratio in gear). 0 under Simple. */
	UPROPERTY(BlueprintReadOnly, Category = "Propulsion")
	float PropRpm = 0.f;

	/** Propeller model: the power the propeller puts into the water, kilowatts. 0 under Simple.
	 * Read-only readout: 0 or more. */
	UPROPERTY(BlueprintReadOnly, Category = "Propulsion")
	float PropPowerKw = 0.f;

	/** Propeller model: how hard the engine works, the water's torque over what the engine could give at this rpm at full throttle (0 idle
	 * or neutral, 1 flat out). For the engine sound. 0 under Simple.
	 * Read-only readout: 0 to 1.5. */
	UPROPERTY(BlueprintReadOnly, Category = "Propulsion")
	float EngineLoad = 0.f;

	/** Propeller model: the share of the propeller disc under the water, 0 (out) to 1 (fully under). Under 0.8 a loaded propeller
	 * ventilates. 0 under Simple. */
	UPROPERTY(BlueprintReadOnly, Category = "Propulsion")
	float PropImmersion = 0.f;

	/** Propeller model: air is drawn down onto the blades (the disc broke the surface under load): thrust and torque drop to Ventilated
	 * Thrust Factor and the engine flares; it clears once the disc has been fully under the water for 0.3 s. */
	UPROPERTY(BlueprintReadOnly, Category = "Propulsion")
	bool bPropVentilated = false;

	/** Propeller model: the slipstream's speed through the water over the propeller's advance (the water's speed into the disc), cm/s, in
	 * either gear: the white boil behind the stern grows with it. A little remains in neutral while the shaft spins down. Astern the jet
	 * leaves forward, along -Prop Axis × Gear, and its whitewater with it. 0 under Simple.
	 * Read-only readout: 0 or more. */
	UPROPERTY(BlueprintReadOnly, Category = "Propulsion")
	float PropJetExcessCmS = 0.f;

	/** Propeller model: the gear, 1 ahead, 0 neutral, -1 astern (the throttle's sign; within ±0.02 of 0 is neutral). 0 under Simple. */
	UPROPERTY(BlueprintReadOnly, Category = "Propulsion")
	int32 Gear = 0;

	/** Propeller model: the propeller disc's centre, world, cm (where to attach a prop-wash effect). Zero under Simple. */
	UPROPERTY(BlueprintReadOnly, Category = "Propulsion")
	FVector PropLocation = FVector::ZeroVector;

	/** Propeller model: the thrust axis ahead, world, unit (turned with the drive on a steerable outboard; the jet leaves the other way
	 * going ahead). +X under Simple. */
	UPROPERTY(BlueprintReadOnly, Category = "Propulsion")
	FVector PropAxis = FVector::ForwardVector;
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

	/**
	 * How the water's dynamic pressure acts on the hull (Triangles hull model only: Voxels and Spheres ignore it, and throw no stern wash or
	 * rooster tail either).
	 * - Displacement (the default): Kerner's pressure drag and suction above. Boats that push through the water (row boats, barges,
	 *   sailing hulls), rafts and props.
	 * - Planing: motorboats, runabouts, jet skis, anything meant to rise onto the plane. The bottom lifts by Savitsky's planing law
	 *   (Planing Pressure Coefficient), the transom ventilates, the hull pays the wave-making resistance of its own pressure (Wave
	 *   Resistance Scale: the hump before it planes), slams take only the vertical entry, and the steady pressure makes waves at its full
	 *   weight (Wave Planing Pressure Scale in the project settings). Pressure Drag Linear, Pressure Drag Quadratic and Pressure Falloff
	 *   are not used on the faces meeting the flow (the suction keeps its settings, capped). The hull stays at its finest level of detail.
	 * Pair Planing with Propulsion Model Propeller: a constant push has no power limit and drives a planing hull far past any real top speed.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydrodynamics")
	EFluidNinjaHullFlow HullFlow = EFluidNinjaHullFlow::Displacement;

	/** Hull Flow Planing: k of the planing law, the share of the dynamic pressure ½ρV² a face meeting the flow takes, times its incidence
	 * (a bottom at trim τ lifts k ½ρV² sin τ per unit area; a face meeting the flow head-on stagnates at k ½ρV²). Savitsky's planing
	 * coefficients come out at about 0.4 (a long, narrow wetted bottom) to 0.8 (a short, wide one); 0.6 fits a 5 m runabout. Higher lifts
	 * the hull sooner and planes it at a flatter trim, and adds the same share of drag; lower lets it ride deeper and trim higher. Default
	 * 0.6. Range 0.1-1.5. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydrodynamics", meta = (EditCondition = "HullFlow == EFluidNinjaHullFlow::Planing", ClampMin = "0.1", ClampMax = "1.5"))
	float PlaningPressureCoefficient = 0.6f;

	/** Hull Flow Planing: scales the wave-making resistance, the drag the waves the hull's own pressure radiates take from it (Havelock's
	 * integral over its wet pieces, at the depth under it). 1 is physical: on a 5 m, 1 t runabout about 0.45 kN at the hump (5 m/s), 0.25
	 * kN at 10 m/s, 0.14 kN at 15 m/s, and a peak in shallow water near the critical speed √(gh). Higher deepens the hump the boat has to
	 * climb before it planes; 0 turns it off (no hump). Evaluated 30 times a second and smoothed over 0.25 s. Default 1. Range 0-3. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydrodynamics", meta = (EditCondition = "HullFlow == EFluidNinjaHullFlow::Planing", ClampMin = "0", ClampMax = "3"))
	float WaveResistanceScale = 1.f;

	/**
	 * The keel: the hull's lateral plane under the water that resists sliding sideways (keel, skeg, a jet ski's pump housing, strakes and
	 * chines), in cm². It acts as a low-aspect-ratio wing at Keel Location: while the hull runs at u and slides across its heading at v
	 * (through the water, the current taken off), the water pushes back against the slide with ½ρ × this area × ((π AR / 2) × |u| × v +
	 * Keel Crossflow Coefficient × v × |v|), and the slide costs the induced drag ½ρ × this area × (π AR / 4) × v² against the way ahead
	 * (AR = 2 × Keel Depth Cm² ÷ this area). The force scales with the share of Keel Depth Cm under the water: a hull riding high grips
	 * less, and in the air not at all. Any Hull Model and Hull Flow, with or without Propulsion.
	 * The main knob for a boat that slides through its turns: more area, less slide. A planing V-bottom's own grip against the slide goes
	 * with sin² of its deadrise (about a tenth of the dynamic pressure at 18°), and Rudder Area Cm2 does not resist sliding at all (its
	 * force follows only the rudder's angle), so a planing hull without a keel skates outward in a hard turn. Typical: a jet ski
	 * 1000-2000 (Keel Location about a third of its length from the stern, at the keel line); a 5 m runabout 1500-3000; an 8 m RIB
	 * 4000-8000; 0 for floating props, which should drift freely. Get Readout → Drift Angle Deg and Keel Force N show its effect.
	 * Default 0 (off). At least 0.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Keel", meta = (ClampMin = "0"))
	float KeelAreaCm2 = 0.f;

	/** Where the keel's side force acts: the centre of its lateral area (the hull's centre of lateral resistance), in the body's frame: cm
	 * from the floating primitive's origin along its axes, at world size (the component's scale is not applied), as Thrust Location. Its
	 * Z is the middle of Keel Depth Cm: put it at the keel line (the hull's bottom). Aft of the centre of mass (a jet ski or runabout:
	 * about a third of the length from the stern) the force also resists yawing and keeps the boat running straight, as a skeg does; near
	 * the centre of mass it fights the slide alone and leaves the turning to the rudder; forward of it the boat turns into its own slide
	 * and can spin out. Well below the centre of mass it also heels the boat outward in a hard turn, as a sailing keel does: raise it
	 * toward the centre of mass (it still has to be under the water) if the boat leans out too far. Default 0 (the origin). */
	UPROPERTY(EditAnywhere, Category = "Keel", meta = (EditCondition = "KeelAreaCm2 > 0", Units = "cm"))
	FVector KeelLocation = FVector::ZeroVector;

	/** The keel's vertical span, centred on Keel Location. The share of it under the water scales the force (a hull riding high on the
	 * plane grips less, an airborne one not at all), and with Keel Area Cm2 it sets the keel's aspect ratio AR = 2 × span² ÷ area (the
	 * hull above mirrors it): a deeper keel of the same area grips harder at a small slide. Typical: a jet ski or runabout 10-20; an 8 m
	 * RIB 20-30; a sailing yacht's fin keel 100 or more. In cm. Default 15. At least 1. */
	UPROPERTY(EditAnywhere, Category = "Keel", meta = (EditCondition = "KeelAreaCm2 > 0", ClampMin = "1", Units = "cm"))
	float KeelDepthCm = 15.f;

	/** The crossflow drag coefficient of the slide: the part of the keel's force that grows with the square of the sideways speed (this ×
	 * v × |v|). It holds a hull sliding at a large angle, or drifting sideways at a crawl, where the lift part (which needs way ahead)
	 * fades. 1 suits a keel or a flat hull side; higher grips harder at large slides; 0 leaves the lift alone. Default 1. Range 0-2. */
	UPROPERTY(EditAnywhere, Category = "Keel", meta = (EditCondition = "KeelAreaCm2 > 0", ClampMin = "0", ClampMax = "2"), AdvancedDisplay)
	float KeelCrossflowCoefficient = 1.f;

	/** A propeller and rudder (body frame, the bow along +X). Throttle and Rudder are set from Blueprint, or the console for testing
	 * (fnb.Boat.Throttle, fnb.Boat.Rudder) when Console Control is on. Default off. */
	UPROPERTY(EditAnywhere, Category = "Propulsion")
	bool bPropulsion = false;

	/** For testing: a change of fnb.Boat.Throttle or fnb.Boat.Rudder sets this body's Throttle and Rudder. Off: only Blueprint sets
	 * them. Default on. */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion"))
	bool bConsoleControl = true;

	/** How the propeller is driven. Simple (the default): Throttle × Max Thrust Newtons while the thrust point is under the water, and Prop
	 * Wash Speed over the rudder; for rowing, paddles, scripted pushes. Propeller: an engine (Engine Power Kw, Engine Max Rpm, Engine
	 * Response Seconds) through Gear Ratio to a propeller (Prop Diameter Cm, Prop Pitch Ratio) with real open-water curves: the boat finds
	 * its own acceleration and top speed, the throttle's sign shifts gear, the disc ventilates when it breaks the surface, its torque leans
	 * the hull and its jet washes the rudder; for motorboats (pair it with Hull Flow Planing). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Propulsion", meta = (EditCondition = "bPropulsion"))
	EFluidNinjaPropulsionModel PropulsionModel = EFluidNinjaPropulsionModel::Simple;

	/** Propeller model: the engine's power at Engine Max Rpm, kilowatts (hp × 0.746). More gives harder acceleration and, with a propeller
	 * sized for it, a higher top speed. A 5 m runabout: 40-90 kW; a jet ski 80-230; a 7 m wake boat 250-350. Default 60. Range 1-2000. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Propulsion", meta = (EditCondition = "bPropulsion && PropulsionModel == EFluidNinjaPropulsionModel::Propeller", ClampMin = "1", ClampMax = "2000"))
	float EnginePowerKw = 60.f;

	/** Propeller model: the engine's rev limit, rev/min, where it gives Engine Power Kw (its torque peaks lower, at 1.25 times the rated
	 * torque from a standstill). An outboard: 5000-6300; an inboard: 3000-5500. Higher, with the same gear and propeller, lets the
	 * propeller turn faster and the boat go faster if the engine has the power. Default 5800. Range 1000-15000. */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion && PropulsionModel == EFluidNinjaPropulsionModel::Propeller", ClampMin = "1000", ClampMax = "15000"))
	float EngineMaxRpm = 5800.f;

	/** Propeller model: the engine's idle, rev/min: in gear at no throttle the propeller turns at this ÷ Gear Ratio and the boat creeps
	 * (zero throttle is neutral: no creep). Default 800. Range 300 up to 0.9 × Engine Max Rpm. */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion && PropulsionModel == EFluidNinjaPropulsionModel::Propeller", ClampMin = "300"), AdvancedDisplay)
	float EngineIdleRpm = 800.f;

	/** Propeller model: how long the engine takes to catch up with the throttle (the time to close about two thirds of a change), s.
	 * Longer feels heavier and slower to answer; shorter snaps. Default 0.4. Range 0.01-5. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Propulsion", meta = (EditCondition = "bPropulsion && PropulsionModel == EFluidNinjaPropulsionModel::Propeller", ClampMin = "0.01", ClampMax = "5", Units = "s"))
	float EngineResponseSeconds = 0.4f;

	/** Propeller model: the engine's turns per propeller turn (an outboard's lower unit: 1.8-2.6; an inboard's gearbox: 1-3). Higher turns
	 * the propeller slower and pushes harder at low speed; lower spins it faster for top speed. Default 2. Range 0.25-10. */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion && PropulsionModel == EFluidNinjaPropulsionModel::Propeller", ClampMin = "0.25", ClampMax = "10"))
	float GearRatio = 2.f;

	/** Propeller model: the propeller's diameter, cm. The power it can absorb grows with the fifth power of it: too big and the engine never
	 * reaches its rev limit (slow, strong), too small and it over-revs (fast, weak at low speed). Size it with Prop Pitch Ratio for the
	 * engine: a 60 kW outboard turns about a 28 cm (11 in) propeller; a 250 kW inboard about 40 cm. Default 28. Range 5-300. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Propulsion", meta = (EditCondition = "bPropulsion && PropulsionModel == EFluidNinjaPropulsionModel::Propeller", ClampMin = "5", ClampMax = "300", Units = "cm"))
	float PropDiameterCm = 28.f;

	/** Propeller model: the pitch over the diameter: how far one turn would screw the propeller forward, over its diameter. Higher pushes
	 * the boat faster at the same rpm (and loads the engine more); lower climbs out of the hole harder and tops out sooner. Planing boats:
	 * 1.1-1.6; displacement hulls and tugs: 0.6-1.0. It sets the generic four-blade curves (when Prop Thrust Curve and Prop Torque Curve
	 * are empty). Default 1.3. Range 0.4-2. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Propulsion", meta = (EditCondition = "bPropulsion && PropulsionModel == EFluidNinjaPropulsionModel::Propeller", ClampMin = "0.4", ClampMax = "2"))
	float PropPitchRatio = 1.3f;

	/** Propeller model: an outboard or a sterndrive: the propeller's thrust turns with the rudder angle (Max Rudder Angle × Rudder), so it
	 * steers hard even at low speed; Rudder Area Cm2 then stands for the lower unit's own side force, which lifts on the boat's own flow
	 * alone (the unit turns with its own jet, so neither the jet nor Prop Wash Speed adds to it). Off: a fixed shaft and a rudder in its
	 * wash. Simple uses it too. Default off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Propulsion", meta = (EditCondition = "bPropulsion"))
	bool bSteerableThrust = false;

	/** Propeller model, optional: the propeller's thrust coefficient KT against the advance ratio J = Va / (n D) (Va the water's speed into
	 * the disc, n its rev/s, D its diameter), ahead, for J from 0 up. Empty (the default): the generic four-blade series for Prop Pitch
	 * Ratio. Use a manufacturer's open-water curve for a specific propeller (KQ goes in Prop Torque Curve; both or neither). */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion && PropulsionModel == EFluidNinjaPropulsionModel::Propeller"), AdvancedDisplay)
	FRuntimeFloatCurve PropThrustCurve;

	/** Propeller model, optional: the propeller's torque coefficient KQ against the advance ratio J, ahead (see Prop Thrust Curve). Used
	 * only with Prop Thrust Curve. Empty (the default): the generic series. */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion && PropulsionModel == EFluidNinjaPropulsionModel::Propeller"), AdvancedDisplay)
	FRuntimeFloatCurve PropTorqueCurve;

	/** Propeller model: the hull's wake at the propeller: it meets the water at (1 - this) of the boat's speed through it. Planing hulls
	 * and outboards: 0-0.1; displacement hulls with the shaft under the hull: 0.15-0.35. Higher loads the propeller more at speed.
	 * Default 0.05. Range 0-0.9. */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion && PropulsionModel == EFluidNinjaPropulsionModel::Propeller", ClampMin = "0", ClampMax = "0.9"), AdvancedDisplay)
	float PropWakeFraction = 0.05f;

	/** Propeller model: astern, the propeller's thrust and torque coefficients are this share of ahead's (its blades work backwards).
	 * Default 0.6. Range 0-1. */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion && PropulsionModel == EFluidNinjaPropulsionModel::Propeller", ClampMin = "0", ClampMax = "1"), AdvancedDisplay)
	float ReverseEfficiency = 0.6f;

	/** Propeller model: while the propeller is ventilated (air drawn onto its blades), its thrust and torque are this share of what they
	 * would be: the boat bogs and the engine flares. Lower makes ventilation harsher. Default 0.45. Range 0-1. */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion && PropulsionModel == EFluidNinjaPropulsionModel::Propeller", ClampMin = "0", ClampMax = "1"), AdvancedDisplay)
	float VentilatedThrustFactor = 0.45f;

	/** Propeller model: the engine's and gearbox's rotating parts, at the engine shaft, kg·m². More makes the rpm answer the throttle and
	 * the load more slowly. An outboard's: about 0.05-0.15; an inboard's with flywheel 0.2-1. Default 0.1. At least 0.001. */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion && PropulsionModel == EFluidNinjaPropulsionModel::Propeller", ClampMin = "0.001"), AdvancedDisplay)
	float EngineInertiaKgM2 = 0.1f;

	/** Propeller model: a left-handed propeller (anticlockwise seen from astern going ahead): its torque leans the hull to starboard instead
	 * of to port. Default off (right-handed). */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion && PropulsionModel == EFluidNinjaPropulsionModel::Propeller"), AdvancedDisplay)
	bool bLeftHandedProp = false;

	/** Where the propeller pushes, in the body's frame: cm from the floating primitive's origin along its axes, at world size (the
	 * component's scale is not applied). Simple: it pushes only while this point is under the water. Propeller: the disc's centre (it
	 * pushes by the share of the disc under the water): put it below the hull's bottom at the stern, as an outboard's propeller hangs.
	 * Default 0 (the origin). */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion", Units = "cm"))
	FVector ThrustLocation = FVector::ZeroVector;

	/** Which way the propeller pushes at full ahead, in the body's frame (zero: forward, +X); astern pushes the other way. Only its
	 * direction counts (it is normalised). Default +X. */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion"))
	FVector ThrustDirection = FVector::ForwardVector;

	/** Propulsion Model Simple only: the propeller's push at Throttle 1 (and astern at -1), newtons. More drives the boat faster and
	 * accelerates it harder. The Propeller model does not use it (its thrust comes from the engine and the propeller's curves). Default
	 * 1500. At least 0; 0 gives no thrust. */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion && PropulsionModel == EFluidNinjaPropulsionModel::Simple", ClampMin = "0"))
	float MaxThrustNewtons = 1500.f;

	/** Propulsion Model Simple with a fixed shaft (Steerable Thrust off) only: the propeller's wash over the rudder at full throttle, so the
	 * boat steers when slow: added to the water's speed past the rudder, times Throttle (astern takes it away). Higher steers harder at
	 * low speed; 0 leaves the rudder to the boat's own speed. The Propeller model takes the wash from its own jet, and a steerable drive
	 * gets none (its thrust already swings). In cm/s. Default 300. At least 0. */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion && PropulsionModel == EFluidNinjaPropulsionModel::Simple && !bSteerableThrust", ClampMin = "0", Units = "cm/s"))
	float PropWashSpeed = 300.f;

	/** Where the rudder sits, in the body's frame: cm from the floating primitive's origin along its axes, at world size (the component's
	 * scale is not applied). It steers only while this point is under the water. Put it aft of the centre of mass (at the stern): its side
	 * force turns the boat by its lever arm, and at the default 0 (the origin, near the centre of mass) it pushes the boat sideways
	 * rather than turning it. */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion", Units = "cm"))
	FVector RudderLocation = FVector::ZeroVector;

	/** The rudder blade's area, cm²: its side force grows with it and with the square of the water's speed past the rudder (the boat's
	 * speed through the water plus the propeller's wash behind a fixed shaft; a steerable drive's lower unit takes the boat's speed alone).
	 * Bigger turns the boat harder. It does not resist the hull sliding sideways (its force follows only the rudder's angle): give a boat
	 * that slides through its turns a Keel Area Cm2. Default 900 (a 30 x 30 cm blade). At least 0; 0: no rudder. */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion", ClampMin = "0"))
	float RudderAreaCm2 = 900.f;

	/** The rudder's angle at Rudder -1 or 1. Its side force grows with the angle only up to about 11 degrees (lift coefficient 2π × the
	 * angle in radians, capped at 1.2), so a larger angle reaches that force at a smaller Rudder input and adds none beyond it. In
	 * degrees. Default 35. Range 0-60. */
	UPROPERTY(EditAnywhere, Category = "Propulsion", meta = (EditCondition = "bPropulsion", ClampMin = "0", ClampMax = "60", Units = "deg"))
	float MaxRudderAngle = 35.f;

	/** The propeller's setting: -1 (full astern) to 1 (full ahead); 0 (the default) stops it. Clamped to that range. Simple: a share of Max
	 * Thrust Newtons. Propeller: |Throttle| is the engine's throttle, which the engine catches up with over Engine Response Seconds; its
	 * sign selects the gear (ahead above 0, astern below 0), and within ±0.02 of 0 the drive is in neutral with the engine idling; Max
	 * Thrust Newtons is not used. Set it from Blueprint, or with fnb.Boat.Throttle when Console Control is on. Needs Propulsion on. */
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
	 * burst's points use it too. Bigger stirs a wider band of water around the hull. Scale it with the hull: 60 suits an 8 m planing
	 * boat, while on a short, fast hull (a 5.5 m airboat at 17 m/s) it draws a white halo and a wedge of lace, and about 25 cm keeps the
	 * lanes dark. In cm. Default 60. At least 1. */
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

	/** Continuous spray sheets while the hull drives fast through the water: a whisker sheet off each side where the water line drives
	 * in hardest (the chines of a planing hull), launched at 2 V sin β (V the hull's speed through the water, β Spray Deadrise Deg) out
	 * and up, and a rooster tail behind a fast propelled stern. Persistent Niagara emitters (Spray Sheet System) that follow the hull,
	 * outside the slam splashes' budget; where their drops come down they leave fresh foam (Spray Landing Foam; FluidNinja Waves
	 * interface version 6). Drawn only within Lod Distances Y, while the body is seen and awake, and while fnb.Effects.SpraySheets is on
	 * (sg.WaterQuality: off at Low and Medium). Default off; the Motorboat preset turns it on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray|Sheets")
	bool bSpraySheets = false;

	/** The spray sheets' Niagara system: it must take the user parameters SprayRate (particles/s), SprayVelocity (world cm/s), SprayCone
	 * (degrees), DropletSize (cm) and WaterZ (cm, where drops die). None (the default): the project default (Project Settings → FluidNinja
	 * Buoyancy → Effects → Spray Sheet System; NS_FNB_HullSpray as the plugin ships). */
	UPROPERTY(EditAnywhere, Category = "Spray|Sheets", meta = (EditCondition = "bSpraySheets"))
	TSoftObjectPtr<UNiagaraSystem> SpraySheetSystem;

	/** The sheets start when the hull drives through the water at least this fast (and stop below 85 % of it). Lower lets a slower hull
	 * throw sheets. In cm/s. Default 400 (where a runabout is climbing onto the plane). At least 0. */
	UPROPERTY(EditAnywhere, Category = "Spray|Sheets", meta = (EditCondition = "bSpraySheets", ClampMin = "0", Units = "cm/s"))
	float SpraySheetMinSpeedCmS = 400.f;

	/** The hull bottom's deadrise, degrees: the V of a planing bottom. The whisker sheet leaves at 2 V sin β in the water's frame, so a
	 * deeper V throws it faster and higher (18°: 6.2 m/s at 10 m/s). Flat bottoms 0-10, runabouts 14-20, deep-V offshore hulls 22-26.
	 * Default 18. Range 0-45. */
	UPROPERTY(EditAnywhere, Category = "Spray|Sheets", meta = (EditCondition = "bSpraySheets", ClampMin = "0", ClampMax = "45", Units = "deg"))
	float SprayDeadriseDeg = 18.f;

	/** Scales how many drops each sheet throws (their spawn rate), not how fast or high. Higher thickens the sheets (more particles drawn);
	 * lower thins them. fnb.Effects.SprayRateScale scales it again (sg.WaterQuality). Default 1. Range 0-4. */
	UPROPERTY(EditAnywhere, Category = "Spray|Sheets", meta = (EditCondition = "bSpraySheets", ClampMin = "0", ClampMax = "4"))
	float SpraySheetRate = 1.f;

	/** A rooster tail: a plume thrown up behind the stern of a fast propelled planing hull (Propulsion on and Hull Flow Planing on the
	 * Triangles hull model; other hulls throw none), rising with the speed (up to about 7 m/s up) and falling behind the boat. Default on;
	 * turn it off for a hull that should throw only its chine sheets. */
	UPROPERTY(EditAnywhere, Category = "Spray|Sheets", meta = (EditCondition = "bSpraySheets"))
	bool bRoosterTail = true;

	/** The rooster tail starts at this speed through the water (and stops below 85 % of it). In cm/s. Default 800. At least 0. */
	UPROPERTY(EditAnywhere, Category = "Spray|Sheets", meta = (EditCondition = "bSpraySheets && bRoosterTail", ClampMin = "0", Units = "cm/s"))
	float RoosterTailMinSpeedCmS = 800.f;

	/** Fresh foam where the sheets' drops come down (predicted from their launch, the water's height and gravity), foam per second at the
	 * landing patch's centre per unit of sheet. Higher leaves whiter lines beside and behind the hull; 0 none. Needs FluidNinja Waves
	 * interface version 6 and the foam on. Default 1. Range 0-5. */
	UPROPERTY(EditAnywhere, Category = "Spray|Sheets", meta = (EditCondition = "bSpraySheets", ClampMin = "0", ClampMax = "5"))
	float SprayLandingFoam = 1.f;

	/** The propeller's whitewater (Propulsion on, Propulsion Model Propeller): the jet's boil behind the stern as fresh foam and the
	 * bubbles it drags down as aged foam (FluidNinja Waves' second foam channel, the long pale trail), stronger with the jet's excess speed
	 * and how much of the disc is under the water; astern it runs forward with the jet. Higher whitens the trail; 0 none. Needs FluidNinja
	 * Waves interface version 6 and the foam on. Default 1. Range 0-5. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (EditCondition = "bPropulsion && PropulsionModel == EFluidNinjaPropulsionModel::Propeller", ClampMin = "0", ClampMax = "5"))
	float PropWashFoam = 1.f;

	/** The stern's whitewater (Hull Flow Planing on the Triangles hull model): once the transom runs dry (about 2.5 √(g × the rest draft),
	 * 2.9 m/s for a 14 cm draft) the water closing behind it breaks into a white trail about a beam wide. Higher whitens it; 0 none. Needs
	 * FluidNinja Waves interface version 6 and the foam on. Default 1. Range 0-5. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (EditCondition = "HullFlow == EFluidNinjaHullFlow::Planing", ClampMin = "0", ClampMax = "5"))
	float SternWashFoam = 1.f;

	/** How many propeller jets the propeller's whitewater (Prop Wash Foam) comes from. The propeller model drives one propeller; a boat
	 * with twin (or triple, quad) outboards gets its thrust from it, but its trail is drawn from this many jets side by side, Prop Wash Jet
	 * Spacing Cm apart: two white streams that merge into one broad band a few metres behind the stern. Default 1; range 1-4. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (EditCondition = "bPropulsion && PropulsionModel == EFluidNinjaPropulsionModel::Propeller", ClampMin = "1", ClampMax = "4", UIMin = "1", UIMax = "4"))
	int32 PropWashJets = 1;

	/** The distance between neighbouring propeller jets (Prop Wash Jets above 1), cm, centred on Thrust Location. Twin outboards on a
	 * 7-9 m boat sit about 65-80 cm apart. Default 70; at least 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (EditCondition = "bPropulsion && PropulsionModel == EFluidNinjaPropulsionModel::Propeller && PropWashJets > 1", ClampMin = "0", Units = "cm"))
	float PropWashJetSpacingCm = 70.f;

	/** How fast the propeller's white trail (Prop Wash Foam) widens behind the stern, degrees each side: the jets' boil spreads at about
	 * this angle and the trail's sideways scatter grows with it. Lower keeps a narrow, transom-wide stream with dark water beside it (a
	 * planing boat's prop wash on open water: 1-3); higher fans it out into a broad band (a slow, heavy boat's churn: 6-10). Default 7;
	 * range 0-20. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (EditCondition = "bPropulsion && PropulsionModel == EFluidNinjaPropulsionModel::Propeller", ClampMin = "0", ClampMax = "20", Units = "deg"))
	float PropWashSpreadDeg = 7.f;

	/** An airboat's centre trail: its air propeller blows the slipstream back over the stern and down onto the water behind it, which
	 * ruffles and whitens a milder trail than a submerged propeller's boil: one broad, soft stream about the fan's width, from the transom
	 * back, widening at Air Fan Wash Spread Deg and fading aft. The hull's own quarter wakes, stern churn and sides form around it as on any
	 * planing hull. Its strength follows Air Fan Throttle, which the owner sets every frame from its own engine; the component applies no
	 * thrust for it, so keep Propulsion off and let the Blueprint push the boat at its fan. Use it instead of Prop Wash Foam on a boat driven
	 * by an air fan. Higher whitens the trail; 0 none (the default). An airboat's trail is about a third to a half as white as a
	 * twin-outboard boat's (Prop Wash Foam 0.7): 0.2-0.4. Needs the hull moving, FluidNinja Waves interface version 6 and the foam on.
	 * Range 0-5. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake|Air Fan", meta = (ClampMin = "0", ClampMax = "5"))
	float AirFanWashFoam = 0.f;

	/** How hard the air fan blows now, 0-1, for Air Fan Wash Foam: set it every frame from the owner's engine (an airboat Blueprint's
	 * applied throttle). 0 lays no fan wash, 1 the full Air Fan Wash Foam. Runtime state, not saved. Default 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Transient, Category = "Wake|Air Fan", meta = (ClampMin = "0", ClampMax = "1"))
	float AirFanThrottle = 0.f;

	/** The air fan's hub, cm, in the hull primitive's space (as Thrust Location): the fan wash's stream is centred on its Y and starts at
	 * the transom, or at the fan if the fan hangs behind the transom. Its height does not matter. Default (0, 0, 0). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake|Air Fan", meta = (EditCondition = "AirFanWashFoam > 0"))
	FVector AirFanLocation = FVector::ZeroVector;

	/** The air fan's diameter, cm: the fan wash's stream starts about this wide and runs about four diameters plus most of the hull's
	 * length behind the transom before it has lost its strength. Airboat propellers are 150-210 cm. Default 180; range 50-400. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake|Air Fan", meta = (EditCondition = "AirFanWashFoam > 0", ClampMin = "50", ClampMax = "400", Units = "cm"))
	float AirFanDiameterCm = 180.f;

	/** How fast the fan wash's stream widens behind the stern, degrees each side: an air propeller's slipstream spreads more than a water
	 * jet, about 8-12 degrees. Lower keeps a narrow centre streak; higher fans it into a broad, faint band. Default 10; range 0-30. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake|Air Fan", meta = (EditCondition = "AirFanWashFoam > 0", ClampMin = "0", ClampMax = "30", Units = "deg"))
	float AirFanWashSpreadDeg = 10.f;

	/** The quarter wakes' whitewater (Hull Flow Planing on the Triangles hull model): the diverging waves a fast hull throws off its transom
	 * corners break, and leave two ragged white bands that spread outward behind the boat at the wake angle, with darker water between
	 * them and the propeller's trail. The angle narrows with speed as for real planing boats (Kelvin's 19.5 degrees at low Froude numbers,
	 * about 1 / (sqrt(2 pi) Fr) above; 14 degrees for an 8 m boat at 15 m/s); the bands start near the hump and are whitest on the plane.
	 * Higher whitens them; 0 none. Needs FluidNinja Waves interface version 6 and the foam on. Default 1; range 0-5. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (EditCondition = "HullFlow == EFluidNinjaHullFlow::Planing", ClampMin = "0", ClampMax = "5"))
	float QuarterWakeFoam = 1.f;

	/** How far behind the transom the quarter wakes keep breaking, in hull lengths. Longer draws longer white bands (and spreads the same
	 * foam over them). Default 4; range 0.5-10. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (EditCondition = "HullFlow == EFluidNinjaHullFlow::Planing && QuarterWakeFoam > 0", ClampMin = "0.5", ClampMax = "10"))
	float QuarterWakeLength = 4.f;

	/** Scales the angle the quarter wakes' white bands run out at, from the wake angle the speed gives (Quarter Wake Foam). The breaking
	 * crests of a planing hull's quarter waves stay closer to its track than the wave pattern's own edge: photographed twin-outboard boats
	 * show band centres about half the wake angle out, the bands widening outward. 1 lays them along the wake angle; 0.5 at half of it.
	 * Default 1; range 0.2-1.5. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (EditCondition = "HullFlow == EFluidNinjaHullFlow::Planing && QuarterWakeFoam > 0", ClampMin = "0.2", ClampMax = "1.5"))
	float QuarterWakeAngleScale = 1.f;

	/** How far behind the transom the quarter wakes start breaking, in hull lengths. The quarter waves steepen for a few metres behind the
	 * transom before their crests break, so a fast boat's white arms begin out beside its track and leave dark water between them and the
	 * propeller's trail; foam laid right at the transom corners is left behind as two lines along the track that fill that dark water.
	 * 0 starts them at the corners; 0.2-0.4 gives the dark lanes of a planing boat's wake. Default 0; range 0-2. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (EditCondition = "HullFlow == EFluidNinjaHullFlow::Planing && QuarterWakeFoam > 0", ClampMin = "0", ClampMax = "2"))
	float QuarterWakeStart = 0.f;

	/** How far outside each transom corner its quarter wake starts breaking, in hull beams. A planing hull's quarter waves rise just outside
	 * its chines and break there, so a fast boat's white arms begin a little out from its sides and leave dark water between them and the
	 * propeller's trail from the transom on, where Quarter Wake Start would move the arms' start aft instead. 0 starts them at the corners;
	 * 0.1-0.3 gives the dark lanes of a twin-outboard boat's wake right behind its transom. Default 0; range 0-1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (EditCondition = "HullFlow == EFluidNinjaHullFlow::Planing && QuarterWakeFoam > 0", ClampMin = "0", ClampMax = "1"))
	float QuarterWakeOffset = 0.f;

	/** The quarter wakes' breaking crests: the white arms gather into diagonal streaks, one per crest, which stay on the water where they
	 * broke and give the arms the saw-toothed outer edge of a real planing wake (the crests run outward and aft at about 55 degrees to the
	 * track, square to the waves at Kelvin's cusp). The streaks' spacing along the track, in cm; a planing boat's crests break about every
	 * 2-4 m. 0 lays even arms. Default 0; range 0-2000. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (EditCondition = "HullFlow == EFluidNinjaHullFlow::Planing && QuarterWakeFoam > 0", ClampMin = "0", ClampMax = "2000", Units = "cm"))
	float QuarterWakeCrestSpacingCm = 0.f;

	/** How much of the quarter wakes' foam gathers on their breaking crests (Quarter Wake Crest Spacing Cm): 0 even arms, 1 white streaks
	 * with darker water between them. The arms keep the same foam overall. Default 0.6; range 0-1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (EditCondition = "HullFlow == EFluidNinjaHullFlow::Planing && QuarterWakeFoam > 0 && QuarterWakeCrestSpacingCm > 0", ClampMin = "0", ClampMax = "1"))
	float QuarterWakeCrestStrength = 0.6f;

	/** The stern ridge (Hull Flow Planing on the Triangles hull model): the water closing behind a planing hull's dry transom rises into a
	 * ridge along its track, the mound a rooster tail stands on, heaped up further by the propellers' boil. A linear wake of the hull's
	 * own pressure leaves a hollow there for tens of metres instead, so seen from the stern a fast boat's wake shows a trench where a real
	 * one shows a raised white ridge. This pushes the water up behind the transom once it runs dry, steadily as the planing pressure goes
	 * in (no impact foam), from the transom back over Stern Ridge Length, strongest half way. The head of the push, in cm: the mound it
	 * would raise standing still; under way the waves carry it aft and lower it. The boat does not feel its own ridge. 0 (the default):
	 * none; 15-40 lifts a fast twin-outboard boat's centre line into a ridge. Range 0-80. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (EditCondition = "HullFlow == EFluidNinjaHullFlow::Planing", ClampMin = "0", ClampMax = "80", Units = "cm"))
	float SternRidgeHeightCm = 0.f;

	/** How far behind the transom the stern ridge's push reaches, in hull lengths (Stern Ridge Height Cm). Longer lifts a longer stretch
	 * of the track. Default 1; range 0.1-4. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (EditCondition = "HullFlow == EFluidNinjaHullFlow::Planing && SternRidgeHeightCm > 0", ClampMin = "0.1", ClampMax = "4"))
	float SternRidgeLength = 1.f;

	/** The stern ridge's width at half its height, in hull beams (Stern Ridge Height Cm). Twin outboards' ridge is about half a beam wide.
	 * Wider spreads the same head over more water. Default 0.5; range 0.1-2. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (EditCondition = "HullFlow == EFluidNinjaHullFlow::Planing && SternRidgeHeightCm > 0", ClampMin = "0.1", ClampMax = "2"))
	float SternRidgeWidth = 0.5f;

	/** The quarter waves' lift (Hull Flow Planing on the Triangles hull model): the divergent waves a planing stern throws off its quarters
	 * stand up as two white walls running out behind it, where a linear wake of the hull's own pressure leaves them a few centimetres
	 * high. This pushes the water up along each quarter wake's line (where its white band runs: Quarter Wake Start behind the corner,
	 * Quarter Wake Offset outside it, at the wake angle the speed and Quarter Wake Angle Scale give), steadily as the planing pressure goes
	 * in (no impact foam), strongest half way along Quarter Wake Lift Length, once the hull is on the plane. The head of the push, in cm:
	 * the mound it would raise standing still. The boat does not feel it. 0 (the default): none; 15-35 for a fast twin-outboard boat.
	 * Range 0-80. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (EditCondition = "HullFlow == EFluidNinjaHullFlow::Planing", ClampMin = "0", ClampMax = "80", Units = "cm"))
	float QuarterWakeLiftCm = 0.f;

	/** How far along each quarter wake its lift reaches, in hull lengths (Quarter Wake Lift Cm). Default 1.5; range 0.2-4. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (EditCondition = "HullFlow == EFluidNinjaHullFlow::Planing && QuarterWakeLiftCm > 0", ClampMin = "0.2", ClampMax = "4"))
	float QuarterWakeLiftLength = 1.5f;

	/** Each quarter wake lift's width at half its height, in hull beams (Quarter Wake Lift Cm). Narrower stands the walls up steeper.
	 * Default 0.4; range 0.1-2. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (EditCondition = "HullFlow == EFluidNinjaHullFlow::Planing && QuarterWakeLiftCm > 0", ClampMin = "0.1", ClampMax = "2"))
	float QuarterWakeLiftWidth = 0.4f;

	/** How long the stern ridge and the quarter wake lift take to die away once the hull slows below the speeds that make them (Stern Ridge
	 * Height Cm, Quarter Wake Lift Cm), in seconds from full to none. Their push follows the speed up at once but fades no faster than
	 * this, so a boat that slows hard lets its mounds settle instead of dropping them in a frame, which would slosh as a wave of its own.
	 * Below 30 cm/s through the water a hull pushes neither, whatever is left. 0: they follow the speed both ways. Default 2; range 0-10. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (EditCondition = "HullFlow == EFluidNinjaHullFlow::Planing", ClampMin = "0", ClampMax = "10", Units = "s"))
	float SternWaveFadeSeconds = 2.f;

	/** The whitewater along the hull's sides (powered or planing hulls): the white rim where the spray root and the water peeling off the
	 * chines run aft along the sides into the quarter wakes, and the bow wave breaking at the stem when the hull pushes hard into the water.
	 * Higher whitens it; 0 none. Needs FluidNinja Waves interface version 6 and the foam on. Default 1; range 0-5. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (ClampMin = "0", ClampMax = "5"))
	float HullSideFoam = 1.f;

	/** How broken the whitewater is (all of it: the propeller trail, the transom churn, the quarter wakes and the sides). 0 lays smooth,
	 * even bands; higher scatters it into the lacy, ragged streaks of real whitewater, flickering from frame to frame. Default 0.6;
	 * range 0-1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (ClampMin = "0", ClampMax = "1"))
	float WhitewaterRaggedness = 0.6f;

	/** How many foam patches this body's whitewater is drawn with each frame (a quarter of it while the body is far or unseen). More
	 * gives finer lace; the foam's overall whiteness does not change with it. A water simulation takes at most 1024 patches a frame from
	 * all its bodies together. Default 240; range 16-600. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wake", meta = (ClampMin = "16", ClampMax = "600"), AdvancedDisplay)
	int32 WhitewaterPatches = 240;

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

	/** Propeller model: an engine loop played while the propeller model runs, attached at the propeller. Float inputs a MetaSound reads are
	 * kept up to date on it: "RPM" (the engine, rev/min), "Load" (0-1.5: how hard it works), "Throttle" (0-1: the lever's position, ahead or
	 * astern, before the engine's response lag), "Ventilated" (1 while the propeller is ventilated: the flare) and "Speed" (the
	 * hull's speed through the water, cm/s), each sent only when it changes. Its volume is Sound Volume. None (the default): the project
	 * default (Project Settings → FluidNinja Buoyancy → Effects → Engine Sound; no sound ships with the plugin), and with that none. */
	UPROPERTY(EditAnywhere, Category = "Sound")
	TSoftObjectPtr<USoundBase> EngineSound;

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
	/** The whitewater this body adds to FluidNinja Waves' foam this frame (its propeller's wash, its stern's, where its spray lands), as
	 * its tick this frame filled it; empty when it has not ticked this frame (its tick turned off, or no hull), so a body that stops
	 * ticking leaves no wash behind. The subsystem hands it to the waves. */
	const TArray<FFluidNinjaWaterWash>& GetWashes() const;
	/** This body's stern ridge and quarter wake lift for the waves this frame (Stern Ridge Height Cm, Quarter Wake Lift Cm): steady upward
	 * impulse pieces (world X, world Y, the
	 * impulse downward into the water over the frame in kg·cm/s, negative, and the splat's area in cm², negative: steady), as its tick this
	 * frame filled them; empty when it has not ticked this frame. The subsystem hands them to the waves with the body's own pieces. */
	const TArray<FVector4f>& GetSternRidge() const;
	/** Whether the planing physics runs on this body: Hull Flow Planing on the Triangles hull model (FillParams gives Voxels and Spheres
	 * the displacement flow). The stern's whitewater and the rooster tail follow it. */
	bool IsPlaningHull() const { return HullFlow == EFluidNinjaHullFlow::Planing && HullModel == EFluidNinjaHullModel::Triangles; }
	/** Whether the hull stays at its finest level of detail (a planing hull: its lift comes from the bottom's facets). */
	bool WantsFinestHull() const { return IsPlaningHull(); }

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
	/** The spray sheets (Spray Sheets) and the whitewater the body adds to the waves' foam (Washes), from the last readout and contacts. */
	void UpdateSheetsAndWash(UPrimitiveComponent& Primitive, float DeltaTime);
	/** One spray sheet: made the first time it is wanted, placed at Launch, its user parameters set; Rate 0 stops it (deactivated once, so
	 * its drops in the air land and the system completes), and a later Rate above 0 starts it afresh. */
	void DriveSheet(int32 Index, const FVector& Launch, const FVector& Velocity, float Rate, float ConeDeg, float DropletCm, float WaterZ);
	void UpdateSound(UPrimitiveComponent& Primitive, float DeltaTime);
	/** The engine loop: made here the first time the propeller model runs (MakeEngineAudio), then kept up to date. */
	void UpdateEngineSound(UPrimitiveComponent& Primitive, float DeltaTime);
	/** Makes the engine loop (EngineAudio) from Engine Sound, else the project's, at the propeller on Primitive, and remembers that it
	 * tried (bEngineAudioTried) whether or not the sound loaded. Begin Play and UpdateEngineSound share it. */
	void MakeEngineAudio(UPrimitiveComponent& Primitive);
	/** Loads the spray sheets' system (Spray Sheet System, else the project's) into LoadedSheetSystem, remembers that it tried
	 * (bSheetSystemTried), and warns once when a system that is set does not load. */
	void LoadSheetSystem();
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
	TObjectPtr<UAudioComponent> EngineAudio;
	/** The engine loop's values last sent (RPM, Load, Throttle, Ventilated, Speed): sent again only when they change. */
	float EngineSent[5] = {-1.f, -1.f, -1.f, -1.f, -1.f};
	/** MakeEngineAudio has run for the propeller model with this sound (Engine Sound, else the project's): a sound that did not load is
	 * tried again only once the propeller model is left and taken up again, or the sound changes. */
	bool bEngineAudioTried = false;
	FSoftObjectPath EngineAudioSoundTried;

	/** The spray sheets: port chine, starboard chine, rooster tail (made when first wanted), and whether each is running (hysteresis). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UNiagaraComponent>> Sheets;
	bool bSheetOn[3] = {false, false, false};

	UPROPERTY(Transient)
	TObjectPtr<UNiagaraSystem> LoadedSheetSystem;
	/** LoadSheetSystem has run for this system (Spray Sheet System, else the project's): a system that did not load is tried again only
	 * when that changes. */
	bool bSheetSystemTried = false;
	FSoftObjectPath SheetSystemTried;

	/** This frame's whitewater for the waves (GetWashes), and the frame (GFrameCounter) UpdateSheetsAndWash filled it on. */
	TArray<FFluidNinjaWaterWash> Washes;
	uint64 WashesFrame = 0;
	/** This frame's stern ridge pieces (GetSternRidge), filled with Washes on WashesFrame. */
	TArray<FVector4f> SternRidge;
	/** The stern ridge's and the quarter wake lift's level, 0-1: their speed ramps, faded down at Stern Wave Fade Seconds. */
	float SternRidgeLevel = 0.f;
	float QuarterLiftLevel = 0.f;

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
