#pragma once

#include "CoreMinimal.h"

class UPrimitiveComponent;

/**
 * The hydrodynamics shared by the physics thread (every step) and the editor's hull analysis. Units are Unreal's: cm, kg, s
 * (forces in kg·cm/s²). After Kerner, "Water interaction model for boats in video games" (2015): every hull triangle is clipped at
 * the water line it actually meets, and each submerged piece gets hydrostatic pressure, pressure drag or suction, skin friction
 * (ITTC 1957) and slamming; the dry part gets air drag.
 */
namespace FluidNinjaBuoyancy
{
	/** How a body meets the water. */
	enum class EModel : uint8
	{
		Triangles,   // the hull surface, clipped at the water line (boats, rafts)
		Voxels,      // cubes filling the hull, each wet by how deep it is (logs, crates, debris)
		Spheres,     // pontoons, each with its cap volume under the water (cheapest)
	};

	/** A closed hull in the body's frame (component space, its scale applied), triangles wound outward; or, for the point models,
	 * the cubes or spheres standing in for it (they keep the surface's vertices, for its extent). */
	struct FHull
	{
		EModel Model = EModel::Triangles;
		TArray<FVector3f> Vertices;
		TArray<FIntVector3> Triangles;
		TArray<FVector3f> Normals;      // per triangle, outward, unit
		TArray<float> Areas;            // per triangle, cm²
		// Point models: centres, sizes (cube edge or sphere radius, cm), volumes (cm³), and weights on lift and drag.
		TArray<FVector3f> Points;
		TArray<float> PointSizes;
		TArray<float> PointVolumes;
		TArray<float> PointLift;
		TArray<float> PointDrag;
		/** Point models: the sections of all points scaled so the whole body shows a quarter of its surface (the mean projected area of a
		 * convex body), for drag in water and air (cubes hidden behind others would otherwise all catch the flow). */
		float PointSectionScale = 1.f;
		FVector3f BoundsMin = FVector3f::ZeroVector;
		FVector3f BoundsMax = FVector3f::ZeroVector;
		float Length = 0.f;             // longest horizontal extent (Reynolds number)
		float TotalArea = 0.f;
		float Volume = 0.f;             // cm³
		FVector3f Centroid = FVector3f::ZeroVector;   // of the enclosed volume
		FString Source;                 // what it was built from

		bool IsValid() const { return (Model == EModel::Triangles ? Triangles.Num() > 0 : Points.Num() > 0) && Volume > 1.f; }
		int32 Elements() const { return Model == EModel::Triangles ? Triangles.Num() : Points.Num(); }
	};

	/** A sphere standing in for part of a hull (body frame), a pontoon: lift and drag weights scale its forces. */
	struct FPontoon
	{
		FVector Location = FVector::ZeroVector;
		float Radius = 25.f;
		float LiftWeight = 1.f;
		float DragWeight = 1.f;
	};

	/** The water around one body, on a world-aligned grid (i along +X, j along +Y), read back from the drawn surface and the
	 * simulation's velocity buffer. Immutable once published. */
	struct FWaterPatch
	{
		FVector2D Origin = FVector2D::ZeroVector;   // world XY of grid point (0, 0)
		double Texel = 1.0;
		int32 NX = 0;
		int32 NY = 0;
		TArray<float> Height;     // cm; NaN where nothing is drawn (FluidNinja's own surface: the Surface Mirror is drawn without the waves)
		TArray<float> Rate;       // dh/dt, cm/s
		TArray<FVector2f> Flow;   // cm/s
		/** FluidNinja Waves: B - B_self (cm), the waves of everything but the body itself (the simulation's B less the body's own field),
		 * when bHasWave; 0 where the body's own field is not known. The body feels Height + κ × this (FWaterView::WaveFeel). */
		TArray<float> Wave;
		double Time = 0.0;        // world seconds when it was drawn
		bool bHasFlow = false;
		bool bHasWave = false;
		/** The highest the water FWaterView::Sample can return stands anywhere on this patch (cm): the largest drawn Height, plus the most
		 * the rate's carry-forward and the waves the body feels could add to it. A hull whose lowest point is above it meets no water, so
		 * Integrate skips sampling its vertices. The publisher fills it where it already walks the texels; left at its default (no
		 * ceiling) nothing is skipped. An underestimate would dry a hull that is in the water: when in doubt, leave it. */
		float HeightCeiling = TNumericLimits<float>::Max();

		/** The current around the body, from a ring of water outside its own reach (a mean and a linear fit, so a turning eddy still
		 * turns it): what a body that makes its own wake is carried and dragged by, rather than the water it just pushed. */
		bool bAmbient = false;
		FVector2D AmbientCentre = FVector2D::ZeroVector;
		FVector2f AmbientFlow = FVector2f::ZeroVector;
		FVector4f AmbientGradient = FVector4f(0.f, 0.f, 0.f, 0.f);   // dFx/dx, dFx/dy, dFy/dx, dFy/dy (1/s)

		FVector2f AmbientAt(double X, double Y) const
		{
			const float DX = static_cast<float>(X - AmbientCentre.X);
			const float DY = static_cast<float>(Y - AmbientCentre.Y);
			return AmbientFlow + FVector2f(AmbientGradient.X * DX + AmbientGradient.Y * DY, AmbientGradient.Z * DX + AmbientGradient.W * DY);
		}

		bool Contains(double X, double Y) const;
		/** Bilinear over the drawn texels; false where there is no water. OutWave (optional): B - B_self there (cm), from the same taps and
		 * weights (the waves count only where the water is drawn); 0 without waves. One index computation serves all four fields. */
		bool Sample(double X, double Y, float& OutHeight, float& OutRate, FVector2f& OutFlow, float* OutWave = nullptr) const;
		/** B - B_self at a point (cm), bilinear over the drawn texels with Sample's weights; 0 without waves or water there. */
		float SampleWave(double X, double Y) const;
	};

	/** Where a hull finds its water: the patch, else a flat height (far field: a water volume), else none. */
	struct FWaterView
	{
		const FWaterPatch* Patch = nullptr;
		float Extrapolate = 0.f;    // seconds to carry the patch's heights forward by their rate
		bool bAmbientFlow = false;  // the current from around the body (FWaterPatch::AmbientAt), not under it
		bool bFlat = false;
		float FlatHeight = 0.f;
		/** κ, how much of FluidNinja Waves the body feels: the height is the patch's W + κ × (B - B_self) (0: FluidNinja's own surface). */
		float WaveFeel = 0.f;

		bool Sample(const FVector& Point, float& OutHeight, FVector& OutWaterVelocity) const;
	};

	/** How the water's dynamic pressure acts on a triangle hull (UFluidNinjaBuoyancyComponent::HullFlow). */
	enum class EHullFlow : uint8
	{
		/** Kerner's pressure drag and suction (the drag coefficients and their falloff powers), as before. */
		Displacement,
		/** A planing hull: the dynamic pressure on a face meeting the flow follows Savitsky's planing law, k · ½ρV² · cos θ (linear in the
		 * incidence, so a bottom at trim τ lifts k · ½ρV² sin τ per unit area); the suction behind the hull is capped at the local
		 * hydrostatic head (the water lets go of a transom: a ventilated stern); a slam takes only the vertical entry speed (the forward
		 * speed's pressure is the planing law's); the steady part of the pressure enters FluidNinja Waves at its full weight; and the hull
		 * pays the wave-making resistance of its own pressure (Havelock), which gives the hump before it planes (its pressure faded in from
		 * a wetted transom below a transom Froude number of about 2.5, where the water closes up behind the stern). */
		Planing,
	};

	/** How the propeller is driven (UFluidNinjaBuoyancyComponent::PropulsionModel). */
	enum class EPropulsionModel : uint8
	{
		/** Throttle × Max Thrust at the thrust point while that point is under water, as before. */
		Simple,
		/** An engine (power, rev limit, response) through a gearbox to a propeller with open-water thrust and torque curves (KT, KQ against
		 * the advance ratio J = Va / nD): thrust and rpm follow the boat's speed through the water, the disc's immersion and ventilation;
		 * the propeller's torque reacts on the hull, and its jet washes the rudder. */
		Propeller,
	};

	/** The propeller's open-water curves: KT and KQ at J = Index × PropTableStep, forward running, linear between, extrapolated beyond. */
	constexpr int32 PropTableSize = 17;

	/** The propulsor's state on one step (physics thread), for the readout, the sounds, the wash and the spray. */
	struct FPropulsorResult
	{
		bool bActive = false;            // a propeller model ran this step
		float Thrust = 0.f;              // along Axis (negative astern), kg·cm/s²
		float ShaftTorque = 0.f;         // the torque the water takes from the propeller, kg·cm²/s²
		float ShaftRpm = 0.f;            // the propeller shaft, rev/min (negative astern)
		float EngineRpm = 0.f;           // the engine, rev/min
		float Power = 0.f;               // delivered to the water, kg·cm²/s³ (× 1e-4 = W)
		float Immersion = 0.f;           // the share of the disc under the water, 0..1
		float JetSpeed = 0.f;            // the slipstream's speed through the water far behind the disc, cm/s
		float AdvanceSpeed = 0.f;        // the water's speed into the disc along its axis, cm/s
		float Load = 0.f;                // the engine's torque over what it can give at this rpm, 0..1
		int8 Gear = 0;                   // 1 ahead, 0 neutral, -1 astern
		bool bVentilated = false;        // air drawn down onto the blades: thrust and torque lost
		FVector Location = FVector::ZeroVector;   // the disc's centre, world
		FVector Axis = FVector::ForwardVector;    // the thrust direction ahead (turned with the drive when it steers), world
	};

	struct FParams
	{
		float Rho = 0.001f;                 // water, kg/cm³
		float RhoAir = 1.2e-6f;             // kg/cm³
		float Gravity = 980.f;              // cm/s²
		float BuoyancyMultiplier = 1.f;
		float ReferenceSpeed = 100.f;       // cm/s, the drag coefficients' reference
		float PressureDragLinear = 0.3f;
		float PressureDragQuadratic = 1.f;
		float PressureFalloff = 0.5f;
		float SuctionDragLinear = 0.3f;
		float SuctionDragQuadratic = 0.5f;
		float SuctionFalloff = 0.5f;
		float ViscousMultiplier = 1.f;
		float KinematicViscosity = 0.01f;   // water, cm²/s
		float SlamMultiplier = 0.5f;
		float AirDragCoefficient = 0.8f;
		FVector Wind = FVector::ZeroVector;
		float FlowCoupling = 1.f;
		FVector AngularDamping = FVector::ZeroVector;   // roll, pitch, yaw, 1/s, scaled by the wetted fraction

		// The keel: the hull's lateral plane under the water (keel, skeg, pump housing, strakes), a low-aspect-ratio wing against the
		// slide at the centre of lateral resistance. Any hull model and flow, with or without propulsion.
		FVector KeelLocal = FVector::ZeroVector;   // its centre (body frame, cm, at world size)
		float KeelArea = 0.f;                      // cm² (0: no keel)
		float KeelDepth = 15.f;                    // its vertical span, cm, centred on KeelLocal: its immersion and aspect ratio
		float KeelCrossflow = 1.f;                 // the crossflow drag coefficient of the slide

		bool bPropulsion = false;
		FVector ThrustLocal = FVector::ZeroVector;
		FVector ThrustDirectionLocal = FVector::ForwardVector;
		float MaxThrust = 0.f;               // kg·cm/s²
		float Throttle = 0.f;                // -1..1
		float PropWashSpeed = 300.f;         // cm/s at full throttle, over the rudder
		FVector RudderLocal = FVector::ZeroVector;
		float RudderArea = 0.f;              // cm²
		float RudderAngle = 0.f;             // rad, positive turns right (bow towards +Y)

		// The hull's flow (triangles only; a point model ignores it).
		EHullFlow HullFlow = EHullFlow::Displacement;
		float PlaningPressureCoefficient = 0.6f;   // k of the planing law
		float WaveResistanceScale = 1.f;           // Havelock's wave-making resistance (planing): 1 physical, 0 off
		float WaterDepth = 0.f;                    // cm under the body, for the wave resistance (0: deep water)
		float HullPieceCm = 25.f;                  // the hull's piece size: the shortest waves its pressure can be said to make
		// FluidNinja Waves (planing): the steady part of each piece's dynamic impulse is marked (its area negative) for the waves' own scale.
		bool bSplitSteadyImpulses = false;

		// The propeller model (Propeller); Simple uses MaxThrust and PropWashSpeed above.
		EPropulsionModel PropulsionModel = EPropulsionModel::Simple;
		float EnginePower = 0.f;             // at the rated rpm, kg·cm²/s³ (W × 1e4)
		float EngineMaxRpm = 5800.f;         // the rev limit, rev/min (rated power is reached there)
		float EngineIdleRpm = 800.f;         // rev/min
		float EngineResponseSeconds = 0.4f;  // the throttle's lag, s
		float EngineInertia = 1000.f;        // the engine's rotating parts, at the engine shaft, kg·cm²
		float GearRatio = 2.f;               // engine turns per propeller turn
		float GearEfficiency = 0.95f;
		float PropDiameter = 28.f;           // cm
		float PropInertia = 200.f;           // kg·cm²
		float PropWakeFraction = 0.05f;      // the hull's wake: the disc meets the water at (1 - w) of the boat's speed through it
		float ReverseEfficiency = 0.6f;      // astern, KT and KQ are this share of ahead's
		float VentilatedFactor = 0.45f;      // KT and KQ while the disc is ventilated
		float PropHand = 1.f;                // 1 right-handed (clockwise from astern going ahead), -1 left-handed
		bool bSteerableThrust = false;       // an outboard or sterndrive: the thrust turns with the rudder angle
		float PropTableStep = 0.1f;          // J between table entries
		float PropKT[PropTableSize] = {};
		float PropKQ[PropTableSize] = {};
	};

	struct FBodyState
	{
		FTransform Transform = FTransform::Identity;   // the body (component) frame, no scale
		FVector CenterOfMass = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		FVector AngularVelocity = FVector::ZeroVector; // rad/s
		float Mass = 1.f;
	};

	/** Per body, kept by the physics thread between steps (slamming needs each triangle's last wetted area). */
	struct FStepState
	{
		TArray<float> PreviousWetArea;
		const FHull* Hull = nullptr;   // the hull (level of detail) the wet areas belong to; a new one starts without slamming
		/** The hull's beam (cm): the length the body's slam scales with, the same at every hull resolution (see the slam in Integrate). */
		float SlamLength = 0.f;
		bool bWasWet = false;          // the body touched the water the step before (an entry is the first wet step after a dry one)
		bool bHasStepped = false;      // there was a step before (a body's first step is no entry)
		/** The water's vertical acceleration under the body (cm/s²), for its added mass (FluidNinjaBuoyancyCallback's TrackWaterAcceleration):
		 * the water the body feels, sampled at its centre of mass on each new patch; the patch's time, the height and its rate then, how
		 * many patches in a row were sampled (0: starting over), and the acceleration, smoothed over about two frames. */
		double WaterSampleTime = -1.0;
		float WaterHeight = 0.f;
		float WaterRate = 0.f;
		float WaterAcceleration = 0.f;
		int32 WaterSamples = 0;

		/** The propeller model: the shaft's speed (rad/s, positive ahead), the engine's (rad/s), the throttle the engine has caught up with
		 * (0..1), the gear, whether the clutch slipped on the last step (the shaft turning against the gear: the engine keeps its own
		 * speed until the two lock), and whether the disc is ventilated (with how long it has been fully under water since). */
		float ShaftOmega = 0.f;
		float EngineOmega = 0.f;
		float EngineThrottle = 0.f;
		int8 Gear = 0;
		bool bClutchSlipping = false;
		bool bVentilated = false;
		float ImmersedSeconds = 0.f;

		/** The wave-making resistance (planing): evaluated every WaveResistanceInterval seconds (0.1 s; Seconds counts the time since),
		 * low-passed, and applied every step (world, kg·cm/s²). */
		float WaveResistanceSeconds = TNumericLimits<float>::Max();
		FVector WaveResistanceRaw = FVector::ZeroVector;
		FVector WaveResistance = FVector::ZeroVector;
	};

	/** The hull's water line sampled in sectors around it (body frame): where it drives into the water (wakes, spray) and where the
	 * water stands on it (the wet line). Each sector keeps its hardest-pushing piece of water line. */
	constexpr int32 WaterlineSectors = 16;

	struct FWaterlineSample
	{
		FVector Position = FVector::ZeroVector;      // on the water line, world
		FVector HullVelocity = FVector::ZeroVector;  // of the hull there, world
		FVector Outward = FVector::ZeroVector;       // horizontal hull normal, unit (a sinking flat bottom: away from the hull's centre)
		float Push = -TNumericLimits<float>::Max();  // cm/s the hull drives into the water there (relative to the water)
		float Plunge = 0.f;                          // cm/s the hull sinks into the water there: the water it displaces runs outward
		bool bValid = false;
	};

	struct FStepResult
	{
		FWaterlineSample Sectors[WaterlineSectors];
		float WaterlineLength = 0.f;    // cm
		float Wash = 0.f;               // mean horizontal speed of the water past the water line, cm/s
		FVector Force = FVector::ZeroVector;
		FVector Torque = FVector::ZeroVector;           // about the centre of mass
		FVector AngularDampingRate = FVector::ZeroVector; // world, 1/s × ω: the caller multiplies by the inertia
		FVector AirForce = FVector::ZeroVector;         // the air (wind) on the dry part, included in Force
		FVector BuoyancyForce = FVector::ZeroVector;
		FVector BuoyancyCentre = FVector::ZeroVector;
		float DisplacedVolume = 0.f;     // cm³ (the hydrostatic lift / ρg)
		float WettedArea = 0.f;          // cm²
		/** The body's slamming power this step, kg·cm²/s³: one slam for the whole body (the sum over the hull pieces driving in at once),
		 * so a hull of many small pieces slams as hard as the same hull of a few big ones. */
		float SlamPower = 0.f;
		FVector SlamLocation = FVector::ZeroVector;   // where the hull piece taking the largest share of it meets the water
		float EntryPower = 0.f;          // the body's entry power, on the step it enters the water (NoteEntry), kg·cm²/s³; 0 on every other step
		float ThrustApplied = 0.f;
		/** The propeller model's state this step (bActive false under Simple, or without propulsion). */
		FPropulsorResult Propulsor;
		/** The wave-making resistance applied this step (planing), world, kg·cm/s² (included in Force). */
		FVector WaveResistance = FVector::ZeroVector;
		/** The vertical force of the propulsion, the rudder and the keel this step (kg·cm/s², included in Force): not the water's dynamic
		 * push on the hull's pieces (a banked keel's side force tilts up or down, and gives the waves no pieces of its own), so the ripple
		 * budget leaves it out. */
		float PropulsionForceZ = 0.f;
		/** The keel's side force against the slide this step, its size (kg·cm/s², included in Force); 0 without a keel (KeelArea), with it
		 * out of the water, or running straight. */
		float KeelForce = 0.f;
		/** A boat's drift (propulsion on, or a keel): the angle between its heading (its +X, level) and its horizontal velocity through the
		 * water at the centre of mass, degrees, signed: positive while it slides toward its right (+Y), so a turn to the right reads
		 * negative; near ±180 going astern. 0 below DriftMinSpeed, while the body is dry, and for other bodies. */
		float DriftAngleDeg = 0.f;
		bool bTouchesWater = false;
		TArray<FVector> Waterline;       // segment pairs, only when asked for

		/** FluidNinja Waves, only when asked for (Integrate's bRippleImpulses): what each wet piece (a hull triangle's wet part, or a voxel or
		 * sphere) pushed into the water this step through the water's dynamic force on it, pressure drag or suction plus slamming (the
		 * hydrostatic lift and skin friction make no waves of their own here). Per piece: world X, world Y, the impulse downward into
		 * the water over the step (kg·cm/s), and the piece's area seen from above (cm²). The water pushing the body up is the body
		 * pushing the water down, so the impulse is the piece's upward force × the step: positive presses the water down, negative
		 * (suction on a rising hull) pulls it up. Pieces with no dynamic force are left out.
		 * Planing (FParams::bSplitSteadyImpulses): each wet piece's dynamic impulse is split by what drives it. The share its horizontal
		 * motion through the water drives (a planing bottom's steady lift) comes as its own piece with its area NEGATIVE; the share its
		 * vertical motion drives (heave, a drop: the transient the static head's change already radiates in part) and every slam come with
		 * a positive area. Readers take the area as its absolute value. */
		TArray<FVector4f> RippleImpulses;

		/** How stiff and how damped the water holds the body this step, for stable steps (StabilizeStep). Heave: the lift gained per cm
		 * deeper (ρ g × the waterplane area, kg/s²). Tilt: the righting moment per radian about world X and Y (ρ g × the waterplane's
		 * second moment about the centre of mass, kg·cm²/s²; Z is 0). Drag: the drag force gained per cm/s faster along world X, Y, Z,
		 * summed over the wet pieces (kg/s), and the drag moment gained per rad/s about world X, Y, Z (kg·cm²/s). */
		float HeaveStiffness = 0.f;
		FVector TiltStiffness = FVector::ZeroVector;
		FVector DragDamping = FVector::ZeroVector;
		FVector SpinDamping = FVector::ZeroVector;
	};

	/** Builds the hull from the primitive's simple collision (convexes, boxes, spheres, capsules), subdivided so no edge is longer
	 * than ResolutionCm (coarser if that would pass MaxTriangles). */
	bool BuildHull(UPrimitiveComponent& Primitive, float ResolutionCm, int32 MaxTriangles, FHull& Out, FString& OutWhy);

	/** Cubes filling the hull (about TargetCount of them, or VoxelSizeCm across when that is above zero), their volumes summing to the
	 * hull's. */
	bool BuildVoxels(const FHull& Surface, float VoxelSizeCm, int32 TargetCount, FHull& Out, FString& OutWhy);

	/** Spheres: the given pontoons, or (none given) four along each side of the hull's bottom, sized to its beam and draft, with
	 * their lift scaled so they hold the hull's volume. */
	bool BuildSpheres(const FHull& Surface, const TArray<FPontoon>& Pontoons, FHull& Out, FString& OutWhy);

	/** One step: the forces of the water (and the air, propulsion) on the hull. bWaterline also lists the water line's segments;
	 * bRippleImpulses also lists the impulse each wet piece gives the water (FStepResult::RippleImpulses, for FluidNinja Waves' y).
	 * FinestHull: the body's finest hull (level 0), on which an entry measures the body's height (FStepResult::EntryPower), so the
	 * level of detail in use doesn't change it; without it (static checks) no entry is noted. */
	void Integrate(const FHull& Hull, const FBodyState& Body, const FWaterView& Water, const FParams& Params, float DeltaSeconds,
		FStepState& State, FStepResult& Out, bool bWaterline = false, bool bRippleImpulses = false, const FHull* FinestHull = nullptr);

	/**
	 * Stable steps. The physics solver integrates forces explicitly: a spring stiffer than the step can follow (a light ball bobbing
	 * faster than the frame rate) or a drag that would stop the body in less than a step overshoots and blows up. The part of the
	 * water's stiffness and drag beyond what the step resolves (ω·dt above 0.5, damping·dt/mass above 0.5) is applied implicitly
	 * (backward Euler, as if the step had been cut finer; the stiffness by a θ-scheme, fnb.Physics.StableTheta, 1 = backward Euler):
	 * the force and torque returned give the velocity change the implicit step would. Bodies the step resolves well are left exactly
	 * as they are. InertiaDiagonal: the body's world inertia about world X, Y, Z. GravityZ: the gravity acting on the body (cm/s², 0
	 * when it has none).
	 * AddedMass (kg) and AddedInertiaDiagonal (kg·cm², about world X, Y, Z): the water the body drags along as it heaves and as it
	 * rolls and pitches (FluidNinja Waves' footprint; 0 for none). Its force is -A·(v̇ - a_w), against the body's acceleration relative
	 * to the water's: WaterAccelerationZ is the water's vertical acceleration under the body (cm/s²; the angular ones are taken as 0),
	 * so on a long wave the water carries the body along and the added mass does not hold it back. The step is solved for the heavier
	 * system, mass + added mass, while the force handed back moves the solver's own, lighter, body to the same velocity. With any of
	 * it the force changes on every wet step, not only where a stiffness or damping is past what the step follows. Returns whether
	 * anything changed.
	 * OutDragChangeZ and OutAddedChangeZ (optional): the parts of the change to Force.Z that are the drag's, -c·Δv (the implicit drag
	 * felt at the step's end velocity instead of its start), and the added mass's, A·(a_w - Δv/dt) (the water moving with the hull
	 * taking or giving momentum), kg·cm/s². The rest of the change is the heave spring's (the hydrostatic lift along the step), which
	 * moves no water of its own. The water's dynamic impulse is what the body actually got from its drag, slam and added mass, so the
	 * ripple impulses are bounded by the explicit dynamic force plus these two. 0 when Z was left alone or nothing changed.
	 */
	bool StabilizeStep(const FStepResult& Result, const FBodyState& Body, const FVector& InertiaDiagonal, float GravityZ, float DeltaSeconds,
		float AddedMass, const FVector& AddedInertiaDiagonal, float WaterAccelerationZ, FVector& InOutForce, FVector& InOutTorque,
		double* OutDragChangeZ = nullptr, double* OutAddedChangeZ = nullptr);

	/** Displaced volume (cm³) of the upright hull on flat calm water at this draft (above its lowest point). */
	float DisplacedVolume(const FHull& Hull, float DraftCm, const FParams& Params, const FQuat& Attitude = FQuat::Identity);

	/** The draft (cm) at which the hull, resting in Attitude (the body frame turned so, a log modelled standing up lies down with
	 * pitch 90), displaces this mass, by bisection. */
	float DraftForMass(const FHull& Hull, float MassKg, const FParams& Params, const FQuat& Attitude = FQuat::Identity);

	/** Metacentric height (cm) for a small heel about the world's X (roll) or Y (pitch) axis of the hull resting in Attitude, at the
	 * given draft and centre of mass (body frame). Negative: it capsizes. */
	float MetacentricHeight(const FHull& Hull, float MassKg, const FVector& CenterOfMassLocal, float DraftCm, int32 Axis, const FParams& Params,
		const FQuat& Attitude = FQuat::Identity);

	/** The lowest and highest point of the hull turned to Attitude (cm, about the body's origin). */
	void RestedExtent(const FHull& Hull, const FQuat& Attitude, float& OutBottom, float& OutTop);

	/** A generic fixed-pitch propeller's open-water curves for its pitch ratio P/D (fitted to the Wageningen B-series' shape: four blades,
	 * blade-area ratio about 0.7), into Out.PropKT / PropKQ / PropTableStep: KT = KT0 (1 - (J/J0)^1.6), KQ = KQ0 (1 - 0.76 (J/J0)^1.6) with
	 * J0 = 1.08 P/D (the advance ratio of no thrust), KT0 = 0.45 (P/D)^1.2 and KQ0 = 0.062 (P/D)^1.7. The peak open-water efficiency comes
	 * out about 0.65-0.75 near J = 0.75 J0, as the series' does. */
	void FillPropellerSeries(float PitchRatio, FParams& Out);

	/**
	 * Havelock's wave-making resistance of a steady pressure distribution moving at U over water Depth deep (0: deep water): the drag the
	 * waves it radiates take from it (kg·cm/s²). Loads: (x along the motion, y across it, the vertical load F (kg·cm/s²), σ (cm): the piece's
	 * load spread as a Gaussian of that width); KMax: the shortest wave (wavenumber, 1/cm) the pressure can be said to make.
	 *   R = 1/(2πρg) ∫ |P̂(k(θ), θ)|² cos θ k³ T / |T - kh sech²(kh)| dθ over θ in (-π/2, π/2),
	 *   g k tanh(kh) = U² k² cos² θ (the waves that keep station with the pressure), T = tanh(kh),
	 *   P̂(k, θ) = Σ F e^{i k (x cos θ + y sin θ)} e^{-k² σ² / 2}.
	 * Checked against Lamb's two-dimensional result (a wide patch, within 2 %). The Jacobian's factor is held at most 10 near the critical
	 * speed √(gh), where the linear theory blows up. For speed the loads are first merged into cells of π / KMax (the shortest wave's half
	 * length: a hull's piece), loads under a thousandth of the largest left out, and the spread taken once per group of cells of about the
	 * same σ: within about 1 % of the loads' own result wherever the pressure's waves are resolved. Thread-safe (its scratch is per thread).
	 */
	float WaveMakingResistance(TArrayView<const FVector4f> Loads, float U, float Rho, float Gravity, float Depth, float KMax);

	/** Takes one splash from the frame's shared splash budget (fnb.Effects.SplashBudgetPerFrame), which the floating bodies' splashes and
	 * UFluidNinjaBuoyancySubsystem::AddWaterContact's spend together: Reach 0 (near) may take the whole budget, anything farther only its
	 * first half; false when there is none left. Game thread. Defined in FluidNinjaBuoyancyComponent.cpp, beside the bodies' own use. */
	bool TakeSharedSplashBudget(int32 Reach);
}
