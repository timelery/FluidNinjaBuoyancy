#pragma once

#include "Engine/DataAsset.h"
#include "FluidNinjaBuoyancyComponent.h"
#include "FluidNinjaBuoyancyPreset.generated.h"

/** How a preset builds the hull from the object's own mesh when it is applied. It sets the component's Hull Model; the three sphere
 * hulls also replace the component's Pontoons. */
UENUM(BlueprintType)
enum class EFluidNinjaPresetHull : uint8
{
	/** The simple collision's surface, cut at the water line, as a shell of triangles about Hull Resolution Cm in size (the component's
	 * Triangles model). Follows the shape best: boats, barrels, logs, anything with a shaped hull. */
	FromCollision,
	/** About Voxel Count cubes filling the simple collision (the component's Voxels model, Voxel Size 0): crates, blocks, debris. */
	Voxels,
	/** One sphere at the centre of the mesh's bounds, its radius the mean of the three half-extents times Sphere Radius Scale (at least
	 * 1 cm). Replaces the component's Pontoons: balls and round props. */
	OneSphere,
	/** Sphere Count spheres in a row along the object's longest axis, the end spheres just inside its bounds, each as thick as the
	 * object's thinner side (times Sphere Radius Scale). Replaces the component's Pontoons: arrows, sticks, poles. */
	SphereRow,
	/** Sphere Count spheres evenly round a ring in the plane of the object's two longer axes, reaching the edge of its bounds, each as
	 * thick as its shortest side, the ring's tube (times Sphere Radius Scale). Replaces the component's Pontoons: ring buoys, tyres. */
	SphereRing,
};

/** Where the body's mass comes from when the preset is applied. */
UENUM(BlueprintType)
enum class EFluidNinjaPresetMass : uint8
{
	/** Density Kg M3 times the volume of the object's simple collision (at its scale), written as the primitive's mass override (at
	 * least 1 g); the component's Mass Mode is set to Body Mass. A bigger object of the same stuff weighs more, so one preset serves
	 * every size. */
	Density,
	/** The component's Rest Draft mode at Rest Draft Cm: at Begin Play the body is given the mass its hull displaces that deep, so it
	 * floats that deep whatever its size or material. */
	RestDraft,
	/** The mass the object already has (Physics → Mass, or its mass override) is left alone; the component's Mass Mode is set to Body
	 * Mass, so it floats wherever that mass puts it, or sinks. */
	KeepBodyMass,
};

/** Which way up the body rests, from the object's shape. Written to the component's Rest Attitude, which Rest Draft, the stability
 * check and Settle On Start use. */
UENUM(BlueprintType)
enum class EFluidNinjaPresetAttitude : uint8
{
	/** As modelled: the component's Rest Attitude is set back to zero. */
	AsPlaced,
	/** Its longest axis lies flat on the water (a log, a bottle on its side). Only a mesh longest along Z is turned (a quarter turn
	 * that lays its Z along X); a mesh longest along X or Y lies flat already and gets a zero Rest Attitude. */
	LongestAxisFlat,
	/** Its longest axis stands up, its positive end on top (a bobber, a buoy). A mesh longest along Z already gets a zero Rest Attitude. */
	LongestAxisUp,
};

/**
 * A preset for the FluidNinja Buoyancy component: what an object is made of and how it behaves in the water, kept apart from its
 * shape. Applying it (right-click an actor → FluidNinja Buoyancy → Apply Preset, the component's Preset panel, or
 * UFluidNinjaBuoyancyPresetLibrary::ApplyPreset from Blueprint or Python) reads the object's own mesh — its bounds and simple
 * collision — builds the hull the preset asks for, sets the mass from the density and the volume, turns it the way it rests, and
 * writes the drag, wake and splash values. Everything it wrote stays editable on the component afterwards.
 *
 * Make your own with Content Browser → Miscellaneous → Data Asset → FluidNinja Buoyancy Preset; the plugin ships a set in
 * /FluidNinjaBuoyancy/Presets (Docs/Presets.md).
 */
UCLASS(BlueprintType, meta = (DisplayName = "FluidNinja Buoyancy Preset"))
class FLUIDNINJABUOYANCY_API UFluidNinjaBuoyancyPreset : public UDataAsset
{
	GENERATED_BODY()

public:
	/** A line saying what this preset makes of an object: the preset's tooltip in the Apply Preset menu, and shown in its details panel.
	 * Not written to the component. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Preset")
	FText Description;

	// ---- Hull

	/** How the hull is built from the object's mesh (see each option). Sets the component's Hull Model; the sphere hulls also replace
	 * its Pontoons. Default From Collision. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hull")
	EFluidNinjaPresetHull Hull = EFluidNinjaPresetHull::FromCollision;

	/** From Collision: the size of the triangles the hull's shell is rebuilt in, written to the component's Hull Resolution Cm. Smaller
	 * follows the shape and the water line more closely and costs more per physics step; larger is cheaper and coarser. In cm. Default
	 * 25 (the component's own default is 40). Applied as at least 2. The shipped presets use 15 (Barrel) to 30 (BoatHull). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hull", meta = (EditCondition = "Hull == EFluidNinjaPresetHull::FromCollision", ClampMin = "1", Units = "cm"))
	float HullResolutionCm = 25.f;

	/** Voxels: about how many cubes fill the simple collision, written to the component's Voxel Count (its Voxel Size is set to 0, so
	 * the count sets the cube size). More follows the shape more closely and costs more per physics step. Default 64 (the component's
	 * own default is 48). Range 1-512. The shipped presets use 64 (Crate) and 216 (6 x 6 x 6 on the blocks). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hull", meta = (EditCondition = "Hull == EFluidNinjaPresetHull::Voxels", ClampMin = "1", ClampMax = "512"))
	int32 VoxelCount = 64;

	/** Sphere Row and Sphere Ring: how many spheres. More spheres follow the row or the ring more closely. Default 8; at least 2. The
	 * shipped presets use 8 (RingBuoy) and 9 (Arrow). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hull", meta = (EditCondition = "Hull == EFluidNinjaPresetHull::SphereRow || Hull == EFluidNinjaPresetHull::SphereRing", ClampMin = "2"))
	int32 SphereCount = 8;

	/** The spheres' radius, as a multiple of the radius fitted to the object (One Sphere: the mean of its three half-extents; a row: the
	 * object's thinner half-extent; a ring: half the tube's thickness). Above 1 the spheres stand out past the object, below 1 they sit
	 * inside it: with Density or Keep Body Mass bigger spheres float it higher; with Rest Draft it still floats at Rest Draft Cm.
	 * Default 1 (every shipped preset); at least 0.1, and each sphere at least 1 cm in radius. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hull", meta = (EditCondition = "Hull != EFluidNinjaPresetHull::FromCollision && Hull != EFluidNinjaPresetHull::Voxels", ClampMin = "0.1"))
	float SphereRadiusScale = 1.f;

	// ---- Mass

	/** Where the body's mass comes from (see each option): Density and Keep Body Mass set the component's Mass Mode to Body Mass, Rest
	 * Draft sets it to Rest Draft. Default Density. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mass")
	EFluidNinjaPresetMass Mass = EFluidNinjaPresetMass::Density;

	/** Density: what the object is made of, kg/m³ (water 1000; pine 500, cork 240, ice 917, iron 7800; a beach ball about 8). Times the
	 * simple collision's volume it gives the mass written as the primitive's mass override (at least 0.001 kg). Lower floats higher;
	 * above 1000 it sinks. Default 500; at least 0.1. The shipped presets use 8 (BeachBall) to 7850 (SteelBlock). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mass", meta = (EditCondition = "Mass == EFluidNinjaPresetMass::Density", ClampMin = "0.1"))
	float DensityKgM3 = 500.f;

	/** Rest Draft: how deep the hull's lowest point sits in calm water at rest, written to the component's Rest Draft Cm; the body is
	 * given the mass its hull displaces that deep. Deeper makes it heavier, shallower lighter. In cm. Default 10 (the component's own
	 * default is 15). Applied as at least 0.5. The shipped presets use 1.2 (AirMattress) to 25 (Barrel). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mass", meta = (EditCondition = "Mass == EFluidNinjaPresetMass::RestDraft", ClampMin = "0.1", Units = "cm"))
	float RestDraftCm = 10.f;

	// ---- Balance

	/** Which way up it rests (see each option), written to the component's Rest Attitude. Default As Placed. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Balance")
	EFluidNinjaPresetAttitude Attitude = EFluidNinjaPresetAttitude::AsPlaced;

	/** Moves the centre of mass along the object's longest axis, as a share of its half-length (+0.3: a third of the way toward the
	 * axis' positive end). An arrow's steel head makes it float nose down; a bottle's base makes it stand up. 0 leaves it centred.
	 * Written to the component's Center Of Mass Offset, replacing any offset it had. Range -1 to 1. Default 0; the Arrow preset uses
	 * 0.32 (its mesh's longest axis must point +X toward the head). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Balance", meta = (ClampMin = "-1", ClampMax = "1"))
	float CentreOfMassShift = 0.f;

	/** Written to the component's Settle On Start: at Begin Play the body is held still until its water has been read, then set at its
	 * rest draft and attitude on that water and released, so it does not drop or slam at the start. Off: it falls from where it was
	 * placed (the Arrow, which is shot, has it off). Default on. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Balance")
	bool bSettleOnStart = true;

	/** Written to the component's Sleep When Settled: once the body has barely moved for two seconds, is out of sight and is farther than
	 * the component's Sleep Distance from the camera, it sleeps and the water leaves it alone until it is seen, comes near or is hit.
	 * Off: it always answers the water (BeachBall and Arrow). Default on. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Balance")
	bool bSleepWhenSettled = true;

	// ---- Hydrodynamics (the component's drag coefficients; see its tooltips)

	/** Written to the component's Pressure Drag Linear: drag where a face pushes into the water, the part that grows with its speed
	 * through the water (speed / 1 m/s; Kerner's Cpd1). Higher brakes slow motion sooner; 0 removes this part. Default 0.3; at least 0.
	 * The Arrow uses 0.05 (a streamlined shaft). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hydrodynamics", meta = (ClampMin = "0"))
	float PressureDragLinear = 0.3f;

	/** Written to the component's Pressure Drag Quadratic: drag where a face pushes into the water, the part that grows with the square of
	 * its speed through the water ((speed / 1 m/s)²; Kerner's Cpd2). Higher brakes fast motion harder; 0 removes this part. Default 1;
	 * at least 0. The Arrow uses 0.08. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hydrodynamics", meta = (ClampMin = "0"))
	float PressureDragQuadratic = 1.f;

	/** Written to the component's Suction Drag Linear: suction where a face pulls away from the water (behind the hull), the part that
	 * grows with its speed through the water (speed / 1 m/s; Kerner's Csd1). 0 removes this part. Default 0.3; at least 0. The Arrow
	 * uses 0.05. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hydrodynamics", meta = (ClampMin = "0"))
	float SuctionDragLinear = 0.3f;

	/** Written to the component's Suction Drag Quadratic: suction where a face pulls away from the water, the part that grows with the
	 * square of its speed through the water ((speed / 1 m/s)²; Kerner's Csd2). 0 removes this part. Default 0.5; at least 0. The Arrow
	 * uses 0.05. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hydrodynamics", meta = (ClampMin = "0"))
	float SuctionDragQuadratic = 0.5f;

	/** Written to the component's Air Drag Coefficient: the drag coefficient of the dry part in the air and the wind (½ · air density ·
	 * this · area · speed²). Higher lets the wind push it and the air slow it more; 0 turns air drag off. Default 0.8; at least 0. The
	 * Arrow uses 0.02, so it keeps its speed in flight. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hydrodynamics", meta = (ClampMin = "0"))
	float AirDragCoefficient = 0.8f;

	// ---- Water

	/** Also write Interaction (how the body stirs FluidNinja) to the component. Off: the component's Interaction is left as it is. Keep
	 * it off in a preset meant for other projects: a preset made for one project should not hand bodies to FluidNinja's tracking in a
	 * project whose NinjaLives do not track. Default off; no shipped preset turns it on. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Water")
	bool bSetInteraction = false;

	/** How the body stirs FluidNinja's flow: Wakes (hull-shaped wakes and impact rings, stock FluidNinja: for hulls and props),
	 * FluidNinja Tracking (one brush at the pivot, needs the modified FluidNinja) or both. See the component's Interaction. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Water", meta = (EditCondition = "bSetInteraction"))
	EFluidNinjaBuoyancyInteraction Interaction = EFluidNinjaBuoyancyInteraction::Wakes;

	/** Written to the component's Wake: the plugin writes wake points where the hull's water line drives into the water (bow wave, side
	 * wake, the ring of a drop) into FluidNinja's flow. Off: the body writes nothing into the flow and rides the water others push (the
	 * BeachBall, RingBuoy and AirMattress presets: a light prop carried by the current should not). Used only when Interaction is Wakes
	 * or FluidNinja Tracking and Wakes; needs Project Settings → FluidNinja Buoyancy → Wake on and DataChannelPointReaderEnabled on the
	 * NinjaLive. Default on. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Water")
	bool bWake = true;

	/** Written to the component's Wet Line: darkens and glosses the hull below the water line and where it was wet a moment ago. Shows
	 * only when the hull's material uses MF_FNB_HullWetness. Off: no wet line. Default on. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Water")
	bool bWetLine = true;

	// ---- Spray

	/** Written to the component's Splash Min Watts: a slam weaker than this makes no splash (the body's noise floor, so a bobbing body's
	 * small slams make none). Raise it for a body that slaps the water all the time; lower it for a light body whose slams are weak. In
	 * watts. Default 20 (the component's own default is 300); at least 0, and 0 turns slam splashes off (entries and bow spray still
	 * run). The shipped presets use 2 (Arrow), 20, and 300 (BoatHull). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spray", meta = (ClampMin = "0"))
	float SplashMinWatts = 20.f;

	/** Written to the component's Max Splash Strength: the largest splash this body throws, as a multiple of the splash system's own size,
	 * and never past the project's Splash Strength Ceiling (default 12). A splash's size is the cube root of its power over Splash
	 * Reference Watts, so this caps only the hardest slams. Default 12; at least 0.2 (splashes under 0.2 are not drawn). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spray", meta = (ClampMin = "0.2"))
	float MaxSplashStrength = 12.f;

	/** Written to the component's Splash Scale: multiplies the size of every splash and spray from this body (an art control; 1 keeps
	 * the project's splash scale). Default 1; at least 0. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spray", meta = (ClampMin = "0"))
	float SplashScale = 1.f;

	/** Written to the component's Spray Sheets: whisker sheets off the chines and a rooster tail while the hull drives fast. Default off;
	 * the Motorboat preset turns it on. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spray")
	bool bSpraySheets = false;

	/** Written to the component's Spray Deadrise Deg: the bottom's V, which sets how fast the whisker sheets leave (2 V sin of the
	 * deadrise). Default 18; range 0-45. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spray", meta = (ClampMin = "0", ClampMax = "45"))
	float SprayDeadriseDeg = 18.f;

	/** Written to the component's Hull Flow: Displacement (Kerner's drag; every shipped preset but Motorboat) or Planing (the planing law,
	 * a ventilated transom and the hull's wave-making resistance: a fast hull rises onto the plane). Default Displacement. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorboat")
	EFluidNinjaHullFlow HullFlow = EFluidNinjaHullFlow::Displacement;

	/** Written to the component's Planing Pressure Coefficient (k of the planing law, p = k half rho V^2 cos). Default 0.6; range 0.1-1.5. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorboat",
		meta = (EditCondition = "HullFlow == EFluidNinjaHullFlow::Planing", ClampMin = "0.1", ClampMax = "1.5"))
	float PlaningPressureCoefficient = 0.6f;

	/** Also give the body a propeller: the component's Propulsion is turned on with the settings below (Console Control off), the
	 * propeller placed by Prop Location Share. Off: the component's propulsion is left as it is. Default off; the Motorboat preset turns
	 * it on. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorboat")
	bool bSetPropulsion = false;

	/** Written to the component's Propulsion Model: Propeller (an engine turning a propeller through a gear) or Simple (a fixed thrust).
	 * Default Propeller. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorboat", meta = (EditCondition = "bSetPropulsion"))
	EFluidNinjaPropulsionModel PropulsionModel = EFluidNinjaPropulsionModel::Propeller;

	/** Written to the component's Engine Power Kw, the power at the rev limit. Default 60 (an 80 hp outboard); range 1-2000. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorboat", meta = (EditCondition = "bSetPropulsion", ClampMin = "1", ClampMax = "2000"))
	float EnginePowerKw = 60.f;

	/** Written to the component's Engine Max Rpm, the rev limit. Default 5800; range 1000-15000. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorboat", meta = (EditCondition = "bSetPropulsion", ClampMin = "1000", ClampMax = "15000"))
	float EngineMaxRpm = 5800.f;

	/** Written to the component's Gear Ratio (engine turns per propeller turn). Default 2; range 0.25-10. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorboat", meta = (EditCondition = "bSetPropulsion", ClampMin = "0.25", ClampMax = "10"))
	float GearRatio = 2.f;

	/** Written to the component's Prop Diameter Cm. Default 28 (an 11 in outboard propeller); range 5-300. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorboat", meta = (EditCondition = "bSetPropulsion", ClampMin = "5", ClampMax = "300"))
	float PropDiameterCm = 28.f;

	/** Written to the component's Prop Pitch Ratio (pitch over diameter: higher is a faster, harder-to-turn propeller). Default 1.3;
	 * range 0.4-2. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorboat", meta = (EditCondition = "bSetPropulsion", ClampMin = "0.4", ClampMax = "2"))
	float PropPitchRatio = 1.3f;

	/** Written to the component's Steerable Thrust: an outboard or sterndrive whose thrust turns with the rudder. Off: a fixed shaft with
	 * a rudder behind it. Default on. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorboat", meta = (EditCondition = "bSetPropulsion"))
	bool bSteerableThrust = true;

	/** Where the propeller goes (the component's Thrust Location and Rudder Location): shares of the mesh's half extents from the centre of
	 * its bounds, along X (-1 the stern; the bow is +X), Y and Z (-1 the bottom). Beyond 1 places it outside the bounds: an outboard's
	 * propeller hangs behind the transom and under the keel, about (-1.05, 0, -1.6) on a runabout. Default (-1.05, 0, -1.6). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorboat", meta = (EditCondition = "bSetPropulsion"))
	FVector PropLocationShare = FVector(-1.05, 0.0, -1.6);

	/** Written to the component's Rudder Area Cm2: the side force of the lower unit or rudder. Default 500; at least 0. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorboat", meta = (EditCondition = "bSetPropulsion", ClampMin = "0"))
	float RudderAreaCm2 = 500.f;

	/** Written to the component's Max Rudder Angle, in degrees. Default 30; range 0-60. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorboat", meta = (EditCondition = "bSetPropulsion", ClampMin = "0", ClampMax = "60"))
	float MaxRudderAngle = 30.f;

	/** Written to the component's Keel Area Cm2 by every preset, whatever Set Propulsion says: the hull's lateral plane under the water
	 * that resists sliding sideways (keel, skeg, pump housing, strakes); more area, less slide in the turns. 0 (the default) turns the
	 * component's keel off, as floating props want; the Motorboat preset uses 2000. A jet ski 1000-2000; a 5 m runabout 1500-3000; an
	 * 8 m RIB 4000-8000. At least 0. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorboat", meta = (ClampMin = "0"))
	float KeelAreaCm2 = 0.f;

	/** Where the keel's side force acts (the component's Keel Location, the hull's centre of lateral resistance): shares of the mesh's
	 * half extents from the centre of its bounds, as Prop Location Share (X -1 the stern, the bow is +X; Z -1 the bottom). Aft of the
	 * centre of mass it also keeps the boat running straight. Default (-0.35, 0, -1): on the keel line about a third of the length from
	 * the stern. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorboat", meta = (EditCondition = "KeelAreaCm2 > 0"))
	FVector KeelLocationShare = FVector(-0.35, 0.0, -1.0);

	/** Written to the component's Keel Depth Cm: the keel's vertical span, centred on Keel Location Share's point. A jet ski or runabout
	 * 10-20; an 8 m RIB 20-30. Default 15; at least 1. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorboat", meta = (ClampMin = "1"))
	float KeelDepthCm = 15.f;
};
