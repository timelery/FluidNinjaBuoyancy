#include "FluidNinjaBuoyancyCallback.h"

#include "Chaos/Particle/ParticleUtilities.h"
#include "Misc/ScopeExit.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "Stats/Stats.h"

FName FFluidNinjaBuoyancyCallback::GetFNameForStatId() const
{
	const static FLazyName StaticName("FFluidNinjaBuoyancyCallback");
	return StaticName;
}

void FFluidNinjaBuoyancyCallback::OnPreSimulate_Internal()
{
	using namespace Chaos;
	++Step;
	const FFluidNinjaBuoyancyInput* Input = GetConsumerInput_Internal();
	if (!Input || Input->Bodies.IsEmpty())
	{
		return;
	}
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FluidNinjaBuoyancy_PhysicsStep);
	const double StartSeconds = FPlatformTime::Seconds();
	const float DeltaSeconds = static_cast<float>(GetDeltaTime_Internal());
	FFluidNinjaBuoyancyOutput& Output = GetProducerOutputData_Internal();
	ON_SCOPE_EXIT
	{
		Output.StepSeconds += static_cast<float>(FPlatformTime::Seconds() - StartSeconds);
	};
	for (const FFluidNinjaBuoyancyBodyInput& BodyInput : Input->Bodies)
	{
		if (!BodyInput.Proxy || !BodyInput.Hull.IsValid())
		{
			continue;
		}
		FRigidBodyHandle_Internal* Body = BodyInput.Proxy->GetPhysicsThreadAPI();
		if (!Body)
		{
			continue;
		}
		const EObjectStateType State = Body->ObjectState();
		if (State != EObjectStateType::Dynamic && State != EObjectStateType::Sleeping)
		{
			continue;
		}

		FluidNinjaBuoyancy::FBodyState BodyState;
		const FRigidTransform3 Pose = FParticleUtilitiesXR::GetActorWorldTransform(Body);
		BodyState.Transform = FTransform(Pose.GetRotation(), Pose.GetTranslation());
		BodyState.CenterOfMass = FParticleUtilitiesXR::GetCoMWorldPosition(Body);
		BodyState.Velocity = Body->V();
		BodyState.AngularVelocity = Body->W();
		BodyState.Mass = static_cast<float>(Body->M());

		FluidNinjaBuoyancy::FWaterView Water;
		Water.Patch = BodyInput.Patch.Get();
		Water.Extrapolate = BodyInput.Extrapolate;
		Water.bAmbientFlow = BodyInput.bAmbientFlow;
		Water.bFlat = BodyInput.bFlat;
		Water.FlatHeight = BodyInput.FlatHeight;

		FFluidNinjaBuoyancyBodyOutput& BodyOutput = Output.Bodies.AddDefaulted_GetRef();
		BodyOutput.Id = BodyInput.Id;
		BodyOutput.Mass = BodyState.Mass;
		BodyOutput.CenterOfMass = BodyState.CenterOfMass;
		FluidNinjaBuoyancy::FStepState& StepState = States.FindOrAdd(BodyInput.Id);
		LastStep.FindOrAdd(BodyInput.Id) = Step;
		FluidNinjaBuoyancy::Integrate(*BodyInput.Hull, BodyState, Water, BodyInput.Params, DeltaSeconds, StepState, BodyOutput.Result, BodyInput.bWaterline);

		const FluidNinjaBuoyancy::FStepResult& Result = BodyOutput.Result;
		if (!Result.bTouchesWater && Result.ThrustApplied == 0.f && Result.Force.IsNearlyZero())
		{
			continue;   // in the air: leave it (and its sleep) alone
		}
		FVector Force = Result.Force;
		FVector Torque = Result.Torque;
		const bool bNeedsInertia = !Result.AngularDampingRate.IsZero() || BodyInput.bStableSteps;
		const FMatrix33 Inertia = bNeedsInertia ? FParticleUtilitiesXR::GetWorldInertia(Body) : FMatrix33(0.0, 0.0, 0.0);
		if (!Result.AngularDampingRate.IsZero())
		{
			Torque += Inertia * Result.AngularDampingRate;
		}
		// Stable steps: the part of the water's stiffness and drag this step is too long for, taken implicitly (light bodies, long steps).
		if (BodyInput.bStableSteps)
		{
			const FVector InertiaDiagonal(Inertia.M[0][0], Inertia.M[1][1], Inertia.M[2][2]);
			const float GravityZ = Body->GravityEnabled() ? -BodyInput.Params.Gravity : 0.f;
			FluidNinjaBuoyancy::StabilizeStep(Result, BodyState, InertiaDiagonal, GravityZ, DeltaSeconds, Force, Torque);
		}
		if (!Force.ContainsNaN() && !Torque.ContainsNaN())
		{
			Body->AddForce(Force);
			Body->AddTorque(Torque);
		}
	}

	// Forget bodies that have not come for a while (destroyed, unregistered).
	if ((Step & 255) == 0)
	{
		for (auto It = LastStep.CreateIterator(); It; ++It)
		{
			if (Step - It.Value() > 600)
			{
				States.Remove(It.Key());
				It.RemoveCurrent();
			}
		}
	}
}
