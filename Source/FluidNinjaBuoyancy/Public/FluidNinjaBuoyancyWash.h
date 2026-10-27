#pragma once

#include "CoreMinimal.h"

/** A patch of whitewater added to FluidNinja Waves' foam this frame (FluidNinja Waves interface version 6): a propeller's wash, a stern's,
 * where spray lands, or UFluidNinjaBuoyancySubsystem::AddWaterFoam. Rates are foam per second at the centre of a Gaussian of RadiusCm (σ).
 * Game thread. */
struct FFluidNinjaWaterWash
{
	FVector2D Location = FVector2D::ZeroVector;   // world XY, cm
	float RadiusCm = 50.f;                        // σ, cm
	float FreshRate = 0.f;                        // fresh white foam per second (F1)
	float AgedRate = 0.f;                         // aged foam and bubbles per second (F2)
};
