#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "FluidNinjaBuoyancySubsystem.generated.h"

class ASceneCapture2D;
class FFluidNinjaBuoyancyCallback;
class FPhysScene_Chaos;
class UMaterialInterface;
class UMaterialParameterCollection;
class UNiagaraComponent;
class UNiagaraDataChannelAsset;
class UPrimitiveComponent;
class UTextureRenderTarget2D;
class UFluidNinjaBuoyancyComponent;
struct FFluidNinjaBuoyancyReadback;
struct FFluidNinjaBuoyancyGatherReadback;
struct FFluidNinjaBuoyancyFlowProbe;
struct FFluidNinjaBuoyancyBodyOutput;
class FFluidNinjaBuoyancyTestBench;
namespace FluidNinjaBuoyancy { struct FWaterPatch; }

/** The wind blowing over the hulls this frame (FluidNinja Buoyancy status, test bench). */
struct FFluidNinjaBuoyancyWind
{
	FVector Velocity = FVector::ZeroVector;   // cm/s, world
	float SpeedMs = 0.f;
	float DirectionDegrees = 0.f;              // the way it blows toward
	bool bOverride = false;
};

/** What FluidNinja Buoyancy knows of one body (test bench readout). */
struct FFluidNinjaBuoyancyBodyDebug
{
	int32 Id = 0;
	int32 Level = 0;
	bool bDormant = false;
	/** Out of the capture with no water volume there: held still rather than dropped. */
	bool bHeld = false;
	bool bInCapture = false;
	bool bFlat = false;
	bool bHasWaterZ = false;
	float WaterZ = 0.f;
	float PatchAge = -1.f;
	int32 PatchPoints = 0;
	float PatchSpacing = 0.f;
	/** The drawn water minus the water volume's top here, learned while in the capture (used beyond it). */
	float FarFieldCorrection = 0.f;
	bool bFarFieldLearned = false;
	/** Verify mode: the gather against per-body copies of the same frame (cm and cm/s over the copied points). */
	int32 VerifyCount = 0;
	float VerifyMeanCm = 0.f;
	float VerifyMaxCm = 0.f;
	float VerifyFlowMeanCmS = 0.f;
	/** Points more than 2 cm apart, and points where only the gather has a current (outside the copy's velocity window). */
	int32 VerifyFar = 0;
	int32 VerifyFlowOnlyGathered = 0;
};

/**
 * FluidNinja Buoyancy: the water that floating bodies (UFluidNinjaBuoyancyComponent) feel, and the physics callback that applies it
 * every physics step.
 *
 * The Surface Mirror: an orthographic depth capture of only the water surface's own primitives (FluidNinja's river spline pieces,
 * surface planes and Niagara water sheets, never a waterfall's face) over the square of the simulation that follows the player (or
 * around the bodies, where no simulation follows the player), so bodies float on the water exactly as drawn, with its pressure
 * waves and tile displacement. Its near plane sits above the highest body, so water passing over higher up is left out. Every
 * frame one compute pass (the gather, FluidNinjaBuoyancyGather.usf) resamples the capture and the simulation's velocity buffer
 * (the current) onto a grid around each body, and one asynchronous readback brings all of them back, each into its body's slot.
 * Without the gather, each body's texels are copied out and read back on their own. Heights are carried forward by their rate of
 * change over the few frames of readback. Outside the capture the top of a water volume (a Physics Volume with Water Volume on)
 * gives a flat height.
 *
 * Flow sources: every FluidNinja simulation the drawn surfaces show (their material's velocity buffer). Where one lies comes from the
 * surface material's TraceMeshPos / TraceMeshSize when the simulation sets them there (one simulation per surface, such as a pool),
 * else from FluidNinja's parameter collection (one simulation following the player, such as the river template). Each body takes
 * the simulation whose surface lies under it.
 *
 * The forces are computed on the physics thread (Chaos sim callback, every step) against the body's current pose; see
 * FluidNinjaBuoyancyCore.h.
 */
UCLASS()
class FLUIDNINJABUOYANCY_API UFluidNinjaBuoyancySubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UFluidNinjaBuoyancySubsystem();
	virtual ~UFluidNinjaBuoyancySubsystem() override;

	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	void Register(UFluidNinjaBuoyancyComponent* Component);
	void Unregister(UFluidNinjaBuoyancyComponent* Component);

	/** One line about the water field and one per body. */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Buoyancy")
	FString GetStatus() const { return Status; }

	/**
	 * fnb.Buoyancy.CalibrateFlow [apply]: measures how fast FluidNinja's dye really moves against what its velocity buffer
	 * says (block matching over a few frames around the first body, or the camera), and the Flow Scale Multiplier that would make
	 * the current the bodies feel match it. "apply" sets it (and saves it to DefaultGame.ini in the editor) when the match is good.
	 */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Buoyancy")
	void CalibrateFlow(bool bApply);

	/** As CalibrateFlow, measured around Where (the test bench: at the selected body). */
	void CalibrateFlowAt(const FVector& Where, bool bApply);

	/** The last calibration's report (empty before one). */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Buoyancy")
	FString GetCalibrationReport() const { return CalibrationReport; }

	/* Testing (the FluidNinja Buoyancy test bench, fnb.Test.*). */
	const FFluidNinjaBuoyancyWind& GetWind() const { return Wind; }
	/** Blows SpeedMs toward DirectionDegrees instead of the project's Wind, until switched off. */
	void SetWindOverride(bool bOn, float SpeedMs = 0.f, float DirectionDegrees = 0.f);
	/** Controlled water: every body floats on flat, still water at Height (no waves, no current, no wakes), for measurements
	 * that the live water's current would swamp. The drawn water is unchanged. */
	void SetControlledWater(bool bOn, float Height = 0.f);
	bool IsWaterControlled() const { return bControlledWater; }
	float GetControlledWaterHeight() const { return ControlledHeight; }
	TArray<UFluidNinjaBuoyancyComponent*> GetBodies() const;
	bool GetBodyDebug(const UFluidNinjaBuoyancyComponent* Component, FFluidNinjaBuoyancyBodyDebug& Out) const;
	/** A fresh slot (new id) for a body that was put back: the physics thread's state, slam peak and water start over. */
	void Reregister(UFluidNinjaBuoyancyComponent* Component);
	float GetFlowScale() const { return FlowScale; }
	bool IsGathering() const;
	/** Verify mode (with the gather on): every body's water is also copied out on its own, and each copy is compared with the
	 * gather of the same frame (FFluidNinjaBuoyancyBodyDebug::Verify*). Switching it on clears the statistics. */
	void SetVerifyGather(bool bOn);
	/** Smoothed costs (ms): game-thread tick, its patch building, physics pre-tick, physics thread a frame. */
	void GetCosts(float& OutTick, float& OutPatches, float& OutPreTick, float& OutPhysics) const { OutTick = TickMs; OutPatches = PatchMs; OutPreTick = PreTickMs; OutPhysics = PhysicsMsPerFrame; }
	int32 GetWakePoints() const { return LastWakePoints; }
	int32 GetCapturedPrimitives() const { return LastCapturedPrimitives; }
	int32 GetSurfacePrimitives() const { return SurfacePrimitives.Num(); }
	int32 GetGatherBodies() const { return LastGatherBodies; }
	int32 GetGatherPoints() const { return LastGatherSamples; }
	/** The test bench of this world (non-shipping game worlds), or null. */
	FFluidNinjaBuoyancyTestBench* GetTestBench() const { return Bench.Get(); }

	/** FluidNinja's own tracking: whether FluidNinja tracks this body (it generates overlap events or carries a NinjaLive's tracking
	 * tag), and whether this plugin handed it over (its Interaction asks for it and FluidNinja tracks relative to the water). */
	void GetTracking(const UFluidNinjaBuoyancyComponent* Component, bool& bOutTracked, bool& bOutHandedOver) const;
	/** Why bodies are or are not handed to FluidNinja's tracking (one line per NinjaLive). */
	const FString& GetTrackingReport() const { return TrackingReport; }
	/** Looks at the NinjaLive actors again and hands bodies over or takes them back (also done every second). */
	void RefreshTracking();

private:
	struct FSlot
	{
		TWeakObjectPtr<UFluidNinjaBuoyancyComponent> Component;
		int32 Id = 0;
		TArray<TSharedPtr<FFluidNinjaBuoyancyReadback, ESPMode::ThreadSafe>> Ring;
		TSharedPtr<const FluidNinjaBuoyancy::FWaterPatch, ESPMode::ThreadSafe> Patch;
		bool bInCapture = false;
		bool bFlat = false;
		float FlatHeight = 0.f;
		double NextFlatQuery = 0.0;
		float LastWaterZ = 0.f;
		bool bHasWaterZ = false;
		float SlamWatts = 0.f;
		FVector SlamLocation = FVector::ZeroVector;
		TSharedPtr<FFluidNinjaBuoyancyBodyOutput> LastOutput;
		// The copied squares keep one size per body (a readback reuses its staging texture, which must not change size).
		int32 HeightSide = 0;
		int32 FlowSide = 0;
		int32 Level = 0;
		/** 0 near, 1 beyond the first level-of-detail distance, 2 beyond the second (whatever levels the hull has). */
		int32 DistanceLevel = 0;
		bool bDormant = false;
		bool bHeld = false;
		// Far field: the top of the water volume here, and how far the drawn water is from it (learned in the capture).
		float VolumeHeight = 0.f;
		bool bVolume = false;
		float FarFieldCorrection = 0.f;
		bool bFarFieldLearned = false;
		// Verify mode statistics.
		int32 VerifyCount = 0;
		double VerifySum = 0.0;
		float VerifyMax = 0.f;
		double VerifyFlowSum = 0.0;
		int32 VerifyFar = 0;
		int32 VerifyFlowOnlyGathered = 0;
		float SettledSeconds = 0.f;
		double DormantSince = 0.0;
		// FluidNinja's own tracking: FluidNinja tracks the body; this plugin handed it over (and what to give back).
		bool bNinjaTracked = false;
		bool bHandedOver = false;
		bool bOverlapBefore = false;
		TArray<FName> AddedTags;
		bool bTrackingWarned = false;
		bool bTrackingNoted = false;
	};

	/** A body the capture serves this frame, the square of water it needs, and the simulation whose current it reads. */
	struct FFootprint
	{
		FSlot* Slot = nullptr;
		FBox2D Area{ForceInit};
		double Margin = 0.0;
		int32 Source = INDEX_NONE;
	};

	/** One FluidNinja simulation the drawn water shows: its velocity buffer, the component running it, a surface material showing it,
	 * where its surfaces lie, and (each frame) its square and units. */
	struct FFlowSource
	{
		TWeakObjectPtr<UTextureRenderTarget2D> Buffer;
		TWeakObjectPtr<UObject> Ninja;
		/** FluidNinja's simulation system (NinjaLiveCore) on the NinjaLive's actor. */
		TWeakObjectPtr<UNiagaraComponent> Core;
		TWeakObjectPtr<UMaterialInterface> Material;
		FBox2D SurfaceArea{ForceInit};
		float SimSpeed = 1.f;
		float SamplingFPS = 60.f;
		int32 Resolution = 0;
		FVector2D Centre = FVector2D::ZeroVector;
		double SizeCm = 0.0;
		bool bValid = false;
		bool bFromMaterial = false;
		/** The simulation's steps per second: NinjaLiveCore's fixed tick at any frame rate, else SamplingFPS at most the frame rate. */
		float StepsPerSecond = 0.f;
		bool bFixedTick = false;
		float FlowScale = 0.f;   // cm/s per velocity-buffer unit
	};

	void PhysicsPreTick(FPhysScene_Chaos* Scene, float DeltaSeconds);
	void RefreshSources();
	void CollectReadbacks();
	void CaptureAndCopy();
	int32 CopyFootprints(const TArray<FFootprint>& Wanted, double Now);
	int32 GatherFootprints(const TArray<FFootprint>& Wanted, double Now);
	void BuildPatch(FSlot& Slot, FFluidNinjaBuoyancyReadback& Data);
	/** Verify mode: a body's copied water against its gathered water of the same frame. */
	void CompareWithGather(FSlot& Slot, const FluidNinjaBuoyancy::FWaterPatch& Copied);
	void BuildGatherPatches(const FFluidNinjaBuoyancyGatherReadback& Data);
	/** Rate of rise, the current around the body, the water under its centre; then the patch is the body's. */
	void FinishPatch(FSlot& Slot, const TSharedRef<FluidNinjaBuoyancy::FWaterPatch, ESPMode::ThreadSafe>& Patch);
	void UpdateFlowProbe();
	void UpdateWind(float DeltaSeconds);
	/** Each source's square (its surface material, else the parameter collection) and flow scale, this frame. */
	void UpdateFlowSources();
	/** The source whose surface lies under Point (the smallest where they overlap), else whose square holds it; INDEX_NONE if none. */
	int32 FindFlowSource(const FVector2D& Point) const;
	const FFlowSource* FindFlowSourceFor(const UTextureRenderTarget2D* Buffer) const;
	void DrawDebug();
	FSlot* FindSlot(int32 Id);
	void UpdateLevelAndSleep(FSlot& Slot, const FVector& Camera, float DeltaSeconds);

	TArray<FSlot> Slots;
	int32 NextId = 1;

	TArray<TSharedPtr<FFluidNinjaBuoyancyGatherReadback, ESPMode::ThreadSafe>> GatherRing;
	int32 GatherCursor = 0;
	int32 LastGatherBodies = 0;
	int32 LastGatherSamples = 0;

	TSharedPtr<FFluidNinjaBuoyancyFlowProbe, ESPMode::ThreadSafe> FlowProbe;
	FString CalibrationReport;

	FFluidNinjaBuoyancyWind Wind;
	bool bWindOverride = false;
	float OverrideWindMs = 0.f;
	float OverrideWindDegrees = 0.f;
	bool bControlledWater = false;
	bool bVerifyGather = false;
	float ControlledHeight = 0.f;
	int32 LastCapturedPrimitives = 0;

	TSharedPtr<FFluidNinjaBuoyancyTestBench> Bench;

	// What FluidNinja Buoyancy costs (milliseconds, smoothed): the game thread's tick and the part of it building patches, the physics
	// pre-tick, and the physics thread's steps per frame and per step.
	float TickMs = 0.f;
	float PatchMs = 0.f;
	float PreTickMs = 0.f;
	float PhysicsMsPerFrame = 0.f;
	float PhysicsMsPerStep = 0.f;

	FFluidNinjaBuoyancyCallback* Callback = nullptr;
	FDelegateHandle PreTickHandle;

	UPROPERTY(Transient)
	TObjectPtr<ASceneCapture2D> Capture;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> CaptureTarget;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialParameterCollection> NinjaCollection;

	/** FluidNinja's global point input, where the wakes go. */
	UPROPERTY(Transient)
	TObjectPtr<UNiagaraDataChannelAsset> WakeChannel;
	int32 LastWakePoints = 0;

	TArray<TWeakObjectPtr<UPrimitiveComponent>> SurfacePrimitives;
	int32 SurfaceLeftOut = 0;
	TArray<FFlowSource> FlowSources;
	double NextSourceRefresh = 0.0;

	// The capture as last rendered (its texels map to the world through these).
	FVector2D CaptureOrigin = FVector2D::ZeroVector;
	double CaptureTop = 0.0;
	double CaptureRange = 0.0;
	double CaptureTexel = 1.0;
	int32 CaptureResolution = 0;

	// The simulation FluidNinja's material parameter collection places (one following the player, such as the river template).
	FVector2D SimCentre = FVector2D::ZeroVector;
	double SimSizeCm = 0.0;
	bool bSimValid = false;
	float FlowScale = 0.f;           // the first source's, cm/s per velocity-buffer unit (status)
	float SmoothedDeltaTime = 1.f / 60.f;

	// The test console variables (fnb.Boat.*) reach propulsion components only when they change.
	float AppliedConsoleThrottle = 0.f;
	float AppliedConsoleRudder = 0.f;

	FString Status;
	uint32 PatchesBuilt = 0;
	FString TrackingReport;
	double NextTrackingRefresh = 0.0;
};
