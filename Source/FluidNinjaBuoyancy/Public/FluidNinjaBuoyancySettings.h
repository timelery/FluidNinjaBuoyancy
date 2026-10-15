#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "FluidNinjaBuoyancySettings.generated.h"

/**
 * FluidNinja Buoyancy: physics bodies floating on FluidNinja LIVE water (Project Settings → Game → FluidNinja Buoyancy). The user
 * guide is Docs/UserGuide.md in the plugin folder.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "FluidNinja Buoyancy"))
class FLUIDNINJABUOYANCY_API UFluidNinjaBuoyancySettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UFluidNinjaBuoyancySettings();

	static const UFluidNinjaBuoyancySettings& Get() { return *GetDefault<UFluidNinjaBuoyancySettings>(); }

	virtual FName GetCategoryName() const override { return TEXT("Game"); }

	/** Floating bodies feel the water. Console: fnb.Buoyancy 0 off, 1 on, 2 on with the debug view. */
	UPROPERTY(config, EditAnywhere, Category = "Water")
	bool bEnabled = true;

	/** Actors carrying one of these tags draw the water surface: FluidNinja's river spline pieces (SplineSource), its sea surfaces
	 * (SeaSurface), and any surface you tag FluidNinjaBuoyancySurface (a pool plane, a Niagara water sheet). The height capture shows
	 * only their surface primitives, leaving out a waterfall's face (steeper than 45 degrees). */
	UPROPERTY(config, EditAnywhere, Category = "Water")
	TArray<FName> SurfaceTags;

	/** FluidNinja's material parameter collection: where a simulation that follows the player is (TraceMeshPos, TraceMeshSize in
	 * metres). Simulations that set those parameters on their surface material instead are found from the material. */
	UPROPERTY(config, EditAnywhere, Category = "Water")
	FSoftObjectPath NinjaParameterCollection;

	/** The surface material's texture parameter holding the simulation's velocity (RG) and density (B). */
	UPROPERTY(config, EditAnywhere, Category = "Water")
	FName VelocityBufferParameter = TEXT("VelocityDensityBuffer");

	/** Width of the square the drawn water is captured over, centred on the simulation that follows the player, else on the first
	 * floating body (FluidNinja's river template: 80 m). */
	UPROPERTY(config, EditAnywhere, Category = "Surface Mirror", meta = (ClampMin = "500", ClampMax = "40000", Units = "cm"))
	float CaptureWidthCm = 8000.f;

	/** Texels across the capture (1024 over 80 m: 7.8 cm). */
	UPROPERTY(config, EditAnywhere, Category = "Surface Mirror", meta = (ClampMin = "64", ClampMax = "4096"))
	int32 CaptureResolution = 1024;

	/** The capture looks down from this far above the highest floating body (or the water under it): water higher up, such as a
	 * river passing over a pool, is above its near plane and left out. */
	UPROPERTY(config, EditAnywhere, Category = "Surface Mirror", meta = (ClampMin = "20", Units = "cm"))
	float NearPlaneAboveBodiesCm = 200.f;

	/** The water read back around each body: its bounds plus this, plus how far it moves in FootprintLeadSeconds. */
	UPROPERTY(config, EditAnywhere, Category = "Surface Mirror", meta = (ClampMin = "0", Units = "cm"))
	float FootprintMarginCm = 100.f;

	UPROPERTY(config, EditAnywhere, Category = "Surface Mirror", meta = (ClampMin = "0", Units = "s"))
	float FootprintLeadSeconds = 0.15f;

	/** Many bodies: one compute pass resamples every body's water (height and current) onto its own grid and one readback brings
	 * it all back, instead of two copies and two readbacks per body. Console: fnb.Buoyancy.Gather 0 for the per-body copies. */
	UPROPERTY(config, EditAnywhere, Category = "Surface Mirror")
	bool bGPUGather = true;

	/** A body's grid points are one capture texel apart, two beyond its first hull level-of-detail distance and four beyond the
	 * second; at most this many across (a larger body is sampled more coarsely). */
	UPROPERTY(config, EditAnywhere, Category = "Surface Mirror", meta = (ClampMin = "8", ClampMax = "256", EditCondition = "bGPUGather"))
	int32 MaxGatherGrid = 128;

	/** Bodies gathered in one frame at most; with more, they take turns. */
	UPROPERTY(config, EditAnywhere, Category = "Surface Mirror", meta = (ClampMin = "1", EditCondition = "bGPUGather"))
	int32 MaxGatherBodies = 256;

	/** The read-back water is a few frames old: its heights are carried forward by their rate of change for at most this long. */
	UPROPERTY(config, EditAnywhere, Category = "Surface Mirror", meta = (ClampMin = "0", ClampMax = "0.5", Units = "s"))
	float MaxExtrapolationSeconds = 0.08f;

	/** The current (cm/s) is the velocity buffer × SimSpeed × cell size × simulation steps per second × this. The steps per second
	 * are the fixed tick of FluidNinja's NinjaLiveCore system (1/60 s: 60 steps a second at any frame rate); a NinjaLiveCore without
	 * a fixed tick steps at FluidNinja's SamplingFPS, at most the frame rate. */
	UPROPERTY(config, EditAnywhere, Category = "Flow", meta = (ClampMin = "0"))
	float FlowScaleMultiplier = 1.f;

	/** Currents faster than this are clamped (the buffer is clamped at FluidNinja's VelocityClamp). */
	UPROPERTY(config, EditAnywhere, Category = "Flow", meta = (ClampMin = "0", Units = "cm/s"))
	float MaxFlowSpeedCmS = 600.f;

	/** Water density (kg/m³). */
	UPROPERTY(config, EditAnywhere, Category = "Physics", meta = (ClampMin = "1"))
	float WaterDensity = 1000.f;

	/** Air density (kg/m³) for the dry part of a hull. */
	UPROPERTY(config, EditAnywhere, Category = "Physics", meta = (ClampMin = "0"))
	float AirDensity = 1.2f;

	/** Keeps light bodies (balls, small props) stable whatever the physics step: the part of the water's spring (lift) and drag
	 * that a step is too long for is applied implicitly, as if the step had been cut finer. Bodies the step resolves well are
	 * untouched, so it works with the engine's default physics (one step a frame, any frame rate), substepping or async physics.
	 * Console: fnb.Buoyancy.Implicit 0 for plain explicit forces (A/B). */
	UPROPERTY(config, EditAnywhere, Category = "Physics")
	bool bStableSteps = true;

	/** The wind on the dry part of hulls (cm/s, world). */
	UPROPERTY(config, EditAnywhere, Category = "Wind", meta = (DisplayName = "Wind"))
	FVector Wind = FVector::ZeroVector;

	/** Bodies push the water: each frame, the pieces of water line driving into it are written to FluidNinja's point input (its
	 * global Niagara Data Channel: Position, PositionPrevious, VelocityAndSize), so the simulation makes their bow waves, wakes and
	 * foam, which rock everything else. The FluidNinja simulation must read that channel (its DataChannelPointReaderEnabled on).
	 * Console: fnb.Buoyancy.Wake 0/1. */
	UPROPERTY(config, EditAnywhere, Category = "Wake")
	bool bWakes = true;

	UPROPERTY(config, EditAnywhere, Category = "Wake", meta = (AllowedClasses = "/Script/Niagara.NiagaraDataChannelAsset"))
	FSoftObjectPath WakeDataChannel;

	/** At most this many wake points a frame for all bodies (FluidNinja reads up to its DataChannelThroughput, 4000). */
	UPROPERTY(config, EditAnywhere, Category = "Wake", meta = (ClampMin = "1"))
	int32 MaxWakePoints = 256;

	/** Defaults for bodies that set none of their own. The spray is spawned where a hull slams into the water or drives hard into
	 * it, scaled by how hard; any Niagara system works. */
	UPROPERTY(config, EditAnywhere, Category = "Effects", meta = (AllowedClasses = "/Script/Niagara.NiagaraSystem"))
	FSoftObjectPath SplashSystem;

	/** Hull sounds (none ship with the plugin; see the user guide, "Hull sounds"). Slap: a one-shot when a hull slams into the water.
	 * Wash: a looping sound of water running along the hull. Creak: a one-shot when the hull is thrown about. Their volume follows
	 * the water; a float input named "Pitch Shift" (semitones) is set on each, which MetaSounds can use. */
	UPROPERTY(config, EditAnywhere, Category = "Effects", meta = (AllowedClasses = "/Script/Engine.SoundBase"))
	FSoftObjectPath SlapSound;

	UPROPERTY(config, EditAnywhere, Category = "Effects", meta = (AllowedClasses = "/Script/Engine.SoundBase"))
	FSoftObjectPath WashSound;

	UPROPERTY(config, EditAnywhere, Category = "Effects", meta = (AllowedClasses = "/Script/Engine.SoundBase"))
	FSoftObjectPath CreakSound;

	UPROPERTY(config, EditAnywhere, Category = "Effects", meta = (AllowedClasses = "/Script/Engine.SoundAttenuation"))
	FSoftObjectPath HullAttenuation;
};
