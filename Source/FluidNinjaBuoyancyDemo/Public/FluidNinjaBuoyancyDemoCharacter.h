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

	/** The third-person camera's spring arm: 400 cm behind the character, turning with the view (the control rotation). Edit its Target
	 * Arm Length to bring the camera nearer or farther. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	TObjectPtr<USpringArmComponent> CameraBoom;

	/** The player's camera, at the end of Camera Boom. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	TObjectPtr<UCameraComponent> FollowCamera;

	/** Mouse look speed: a multiplier on the mouse's movement each frame before it reaches the controller's yaw and pitch input (degrees
	 * per mouse count when the controller's own input scale is 1). Higher turns faster. Default 1; at least 0, and 0 turns mouse look off.
	 * The mouse does not look while its cursor is shown (Tab, for the demo river's panel). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input", meta = (ClampMin = "0"))
	float MouseSensitivity = 1.f;

	/** Right stick look speed at full tilt: this times the frame time goes to the controller's yaw and pitch input (degrees a second when
	 * the controller's own input scale is 1). Higher turns faster. Default 120; at least 0, and 0 turns stick look off. Stick deflections
	 * under 0.15 are ignored (dead zone). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input", meta = (ClampMin = "0"))
	float StickLookRate = 120.f;

private:
	void UpdateInput(float DeltaSeconds);

	bool bJumpHeld = false;
};
