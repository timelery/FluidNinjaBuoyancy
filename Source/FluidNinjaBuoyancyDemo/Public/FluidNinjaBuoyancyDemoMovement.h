#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "FluidNinjaBuoyancyDemoMovement.generated.h"

/**
 * The demo character's movement, with a push that suits floating things. The engine's default push gives every simulated body the
 * same impulse and force whatever it weighs (Push Force Factor 750000, Initial Push Force Factor 500), applied at the contact
 * point: a 200 kg boat feels a firm shove, a 0.8 kg beach ball is fired off at several metres a second, spinning.
 *
 * Here the character pushes a body toward the speed it walks or swims into it, never past it, with at most Max Push Force, at the
 * body's centre of mass: a ball is shoved along at walking pace without being spun, a crate follows more slowly, a boat has to be
 * leaned on to get it going.
 */
UCLASS()
class FLUIDNINJABUOYANCYDEMO_API UFluidNinjaBuoyancyDemoMovement : public UCharacterMovementComponent
{
	GENERATED_BODY()

public:
	/** The most the character pushes a simulated body with, in newtons: each frame a body gets at most this force times the frame time
	 * (at least 1 ms) as an impulse, shared by every hit on it that frame, and never more than brings it up to the character's speed into
	 * it. Higher shoves heavy bodies (a boat) along sooner; light ones already reach walking pace. Default 350; at least 0, and 0 pushes
	 * nothing. On foot the push is horizontal; swimming, in any direction. Needs Enable Physics Interaction; fnb.Demo.EnginePush 1 uses the
	 * engine's push instead. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Physics Interaction", meta = (ClampMin = "0", Units = "N"))
	float MaxPushForce = 350.f;

protected:
	virtual void ApplyImpactPhysicsForces(const FHitResult& Impact, const FVector& ImpactAcceleration, const FVector& ImpactVelocity) override;

private:
	/** What this frame's pushes have given each body already (movement can hit the same body several times in a frame). */
	TMap<TWeakObjectPtr<UPrimitiveComponent>, float> GivenThisFrame;
	uint64 GivenFrame = 0;
};
