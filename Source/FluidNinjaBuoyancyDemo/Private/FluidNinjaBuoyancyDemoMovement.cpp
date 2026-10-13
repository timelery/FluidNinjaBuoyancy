#include "FluidNinjaBuoyancyDemoMovement.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "PhysicsEngine/BodyInstance.h"

namespace
{
	TAutoConsoleVariable<int32> CVarEnginePush(TEXT("fnb.Demo.EnginePush"), 0,
		TEXT("1: the demo character pushes floating bodies with the engine's default push instead of its own (A/B)."));
}

void UFluidNinjaBuoyancyDemoMovement::ApplyImpactPhysicsForces(const FHitResult& Impact, const FVector& ImpactAcceleration, const FVector& ImpactVelocity)
{
	if (CVarEnginePush.GetValueOnGameThread() != 0)
	{
		Super::ApplyImpactPhysicsForces(Impact, ImpactAcceleration, ImpactVelocity);
		return;
	}
	if (!bEnablePhysicsInteraction || !Impact.bBlockingHit)
	{
		return;
	}
	UPrimitiveComponent* Other = Impact.GetComponent();
	FBodyInstance* Body = Other ? Other->GetBodyInstance(Impact.BoneName) : nullptr;
	const UWorld* World = GetWorld();
	if (!Body || !Body->IsInstanceSimulatingPhysics() || !World)
	{
		return;
	}
	if (GivenFrame != GFrameCounter)
	{
		GivenFrame = GFrameCounter;
		GivenThisFrame.Reset();
	}

	// Into the body; on foot along the ground only (no pressing things under or lifting them), swimming in any direction.
	FVector Direction = -Impact.ImpactNormal;
	if (!IsSwimming())
	{
		Direction.Z = 0.0;
	}
	if (!Direction.Normalize())
	{
		return;
	}
	// How fast the character goes into it: its velocity, or where its input takes it at full speed.
	double Into = FVector::DotProduct(ImpactVelocity, Direction);
	if (!ImpactAcceleration.IsNearlyZero())
	{
		Into = FMath::Max(Into, FVector::DotProduct(ImpactAcceleration.GetSafeNormal(), Direction) * GetMaxSpeed() * FMath::Clamp(GetAnalogInputModifier(), 0.f, 1.f));
	}
	const double Lack = Into - FVector::DotProduct(Body->GetUnrealWorldVelocity(), Direction);
	if (Lack <= 0.0)
	{
		return;
	}
	// The impulse that brings it up to that speed, within what Max Push Force gives in a frame (less what it has had this frame).
	float& Given = GivenThisFrame.FindOrAdd(Other);
	const double Budget = static_cast<double>(MaxPushForce) * 100.0 * FMath::Max(World->GetDeltaSeconds(), 1e-3f) - Given;
	const double Impulse = FMath::Min(static_cast<double>(Body->GetBodyMass()) * Lack, Budget);
	if (Impulse <= 0.0)
	{
		return;
	}
	Given += static_cast<float>(Impulse);
	Other->AddImpulse(Direction * Impulse, Impact.BoneName, false);   // at the centre of mass: a push slides a float, it does not spin it
}
