#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "FluidNinjaBuoyancyDemoCharacter.generated.h"

class UCameraComponent;
class USpringArmComponent;

/**
 * The demo level's player: a third-person UE5 mannequin (Manny, with the Third Person template's unarmed Animation Blueprint) that
 * walks the decks and the dock, wades down the pool's steps and swims in its water volume, and pushes and stands on the floating
 * things.
 *
 * Controls (read straight from the keys, so it needs no input mappings or Enhanced Input setup in the project): WASD or the left
 * stick to move, the mouse or the right stick to look, Space or the bottom face button to jump (held: swim up). While swimming,
 * moving forward follows the view, so looking down dives.
 */
UCLASS(Blueprintable, meta = (DisplayName = "FluidNinja Buoyancy Demo Character"))
class FLUIDNINJABUOYANCYDEMO_API AFluidNinjaBuoyancyDemoCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AFluidNinjaBuoyancyDemoCharacter(const FObjectInitializer& ObjectInitializer);

	virtual void Tick(float DeltaSeconds) override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	TObjectPtr<UCameraComponent> FollowCamera;

	/** Mouse look: degrees per mouse count. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input", meta = (ClampMin = "0"))
	float MouseSensitivity = 1.f;

	/** Right stick look: degrees a second at full tilt. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input", meta = (ClampMin = "0"))
	float StickLookRate = 120.f;

private:
	void UpdateInput(float DeltaSeconds);

	bool bJumpHeld = false;
};
