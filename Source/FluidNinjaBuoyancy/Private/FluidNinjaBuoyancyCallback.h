#pragma once

#include "CoreMinimal.h"
#include "Chaos/SimCallbackInput.h"
#include "Chaos/SimCallbackObject.h"
#include "FluidNinjaBuoyancyCore.h"

namespace Chaos { class FSingleParticlePhysicsProxy; }

/** One floating body, as the game thread hands it to the physics thread each frame. */
struct FFluidNinjaBuoyancyBodyInput
{
	Chaos::FSingleParticlePhysicsProxy* Proxy = nullptr;
	int32 Id = 0;
	TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe> Hull;
	TSharedPtr<const FluidNinjaBuoyancy::FWaterPatch, ESPMode::ThreadSafe> Patch;
	float Extrapolate = 0.f;
	bool bAmbientFlow = false;
	bool bFlat = false;
	float FlatHeight = 0.f;
	FluidNinjaBuoyancy::FParams Params;
	bool bWaterline = false;
	/** Stable steps (FluidNinjaBuoyancy::StabilizeStep). */
	bool bStableSteps = true;
};

struct FFluidNinjaBuoyancyInput : public Chaos::FSimCallbackInput
{
	TArray<FFluidNinjaBuoyancyBodyInput> Bodies;

	void Reset()
	{
		Bodies.Reset();
	}
};

struct FFluidNinjaBuoyancyBodyOutput
{
	int32 Id = 0;
	float Mass = 0.f;
	FVector CenterOfMass = FVector::ZeroVector;
	FluidNinjaBuoyancy::FStepResult Result;
};

struct FFluidNinjaBuoyancyOutput : public Chaos::FSimCallbackOutput
{
	TArray<FFluidNinjaBuoyancyBodyOutput> Bodies;
	/** What the step cost on the physics thread. */
	float StepSeconds = 0.f;

	void Reset()
	{
		Bodies.Reset();
		StepSeconds = 0.f;
	}
};

/** Runs every physics step (each frame, each substep, or each async step): the water's forces on each floating body, at its current
 * pose. */
class FFluidNinjaBuoyancyCallback : public Chaos::TSimCallbackObject<FFluidNinjaBuoyancyInput, FFluidNinjaBuoyancyOutput>
{
public:
	virtual FName GetFNameForStatId() const override;

private:
	virtual void OnPreSimulate_Internal() override;

	/** Per body, physics thread only. */
	TMap<int32, FluidNinjaBuoyancy::FStepState> States;
	TMap<int32, int64> LastStep;
	int64 Step = 0;
};
