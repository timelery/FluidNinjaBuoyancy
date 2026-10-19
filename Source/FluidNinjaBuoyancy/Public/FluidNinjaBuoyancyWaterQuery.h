#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FluidNinjaBuoyancyWaterQuery.generated.h"

class UFluidNinjaBuoyancySubsystem;
class USkeletalMeshComponent;

/**
 * The water at a point, as FluidNinja Buoyancy knows it this frame (UFluidNinjaBuoyancyWaterLibrary::GetWaterAtLocation,
 * UFluidNinjaBuoyancySubsystem::SampleWater): one answer for swim code, from the same readings the floating bodies feel.
 *
 * SurfaceZ is the surface a swimmer floats on: the still water W (FluidNinja's own surface as the Surface Mirror drew it, carried forward
 * by its rate, where a floating body's patch holds the point; else the far field, a water volume's top or the project's published water)
 * plus WaveHeight, the free wave height η of FluidNinja Waves there (B − A: the core's drawn wave height less every body's static head, so a
 * swimmer rides the waves without sinking into its own hollow or a hull's), read back from the simulation's pressure buffer with the current
 * probe. Normal is the surface's from η's slope (up without waves). Current is FluidNinja's flow on the ring around the point (the swimmer's
 * own wake left out: Swimmer Current Ring Inner / Outer Cm), Z 0. Depth is how far the point is under SurfaceZ (negative above it).
 * AgeSeconds is how old the readback the current and the waves came from is (two or three frames); the patch's own age is folded into its
 * carry-forward. Without the modified core there are no waves: SurfaceZ is the still water.
 */
USTRUCT(BlueprintType)
struct FLUIDNINJABUOYANCY_API FFluidNinjaWaterSample
{
	GENERATED_BODY()

	/** There is water here: a floating body's patch holds the point, or the far field answers for it (by default a water volume, a Physics
	 * Volume with Water Volume on, spanning the point in X and Y: the one whose top is nearest; a project's own subsystem may answer from its
	 * published water). False: every other field keeps its default (0, Normal straight up), as while FluidNinja Buoyancy is off. */
	UPROPERTY(BlueprintReadOnly, Category = "Water")
	bool bWater = false;

	/** The surface a swimmer floats on: Still Z plus Wave Height, world Z in cm. Under the test bench's controlled water: its flat height.
	 * Any world height. */
	UPROPERTY(BlueprintReadOnly, Category = "Water", meta = (Units = "cm"))
	float SurfaceZ = 0.f;

	/** The still water the waves ride on (FluidNinja's surface as drawn, carried forward by its rate, or the far field), world Z in cm:
	 * what a swimmer's static head is measured against.
	  * Any world height. */
	UPROPERTY(BlueprintReadOnly, Category = "Water", meta = (Units = "cm"))
	float StillZ = 0.f;

	/** The surface's normal, a unit vector from the slope of the waves; straight up (0, 0, 1) without waves or before they are read. */
	UPROPERTY(BlueprintReadOnly, Category = "Water")
	FVector Normal = FVector::UpVector;

	/** FluidNinja's current around the point, world cm/s, Z always 0 (read on a ring around the point, so a swimmer's own wake is left out:
	 * Swimmer Current Ring Inner / Outer Cm). Zero until a reading has arrived (Has Current), and under the test bench's controlled water. */
	UPROPERTY(BlueprintReadOnly, Category = "Water", meta = (Units = "cm/s"))
	FVector Current = FVector::ZeroVector;

	/** The free wave height η here, in cm, above (positive) or below (negative) the still water: FluidNinja Waves' drawn height less every
	 * body's static head. 0 without waves (no modified core, no wave simulation here, or before the reading arrives: Has Waves). */
	UPROPERTY(BlueprintReadOnly, Category = "Water", meta = (Units = "cm"))
	float WaveHeight = 0.f;

	/** How far the point is under Surface Z, in cm; negative: above it. */
	UPROPERTY(BlueprintReadOnly, Category = "Water", meta = (Units = "cm"))
	float Depth = 0.f;

	/** How old the GPU readback the current and the waves came from is, in seconds: usually two or three frames. 0 when neither was read.
	 * Read-only: 0 or more. */
	UPROPERTY(BlueprintReadOnly, Category = "Water", meta = (Units = "s"))
	float AgeSeconds = 0.f;

	/** The still water came from a floating body's patch (the Surface Mirror and the gather), so it follows FluidNinja's drawn surface;
	 * false: the flat far field. */
	UPROPERTY(BlueprintReadOnly, Category = "Water")
	bool bFromPatch = false;

	/** A current reading has arrived here (GetCurrentAtLocation's rules: a few frames after the first ask at a new place, inside a
	 * simulation's square). False: Current is zero, not measured. */
	UPROPERTY(BlueprintReadOnly, Category = "Water")
	bool bHasCurrent = false;

	/** The waves were read here (the modified core's pressure buffer, a wave simulation under the point). False: Wave Height is 0 and
	 * Normal is straight up. */
	UPROPERTY(BlueprintReadOnly, Category = "Water")
	bool bHasWaves = false;
};

/**
 * One query for swim code: the water at a point from FluidNinja Buoyancy's readings (FFluidNinjaWaterSample), the drift current filtered
 * the way a swim mode should take it, the live water of a registered swimmer, and a poke the footsteps can make when wading.
 */
UCLASS()
class FLUIDNINJABUOYANCY_API UFluidNinjaBuoyancyWaterLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * The water at Location this frame (see FFluidNinjaWaterSample). Each call also asks for a fresh reading of the current and the waves
	 * there, so call it every frame for a moving swimmer; the first calls at a new place return the still water alone until the reading
	 * arrives (bHasCurrent, bHasWaves). False (and bWater false) where no water is known, and while FluidNinja Buoyancy is off.
	 */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Water", meta = (WorldContext = "WorldContextObject"))
	static bool GetWaterAtLocation(const UObject* WorldContextObject, FVector Location, FFluidNinjaWaterSample& OutSample);

	/** As GetWaterAtLocation, on the subsystem. */
	static bool SampleWater(UFluidNinjaBuoyancySubsystem& Subsystem, const FVector& Location, FFluidNinjaWaterSample& OutSample);

	/**
	 * The current a swim mode drifts with, from the raw reading: low-passed over Smooth Seconds (a reading two or three frames old, and
	 * FluidNinja's eddies, must not jerk the swimmer), and dead-banded below Dead Band Cm S (a still pool reads a few cm/s of noise: no
	 * creeping). Previous is last frame's result; bMeasured false (no reading this frame) eases it toward zero over the same time.
	 */
	UFUNCTION(BlueprintPure, Category = "FluidNinja|Water")
	static FVector FilterDriftCurrent(FVector Previous, FVector Measured, bool bMeasured, float DeltaSeconds, float SmoothSeconds = 0.5f, float DeadBandCmS = 10.f);

	/**
	 * The live water of a registered swimmer (Register Swimmer, or a FluidNinja Swimmer component): the sample the plugin took at its hips
	 * this frame, the one source of truth its contacts, its displacement and its wake used. Drift Current is the swimmer's filtered drift
	 * (FilterDriftCurrent at the project's smoothing). False for a mesh that is not registered, or before its first frame in the water.
	 */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Water", meta = (WorldContext = "WorldContextObject"))
	static bool GetSwimmerWater(const UObject* WorldContextObject, const USkeletalMeshComponent* Mesh, FFluidNinjaWaterSample& OutSample, FVector& OutDriftCurrent);

	/**
	 * A poke into the water for a footstep in the shallows, a paddle, a thrown stone: an impulse of Strength newton-seconds pressing the
	 * water down over a disc Radius Cm wide at Location (lifting it when negative), into FluidNinja Waves at the next frame, as the swimmers'
	 * part entries are (scaled by Swimmer Impulse Scale and Wave Impulse Scale). A wading footstep is about 0.6 N·s over 8 cm (a foot's added
	 * water, 0.6 kg, stamped in at 1 m/s). Nothing without the waves. The footstep's own splash and sound stay the footstep system's.
	 */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Water", meta = (WorldContext = "WorldContextObject"))
	static bool PokeAt(const UObject* WorldContextObject, FVector Location, float RadiusCm, float StrengthNs);

	/** The subsystem of WorldContextObject's world (the project's derived one when it has one), or null. */
	static UFluidNinjaBuoyancySubsystem* FindSubsystem(const UObject* WorldContextObject);
};
