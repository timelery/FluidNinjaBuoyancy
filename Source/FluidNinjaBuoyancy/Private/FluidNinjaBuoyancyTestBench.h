#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"

class IInputProcessor;
class SWidget;
class UGameViewportClient;
class UPrimitiveComponent;
class UFluidNinjaBuoyancyComponent;
class UFluidNinjaBuoyancySubsystem;

/**
 * FluidNinja Buoyancy test bench: a panel at the top right of the game viewport (PIE by default; never in shipping builds), labels over every
 * floating body, and one console command, so a person in Play and a script can drive and read the same tests.
 *
 * The panel shows the water (live FluidNinja water, or controlled flat still water), the current's scale and calibration, the wind
 * (the project's or a test wind), FluidNinja Buoyancy's costs, a line per body (model, state, draft against its rest draft, tilt, speed, the water's
 * speed, the wind's force) and the selected body in detail, the running test and the latest results.
 *
 * Tests (pass/fail where the answer is clear; a live current swamps such measurements, so these run on controlled water):
 * (in free water: the bodies ignore the scenery for the run, so a bank or rock cannot hold them)
 *   settle    every body put back and settled: draft against its rest draft, and still
 *   capsize   stable bodies heeled 30 degrees: they right themselves
 *   drop      bodies dropped from 3 m: no blow-up, they come back to rest at their draft (slam power reported)
 *   wind      a test wind of 8 then 16 m/s along the view: bodies drift with it, faster in the stronger wind
 *   throttle  bodies with propulsion: speed at full throttle, heading kept, then turning with full rudder
 * and on the live water:
 *   gather    the water under each body read by the GPU gather and by per-body copies agrees (costs of both reported)
 *   walkaway  the player goes 70 m away and back: no body sinks while out of the capture, all float again after
 *   all       every test in turn
 * Results go to the panel, the log (LogFluidNinjaBuoyancy "FluidNinja Buoyancy test") and Saved/FluidNinjaBuoyancy/TestResults/<time>_<test>.json (latest.json too).
 *
 * The panel is off by default: fnb.TestHUD 1 shows it in PIE, 2 in any non-shipping game world, once the world has floating bodies.
 * The console command works either way: fnb.Test <verb> (fnb.Test help lists them). Hotkeys while the panel is up and the game
 * viewport has focus, Ctrl+Shift with: B hide, R reset bodies, PageUp/PageDown select, T run all tests, W cycle the test wind,
 * C controlled water, D debug view, K capsize the selected body, J drop it.
 */
class FFluidNinjaBuoyancyTestBench : public TSharedFromThis<FFluidNinjaBuoyancyTestBench>
{
public:
	static TSharedPtr<FFluidNinjaBuoyancyTestBench> Create(UFluidNinjaBuoyancySubsystem& Subsystem);
	~FFluidNinjaBuoyancyTestBench();
	void Remove();
	void Tick(float DeltaSeconds);

	/** One console verb with its arguments (fnb.Test); returns what it did. */
	FString Command(const TArray<FString>& Args);

	void Select(int32 Step);
	bool SelectByName(const FString& Part);
	UFluidNinjaBuoyancyComponent* GetSelected() const;
	void ResetBodies(bool bAll);
	/** Controlled flat still water at the water height under the bodies (or Height when given), or the live water. */
	void SetControlledWater(bool bOn, const float* Height = nullptr);
	/** A test wind (SpeedMs toward Degrees, or along the view when Degrees is null), or back to the project's Wind. */
	void SetTestWind(bool bOn, float SpeedMs = 0.f, const float* Degrees = nullptr);
	void CycleWind();
	void Push(float NewtonSeconds);
	void Capsize(float Degrees);
	void Drop(float Metres);
	void BringHere();
	void SetBoat(float Throttle, float Rudder);
	void ToggleDebugView();
	/** Free water: the bodies ignore the scenery (banks, rocks, the bottom), so a measurement is not cut short by them; they still
	 * meet each other. The controlled-water tests turn it on for their run. */
	void SetSceneryCollision(bool bCollide);
	bool IsSceneryCollision() const { return SceneryOff.IsEmpty(); }
	void Run(const FString& Name);
	void Stop();
	bool IsRunning() const { return Current.IsValid(); }
	void ToggleVisible() { bVisible = !bVisible; }
	bool IsGameFocused() const;

private:
	struct FTrack;
	struct FPhase;
	struct FRun;
	struct FResult
	{
		FString Test;
		FString Body;
		int32 Verdict = 0;   // 1 pass, -1 fail, 0 information
		FString Text;
	};

	void AttachPanel();
	TSharedRef<SWidget> Build();
	FText Readout() const;
	void DrawLabels() const;
	UWorld* GetWorld() const;
	UFluidNinjaBuoyancySubsystem* GetHydro() const { return Subsystem.Get(); }
	FVector ViewLocation() const;
	float ViewYaw() const;
	/** Water height under the bodies now (mean of those that know theirs). */
	bool BodiesWaterHeight(float& OutHeight) const;
	FString BodyName(const UFluidNinjaBuoyancyComponent* Body) const;
	FString BodyLine(const UFluidNinjaBuoyancyComponent* Body, bool bSelected) const;
	/** The body's tilt from its rest attitude (degrees) and its draft against the water it knows (cm; false when none). */
	float Tilt(const UFluidNinjaBuoyancyComponent* Body) const;
	bool Draft(const UFluidNinjaBuoyancyComponent* Body, float& OutDraft) const;
	void Report(const FString& Test, const FString& Body, int32 Verdict, const FString& Text);
	void SaveResults(const FString& Test, int32 FirstResult) const;

	// Tests.
	void StartRun(TUniquePtr<FRun> Run);
	void FinishRun(bool bStopped);
	TUniquePtr<FRun> MakeRun(const FString& Name);
	void TickRun();

	TWeakObjectPtr<UFluidNinjaBuoyancySubsystem> Subsystem;
	TWeakObjectPtr<UGameViewportClient> Viewport;
	TSharedPtr<SWidget> Widget;
	TSharedPtr<IInputProcessor> Input;
	bool bVisible = true;
	bool bLabels = true;
	TWeakObjectPtr<UFluidNinjaBuoyancyComponent> Selected;
	int32 WindPreset = 0;

	struct FSavedResponse
	{
		ECollisionResponse WorldStatic = ECR_Block;
		ECollisionResponse WorldDynamic = ECR_Block;
	};
	TMap<TWeakObjectPtr<UPrimitiveComponent>, FSavedResponse> SceneryOff;

	TUniquePtr<FRun> Current;
	TArray<FString> Queue;
	FString QueueName;
	int32 QueueFirstResult = 0;
	TArray<FResult> Results;
	mutable FText CachedText;
	mutable double CachedAt = -1.0;
};
