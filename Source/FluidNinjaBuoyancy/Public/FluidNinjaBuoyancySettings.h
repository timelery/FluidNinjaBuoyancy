#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Templates/SubclassOf.h"
#include "FluidNinjaBuoyancySettings.generated.h"

/** FluidNinja Waves overrides for one simulation (one entry of Wave Sims), found by its NinjaLive actor's name or label. Each override has
 * its own tick box; an unticked field takes the project's Waves setting. */
USTRUCT()
struct FFluidNinjaWaveSimSettings
{
	GENERATED_BODY()

	/** The NinjaLive actor this entry is for: its object name, or its label in the editor. None matches nothing. When several entries name
	 * the same actor, the last one wins; an actor carrying a FluidNinja Wave Sim component is set up by that component and its entry is
	 * ignored (logged once). */
	UPROPERTY(EditAnywhere, Category = "Waves")
	FName NinjaLive;

	/** Waves on this simulation. Off: the plugin switches its core's waves off (User.WaveSimEnabled false), its bodies' own fields are
	 * released and its floaters feel FluidNinja's own surface. Default on. Needs Waves on; turn it off for a simulation whose waves are not
	 * worth their cost. */
	UPROPERTY(EditAnywhere, Category = "Waves")
	bool bEnabled = true;

	/** Use Depth Cm for this simulation instead of Wave Depth Cm: everywhere in it, FluidNinja's height field bed included. Unticked: Wave
	 * Depth Cm, and water Z minus the bed wherever the height field has one. */
	UPROPERTY(EditAnywhere, Category = "Waves", meta = (InlineEditConditionToggle))
	bool bOverrideDepth = false;

	/** This simulation's water depth for the waves, everywhere in it, instead of Wave Depth Cm and of the depth FluidNinja's height field
	 * would give (water Z minus its bed); sent as User.Wave.DepthCm with User.Wave.DepthFromBed 0. The waves slow and steepen in shallower
	 * water. In cm. Default 200 (the demo's pool 400, its river 300). At least 1. Use it for a pool or channel whose height field reads its
	 * water surface rather than its floor. */
	UPROPERTY(EditAnywhere, Category = "Waves", meta = (EditCondition = "bOverrideDepth", ClampMin = "1", Units = "cm"))
	float DepthCm = 200.f;

	/** Use Edge Open for this simulation instead of Wave Edge Open. */
	UPROPERTY(EditAnywhere, Category = "Waves", meta = (InlineEditConditionToggle))
	bool bOverrideEdgeOpen = false;

	/** This simulation's window border, instead of Wave Edge Open: open water (on: waves leave through the Wave Sponge Cm band) or a wall that
	 * reflects them (off: a walled pool, absorbing at Wall Absorption). Default on (the demo's pool: off). */
	UPROPERTY(EditAnywhere, Category = "Waves", meta = (EditCondition = "bOverrideEdgeOpen"))
	bool bEdgeOpen = true;

	/** Use Wall Absorption for this simulation instead of Wave Wall Absorption. */
	UPROPERTY(EditAnywhere, Category = "Waves", meta = (InlineEditConditionToggle))
	bool bOverrideWallAbsorption = false;

	/** How strongly this simulation's walled border takes energy out of the waves reaching it, instead of Wave Wall Absorption: 1/s on the
	 * water's vertical speed within Wall Band Cm of the wall (User.Wave.WallAbsorption). Higher: weaker echoes; 0: the walls give back
	 * everything. Default 0.4 (about 80% of a 2 m wave's height comes back). At least 0. Used only where the border is a wall (Edge Open
	 * off). */
	UPROPERTY(EditAnywhere, Category = "Waves", meta = (EditCondition = "bOverrideWallAbsorption", ClampMin = "0"))
	float WallAbsorption = 0.4f;

	/** Use Wall Band Cm for this simulation instead of Wave Wall Band Cm. */
	UPROPERTY(EditAnywhere, Category = "Waves", meta = (InlineEditConditionToggle))
	bool bOverrideWallBand = false;

	/** Width of the band along this simulation's walled border that its wall absorption acts in, instead of Wave Wall Band Cm
	 * (User.Wave.WallBandCm). Wider: the absorption acts over more water. In cm. Default 50. At least 0; 0 turns the wall absorption off. */
	UPROPERTY(EditAnywhere, Category = "Waves", meta = (EditCondition = "bOverrideWallBand", ClampMin = "0", Units = "cm"))
	float WallBandCm = 50.f;

	/** Use Normal Scale for this simulation's surfaces instead of the value Wave Normal Scale Factor gives. */
	UPROPERTY(EditAnywhere, Category = "Waves", meta = (InlineEditConditionToggle))
	bool bOverrideNormalScale = false;

	/** The surface material's WaveNormalScale on this simulation's surfaces, instead of Wave Normal Scale Factor / (0.005 × its size in cm).
	 * Higher: the waves bend the drawn normal more (stronger ripple shading); 0: the waves do not bend it. Default 0.1 (what the factor's
	 * default gives a 20 m simulation). At least 0. */
	UPROPERTY(EditAnywhere, Category = "Waves", meta = (EditCondition = "bOverrideNormalScale", ClampMin = "0"))
	float NormalScale = 0.1f;

	/** The wave grid's scale-down for this simulation, S = 1, 2 or 4 (User.Wave.ScaleDown): its waves run on its pressure buffer's cells over
	 * S, cells S pressure cells wide, so a tick costs 1 / S² of what it does at 1 and each body's own field spans S times the water for the
	 * same cost; the coarser cells drop the shortest waves, and a change starts the waves afresh. 0 (the default): the console variable
	 * fnb.Waves.ScaleDown (default 1; the quality tiers set 4 at Low, 2 at Medium). Range 0-4; 3 is taken as 2. Needs a core at interface
	 * version 3 or later; an older core runs at 1 and says so once. */
	UPROPERTY(EditAnywhere, Category = "Waves", meta = (ClampMin = "0", ClampMax = "4"))
	int32 ScaleDownOverride = 0;

	/** How much this simulation matters when fnb.Waves.MaxActiveSims keeps only the simulations nearest the camera running: its distance from
	 * the camera (to its nearest floating body or swimmer) is divided by this before ranking, so at 2 it counts as half as far away. Higher
	 * keeps it running from farther off. Default 1. At least 0.01. The quality tiers set fnb.Waves.MaxActiveSims to 1 at Low, 2 Medium,
	 * 3 High and 4 Epic (the tier a desktop project gets without asking); at 0 (Cinematic, and the variable's own default) every simulation
	 * runs and this does nothing. */
	UPROPERTY(EditAnywhere, Category = "Waves", meta = (ClampMin = "0.01"))
	float Importance = 1.f;
};

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

	/** The settings every caller reads. A project with a derived settings class (its own config section and extra fields) registers it
	 * through OverrideClass() at module start, and Get() returns that class's defaults from then on. */
	static const UFluidNinjaBuoyancySettings& Get() { return *CastChecked<UFluidNinjaBuoyancySettings>(SettingsClass()->GetDefaultObject()); }
	static UFluidNinjaBuoyancySettings& GetMutable() { return *CastChecked<UFluidNinjaBuoyancySettings>(SettingsClass()->GetDefaultObject()); }
	static TSubclassOf<UFluidNinjaBuoyancySettings>& OverrideClass();
	static UClass* SettingsClass()
	{
		const TSubclassOf<UFluidNinjaBuoyancySettings>& Override = OverrideClass();
		return Override ? Override.Get() : StaticClass();
	}

	virtual FName GetCategoryName() const override { return TEXT("Game"); }
	/** The class defaults loaded from config: stale keys in the settings' ini section (a setting renamed or removed) are migrated or
	 * reported once (FluidNinjaBuoyancyModule.cpp, ConfigRedirects). */
	virtual void PostInitProperties() override;

	/*
	 * Each switch below has one authority, the setting; its console variable (fnb.Buoyancy, .Wake, .Gather, .Implicit) is a debugging
	 * override that reads -1 by default (the setting decides) and 0 or 1 for the session. Code reads the switch through these, never the
	 * setting and the variable side by side.
	 */
	/** Floating bodies feel the water: Enabled, unless fnb.Buoyancy overrides it (0 off; 1 or 2 on). */
	bool IsEnabled() const;
	/** The debug view is on (fnb.Buoyancy 2): the water line, the forces, the water read back. */
	static bool IsDebugView();
	/** The floating bodies write wakes into FluidNinja: Wakes, unless fnb.Buoyancy.Wake overrides it (0 off, 1 on); never while the water is
	 * off. Each body's own Wake switch and Interaction still apply. */
	bool WakesEnabled() const;
	/** Swimmers write wakes into FluidNinja: Swimmer Wakes, and not while fnb.Buoyancy.Wake is 0 (a 1 does not turn them on); never while
	 * the water is off. */
	bool SwimmerWakesEnabled() const;
	/** Every body's water in one GPU pass: GPU Gather, unless fnb.Buoyancy.Gather overrides it. */
	bool GatherEnabled() const;
	/** The water's stiffness and drag a step is too long for applied implicitly: Stable Steps, unless fnb.Buoyancy.Implicit overrides it. */
	bool StableStepsEnabled() const;

	/** Master switch: floating bodies feel the water, and the plugin's waves, wakes and current reads run. Off: bodies get no water forces
	 * and none of those run. Default on. Console: fnb.Buoyancy -1 this setting (the default), 0 off, 1 on, 2 on with the debug view (water
	 * line, forces, the water read back); a console value holds for the session. */
	UPROPERTY(config, EditAnywhere, Category = "Water")
	bool bEnabled = true;

	/** Actors carrying one of these tags draw the water surface the bodies float on: FluidNinja's river spline pieces (SplineSource), its sea
	 * surfaces (SeaSurface), and any surface you tag FluidNinjaBuoyancySurface (a pool plane, a Niagara water sheet). The height capture shows
	 * only their visible surface primitives, leaving out a waterfall's face (steeper than 45 degrees), and their materials give each
	 * simulation's velocity and pressure buffers. Empty: nothing is captured. Add a tag when your water surface carries one of its own. */
	UPROPERTY(config, EditAnywhere, Category = "Water")
	TArray<FName> SurfaceTags;

	/** FluidNinja's material parameter collection, where a simulation that follows the player says where it is (TraceMeshPos, TraceMeshSize
	 * in metres). Simulations that set those parameters on their surface material instead are found from the material. Default: FluidNinja
	 * LIVE's MP_NinjaMaterialParamCollection01 at its default install path (/Game/FluidNinjaLive); change it if FluidNinja is installed
	 * elsewhere. */
	UPROPERTY(config, EditAnywhere, Category = "Water")
	FSoftObjectPath NinjaParameterCollection;

	/** The surface material's texture parameter holding the simulation's velocity (RG) and density (B). Through it the plugin finds the
	 * simulation each surface shows, its current, and (on the same material) its pressure buffer for FluidNinja Waves; a name the material
	 * lacks finds no simulation there and reads no current. Default VelocityDensityBuffer, FluidNinja's own name: change it only for a
	 * surface material that names it differently. */
	UPROPERTY(config, EditAnywhere, Category = "Water")
	FName VelocityBufferParameter = TEXT("VelocityDensityBuffer");

	/** Width of the square the drawn water is captured over (the Surface Mirror), centred on the simulation that follows the player, else on
	 * the middle of the floating bodies. Bodies outside it float on the far field (water volumes). Wider: more water covered, at a coarser
	 * texel (this width over Capture Resolution). In cm. Default 8000 (FluidNinja's river template: 80 m). Range 500-40000. */
	UPROPERTY(config, EditAnywhere, Category = "Surface Mirror", meta = (ClampMin = "500", ClampMax = "40000", Units = "cm"))
	float CaptureWidthCm = 8000.f;

	/** Texels across the capture: the detail of the water the bodies read (1024 over 80 m: 7.8 cm a texel; a body's gather points are one
	 * texel apart). Higher: finer water for more GPU time and memory. Range 64-4096. Default 1024. The quality tier scales it
	 * (fnb.Capture.Scale: 0.25 Low, 0.5 Medium, 0.75 High, 1 Epic, 2 Cinematic) and the result is clamped to 64-4096. */
	UPROPERTY(config, EditAnywhere, Category = "Surface Mirror", meta = (ClampMin = "64", ClampMax = "4096"))
	int32 CaptureResolution = 1024;

	/** The capture looks down from this far above the highest floating body (or the water under it): water higher up, such as a river
	 * passing over a pool, is above its near plane and left out. Lower it if water passing closer above the bodies shows in the capture.
	 * In cm. Default 200. At least 20. */
	UPROPERTY(config, EditAnywhere, Category = "Surface Mirror", meta = (ClampMin = "20", Units = "cm"))
	float NearPlaneAboveBodiesCm = 200.f;

	/** The water read back around each body: its bounds plus this on every side, plus how far it moves in Footprint Lead Seconds. Larger:
	 * more water read back per body (more GPU work and readback). In cm. Default 100. At least 0. */
	UPROPERTY(config, EditAnywhere, Category = "Surface Mirror", meta = (ClampMin = "0", Units = "cm"))
	float FootprintMarginCm = 100.f;

	/** How far ahead of a moving body its water is read: its horizontal speed times this, added to Footprint Margin Cm, at most 200 cm. Raise
	 * it if fast bodies outrun their read-back water, which is a few frames old. In s. Default 0.15. At least 0; 0 reads no lead. */
	UPROPERTY(config, EditAnywhere, Category = "Surface Mirror", meta = (ClampMin = "0", Units = "s"))
	float FootprintLeadSeconds = 0.15f;

	/** Every body's water (height and current) resampled in one compute pass onto its own grid and brought back in one readback, instead of
	 * two copies and two readbacks per body: cheaper with many bodies. Max Gather Grid and Max Gather Bodies apply only to it. Default on.
	 * Console: fnb.Buoyancy.Gather -1 this setting (the default), 0 the per-body copies, 1 the gather (A/B). */
	UPROPERTY(config, EditAnywhere, Category = "Surface Mirror")
	bool bGPUGather = true;

	/** The most grid points across a body's water in the gather. A body's points are one capture texel apart, two beyond its first hull
	 * level-of-detail distance and four beyond the second; a body too big for this many is sampled more coarsely. Higher: large bodies read
	 * finer water for more GPU work. Range 8-256. Default 128. The quality tier may cap it (fnb.Gather.MaxGrid: 32 Low, 64 Medium, 96 High;
	 * 0 leaves this setting). */
	UPROPERTY(config, EditAnywhere, Category = "Surface Mirror", meta = (ClampMin = "8", ClampMax = "256", EditCondition = "bGPUGather"))
	int32 MaxGatherGrid = 128;

	/** Bodies gathered in one frame at most; with more due, they take turns and the others keep their last water. At least 1. Default 256.
	 * The quality tier may lower the per-frame count (fnb.Gather.BudgetPerFrame: 16 Low, 64 Medium, 128 High, the nearest, fastest and
	 * longest-waiting bodies first; 0 leaves this setting). */
	UPROPERTY(config, EditAnywhere, Category = "Surface Mirror", meta = (ClampMin = "1", EditCondition = "bGPUGather"))
	int32 MaxGatherBodies = 256;

	/** The read-back water is a few frames old: its heights are carried forward by their rate of change for at most this long. It also bounds
	 * the gap between a body's gathers: a body about to pass it is gathered first, whatever its turn. Lower: fresher water, more gathers when
	 * the gather is busy. In s. Default 0.08. Range 0-0.5. */
	UPROPERTY(config, EditAnywhere, Category = "Surface Mirror", meta = (ClampMin = "0", ClampMax = "0.5", Units = "s"))
	float MaxExtrapolationSeconds = 0.08f;

	/** Scales the current the floating bodies and swimmers feel. The current (cm/s) is the velocity buffer × SimSpeed × cell size ×
	 * simulation steps per second × this. The steps per second are the fixed tick of FluidNinja's NinjaLiveCore system (1/60 s: 60 steps a
	 * second at any frame rate); a NinjaLiveCore without a fixed tick steps at FluidNinja's SamplingFPS, at most the frame rate. Default 1.
	 * At least 0; 0 reads no current. The console command fnb.Buoyancy.CalibrateFlow, run in Play, measures the value that matches
	 * FluidNinja's dye ('apply' sets and saves it when the match is good). */
	UPROPERTY(config, EditAnywhere, Category = "Flow", meta = (ClampMin = "0"))
	float FlowScaleMultiplier = 1.f;

	/** Currents faster than this are clamped, for the floating bodies, the swimmers and Get Current At Location (FluidNinja also clamps its
	 * buffer, at its VelocityClamp). Raise it if a fast river reads slower than it runs. In cm/s. Default 600. At least 0; 0 reads no
	 * current. */
	UPROPERTY(config, EditAnywhere, Category = "Flow", meta = (ClampMin = "0", Units = "cm/s"))
	float MaxFlowSpeedCmS = 600.f;

	/** Water density for buoyancy and the water's drag, and for the wave sources, the added mass and the bow spray's power. Higher: bodies
	 * float higher and are pushed harder. In kg/m³. Default 1000. At least 1. */
	UPROPERTY(config, EditAnywhere, Category = "Physics", meta = (ClampMin = "1"))
	float WaterDensity = 1000.f;

	/** Air density for the drag and the wind on the dry part of a hull. Higher: more air drag, and a stronger push from Wind. In kg/m³.
	 * Default 1.2. At least 0; 0: no air drag and no wind force. */
	UPROPERTY(config, EditAnywhere, Category = "Physics", meta = (ClampMin = "0"))
	float AirDensity = 1.2f;

	/** Keeps light bodies (balls, small props) stable whatever the physics step: the part of the water's spring (lift) and drag
	 * that a step is too long for is applied implicitly, as if the step had been cut finer. Bodies the step resolves well are
	 * untouched, so it works with the engine's default physics (one step a frame, any frame rate), substepping or async physics.
	 * It also carries FluidNinja Waves' added mass (Wave Added Mass Scale does nothing with it off). Default on.
	 * Console: fnb.Buoyancy.Implicit -1 this setting (the default), 0 plain explicit forces, 1 stable steps (A/B). */
	UPROPERTY(config, EditAnywhere, Category = "Physics")
	bool bStableSteps = true;

	/** The wind on the dry part of hulls: a world velocity in cm/s (X, Y, Z) that pushes each body through its Air Drag Coefficient and
	 * Air Density. Default 0 (calm). The test bench's wind replaces it while set. */
	UPROPERTY(config, EditAnywhere, Category = "Wind", meta = (DisplayName = "Wind"))
	FVector Wind = FVector::ZeroVector;

	/** Bodies push the water: each frame, the pieces of water line driving into it are written to FluidNinja's point input (its
	 * global Niagara Data Channel: Position, PositionPrevious, VelocityAndSize), so the simulation makes their bow waves, wakes and
	 * foam, which rock everything else. The FluidNinja simulation must read that channel (its DataChannelPointReaderEnabled on), and
	 * Wake Data Channel must name it. The project default: each body's Wake switch can turn its own off, and its Interaction chooses
	 * wakes, FluidNinja's tracking or both. Default on. Needs no modified FluidNinja core.
	 * Console: fnb.Buoyancy.Wake -1 this setting (the default), 0 no wakes at all (the swimmers' too), 1 on (A/B). */
	UPROPERTY(config, EditAnywhere, Category = "Wake")
	bool bWakes = true;

	/** The Niagara Data Channel the wake points are written to: FluidNinja's global channel (default NinjaDataChannel1_Global, under
	 * /Game/FluidNinjaLive/Core/DataChannels). The floating bodies' wakes and the swimmers' wakes and strokes go there; empty or missing,
	 * none are written. Change it if FluidNinja is installed elsewhere or your simulation reads another channel. */
	UPROPERTY(config, EditAnywhere, Category = "Wake", meta = (AllowedClasses = "/Script/Niagara.NiagaraDataChannelAsset"))
	FSoftObjectPath WakeDataChannel;

	/** At most this many wake points a frame for all floating bodies together (swimmers not counted); past it, the rest are dropped for that
	 * frame (first come, in body order). FluidNinja reads up to its DataChannelThroughput, 4000. Higher: bodies in a crowd keep their wakes. At
	 * least 1. Default 256. The quality tier may cap it (fnb.Wake.MaxPoints: 64 Low, 128 Medium, 192 High; 0 leaves this setting). */
	UPROPERTY(config, EditAnywhere, Category = "Wake", meta = (ClampMin = "1"))
	int32 MaxWakePoints = 256;

	/** FluidNinja's own tracking, for bodies whose Interaction asks for it: the NinjaLive property (on the actor or its NinjaLive
	 * component) holding the component tag it tracks, FluidNinja's Track Actor Primitive Components With Tag. A body handed to the
	 * tracking gets that tag, besides Generate Overlap Events, which FluidNinja's tracking needs either way. Default
	 * TrackActorPrimitiveComponentsWithTag, FluidNinja's own name: change it only if your NinjaLive names it differently. */
	UPROPERTY(config, EditAnywhere, Category = "FluidNinja Tracking")
	FName TrackingTagProperty = TEXT("TrackActorPrimitiveComponentsWithTag");

	/** The NinjaLive property (on the actor, or failing that its NinjaLive component) listing the object types it tracks, FluidNinja's
	 * Overlap Filter Inclusive Obj Type. A body is handed to the tracking only if a NinjaLive whose interaction volume overlaps it will
	 * select it: its object type is in that NinjaLive's list, or that NinjaLive tracks by tag (the plugin adds the tag). Otherwise the
	 * log says why it is not tracked. Default OverlapFilterInclusiveObjType, FluidNinja's own name. */
	UPROPERTY(config, EditAnywhere, Category = "FluidNinja Tracking")
	FName ObjectTypeFilterProperty = TEXT("OverlapFilterInclusiveObjType");

	/** The NinjaLive component whose bounds are where it tracks (FluidNinja's InteractionVolume). A body outside every NinjaLive's
	 * volume is not handed over until it enters one; a NinjaLive without this component selects nothing. Default InteractionVolume,
	 * FluidNinja's own name. */
	UPROPERTY(config, EditAnywhere, Category = "FluidNinja Tracking")
	FName InteractionVolumeComponent = TEXT("InteractionVolume");

	/** How a NinjaLive says its tracking paints a body's velocity relative to the water under it (instead of its world velocity, which
	 * makes a current speed itself up around floating bodies): a bool or number property of this name on the NinjaLive actor or one
	 * of its components, or a float user parameter of its simulation; on above 0.5. Stock FluidNinja has no such switch; the modified
	 * core shipped with FluidNinja Waves has the user parameter TrackVelocityRelativeToWater (the default name; set it to 1 on the
	 * NinjaLive in the level). Bodies are handed to the tracking only while every NinjaLive in the level says so and also paints no
	 * density for them (Tracked Density Switch). Console: fnb.Buoyancy.Tracking 1 (the default) as described, 0 never hands a body over,
	 * 2 always does (testing: today's world-velocity painting speeds a current up). */
	UPROPERTY(config, EditAnywhere, Category = "FluidNinja Tracking")
	FName RelativeTrackingSwitch = TEXT("TrackVelocityRelativeToWater");

	/** How a NinjaLive says tracked bodies paint velocity but no density: FluidNinja's Selectively Kill Brush Density Keep Velocity,
	 * a number property of this name on the NinjaLive actor or one of its components, or an int user parameter of its simulation
	 * (User.Painter.<name> or User.<name>). 1 (ARRAY: primitives and bones), 4 (ARRAY and CHAOS) and 5 (ALL) count; 5 also takes the
	 * density out of this plugin's wakes, which are data-channel points. FluidNinja weights each tick's push of its river driver and
	 * brushes by the local density, so a tracked floater that paints density makes the current push harder along its path, even when
	 * its velocity is taken relative to the water. Set it on the NinjaLive in the level; changing it during play was not tested.
	 * Default SelectivelyKillBrushDensityKeepVelocity, FluidNinja's own name. */
	UPROPERTY(config, EditAnywhere, Category = "FluidNinja Tracking")
	FName TrackedDensitySwitch = TEXT("SelectivelyKillBrushDensityKeepVelocity");

	/** The spray spawned where a hull slams into the water or drives hard into it, scaled by how hard (Splash Reference Watts), and the
	 * swimmers' splashes; any Niagara system works. The default for bodies that set no Splash System of their own (default the plugin's
	 * NS_FNB_HullSplash). Empty: those bodies and the swimmers spawn no splash. */
	UPROPERTY(config, EditAnywhere, Category = "Effects", meta = (AllowedClasses = "/Script/Niagara.NiagaraSystem"))
	FSoftObjectPath SplashSystem;

	/** Splashes on one scale for every floating body and swimmer: a slam of this power (watts) makes a splash of strength 1 (the splash
	 * system at its own size), and the strength grows with the cube root of the power (eight times the power, twice the splash), so splash
	 * sizes compare across bodies, heights and stations. Each body's Splash Min Watts is its noise floor and its Splash Scale an art
	 * multiplier on top. At 70 W a 238 kg boat dropped 3 m (5.8 kW) throws a splash of 4.4, and a 175 kg crate dropped 3 m (35 kW)
	 * throws 8. Lower: every splash larger. In watts. Default 70. At least 1. */
	UPROPERTY(config, EditAnywhere, Category = "Effects", meta = (ClampMin = "1"))
	float SplashReferenceWatts = 70.f;

	/** The largest splash strength on the project's scale: harder slams are capped here, for bodies and swimmers alike (a body's Max Splash
	 * Strength may cap its own lower). Splashes under strength 0.2 are not drawn. Default 12. At least 0.6. */
	UPROPERTY(config, EditAnywhere, Category = "Effects", meta = (ClampMin = "0.6"))
	float SplashStrengthCeiling = 12.f;

	/** The splash strength a slam of Watts makes on the project's scale (0 for none). */
	float SplashStrength(float Watts) const
	{
		return Watts > 0.f ? FMath::Min(FMath::Pow(Watts / FMath::Max(SplashReferenceWatts, 1.f), 1.f / 3.f), SplashStrengthCeiling) : 0.f;
	}

	/** Hull sound: a one-shot where a hull slams into the water, louder the harder the slam, for bodies without a Slap Sound of their own.
	 * None ship with the plugin (see the user guide, "Hull sounds"); empty: no slaps. A float input "Pitch Shift" (semitones, random
	 * -1.5 to 1.5) is set on it, which MetaSounds can use. */
	UPROPERTY(config, EditAnywhere, Category = "Effects", meta = (AllowedClasses = "/Script/Engine.SoundBase"))
	FSoftObjectPath SlapSound;

	/** Hull sound: a looping sound of water running along the hull, for bodies without a Wash Sound of their own; its volume follows the
	 * water's speed past the water line and the wetted length (a little even at rest). None ships with the plugin; empty: no wash. Its float
	 * input "Pitch Shift" (semitones) runs from -3 at rest to 2 at full speed. */
	UPROPERTY(config, EditAnywhere, Category = "Effects", meta = (AllowedClasses = "/Script/Engine.SoundBase"))
	FSoftObjectPath WashSound;

	/** Hull sound: a one-shot when a hull in the water is thrown about (a sudden change of spin, or a hard slam), for bodies without a Creak
	 * Sound of their own. None ships with the plugin; empty: no creaks. Its float input "Pitch Shift" (semitones) is random, -2 to 1. */
	UPROPERTY(config, EditAnywhere, Category = "Effects", meta = (AllowedClasses = "/Script/Engine.SoundBase"))
	FSoftObjectPath CreakSound;

	/** The attenuation (how the hull sounds fade with distance) for bodies without an Attenuation of their own: their slap, wash, creak and
	 * drip sounds. Default the plugin's SA_FNB_Hull. Empty: each sound's own attenuation settings. */
	UPROPERTY(config, EditAnywhere, Category = "Effects", meta = (AllowedClasses = "/Script/Engine.SoundAttenuation"))
	FSoftObjectPath HullAttenuation;

	/** FluidNinja Waves: floating bodies and the swimmer make real water waves in FluidNinja's own simulation (the modified NinjaLiveCore
	 * runs a wave field beside its fluid, on its pressure buffer's cells): each body's static head (how deep its hull sits below the
	 * wave-free surface) and the impulses of the slam and drag that stop it, written each frame into a source texture per simulation
	 * (User.WaveSourceTexture). FluidNinja's surface draws the waves (MF_NinjaOutput_BaseFunction_SURFACE) and the other floaters feel
	 * them: each body feels the waves of everything but itself (B - B_self, its own field taken out by a self field that follows it, and
	 * put back as radiation damping). Needs the modified NinjaLiveCore (User.WaveSimEnabled): a stock one makes no waves, with an error.
	 * Default on. Off: no simulation runs waves and the bodies float on FluidNinja's own surface; Wave Sims entries and Wave Sim
	 * components can turn single simulations off. */
	UPROPERTY(config, EditAnywhere, Category = "Waves")
	bool bWaves = true;

	/** The water's depth the waves run at where FluidNinja's height field is off (User.Wave.DepthCm); where it is on, the core takes water Z
	 * minus its bed. The waves slow and steepen in shallow water (ω² = (g·k + σ/ρ·k³)·tanh(k·d)). The bodies' own fields use it too, and
	 * Wave Depth From Ground never reads deeper. In cm. Default 200. At least 1. Each simulation may override it, height field included
	 * (Wave Sims). */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves", ClampMin = "1", Units = "cm"))
	float WaveDepthCm = 200.f;

	/** Damping of every wave (User.Wave.Damping, 1/s on the surface's vertical speed). Higher: rings die out sooner; 0.02 (the default) lets
	 * rings cross a pool. At least 0; 0: no damping beyond the sponge, the walls, the shore and breaking. The bodies' own fields use the
	 * same. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves", ClampMin = "0"))
	float WaveDamping = 0.02f;

	/** Width of the absorbing band at an open simulation window's edge (User.Wave.SpongeCm): waves running out of the window fade there
	 * instead of coming back in (8/s at the edge, falling as the square to 0 at this distance). Used only where Wave Edge Open is on. The
	 * bodies' own fields use the same width, so a wider band also makes each own field larger (more GPU work). In cm. Default 300. At
	 * least 0; 0: no sponge. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves", ClampMin = "0", Units = "cm"))
	float WaveSpongeCm = 300.f;

	/** The simulation window's border is open water (User.Wave.EdgeOpen 1: waves leave through the sponge), or a wall that reflects them
	 * (0, a walled pool, absorbing at Wave Wall Absorption). Default on. Each simulation may override it (Wave Sims). */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves"))
	bool bWaveEdgeOpen = true;

	/** Where a simulation's window border is a wall: how strongly it takes energy out of the waves reaching it (User.Wave.WallAbsorption,
	 * 1/s on the water's motion within Wave Wall Band of the wall), as a pool's gutter or skimmer does: a wall gives back most of a wave,
	 * not all of it. 0.4 (the default) returns about 80% of a 2 m wave's height. 0: the walls give back everything, and a closed basin keeps
	 * every wave (a floater's own waves then build up in it over minutes). At least 0. The bodies' own fields use the same. Each simulation
	 * may override it. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves", ClampMin = "0"))
	float WaveWallAbsorption = 0.4f;

	/** Width of the band along a walled window border that Wave Wall Absorption acts in (User.Wave.WallBandCm). Wider: the absorption acts
	 * over more water. In cm. Default 50. At least 0; 0 turns the wall absorption off. Each simulation may override it. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves", ClampMin = "0", Units = "cm"))
	float WaveWallBandCm = 50.f;

	/** Water shallower than this absorbs waves, more the shallower, as a beach does (User.Wave.ShoreDepthCm): 8/s at no depth, falling
	 * linearly to 0 at this depth. Deeper: a wider absorbing shore. In cm. Default 10. At least 0; 0: no shore absorption. The bodies' own
	 * fields use the same. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves", ClampMin = "0", Units = "cm"))
	float WaveShoreDepthCm = 10.f;

	/** Waves steeper than this slope (rise over run, cell to cell) break, and so do crests higher than 0.6 of the depth
	 * (User.Wave.BreakingSlope). 1.0: a safety net for the cell-sized spikes and columns a bad source makes (slopes of several), not a
	 * Stokes limit. At 0.4 it damped the near wake of any hull with vertical topsides (a row boat's convex hull steps from its draft to
	 * nothing at the waterline, and the linear wake of that step is steeper than 0.4 over much of the water around and behind it), and
	 * the towed row boat's Kelvin wake came out at 0.52 of the exact linear wake; at 1.0 it reads 1.00-1.03 (the bench's ripple-kelvin).
	 * Leaving the cells near a hull out of the judgement does not help: the steep water is the wake itself. Lower: more of the steep water
	 * breaks (damped); higher: only steeper spikes break. Default 1. Range 0.05-2. The crest test (0.6 of the depth) applies whatever this
	 * says. The bodies' own fields use the same. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves", ClampMin = "0.05", ClampMax = "2"))
	float WaveBreakingSlope = 1.f;

	/** Damping of the shortest waves the grid carries (User.Wave.Hyperviscosity, 1/s at the grid's finest wavelength, falling as k⁴ for
	 * longer ones): keeps the cell-sized ripples a hull's sharp side makes from ringing. Higher: grid-scale ripples die sooner; 0: none.
	 * Default 0.5. At least 0; the core clamps it to 50 (the bodies' own fields take it as given). */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves", ClampMin = "0"))
	float WaveHyperviscosity = 0.5f;

	/** How much FluidNinja's current carries the waves (User.Wave.CurrentCoupling): 1 sweeps them down the creek with the water, 0 leaves
	 * them in still water. Default 1. At least 0; above 1 carries them faster than the water. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves", ClampMin = "0"))
	float WaveCurrentCoupling = 1.f;

	/** κ: how much of the waves the floating bodies feel (times each body's own Wave Feel Scale). 1 is physical; 0 floats them on
	 * FluidNinja's own surface (the waves are still made and drawn), and a body whose κ is 0 gets no own field, which saves its cost.
	 * Default 1. Range 0-2. Bodies farther from the camera than fnb.Waves.SelfFieldDistanceCm (default 6000 cm; the quality tiers set 1500
	 * Low, 3000 Medium, 4500 High, 6000 Epic, 0 = every body at Cinematic) feel no waves whatever this says. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves", ClampMin = "0", ClampMax = "2"))
	float WaveFeelScale = 1.f;

	/** The drag of each body's own waves, put back in every physics step (heave, roll and pitch, from its rest footprint at each motion's
	 * natural frequency), since what a body feels has its own waves taken out. Off: a body loses only what the other waves take from it,
	 * and bobs on for longer. Default on. Console: fnb.Waves.RadiationScale scales it (1 as calibrated, 0 none; A/B). */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves"))
	bool bRadiationDamping = true;

	/** The water each body drags along as it heaves, rolls and pitches (its added mass, from its rest footprint on the simulation's cells
	 * by strip theory: ρ π B² / 8 per unit length of the footprint, times L / (L + B) for its ends; roll and pitch from the same strips by
	 * their arm squared), scaled by this. The body responds as the heavier system (a crate's heave period lengthens by about a third, a
	 * ball's by more) and its radiation damping is worked out at that slower frequency. 1 is physical; 0 none. Stable Steps carry it (the
	 * force is rewritten so the physics solver's own step lands where the heavier body's would): with Stable Steps off it does nothing.
	 * Default 1. At least 0. A body without an own field (see Wave Feel Scale) gets none. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves", ClampMin = "0"))
	float WaveAddedMassScale = 1.f;

	/** The bodies' own fields take the water's depth under each body from the ground below it (a trace down from the water), at most
	 * Wave Depth (or the simulation's override), as the core does with its landscape height field and Clamp Height Lower Values on: beds
	 * flattened by the clamp run at Wave Depth, only shallower ones at their own. Off: Wave Depth everywhere, as the core does with the
	 * height field off. Where the trace finds no ground, Wave Depth too. Off by default: a trace often meets a water mesh with collision
	 * at the surface and reads a depth of 1 cm. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves"))
	bool bWaveDepthFromGround = false;

	/** Source 1: each hull's static head, how deep it sits below the wave-free surface (open interiors included); its motion makes the
	 * impact rings, bow waves and the Kelvin wake. Default on. Off: hulls press no static head, and the swimmers push no water aside either
	 * (Swimmer Waves); only the impulses make waves. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves"))
	bool bWaveDisplacement = true;

	/** Source 2: the slam and drag that stop a hull push the same impulse into the water, equal and opposite (the Cauchy–Poisson impulse
	 * problem), faded by its depth under the surface. It also gates Add Water Impulse: the swimmers' entries, kicks and strokes, and Poke At.
	 * Default on. Off: only the static head makes waves. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves"))
	bool bWaveImpulse = true;

	/** Scales the impulse source. The static head already radiates a falling body's waves; the slam and drag impulses add the part a
	 * linear field misses, and at 1 they counted the same water twice (impulse rings 2 to 3 times Evers' measurements). 0.35 matches the
	 * first crests of the ripple-evers bench. It scales Add Water Impulse too (the swimmers' entries, kicks and strokes, Poke At). Default
	 * 0.35. At least 0; 0 sends no impulses. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves && bWaveImpulse", ClampMin = "0"))
	float WaveImpulseScale = 0.35f;

	/** The impulses are spread over a Gaussian this wide (σ), or three times the σ of the piece of hull each came from if that is wider,
	 * at most sixteen wave cells (under a quarter of a cell: bilinear between the four nearest cells). The impulse each puts in is unchanged.
	 * Wider: lower, smoother rings; narrower: sharper ones, and an impulse pressed into too small a patch digs a pit that springs back as a
	 * spike. Add Water Impulse spreads at least this wide too. In cm. Default 10. At least 0. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves", ClampMin = "0", Units = "cm"))
	float WaveSourceSmoothCm = 10.f;

	/** The most impulse pieces (one per wet hull piece per physics step, all bodies) sent to the waves in a frame. Past it, each body's
	 * neighbouring pieces pressing the same way are merged into one at their centre (in squares of a wave cell, doubled until they fit),
	 * so the frame's whole impulse and where it lands are kept; this is logged once. 0 sends none. Default 32768. The quality tier may cap
	 * it (fnb.Waves.MaxImpulses: 1024 Low, 4096 Medium, 16384 High; 0 leaves this setting). */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves", ClampMin = "0"))
	int32 MaxWaveImpulses = 32768;

	/** The foam channel (needs a core at interface version 4): whitecaps where waves break, wake trails a moving hull's waterline sheds and
	 * impact rings where something lands, accumulated by the core per wave cell and drawn by the surface material; never fed back into the
	 * waves. Off: the core accumulates none and the surfaces stay clean; the waves are identical either way. Default on. The console
	 * variable fnb.Waves.Foam, the quality tiers' switch (0 at Low, 1 otherwise), must be 1 as well. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves"))
	bool bFoam = true;

	/** How fast the foam fades (User.Wave.FoamDecay, 1/s): 0.7 is an e-fold in about 1.4 s, a boat's trail gone in about 1.5 s. Higher:
	 * shorter trails and rings. Default 0.7. At least 0; 0: the foam never fades (each cell saturates at 1). */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves && bFoam", ClampMin = "0"))
	float FoamDecay = 0.7f;

	/** Scale of the foam breaking waves make (User.Wave.FoamBreaking): the breaking decay times the water's speed. The whitecaps. Default 1.
	 * At least 0; 0: none. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves && bFoam", ClampMin = "0"))
	float FoamBreaking = 1.f;

	/** Scale of the foam a moving hull's waterline sheds (User.Wave.FoamWake): its static head's edge times its speed through the water. The
	 * wake trails and bow foam. Default 1. At least 0; 0: none. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves && bFoam", ClampMin = "0"))
	float FoamWake = 1.f;

	/** Scale of the foam an impulse makes (User.Wave.FoamImpact): slams, Add Water Impulse, pokes and swimmer strokes. The impact rings.
	 * Default 1. At least 0; 0: none. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves && bFoam", ClampMin = "0"))
	float FoamImpact = 1.f;

	/** The surface material's scalar that scales the drawn wave height (MF_NinjaOutput_BaseFunction_SURFACE's WaveHeightScale): set to 0 on
	 * the captured surfaces' dynamic materials while the Surface Mirror is drawn, so the mirror shows FluidNinja's own surface W, and put
	 * back right after. Default WaveHeightScale, the modified core's name: it must match the surface material's scalar. */
	UPROPERTY(config, EditAnywhere, Category = "Waves")
	FName WaveHeightScaleParameter = TEXT("WaveHeightScale");

	/** The surface material's scalar that scales how far the waves bend the drawn normal (MF_NinjaOutput_BaseFunction_SURFACE's
	 * WaveNormalScale): set on each simulation's surface dynamic materials about once a second (Wave Normal Scale Factor, or the
	 * simulation's override). Default WaveNormalScale, the modified core's name: it must match the surface material's scalar. */
	UPROPERTY(config, EditAnywhere, Category = "Waves")
	FName WaveNormalScaleParameter = TEXT("WaveNormalScale");

	/** A simulation's surfaces get WaveNormalScale = this / (0.005 × its size in cm): 0.1 on a 20 m pool, 0.025 on an 80 m river, which
	 * keeps the drawn normal true to the waves' slope whatever the simulation's size. Higher: stronger ripple shading on every simulation;
	 * 0: the waves do not bend the drawn normal. Default 1. At least 0. A Wave Sims entry's Normal Scale replaces it for its simulation. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves", ClampMin = "0"))
	float WaveNormalScaleFactor = 1.f;

	/** The surface material's texture parameter holding the simulation's pressure buffer (RT_PressureDivergence): on a wave simulation its
	 * B is the drawn wave height η + P and its A the static head P. Its size gives the wave cells (the simulation's size / its width). It
	 * is read only once the core has made it RGBA16F (a stock RG16F one has no B, and its A reads 1). Default PressureDivergenceBuffer,
	 * FluidNinja's name; a surface material without it gets no waves on its simulation (logged). */
	UPROPERTY(config, EditAnywhere, Category = "Waves")
	FName PressureBufferParameter = TEXT("PressureDivergenceBuffer");

	/** Wave Follow: every frame the drawn wave height under the view (the pressure buffer's B, η + P, of the simulation under the camera,
	 * else of the nearest one) is copied into Wave Follow Target and its square published in Wave Follow Collection, so a material can move
	 * with the waves: MF_FNB_WaveFollow (Content/Materials) lifts each vertex by the waves' height where it stands. Floating plants, leaves
	 * and litter placed as plain meshes or foliage instances then bob and tilt with wakes, ripples and the wind sea without being buoyancy
	 * bodies. They follow FluidNinja Waves only (FluidNinja's own mesh distortion, a few cm, is not copied) and on one simulation at a time;
	 * on any other, outside its square, and in the editor they stand still. Costs one GPU copy of the pressure buffer a frame (8 MB at 1024
	 * cells). Default on. Off: the materials stand still. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves"))
	bool bWaveFollow = true;

	/** The render target Wave Follow copies the wave height into, which MF_FNB_WaveFollow samples. It takes the pressure buffer's size and
	 * format (RGBA16F) when the followed simulation changes; a stock core's RG16F buffer carries no wave height and is not copied. Default
	 * the plugin's RT_FNB_WaveFollow: a material that follows the waves must sample the same target. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves && bWaveFollow", AllowedClasses = "/Script/Engine.TextureRenderTarget2D"))
	FSoftObjectPath WaveFollowTarget = FSoftObjectPath(TEXT("/FluidNinjaBuoyancy/Materials/RT_FNB_WaveFollow.RT_FNB_WaveFollow"));

	/** The parameter collection Wave Follow publishes in, which MF_FNB_WaveFollow reads: Window (the copied square's centre X and Y and its
	 * size, in cm, and W 1 while the copy is valid, else 0: the materials stand still) and Shape (the surface's WaveHeightScale, Wave Follow
	 * Max Height Cm, Wave Follow Edge Fade Cm). Default the plugin's MPC_FNB_WaveFollow. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves && bWaveFollow", AllowedClasses = "/Script/Engine.MaterialParameterCollection"))
	FSoftObjectPath WaveFollowCollection = FSoftObjectPath(TEXT("/FluidNinjaBuoyancy/Materials/MPC_FNB_WaveFollow.MPC_FNB_WaveFollow"));

	/** How far in from the edge of the followed simulation's square the following eases from full to none, so floating things there do not
	 * step where the waves stop. In cm. Default 300. Range 0-5000; 0: no easing. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves && bWaveFollow", ClampMin = "0", ClampMax = "5000", Units = "cm"))
	float WaveFollowEdgeFadeCm = 300.f;

	/** The most a following vertex moves up or down: the wave height is clamped to it, which also bounds how far the material moves things
	 * past their bounds (a material that follows the waves should allow this much World Position Offset). A bow wave higher than this
	 * flattens on the plants. In cm. Default 60. Range 1-500. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves && bWaveFollow", ClampMin = "1", ClampMax = "500", Units = "cm"))
	float WaveFollowMaxHeightCm = 60.f;

	/** Per simulation: overrides by NinjaLive actor name (its name or label), for a walled pool or a simulation of its own depth. Two ship
	 * for the demo level (FNB_DemoPool): Pool_FluidNinja, a closed pool 4 m deep, and River_FluidNinja, a river lane 3 m deep. A NinjaLive
	 * actor carrying a FluidNinja Wave Sim component (UFluidNinjaWaveSimComponent) is set up by that component instead, and an entry here
	 * naming it is ignored (logged once). A simulation with no entry takes the project's Waves settings. */
	UPROPERTY(config, EditAnywhere, Category = "Waves", meta = (EditCondition = "bWaves"))
	TArray<FFluidNinjaWaveSimSettings> WaveSims;

	/*
	 * Swimmers: everything a swimmer does to the water is switched here. Three things, each its own switch: its body pushes the water aside
	 * (Swimmer Waves: the static head FluidNinja Waves draw and the floaters feel), its parts splash and press rings where they go in
	 * (Swimmer Contacts), and its push through the water is written into FluidNinja's point input (Swimmer Wakes). A registered swimmer's
	 * FFluidNinjaSwimmerSource turns the first two off for itself alone.
	 */
	/** Swimmers push the water aside (FluidNinja Waves' static head, as a hull's): the player's pawn as a body lying along the water, sized
	 * from its capsule and holding Swimmer Volume Litres, and each registered swimmer by its head, chest and hips, so swimming, wading and
	 * jumping in make waves. A pawn whose capsule has no collision (seated in a boat, a boarding clip) makes none. Default on. Needs Waves
	 * and Wave Displacement; a registered swimmer's source can turn it off for that swimmer alone. */
	UPROPERTY(config, EditAnywhere, Category = "Swimmers", meta = (EditCondition = "bWaves"))
	bool bSwimmerWaves = true;

	/** The player's pawn's own volume, litres: the most water it pushes aside. The body is drawn as a lying ellipsoid turned to the pawn's
	 * facing, 0.95 of the capsule's half height long each way and 0.65 of its radius wide each way, as thick as this volume makes it (76 L:
	 * semi-axes 85 × 23 cm, 9.4 cm thick each way); it presses only its part under the surface, and comes in as the water rises from the
	 * pawn's waist to its chest (wading presses little). Higher: taller waves around the swimmer. Default 76 (a 75 kg swimmer at the human
	 * body's mean density, about 985 kg/m³). At least 1. Registered swimmers' spheres are not affected. */
	UPROPERTY(config, EditAnywhere, Category = "Swimmers", meta = (EditCondition = "bWaves && bSwimmerWaves", ClampMin = "1"))
	float SwimmerVolumeLitres = 76.f;

	/** Swimmers' hands, forearms, feet and head (a diver's whole body) splash where they go into the water and press a ring into it
	 * (FluidNinja Waves), sized by how big they are and how fast they go in; feet kicking just under the surface churn it. The player's
	 * pawn (its skeletal mesh, while it is in the water) and every swimmer registered with UFluidNinjaBuoyancySubsystem::RegisterSwimmer (an AI
	 * swimmer or diver, say) take part. The splash is the project's Splash System; the rings need Wave Impulse. Default on. Off also stops
	 * the player's pawn's stroke push and dive splash (the pawn is followed only while this and Player Contacts are on); registered
	 * swimmers keep theirs. */
	UPROPERTY(config, EditAnywhere, Category = "Swimmers")
	bool bSwimmerContacts = true;

	/** The player's pawn makes body-part splashes and rings too. Off: only registered swimmers do (turn it off if the pawn's own swim
	 * animation already spawns stroke splashes); the pawn's stroke push and dive splash stop with them. Default on. Ignored when the pawn's
	 * mesh is itself a registered swimmer: that swimmer makes the contacts. */
	UPROPERTY(config, EditAnywhere, Category = "Swimmers", meta = (EditCondition = "bSwimmerContacts"))
	bool bPlayerContacts = true;

	/** A body part going in slower than this (straight down) makes no splash and no ring; one coming out faster than 1.3 times it (a
	 * swimmer profile's Exit Speed Factor) makes a small splash. Higher: only faster entries splash. In cm/s. Default 60. At least 1 in use;
	 * a swimmer profile's Entry Min Cm S (0 or more) replaces it for that swimmer. */
	UPROPERTY(config, EditAnywhere, Category = "Swimmers", meta = (EditCondition = "bSwimmerContacts", ClampMin = "0", Units = "cm/s"))
	float SwimmerEntryMinCmS = 60.f;

	/** Scales the body parts' splashes and a dive's entry splash (1: the splash a slam of the same power would make, on the Splash Reference
	 * Watts scale), times a swimmer profile's Splash Scale. Default 1. At least 0; 0: no swimmer splashes. Editable whatever Swimmer
	 * Contacts says: a registered swimmer's dive splash uses it with Swimmer Contacts off too. */
	UPROPERTY(config, EditAnywhere, Category = "Swimmers", meta = (ClampMin = "0"))
	float SwimmerSplashScale = 1.f;

	/** Scales the body parts' impulses into the waves (1: the part's added mass times the speed it goes in at) and Poke At's (the Blueprint
	 * push on the waves, which works whatever Swimmer Contacts says), times a swimmer profile's Impulse Scale for
	 * the parts; Wave Impulse Scale applies on top, and Wave Impulse must be on. Higher: taller rings where limbs go in. Default 1. At least
	 * 0; 0: none. Editable whatever Swimmer Contacts says, since Poke At uses it with Swimmer Contacts off too. */
	UPROPERTY(config, EditAnywhere, Category = "Swimmers", meta = (ClampMin = "0"))
	float SwimmerImpulseScale = 1.f;

	/** A part going in less than this long after another part of the same swimmer went in nearby (Swimmer Cavity Cm) enters the air
	 * pocket that one opened: a third of the splash and half the impulse. A pencil or head-first entry stays small (its body follows its
	 * feet or hands into their hole); a tuck, its hips and feet slapping in side by side at once, does not. In s. Default 0.35. At least 0;
	 * 0: no air pockets. */
	UPROPERTY(config, EditAnywhere, Category = "Swimmers", meta = (EditCondition = "bSwimmerContacts", ClampMin = "0", Units = "s"))
	float SwimmerCavitySeconds = 0.35f;

	/** How near (across the surface) a part must go in to another's entry to enter its air pocket (Swimmer Cavity Seconds). Larger: more
	 * entries count as following one another. In cm. Default 30. At least 0; 0: no air pockets. */
	UPROPERTY(config, EditAnywhere, Category = "Swimmers", meta = (EditCondition = "bSwimmerContacts", ClampMin = "0", Units = "cm"))
	float SwimmerCavityCm = 30.f;

	/** Feet moving up and down at most this deep under the surface churn it (a flutter kick), more the nearer they are: going up they push
	 * the water above up, going down they draw it down. In cm. Default 25. At least 0; 0: no kicks. The kicks need Wave Impulse. */
	UPROPERTY(config, EditAnywhere, Category = "Swimmers", meta = (EditCondition = "bSwimmerContacts", ClampMin = "0", Units = "cm"))
	float SwimmerKickDepthCm = 25.f;

	/** Scales the strokes' push on the water (FFluidNinjaSwimmerPose::StrokeImpulse, the FluidNinja Swimmer component's Add Stroke
	 * Impulse): the wake point a stroke writes into FluidNinja's point input (with Swimmer Wakes) and the small impulse it presses into the
	 * waves (with Wave Impulse), both fading as the chest goes deeper under the surface. 0: strokes do nothing. Default 1. At least 0.
	 * Registered swimmers' strokes run whatever Swimmer Contacts says (so it stays editable with Swimmer Contacts off); the player's
	 * pawn's need Swimmer Contacts and Player Contacts. */
	UPROPERTY(config, EditAnywhere, Category = "Swimmers", meta = (ClampMin = "0"))
	float SwimmerStrokeScale = 1.f;

	/** Swimmers write their push through the water into FluidNinja's point input as moving brush points (the Wake Data Channel, as the
	 * floating bodies' wakes), and a ring pushing outward where they plunge in. The player's pawn while its Character Movement swims over a
	 * water volume (wading: at each foot), and every registered swimmer in its water; either leaves the surface alone once its top is 50 cm
	 * under it. FluidNinja ignores points slower than about 40 cm/s, and a faster point paints dye that speeds a river up along its flow:
	 * at the default gain only the plunge ring and fast swimming against a current reach the water (Docs/FluidNinjaWaves.md, Wakes and the
	 * current). Needs no modified FluidNinja core. Default on. Console: fnb.Buoyancy.Wake 0 turns these off with the floating bodies' wakes
	 * (1 does not turn them on), and fnb.Wake.MinWriteCmS (default 40) skips points slower than that. */
	UPROPERTY(config, EditAnywhere, Category = "Swimmers")
	bool bSwimmerWakes = true;

	/** The wake's target: the current plus this share of the swimmer's speed through the water (a hull's Wake Strength). Each frame the
	 * wake writes Swimmer Wake Gain of what the water at the swimmer still lacks of it. Higher: a stronger push behind a moving swimmer.
	 * Default 0.35. Range 0-1. A registered swimmer whose provider returns a Wake Tuning with Override on (the FluidNinja Swimmer
	 * component's Wake Tuning) uses its own values in place of this and the other swimmer wake settings below. */
	UPROPERTY(config, EditAnywhere, Category = "Swimmers", meta = (EditCondition = "bSwimmerWakes", ClampMin = "0", ClampMax = "1"))
	float SwimmerWakeStrength = 0.35f;

	/** Share of what the water at a swimmer still lacks of the target that the wake writes each frame (a hull's Wake Gain). FluidNinja
	 * ignores points under about 40 cm/s; above that it drives the water to about 2.3 times the written speed in still water and speeds a
	 * river up along its flow. Measured in the demo: at 0.2 a 3 m/s swimmer leaves no wake in still water, and swimming upstream speeds the
	 * river up around it by 1 to 1.3 m/s; at 0.6 still water shows a wake (about 0.5 m/s just behind the swimmer), but a river speeds up
	 * around a swimmer that only holds its place. Default 0.2. Range 0-1. A swimmer's own Wake Tuning (Override on) replaces it. */
	UPROPERTY(config, EditAnywhere, Category = "Swimmers", meta = (EditCondition = "bSwimmerWakes", ClampMin = "0", ClampMax = "1"))
	float SwimmerWakeGain = 0.2f;

	/** Brush size of a swimmer's wake point: 0.6 to 1.5 times this, growing with the speed from 60 to 150 cm/s. Wading uses two thirds of it
	 * at each foot, a plunge ring 1.2 times it, a stroke's point the same as swimming. Larger: the push spread over more water. In cm.
	 * Default 70. At least 1. A swimmer's own Wake Tuning (Override on) replaces it. */
	UPROPERTY(config, EditAnywhere, Category = "Swimmers", meta = (EditCondition = "bSwimmerWakes", ClampMin = "1", Units = "cm"))
	float SwimmerWakeSizeCm = 70.f;

	/** A swimmer moving slower than this through the water leaves no wake, and a stroke whose push is slower writes no point. The console
	 * variable fnb.Wake.MinWriteCmS (default 40, about where FluidNinja starts to see a point) is a second floor, on the written point's
	 * speed. In cm/s. Default 15. At least 0. A swimmer's own Wake Tuning (Override on) replaces it. */
	UPROPERTY(config, EditAnywhere, Category = "Swimmers", meta = (EditCondition = "bSwimmerWakes", ClampMin = "0", Units = "cm/s"))
	float SwimmerWakeMinSpeedCmS = 15.f;

	/** The current a swimmer is in is read on a ring around it, not under it where its own wake is: from this far out to Swimmer Current
	 * Ring Outer Cm. The wakes read it there (they push only what the water around them lacks), and so does Get Current At Location when
	 * its radii are left negative. In cm. Default 70. At least 0; 0 reads the whole disc. A swimmer's own Wake Tuning (Override on)
	 * replaces it for that swimmer's wake. */
	UPROPERTY(config, EditAnywhere, Category = "Swimmers", meta = (ClampMin = "0", Units = "cm"))
	float SwimmerCurrentRingInnerCm = 70.f;

	/** The outer radius of the ring a swimmer's current is read on (see Swimmer Current Ring Inner Cm). Wider: the current averaged over
	 * more water. In cm. Default 160. At least 10, and at least 10 cm past the inner radius (a smaller value is moved out to that). A
	 * swimmer's own Wake Tuning (Override on) replaces it for that swimmer's wake. */
	UPROPERTY(config, EditAnywhere, Category = "Swimmers", meta = (ClampMin = "10", Units = "cm"))
	float SwimmerCurrentRingOuterCm = 160.f;
};
