#pragma once

#include "CoreMinimal.h"

/** The player's existing tapered body, shared with registered humanoid pawns and their diagnostics. */
struct FFluidNinjaSwimmerBodyShape
{
	double HalfLength = 0.0;
	double HalfWidth = 0.0;
	double HalfThickness = 0.0;
	float WetScale = 0.f;
	float DisplacedLitres = 0.f;

	static FFluidNinjaSwimmerBodyShape Make(float CapsuleRadius, float CapsuleHalfHeight, double CentreZ, double WaterZ, float VolumeLitres)
	{
		FFluidNinjaSwimmerBodyShape Shape;
		Shape.HalfLength = FMath::Max(0.95 * static_cast<double>(CapsuleHalfHeight), 1.0);
		Shape.HalfWidth = FMath::Max(0.65 * static_cast<double>(CapsuleRadius), 1.0);
		const double Volume = FMath::Max(static_cast<double>(VolumeLitres), 0.0);
		Shape.HalfThickness = Volume * 1000.0 / (4.0 / 3.0 * UE_DOUBLE_PI * Shape.HalfLength * Shape.HalfWidth);
		// Waders leave the displacement to their feet; the body fades in between waist and chest depth.
		const double Wet = (WaterZ - (CentreZ - CapsuleHalfHeight)) / FMath::Max(2.0 * CapsuleHalfHeight, 1.0);
		Shape.WetScale = static_cast<float>(FMath::SmoothStep(0.5, 0.75, Wet));
		if (Shape.HalfThickness > 0.0)
		{
			// Integrate the ellipsoid's horizontal sections below the water plane (physical volume, before the wave depth fade).
			const double H = FMath::Clamp((WaterZ - CentreZ) / Shape.HalfThickness, -1.0, 1.0);
			Shape.DisplacedLitres = static_cast<float>(Volume * (0.5 + 0.75 * H - 0.25 * H * H * H) * Shape.WetScale);
		}
		return Shape;
	}
};
