#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "InputCoreTypes.h"
#include "FluidNinjaBuoyancyDemoNight.generated.h"

class UDirectionalLightComponent;
class UPostProcessComponent;
class USceneComponent;

/** How dark a lantern night is (AFluidNinjaBuoyancyDemoNight). */
USTRUCT(BlueprintType)
struct FLUIDNINJABUOYANCYDEMO_API FFluidNinjaNightLook
{
	GENERATED_BODY()

	/** The night's faint ambient: a cool, shadowless moonlight, in lux. Higher shows more of the banks and the water; lower leaves only
	 * what the lanterns light. Default 0.5. At least 0; 0 turns the moon off (black but for the lanterns). Its look depends on Exposure
	 * Bias as much as on this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Night", meta = (ClampMin = "0", Units = "Lux"))
	float MoonLux = 0.5f;

	/** The level's sky lights at this fraction of their own intensity during the night: a little adds an even ambient from the sky light's
	 * captured sky. Default 0 (sky lights off). Range 0-1. A sky light holding a day capture is very bright at night exposure, so try
	 * 0.001-0.01 first; a sky light that recaptures in real time sees the hidden sky and adds nothing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Night", meta = (ClampMin = "0", ClampMax = "1"))
	float SkyLightScale = 0.f;

	/** The night's fixed exposure (manual exposure, no physical camera), as an exposure compensation in stops: each step up doubles the
	 * brightness of the whole image, lanterns included. Default -3, the lantern's own candle-only look. Range -15 to 15. Raise it if the
	 * lanterns read dull, lower it if their paper burns out. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Night", meta = (ClampMin = "-15", ClampMax = "15"))
	float ExposureBias = -3.f;
};

/**
 * A night for floating lanterns, while this actor is in the world: every light in the level is switched off (the sun, the sky light,
 * any lamps: all but the lanterns' own and lights spawned after it), the sky atmosphere, height fog and clouds are hidden, a dim moon
 * gives a faint ambient (Look.MoonLux) and the exposure is held fixed (Look.ExposureBias) so the candles read as candles. Destroying it
 * puts everything back as it was. Nothing in the level is changed or saved: it is all run time.
 *
 * While it runs, Drop Ball Key (B) drops a large, heavy ball into the middle of the lanterns wherever they have drifted
 * (AFluidNinjaBuoyancyDemoBall), from anywhere: the player can follow the lanterns down the river and drop balls into them.
 *
 * Console: fnb.Demo.Night [moon lux] [sky light scale] [exposure bias] tunes a running night live (no arguments: the values now);
 * fnb.Demo.DropBall drops a ball as the key does.
 */
UCLASS(NotPlaceable, meta = (DisplayName = "FluidNinja Buoyancy Demo Night"))
class FLUIDNINJABUOYANCYDEMO_API AFluidNinjaBuoyancyDemoNight : public AActor
{
	GENERATED_BODY()

public:
	AFluidNinjaBuoyancyDemoNight();

	/** How dark the night is: the moon, the sky lights' share and the exposure. Set when the night begins (Begin); Apply Look or the
	 * console command fnb.Demo.Night applies a change while it runs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Night")
	FFluidNinjaNightLook Look;

	/** The actor's root, at the world origin: the night has no place, only its moon's direction. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Night")
	TObjectPtr<USceneComponent> Root;

	/** The moon: a movable, shadowless, pale blue directional light (pitch -38, yaw 35) at Look's Moon Lux; hidden at 0. It does not light
	 * the sky atmosphere, which the night hides. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Night")
	TObjectPtr<UDirectionalLightComponent> Moon;

	/** The night's exposure: an unbound post process at priority 10000 (above the level's volumes) holding manual exposure at Look's
	 * Exposure Bias. It sets only the exposure; every other post process setting stays the level's. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Night")
	TObjectPtr<UPostProcessComponent> Exposure;

	/** The key that drops a ball into the middle of the lanterns while the night runs, wherever the player is. Default B (free in the
	 * demo level and in GASP's sandbox input). None: no key (fnb.Demo.DropBall still drops one). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Night")
	FKey DropBallKey;

	/** How high above the lanterns the ball is let go, in cm: higher hits the water faster, so it plunges deeper and makes bigger waves.
	 * Default 400. At least 50. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Night", meta = (ClampMin = "50", UIMax = "1500", Units = "cm"))
	float DropHeightCm = 400.f;

	/** How many dropped balls stay in the water at once: a further drop takes away the oldest. Default 4. Range 1-20. The night's end
	 * takes them all away. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Night", meta = (ClampMin = "1", ClampMax = "20"))
	int32 MaxBalls = 4;

	/** Drops a ball into the middle of the lanterns now (as Drop Ball Key does). */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Demo")
	void DropBall();

	virtual void Tick(float DeltaSeconds) override;

	/** Applies Look now (the moon, the sky lights' share, the exposure). */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Demo")
	void ApplyLook();

	/** Spawns a night in World with Look (one at a time: an existing night takes the new Look instead). */
	static AFluidNinjaBuoyancyDemoNight* Begin(UWorld& World, const FFluidNinjaNightLook& Look);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	/** What was switched off or dimmed, to put back. */
	TArray<TWeakObjectPtr<USceneComponent>> Hidden;
	TMap<TWeakObjectPtr<USceneComponent>, float> SkyLights;
	/** The balls dropped this night, oldest first. */
	TArray<TWeakObjectPtr<AActor>> Balls;
};
