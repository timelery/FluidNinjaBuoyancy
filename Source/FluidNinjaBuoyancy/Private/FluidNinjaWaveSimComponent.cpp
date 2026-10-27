#include "FluidNinjaWaveSimComponent.h"

#include "GameFramework/Actor.h"

FFluidNinjaWaveSimSettings UFluidNinjaWaveSimComponent::ToSettings() const
{
	// Exactly what an ini entry for this actor would read: the actor by its name (the waves match a Wave Sims entry by name or label; the
	// name is what the owner has for certain), Depth Cm everywhere unless the height field's bed is wanted, and the window's edge as the
	// component states it: a closed basin is a walled edge with its absorption and band, anything else open water. Normal Scale stays the
	// project's.
	FFluidNinjaWaveSimSettings Out;
	const AActor* Owner = GetOwner();
	Out.NinjaLive = Owner ? Owner->GetFName() : NAME_None;
	Out.bEnabled = bWaves;
	Out.bOverrideDepth = !bDepthFromBed;
	Out.DepthCm = DepthCm;
	Out.bOverrideEdgeOpen = true;
	Out.bEdgeOpen = !bWalled;
	Out.bOverrideWallAbsorption = bWalled;
	Out.WallAbsorption = WallAbsorption;
	Out.bOverrideWallBand = bWalled;
	Out.WallBandCm = WallBandCm;
	Out.ScaleDownOverride = ScaleDownOverride;
	Out.Importance = Importance;
	// Its damping, foam and surface, as the entry's fields of the same names.
	Out.bOverrideDamping = bOverrideDamping;
	Out.Damping = Damping;
	Out.bOverrideFoamDecay = bOverrideFoamDecay;
	Out.FoamDecay = FoamDecay;
	Out.bOverrideFoamAgedDecay = bOverrideFoamAgedDecay;
	Out.FoamAgedDecay = FoamAgedDecay;
	Out.bOverrideFoamWake = bOverrideFoamWake;
	Out.FoamWake = FoamWake;
	Out.bOverrideFoamTransfer = bOverrideFoamTransfer;
	Out.FoamTransfer = FoamTransfer;
	Out.bOverrideFoamDiffusion = bOverrideFoamDiffusion;
	Out.FoamDiffusion = FoamDiffusion;
	Out.bOverrideFoamAgedDiffusion = bOverrideFoamAgedDiffusion;
	Out.FoamAgedDiffusion = FoamAgedDiffusion;
	Out.SurfaceScalars = SurfaceScalars;
	Out.SurfaceColors = SurfaceColors;
	Out.SurfaceTextures = SurfaceTextures;
	return Out;
}
