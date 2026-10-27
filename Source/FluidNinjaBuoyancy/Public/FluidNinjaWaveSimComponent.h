#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FluidNinjaBuoyancySettings.h"
#include "FluidNinjaWaveSimComponent.generated.h"

/**
 * FluidNinja Waves on one simulation, carried by the level: add it to a NinjaLive actor and it stands in for that actor's Wave Sims entry
 * (Project Settings → FluidNinja Buoyancy → Waves → Wave Sims), so the facts about the water (its depth, whether it is a closed basin)
 * travel with the actor rather than with a project-wide list keyed by its name. A NinjaLive with this component is set up from it alone;
 * an ini entry naming it as well is ignored, and the waves log that once. Tools → FluidNinja Buoyancy: Check Setup lists both. It carries
 * the entry's depth, walls, scale-down, importance, damping, foam and surface settings; Normal Scale, Current Coupling and the wind sea
 * stay the ini entry's own.
 */
UCLASS(ClassGroup = (FluidNinjaBuoyancy), meta = (BlueprintSpawnableComponent, DisplayName = "FluidNinja Wave Sim"))
class FLUIDNINJABUOYANCY_API UFluidNinjaWaveSimComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	/** This simulation makes waves. Off: it runs none (User.WaveSimEnabled false) and its floaters feel FluidNinja's own surface. Default on.
	 * Needs the project's Waves setting and the modified FluidNinja core; turn it off for water whose waves nobody sees or feels. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves")
	bool bWaves = true;

	/** This simulation's water depth for the waves everywhere in it (User.Wave.DepthCm), instead of the project's Wave Depth and of the depth
	 * FluidNinja's height field would give: shallower water makes slower, steeper waves, deeper water faster ones. In cm. Default 200; values
	 * under 1 are used as 1. Set it to the real depth of the pool or channel (waves that run too fast or too slow mean it is wrong). Read only
	 * while Depth From Bed is off. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves", meta = (ClampMin = 0, Units = "cm"))
	float DepthCm = 200.f;

	/** The core takes the depth from FluidNinja's height field where it has one (water Z minus its bed, User.Wave.DepthFromBed), and the
	 * project's Wave Depth where it has none; Depth Cm is then not used. Off (the default): Depth Cm everywhere, for a pool or channel whose
	 * height field reads its water surface rather than its floor. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves")
	bool bDepthFromBed = false;

	/** A closed basin: the simulation window's border is a wall that reflects the waves (User.Wave.EdgeOpen 0) and takes energy out of them
	 * within Wall Band of it at Wall Absorption, as a pool's gutter does. Off (the default): open water, where waves leave through the sponge.
	 * Turn it on for a pool whose walls lie at the simulation window's edge. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves")
	bool bWalled = false;

	/** How strongly a walled border takes energy out of the waves reaching it (User.Wave.WallAbsorption, 1/s on the water's motion within
	 * Wall Band of the wall): higher calms a closed pool sooner, lower keeps more of its echoes. Default 0.4, which returns about 80% of a
	 * 2 m wave's height. At least 0; 0 returns everything, and a closed basin then keeps every wave a floater makes (floaters bob for
	 * minutes). Only while Walled is on. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves", meta = (EditCondition = "bWalled", ClampMin = 0))
	float WallAbsorption = 0.4f;

	/** Width of the band along a walled border that Wall Absorption acts in (User.Wave.WallBandCm), in cm: wider takes energy out over more
	 * of the water. Default 50; at least 0. Only while Walled is on. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves", meta = (EditCondition = "bWalled", ClampMin = 0, Units = "cm"))
	float WallBandCm = 50.f;

	/** The wave grid's scale-down for this simulation, 1, 2 or 4 (User.Wave.ScaleDown): its waves run on its pressure buffer's cells over this,
	 * so a tick costs a quarter at 2 and a sixteenth at 4, and the waves are drawn that much coarser (short ripples are lost). 0 (the default):
	 * the console variable fnb.Waves.ScaleDown, which the quality tiers (sg.WaterQuality) set. 3 is used as 2. Needs a core at interface
	 * version 3 (an older one runs at 1). Set it when the scale-down is a fact about this water, such as 1 for a small pool with fine cells. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves", meta = (ClampMin = 0, ClampMax = 4))
	int32 ScaleDownOverride = 0;

	/** How much this simulation matters when fnb.Waves.MaxActiveSims keeps only the simulations nearest the camera running waves (the
	 * quality tiers cap it: 4 at Epic, the desktop default): its distance from the camera (to its nearest floating body or swimmer) is divided
	 * by this before the simulations are ranked, so at 2 it counts as half as far away (the player's pool over a distant pond). Default 1;
	 * at least 0.01. Raise it for the water the player watches; no effect while fnb.Waves.MaxActiveSims is 0 (all run). */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves", meta = (ClampMin = 0.01))
	float Importance = 1.f;

	/** Use Damping for this simulation instead of the project's Wave Damping. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves", meta = (InlineEditConditionToggle))
	bool bOverrideDamping = false;

	/** How fast this simulation's waves lose their energy (User.Wave.Damping, 1/s), instead of the project's Wave Damping (0.02: an e-fold
	 * in 50 s, as open water). The wake seen behind a moving boat is only a second or two old, while the waves that catch up with a stopped
	 * boat are 5-15 s old, so 0.1-0.3 settles a stopped boat's water in a few seconds and barely changes the moving wake. The bodies' own
	 * fields take the same rate. Default 0.02; at least 0. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves", meta = (EditCondition = "bOverrideDamping", ClampMin = 0))
	float Damping = 0.02f;

	/** Use Foam Decay for this simulation instead of the project's. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves|Foam", meta = (InlineEditConditionToggle))
	bool bOverrideFoamDecay = false;

	/** How fast this simulation's white foam fades (User.Wave.FoamDecay, 1/s), instead of the project's Foam Decay: 0.7 is an e-fold in
	 * about 1.4 s; lower leaves longer white trails (a fast boat's wake stays white for tens of metres at 0.3-0.5). Default 0.7; at least 0. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves|Foam", meta = (EditCondition = "bOverrideFoamDecay", ClampMin = 0))
	float FoamDecay = 0.7f;

	/** Use Foam Aged Decay for this simulation instead of the project's. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves|Foam", meta = (InlineEditConditionToggle))
	bool bOverrideFoamAgedDecay = false;

	/** How fast this simulation's aged foam (the long pale trail) fades (User.Wave.FoamAgedDecay, 1/s), instead of the project's Foam Aged
	 * Decay: 0.02 is an e-fold in 50 s; higher gives shorter pale trails (0.08: about 12 s). Default 0.02; at least 0. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves|Foam", meta = (EditCondition = "bOverrideFoamAgedDecay", ClampMin = 0))
	float FoamAgedDecay = 0.02f;

	/** Use Foam Wake for this simulation instead of the project's. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves|Foam", meta = (InlineEditConditionToggle))
	bool bOverrideFoamWake = false;

	/** Scale of the foam a moving hull's waterline sheds in this simulation (User.Wave.FoamWake), the core's own bow and side foam, instead
	 * of the project's Foam Wake. Water whose boats draw their whitewater from their own Wake settings (Prop Wash Foam, Air Fan Wash Foam,
	 * Quarter Wake Foam) wants little of it (0.1-0.3), or it rings the bow and fills the dark lanes. The core's waterline term is a cell or
	 * two wide, so on finer cells the same value sheds less foam per metre of hull. Default 1; at least 0. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves|Foam", meta = (EditCondition = "bOverrideFoamWake", ClampMin = 0))
	float FoamWake = 1.f;

	/** Use Foam Transfer for this simulation instead of the project's. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves|Foam", meta = (InlineEditConditionToggle))
	bool bOverrideFoamTransfer = false;

	/** The share of this simulation's fading white foam that becomes aged foam (User.Wave.FoamTransfer), instead of the project's Foam
	 * Transfer. Lower keeps the dark water between a wake's white bands dark; 0 makes aged foam only from the wash. Default 0.5; range 0-1. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves|Foam", meta = (EditCondition = "bOverrideFoamTransfer", ClampMin = 0, ClampMax = 1))
	float FoamTransfer = 0.5f;

	/** Use Foam Diffusion for this simulation instead of the project's. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves|Foam", meta = (InlineEditConditionToggle))
	bool bOverrideFoamDiffusion = false;

	/** How fast this simulation's white foam spreads (User.Wave.FoamDiffusion, cm²/s), instead of the project's Foam Diffusion: a trail
	 * widens by about the square root of 4 D t. Lower keeps whitewater crisp and its lanes sharp (10); higher softens it. Default 30; at
	 * least 0. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves|Foam", meta = (EditCondition = "bOverrideFoamDiffusion", ClampMin = 0))
	float FoamDiffusion = 30.f;

	/** Use Foam Aged Diffusion for this simulation instead of the project's. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves|Foam", meta = (InlineEditConditionToggle))
	bool bOverrideFoamAgedDiffusion = false;

	/** How fast this simulation's aged foam spreads (User.Wave.FoamAgedDiffusion, cm²/s), instead of the project's Foam Aged Diffusion:
	 * 200 adds about 2 m to a pale trail in a minute; lower keeps it narrow (50). Default 200; at least 0. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves|Foam", meta = (EditCondition = "bOverrideFoamAgedDiffusion", ClampMin = 0))
	float FoamAgedDiffusion = 200.f;

	/** Scalar parameters set on this simulation's surface materials, by name, about once a second: its own look on a FluidNinja material
	 * other water shares, without editing the material. For the whitewater: WaveFoamStrength, WaveFoamAgedStrength (0-1), WaveFoamLace
	 * (0-1), WaveFoamLaceTileCm (100-1000), WaveFoamHeightCm (how far whitewater stands up, 0-40), WaveFoamRoughness (0-5). A name the
	 * material lacks is skipped. Empty by default. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves|Surface")
	TMap<FName, float> SurfaceScalars;

	/** Colour parameters set on this simulation's surface materials, by name, as Surface Scalars: WaveFoamColor (above 1 brightens the
	 * foam), WaveFoamAgedColor. A name the material lacks is skipped. Empty by default. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves|Surface")
	TMap<FName, FLinearColor> SurfaceColors;

	/** Texture parameters set on this simulation's surface materials, by name, as Surface Scalars: WaveFoamLaceTexture (its red channel
	 * breaks the foam into lace; FluidNinja's T_Crumple_MultiLevel gives whitewater's bubbly clumps). A name the material lacks, or a
	 * texture that does not load, is skipped. Empty by default. */
	UPROPERTY(EditAnywhere, Category = "FluidNinja Waves|Surface")
	TMap<FName, TSoftObjectPtr<UTexture>> SurfaceTextures;

	/** The per-simulation settings as the ini list carries them, for the waves to consume in place of an ini entry. */
	FFluidNinjaWaveSimSettings ToSettings() const;
};
