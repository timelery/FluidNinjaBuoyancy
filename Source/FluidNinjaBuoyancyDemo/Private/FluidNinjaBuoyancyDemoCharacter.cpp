#include "FluidNinjaBuoyancyDemoCharacter.h"

#include "FluidNinjaBuoyancyDemoMovement.h"
#include "Animation/AnimInstance.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputCoreTypes.h"
#include "UObject/ConstructorHelpers.h"

AFluidNinjaBuoyancyDemoCharacter::AFluidNinjaBuoyancyDemoCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UFluidNinjaBuoyancyDemoMovement>(ACharacter::CharacterMovementComponentName))
{
	PrimaryActorTick.bCanEverTick = true;
	GetCapsuleComponent()->InitCapsuleSize(42.f, 96.f);

	// Turn toward where it moves; the camera turns with the view (as the Third Person template).
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	Movement->bOrientRotationToMovement = true;
	Movement->RotationRate = FRotator(0.f, 500.f, 0.f);
	Movement->MaxWalkSpeed = 500.f;
	Movement->MinAnalogWalkSpeed = 20.f;
	Movement->BrakingDecelerationWalking = 2000.f;
	Movement->BrakingDecelerationFalling = 1500.f;
	Movement->JumpZVelocity = 500.f;
	Movement->AirControl = 0.35f;
	Movement->MaxSwimSpeed = 300.f;
	Movement->Mass = 80.f;
	Movement->GetNavAgentPropertiesRef().bCanSwim = true;

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(GetRootComponent());
	CameraBoom->TargetArmLength = 400.f;
	CameraBoom->bUsePawnControlRotation = true;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

	// The UE5 mannequin and the Third Person template's unarmed Animation Blueprint (idle, walk and run, jump, fall, land, foot IK),
	// in the plugin's demo content. Feet at the capsule's bottom, facing +X.
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> MeshFinder(TEXT("/FluidNinjaBuoyancy/Demo/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple"));
	static ConstructorHelpers::FClassFinder<UAnimInstance> AnimFinder(TEXT("/FluidNinjaBuoyancy/Demo/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed"));
	if (MeshFinder.Succeeded())
	{
		GetMesh()->SetSkeletalMeshAsset(MeshFinder.Object);
	}
	if (AnimFinder.Succeeded())
	{
		GetMesh()->SetAnimationMode(EAnimationMode::AnimationBlueprint);
		GetMesh()->SetAnimInstanceClass(AnimFinder.Class);
	}
	GetMesh()->SetRelativeLocationAndRotation(FVector(0.f, 0.f, -96.f), FRotator(0.f, -90.f, 0.f));
}

void AFluidNinjaBuoyancyDemoCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (IsLocallyControlled())
	{
		UpdateInput(DeltaSeconds);
	}
}

void AFluidNinjaBuoyancyDemoCharacter::UpdateInput(float DeltaSeconds)
{
	APlayerController* Player = Cast<APlayerController>(GetController());
	if (!Player)
	{
		return;
	}
	auto Down = [Player](const FKey& Key) { return Player->IsInputKeyDown(Key) ? 1.f : 0.f; };
	auto Axis = [Player](const FKey& Key)
	{
		const float Value = Player->GetInputAnalogKeyState(Key);
		return FMath::Abs(Value) > 0.15f ? Value : 0.f;   // dead zone
	};

	// Look.
	float MouseX = 0.f;
	float MouseY = 0.f;
	if (!Player->ShouldShowMouseCursor())   // with the cursor out (the demo's river panel), the mouse points instead of looking
	{
		Player->GetInputMouseDelta(MouseX, MouseY);
	}
	AddControllerYawInput(MouseX * MouseSensitivity + Axis(EKeys::Gamepad_RightX) * StickLookRate * DeltaSeconds);
	AddControllerPitchInput(-MouseY * MouseSensitivity - Axis(EKeys::Gamepad_RightY) * StickLookRate * DeltaSeconds);

	// Move: along the ground the view's heading; swimming, the view itself (look down to dive).
	const float Forward = FMath::Clamp(Down(EKeys::W) - Down(EKeys::S) + Axis(EKeys::Gamepad_LeftY), -1.f, 1.f);
	const float Right = FMath::Clamp(Down(EKeys::D) - Down(EKeys::A) + Axis(EKeys::Gamepad_LeftX), -1.f, 1.f);
	const FRotator View = Player->GetControlRotation();
	const bool bSwimming = GetCharacterMovement()->IsSwimming();
	const FRotator Heading = bSwimming ? View : FRotator(0.f, View.Yaw, 0.f);
	AddMovementInput(FRotationMatrix(Heading).GetUnitAxis(EAxis::X), Forward);
	AddMovementInput(FRotationMatrix(FRotator(0.f, View.Yaw, 0.f)).GetUnitAxis(EAxis::Y), Right);

	// Jump; held while swimming, swim up.
	const bool bJump = Down(EKeys::SpaceBar) > 0.f || Down(EKeys::Gamepad_FaceButton_Bottom) > 0.f;
	if (bSwimming)
	{
		if (bJump)
		{
			AddMovementInput(FVector::UpVector, 1.f);
		}
	}
	else if (bJump && !bJumpHeld)
	{
		Jump();
	}
	else if (!bJump && bJumpHeld)
	{
		StopJumping();
	}
	bJumpHeld = bJump;
}
