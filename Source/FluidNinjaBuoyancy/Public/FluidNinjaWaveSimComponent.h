#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FluidNinjaBuoyancySettings.h"
#include "FluidNinjaWaveSimComponent.generated.h"

/**
 * FluidNinja Waves on one simulation, carried by the level: add it to a NinjaLive actor and it stands in for that actor's Wave Sims entry
 * (Project Settings → FluidNinja Buoyancy → Waves → Wave Sims), so the facts about the water (its depth, whether it is a closed basin)
 * travel with the actor rather than with a project-wide list keyed by its name. A NinjaLive with this component is set up from it alone;
 * an ini entry naming it as well is ignored, and the waves log that once. Tools → FluidNinja Buoyancy: Check Setup lists both.
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

	/** The per-simulation settings as the ini list carries them, for the waves to consume in place of an ini entry. */
	FFluidNinjaWaveSimSettings ToSettings() const;
};
