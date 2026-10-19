#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "FluidNinjaBuoyancyDemoBall.generated.h"

class UFluidNinjaBuoyancyComponent;
class UPointLightComponent;
class UStaticMeshComponent;

/**
 * A large, heavy floating ball for the lantern night: dropped from a height into the middle of the lantern group (B during a lantern
 * night, or fnb.Demo.DropBall), it plunges deep, throws out a ring of waves and a wake ring into FluidNinja's flow that push the
 * lanterns aside, then bobs back up and floats among them: what a body with real buoyancy does and FluidNinja's own particle-driven
 * floaters cannot. By default 1 m across and 200 kg, so it floats with about 40% of itself under water. It glows a cool blue and casts
 * a soft light round itself, so it can be followed in the dark and its splash and rings show.
 */
UCLASS(meta = (DisplayName = "FluidNinja Buoyancy Demo Ball"))
class FLUIDNINJABUOYANCYDEMO_API AFluidNinjaBuoyancyDemoBall : public AActor
{
	GENERATED_BODY()

public:
	AFluidNinjaBuoyancyDemoBall();

	/** The ball: the engine's sphere, scaled to Diameter Cm, simulating physics with continuous collision so a fast drop never passes
	 * through a lantern. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ball")
	TObjectPtr<UStaticMeshComponent> Ball;

	/** The ball's FluidNinja Buoyancy: one pontoon the size of the ball (Spheres), Body Mass, dropped rather than set on the water, and
	 * Interaction Wakes with Wake on, so its plunge writes an impact ring into FluidNinja's flow as well as the waves. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ball")
	TObjectPtr<UFluidNinjaBuoyancyComponent> Buoyancy;

	/** A soft, shadowless light at the ball's centre (Glow Lumens), so the water round it, its splash and its rings show in the dark. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ball")
	TObjectPtr<UPointLightComponent> GlowLight;

	/** The colour the ball glows and lights the water round it (its paint's emissive colour and the light's colour). Default a cool blue
	 * (0.45, 0.72, 1.0) that stands apart from the warm lanterns. Read at Begin Play. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball")
	FLinearColor GlowColor = FLinearColor(0.45f, 0.72f, 1.f);

	/** How brightly the ball's paint glows (emissive strength): higher reads brighter in the dark. Default 6, tuned for the lantern night's
	 * exposure (bias -3). At least 0; 0 does not glow. Read at Begin Play. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball", meta = (ClampMin = "0", UIMax = "50"))
	float GlowStrength = 6.f;

	/** The ball's light, in lumens: how much it lights the water round it (out to about 6 m). Default 60, about five lanterns' candles.
	 * At least 0; 0 has no light. Read at Begin Play. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball", meta = (ClampMin = "0", UIMax = "500", Units = "Lumens"))
	float GlowLumens = 60.f;

	/** The ball's diameter, in cm: larger displaces more water, makes bigger waves and pushes the lanterns further. Default 100. At least
	 * 20. Read at Begin Play. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball", meta = (ClampMin = "20", UIMax = "300", Units = "cm"))
	float DiameterCm = 100.f;

	/** The ball's mass, in kg: heavier plunges deeper and floats lower. It floats while lighter than the water its whole volume holds
	 * (524 kg at 1 m across) and sinks when heavier. Default 200 (it floats about 40% under). At least 1. Read at Begin Play. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball", meta = (ClampMin = "1", UIMax = "1000", Units = "kg"))
	float MassKg = 200.f;

	/**
	 * Drops a ball into the middle of the lanterns afloat in World (the mean place of every FluidNinja Buoyancy Demo Lantern), from
	 * HeightCm above them. Returns the ball, or null when no lanterns are afloat.
	 */
	static AFluidNinjaBuoyancyDemoBall* DropOnLanterns(UWorld& World, float HeightCm = 400.f);

protected:
	virtual void BeginPlay() override;
};
