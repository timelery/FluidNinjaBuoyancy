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
		TArray<float> Height;     // cm; NaN where nothing is drawn
		TArray<float> Rate;       // dh/dt, cm/s
		TArray<FVector2f> Flow;   // cm/s
		double Time = 0.0;        // world seconds when it was drawn
		bool bHasFlow = false;

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
		/** Bilinear over the drawn texels; false where there is no water. */
		bool Sample(double X, double Y, float& OutHeight, float& OutRate, FVector2f& OutFlow) const;
	};

	/** Where a hull finds its water: the patch, else a flat height (far field: a water volume), else none. */
	struct FWaterView
	{
		const FWaterPatch* Patch = nullptr;
		float Extrapolate = 0.f;    // seconds to carry the patch's heights forward by their rate
		bool bAmbientFlow = false;  // the current from around the body (FWaterPatch::AmbientAt), not under it
		bool bFlat = false;
		float FlatHeight = 0.f;

		bool Sample(const FVector& Point, float& OutHeight, FVector& OutWaterVelocity) const;
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

		bool bPropulsion = false;
		FVector ThrustLocal = FVector::ZeroVector;
		FVector ThrustDirectionLocal = FVector::ForwardVector;
		float MaxThrust = 0.f;               // kg·cm/s²
		float Throttle = 0.f;                // -1..1
		float PropWashSpeed = 300.f;         // cm/s at full throttle, over the rudder
		FVector RudderLocal = FVector::ZeroVector;
		float RudderArea = 0.f;              // cm²
		float RudderAngle = 0.f;             // rad, positive turns right (bow towards +Y)
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
	};

	/** The hull's water line sampled in sectors around it (body frame): where it drives into the water (wakes, spray) and where the
	 * water stands on it (the wet line). Each sector keeps its hardest-pushing piece of water line. */
	constexpr int32 WaterlineSectors = 16;

	struct FWaterlineSample
	{
		FVector Position = FVector::ZeroVector;      // on the water line, world
		FVector HullVelocity = FVector::ZeroVector;  // of the hull there, world
		FVector Outward = FVector::ZeroVector;       // horizontal hull normal, unit
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
		float SlamPower = 0.f;           // largest slamming power this step, kg·cm²/s³
		FVector SlamLocation = FVector::ZeroVector;
		float ThrustApplied = 0.f;
		bool bTouchesWater = false;
		TArray<FVector> Waterline;       // segment pairs, only when asked for

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

	/** One step: the forces of the water (and the air, propulsion) on the hull. */
	void Integrate(const FHull& Hull, const FBodyState& Body, const FWaterView& Water, const FParams& Params, float DeltaSeconds,
		FStepState& State, FStepResult& Out, bool bWaterline = false);

	/**
	 * Stable steps. The physics solver integrates forces explicitly: a spring stiffer than the step can follow (a light ball bobbing
	 * faster than the frame rate) or a drag that would stop the body in less than a step overshoots and blows up. The part of the
	 * water's stiffness and drag beyond what the step resolves (ω·dt above 0.5, damping·dt/mass above 0.5) is applied implicitly
	 * (backward Euler, as if the step had been cut finer): the force and torque returned give the velocity change the implicit step
	 * would. Bodies the step resolves well are left exactly as they are. InertiaDiagonal: the body's world inertia about world X, Y,
	 * Z. GravityZ: the gravity acting on the body (cm/s², 0 when it has none). Returns whether anything changed.
	 */
	bool StabilizeStep(const FStepResult& Result, const FBodyState& Body, const FVector& InertiaDiagonal, float GravityZ, float DeltaSeconds,
		FVector& InOutForce, FVector& InOutTorque);

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
}
