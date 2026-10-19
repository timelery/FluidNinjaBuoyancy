#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "FluidNinjaBuoyancyDemoLantern.generated.h"

class UBoxComponent;
class UFluidNinjaBuoyancyComponent;

/** Where and how a group of floating lanterns is set on the water (AFluidNinjaBuoyancyDemoLantern::SpawnGroup). */
USTRUCT(BlueprintType)
struct FLUIDNINJABUOYANCYDEMO_API FFluidNinjaLanternGroup
{
	GENERATED_BODY()

	/** How many lanterns are set on the water: more makes a bigger, denser group (and costs two candle lights and about 35k triangles
	 * each). Default 20. Range 1-100. If the ellipse is too small for them at Spacing Cm, fewer are placed and the log says so. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lanterns", meta = (ClampMin = "1", ClampMax = "100"))
	int32 Count = 20;

	/** The group's half length along the river (cm): the lanterns are scattered in an ellipse reaching this far downstream and upstream
	 * of its centre, denser towards the middle. Larger spreads them along the river; smaller packs them (fewer fit). Default 230. At
	 * least 20. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lanterns", meta = (ClampMin = "20", Units = "cm"))
	float HalfLengthCm = 230.f;

	/** The group's half width across the river (cm): the ellipse reaches this far to each side of the line through the floaters' middle.
	 * Keep it inside the channel (half its width less about 30 cm), or lanterns are set on the bank. Default 130. At least 20. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lanterns", meta = (ClampMin = "20", Units = "cm"))
	float HalfWidthCm = 130.f;

	/** The least distance between two lanterns' centres (cm). Smaller packs them tighter; under 51 cm (the 36 cm base corner to corner)
	 * turned lanterns can touch at the start. Default 55. At least 40. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lanterns", meta = (ClampMin = "40", Units = "cm"))
	float SpacingCm = 55.f;

	/** The gap (cm) left between the group and the river's other bodies: the group is set down this far downstream of the floaters, and
	 * no lantern within this distance of any other body's bounds. Larger keeps them further apart. Default 150. At least 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lanterns", meta = (ClampMin = "0", Units = "cm"))
	float ClearanceCm = 150.f;

	/** The lantern drawn on each floating base: an actor Blueprint with its pivot at the bottom centre of a 36 cm base and no collision of
	 * its own; a numeric PhaseOffset variable, if it has one, gets a random value per lantern. None: the plugin's FluidNinja water lantern
	 * (/FluidNinjaBuoyancy/Demo/Lantern/BP_FNB_Lantern). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lanterns")
	TSoftClassPtr<AActor> LanternClass;
};

/**
 * A lit paper lantern floating on FluidNinja: a 36 x 36 cm wooden base (a box hull floating 0.9 cm deep, about 1.2 kg) carrying the
 * lantern Blueprint (Lantern Class: by default the plugin's FluidNinja water lantern, candle light and flicker included). It is set
 * on the water where it is spawned, rides the waves and drifts with the current; it writes no wakes, as a light floater should.
 * SpawnGroup scatters a group of them on a river; the demo river's Lantern night does that.
 */
UCLASS(meta = (DisplayName = "FluidNinja Buoyancy Demo Lantern"))
class FLUIDNINJABUOYANCYDEMO_API AFluidNinjaBuoyancyDemoLantern : public AActor
{
	GENERATED_BODY()

public:
	AFluidNinjaBuoyancyDemoLantern();

	/** The floating base: a 36 x 36 x 6 cm box (hidden in game; the lantern draws the real base) that FluidNinja Buoyancy floats, with
	 * physics on and overlap events off. Its mass comes from the Buoyancy component's Rest Draft (0.9 cm: about 1.2 kg). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lantern")
	TObjectPtr<UBoxComponent> Base;

	/** The base's FluidNinja Buoyancy: Triangles hull at 3 cm, Rest Draft 0.9 cm, set on the water at Begin Play (Settle On Start), and
	 * Interaction Wakes with Wake off, so it rides the current and the waves but writes no wakes. Its settings can be changed per lantern
	 * like any FluidNinja Buoyancy component's. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lantern")
	TObjectPtr<UFluidNinjaBuoyancyComponent> Buoyancy;

	/** The lantern drawn on the base: an actor Blueprint with its pivot at the bottom centre of its base, spawned and attached at Begin
	 * Play. None: the plugin's FluidNinja water lantern. If it does not load, the base floats unseen and the log says so. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lantern")
	TSoftClassPtr<AActor> LanternClass;

	/** The lantern Blueprint's flicker phase, written into its PhaseOffset variable before its construction script runs: lanterns with
	 * different values flicker out of step. Any value; Spawn Group gives each lantern a random one from 0 to 100. Default 0. Read at Begin
	 * Play. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lantern", meta = (UIMin = "0", UIMax = "100"))
	float PhaseOffset = 0.f;

	AActor* GetLantern() const { return Lantern; }

	/**
	 * Scatters Group.Count lanterns on the water in an ellipse round Center, its long axis along Downstream, at random headings, no two
	 * nearer than Group.SpacingCm and none within Group.ClearanceCm of an Avoid actor's bounds. Each is set on the water where it lands.
	 * Returns how many were placed (fewer than Count when the ellipse is too small for them; the log says so).
	 */
	static int32 SpawnGroup(UWorld& World, const FFluidNinjaLanternGroup& Group, const FVector& Center, const FVector& Downstream, const TArray<AActor*>& Avoid,
		TArray<AFluidNinjaBuoyancyDemoLantern*>& OutLanterns);

	/**
	 * Where to put a lantern group on a river so it clears the bodies already floating at its head: on the line through their middle,
	 * downstream of the last of them by Group.ClearanceCm plus the group's half length. Z is the bodies' mean height (the lanterns settle
	 * on the water from there). Without bodies, Fallback.
	 */
	static FVector GroupCenterBeyond(const TArray<AActor*>& Bodies, const FVector& Downstream, const FFluidNinjaLanternGroup& Group, const FVector& Fallback);

	/** The plugin's FluidNinja water lantern Blueprint. */
	static const TCHAR* DefaultLanternPath;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	UPROPERTY(Transient)
	TObjectPtr<AActor> Lantern;
};
