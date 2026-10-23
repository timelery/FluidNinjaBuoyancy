#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
#include "FluidNinjaBuoyancyBench.h"
#include "Subsystems/WorldSubsystem.h"
#include "FluidNinjaBuoyancySubsystem.generated.h"

class ASceneCapture2D;
class FFluidNinjaBuoyancyCallback;
class FFluidNinjaBuoyancyReadbackPoller;
class FPhysScene_Chaos;
class UMaterialInterface;
class UMaterialParameterCollection;
class UNiagaraComponent;
class UNiagaraDataChannelAsset;
class UPrimitiveComponent;
class USkeletalMeshComponent;
class UTextureRenderTarget2D;
class UFluidNinjaBuoyancyComponent;
class UFluidNinjaBuoyancyWaves;
struct FFluidNinjaBuoyancyReadback;
struct FFluidNinjaBuoyancyGatherReadback;
struct FFluidNinjaBuoyancyFlowProbe;
struct FFluidNinjaBuoyancyCurrentReadback;
struct FFluidNinjaBuoyancyBodyOutput;
class FFluidNinjaBuoyancySwimmers;
class UFluidNinjaBuoyancySubsystem;
struct FFluidNinjaWaterSample;
namespace FluidNinjaBuoyancy { struct FWaterPatch; }

/** Makes the test bench of one world (FluidNinjaBuoyancyBench.h): bound by the FluidNinjaBuoyancyDeveloper module, where the bench
 * lives; unbound (so no world has a bench) in builds without that module, such as shipping. */
DECLARE_DELEGATE_RetVal_OneParam(TSharedPtr<IFluidNinjaBuoyancyBench>, FFluidNinjaBuoyancyBenchFactory, UFluidNinjaBuoyancySubsystem&);

/** Test bench: the ways floating bodies stir FluidNinja (Docs/TagApproaches.md), set for the whole level by SetStirApproach (the bench's
 * stir verb). Not an editor or Blueprint setting. */
UENUM()
enum class EFluidNinjaBuoyancyStirApproach : uint8
{
	/** FluidNinja Buoyancy's wakes on every body; each NinjaLive with its own settings. */
	Wakes,
	/** FluidNinja's tracking, opt-in by tag: every NinjaLive tracks relative to the water with no density, tracks the tag FluidNinjaTracked and
	 * leaves PhysicsBody out of its object-type filter. Needs the modified FluidNinja core's relative tracking. */
	TrackingByTag,
	/** FluidNinja's tracking by object type: as above, but with no tag and PhysicsBody in every NinjaLive's filter. */
	TrackingByType,
	/** Wakes and FluidNinja's tracking (by tag) on every body. */
	WakesAndTracking,
};


/** The wind blowing over the hulls this frame (FluidNinja Buoyancy status, test bench). */
struct FFluidNinjaBuoyancyWind
{
	FVector Velocity = FVector::ZeroVector;   // cm/s, world
	float SpeedMs = 0.f;
	float DirectionDegrees = 0.f;              // the way it blows toward
	bool bOverride = false;
};

/** What FluidNinja Buoyancy costs (milliseconds): smoothed over the last frames for a readout, and this frame's own for a profile
 * (the CSV profiler gets the frame's). */
struct FFluidNinjaBuoyancyCosts
{
	// Smoothed: the game thread's tick and the part of it building patches, the physics pre-tick, the physics thread's steps a frame and
	// one step.
	float TickMs = 0.f;
	float PatchMs = 0.f;
	float PreTickMs = 0.f;
	float PhysicsMs = 0.f;
	float PhysicsMsPerStep = 0.f;
	// This frame's, as measured.
	float FrameTickMs = 0.f;
	float FramePatchMs = 0.f;
	float FramePreTickMs = 0.f;
	float FramePhysicsMs = 0.f;
	/** How old the water in use is: frames since the newest readback the bodies' patches came from was queued (0 before any), counted at
	 * the frame the patches were built in. With fnb.Gather.FastReadback that is the frame whose physics first uses them (about 2); with the
	 * slow path the physics uses them one frame later still (about 4 counted, 5 in use). */
	int32 ReadbackAgeFrames = 0;
};

class UFluidNinjaBuoyancySubsystem;

/** The subsystem's pre-physics tick (TG_PrePhysics, with fnb.Gather.FastReadback): the water the render thread read back by the end of its
 * last frame becomes the bodies' patches before this frame's physics takes them (UFluidNinjaBuoyancySubsystem::TickPrePhysics). Not a
 * USTRUCT: it is registered and unregistered by the subsystem alone, never serialized or copied. */
struct FFluidNinjaBuoyancyPrePhysicsTickFunction : public FTickFunction
{
	UFluidNinjaBuoyancySubsystem* Target = nullptr;

	virtual void ExecuteTick(float DeltaTime, ELevelTick TickType, ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent) override;
	virtual FString DiagnosticMessage() override;
	virtual FName DiagnosticContext(bool bDetailed) override;
};

/** A swimmer FluidNinja Buoyancy takes besides the player's pawn: a skeletal mesh in the water, moved and animated by its owner (an AI
 * swimmer or diver, say). Its body parts splash and press rings into FluidNinja Waves (Project Settings → FluidNinja Buoyancy → Swimmers). */
USTRUCT(BlueprintType)
struct FLUIDNINJABUOYANCY_API FFluidNinjaSwimmerSource
{
	GENERATED_BODY()

	/** The still water's height at the swimmer, world Z in cm: its parts' entries, its displacement and its wake are judged against it.
	 * Set it to the water surface the swimmer is in (register again to change it). Default 0. Ignored for a swimmer whose water comes from
	 * the water query (a FluidNinja Swimmer component with Water From Query on): that one uses the live still water at its hips.
	  * Any world height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Swimmer", meta = (Units = "cm"))
	float WaterZ = 0.f;

	/** Its head, chest and hips push the water aside (FluidNinja Waves' static head, as the player's capsule does): the body's bob makes
	 * waves and the floaters near it feel them. Off: it makes no displacement waves (its Displaced Litres reads 0). Needs the project's
	 * Waves and Swimmer Waves settings and the modified FluidNinja core. Default on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Swimmer")
	bool bDisplacement = true;

	/** A humanoid pawn uses the player's smooth, lying body and project Swimmer Volume Litres instead of three animated bone spheres.
	 * Requires a capsule root; ignored for mesh-only swimmers. Keep off for animal profiles. Default off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Swimmer")
	bool bUsePawnBody = false;

	/** Its hands, forearms, feet and head splash where they go into the water and press rings into it; its feet churn it. Off: no part
	 * splashes, rings or entry stats. Needs the project's Swimmer Contacts setting (the rings also need Waves). Default on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Swimmer")
	bool bPartContacts = true;

	/** Its chest, hips and thighs make contacts too (a diver going in headfirst or a jump): more splashes and a bigger impulse on entry.
	 * Off (the default): limbs and head only, which suits a swimmer at the surface. Only with Part Contacts on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Swimmer")
	bool bWholeBody = false;
};

/** What a swimmer's body parts did to the water (UFluidNinjaBuoyancySubsystem::GetSwimmerStats). */
USTRUCT(BlueprintType)
struct FLUIDNINJABUOYANCY_API FFluidNinjaSwimmerStats
{
	GENERATED_BODY()

	/** Body parts that went into the water since the swimmer was registered (the player: since play began, or since its pawn's mesh
	 * changed). A part counts when it goes under moving down at least Swimmer Entry Min Cm S (or its profile's). 0 or more. */
	UPROPERTY(BlueprintReadOnly, Category = "Swimmer")
	int32 Entries = 0;

	/** Entries a minute, counted over the last 20 seconds (entries in that window times 3). 0 or more. */
	UPROPERTY(BlueprintReadOnly, Category = "Swimmer")
	float EntriesPerMinute = 0.f;

	/** The last part that went in (its bone); None before any entry. */
	UPROPERTY(BlueprintReadOnly, Category = "Swimmer")
	FName LastPart;

	/** How fast the last part went in, straight down, in cm/s (0 before any entry; at least the entry threshold after one). */
	UPROPERTY(BlueprintReadOnly, Category = "Swimmer", meta = (Units = "cm/s"))
	float LastEntrySpeedCmS = 0.f;

	/** The impulse the last part pressed into the water, newton-seconds: its added mass times its entry speed, times Swimmer Impulse
	 * Scale and its profile's, halved when it went into the pocket an earlier part had just opened. 0 before any entry.
	  * Read-only: 0 or more. */
	UPROPERTY(BlueprintReadOnly, Category = "Swimmer")
	float LastImpulseNs = 0.f;

	/** The last part's splash strength on the project's splash scale (1: the splash system at its own size; it grows with the cube root of
	 * the slam's power over Splash Reference Watts), times Swimmer Splash Scale and its profile's, a third inside an earlier part's pocket.
	 * 0: too small for a splash (under 0.2), or no entry yet.
	  * Read-only: 0 or more; its ceiling is Splash Strength Ceiling times the swimmer and profile splash scales. */
	UPROPERTY(BlueprintReadOnly, Category = "Swimmer")
	float LastSplash = 0.f;

	/** When the last part went in (world time, seconds); -1 before any.
	 * Read-only: -1, or 0 and more. */
	UPROPERTY(BlueprintReadOnly, Category = "Swimmer")
	double LastEntryTime = -1.0;

	/** The body's water volume below the still surface, litres: its bone spheres or the player's tapered body with its wading fade.
	 * 0 without displacement, or while the swimmer is hidden or out of the water. Before the rendering's deep-water fade.
	  * Read-only: 0 or more. */
	UPROPERTY(BlueprintReadOnly, Category = "Swimmer")
	float DisplacedLitres = 0.f;

	/** The current pose uses the player's capsule-referenced, tapered displacement body rather than bone spheres. */
	UPROPERTY(BlueprintReadOnly, Category = "Swimmer")
	bool bUsesPawnBody = false;

	/** Body parts enabled for contact impulses in this pose (Manny/Quinn: 7 for the player profile, 11 for Whole Body). */
	UPROPERTY(BlueprintReadOnly, Category = "Swimmer")
	int32 ContactPartCount = 0;

	/** The parts that went in during the current burst: a burst starts with the first entry after half a second with every part out of the
	 * water (a dive, a jump). 0 or more. */
	UPROPERTY(BlueprintReadOnly, Category = "Swimmer")
	int32 BurstParts = 0;

	/** The largest splash strength in the current burst (on Last Splash's scale, before the 0.2 cut-off). 0 before any entry.
	 * Read-only: 0 or more. */
	UPROPERTY(BlueprintReadOnly, Category = "Swimmer")
	float BurstLargestSplash = 0.f;

	/** The part (its bone) that made the largest splash in the current burst; None before any entry. */
	UPROPERTY(BlueprintReadOnly, Category = "Swimmer")
	FName BurstLargestPart;

	/** The impulse all its parts pressed into the water in the current burst, newton-seconds (the sum of their Last Impulse Ns). 0 before
	 * any entry.
	  * Read-only: 0 or more. */
	UPROPERTY(BlueprintReadOnly, Category = "Swimmer")
	float BurstImpulseNs = 0.f;
};

/** One GPU allocation FluidNinja Buoyancy owns (fnb.Memory, UFluidNinjaBuoyancySubsystem::ReportMemory): a render target or a readback's
 * staging memory, its size (texels; elements by 1 for a buffer), its format and its megabytes. */
struct FFluidNinjaBuoyancyMemoryItem
{
	FString Name;
	FIntPoint Size = FIntPoint::ZeroValue;
	FString Format;
	double MegaBytes = 0.0;
};

/** The quality tier in use (sg.WaterQuality; FluidNinjaBuoyancyModule.cpp applies its cvars). */
namespace FluidNinjaBuoyancyQuality
{
	/** 0 Low, 1 Medium, 2 High, 3 Epic, 4 Cinematic: sg.WaterQuality, or sg.EffectsQuality while it follows that. */
	FLUIDNINJABUOYANCY_API int32 GetLevel();
	/** sg.WaterQuality is -1: the tier follows sg.EffectsQuality. */
	FLUIDNINJABUOYANCY_API bool IsFollowing();
	FLUIDNINJABUOYANCY_API const TCHAR* GetTierName(int32 Level);
	/** "High (sg.WaterQuality -1 follows sg.EffectsQuality 2)", for the status line. */
	FLUIDNINJABUOYANCY_API FString Describe();
}

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
 * Without the gather, each body's texels are copied out and read back on their own. The readback is looked for at the end of every
 * render frame and taken before the next game frame's physics (fnb.Gather.FastReadback, TickPrePhysics), so the bodies feel water about
 * two frames old; heights are carried forward by their rate of change over those frames, and further within a frame step by step.
 * Outside the capture the top of a water volume (a Physics Volume with Water Volume on) gives a flat height.
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
class FLUIDNINJABUOYANCY_API UFluidNinjaBuoyancySubsystem : public UTickableWorldSubsystem, public IFluidNinjaBuoyancyBenchHost
{
	GENERATED_BODY()

public:
	UFluidNinjaBuoyancySubsystem();
	virtual ~UFluidNinjaBuoyancySubsystem() override;

	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	/** The base subsystem steps aside when a project derives its own (a customization layer): one water system per world. */
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual TStatId GetStatId() const override;
	/** TG_PrePhysics, every frame (FFluidNinjaBuoyancyPrePhysicsTickFunction): with fnb.Gather.FastReadback, the readbacks finished by the end of
	 * the last render frame become the bodies' patches here, before PhysicsPreTick hands the physics its water. */
	void TickPrePhysics(float DeltaSeconds);

	void Register(UFluidNinjaBuoyancyComponent* Component);
	void Unregister(UFluidNinjaBuoyancyComponent* Component);

	/** One line about the water field and one per body. */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Buoyancy")
	virtual FString GetStatus() const override { return Status; }

	/**
	 * fnb.Buoyancy.CalibrateFlow [apply]: measures how fast FluidNinja's dye really moves against what its velocity buffer
	 * says (block matching over a few frames around the first body, or the camera), and the Flow Scale Multiplier that would make
	 * the current the bodies feel match it. "apply" sets it (and saves it to DefaultGame.ini in the editor) when the match is good.
	 */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Buoyancy")
	virtual void CalibrateFlow(bool bApply) override;

	/** As CalibrateFlow, measured around Where (the test bench: at the selected body). */
	virtual void CalibrateFlowAt(const FVector& Where, bool bApply) override;

	/** The last calibration's report (empty before one). */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Buoyancy")
	virtual FString GetCalibrationReport() const override { return CalibrationReport; }

	/* Testing: the host side of the bench (IFluidNinjaBuoyancyBenchHost, FluidNinjaBuoyancyBench.h). The bench itself lives in the
	 * FluidNinjaBuoyancyDeveloper module and reaches the water only through that interface and the component's public API. */
	virtual UWorld* GetBenchWorld() const override;
	virtual const FFluidNinjaBuoyancyWind& GetWind() const override { return Wind; }
	/** Blows SpeedMs toward DirectionDegrees instead of the project's Wind, until switched off. */
	virtual void SetWindOverride(bool bOn, float SpeedMs = 0.f, float DirectionDegrees = 0.f) override;
	/** Controlled water: every body floats on flat, still water at Height (no waves, no current, no wakes), for measurements
	 * that the live water's current would swamp. The drawn water is unchanged. */
	virtual void SetControlledWater(bool bOn, float Height = 0.f) override;
	virtual bool IsWaterControlled() const override { return bControlledWater; }
	virtual float GetControlledWaterHeight() const override { return ControlledHeight; }
	virtual TArray<UFluidNinjaBuoyancyComponent*> GetBodies() const override;
	virtual bool GetBodyDebug(const UFluidNinjaBuoyancyComponent* Component, FFluidNinjaBuoyancyBodyDebug& Out) const override;
	/** A fresh slot (new id) for a body that was put back: the physics thread's state, slam peak and water start over. */
	void Reregister(UFluidNinjaBuoyancyComponent* Component);
	virtual float GetFlowScale() const override { return FlowScale; }
	virtual bool IsGathering() const override;
	/** Verify mode (with the gather on): every body's water is also copied out on its own, and each copy is compared with the
	 * gather of the same frame (FFluidNinjaBuoyancyBodyDebug::Verify*). Switching it on clears the statistics. */
	virtual void SetVerifyGather(bool bOn) override;
	/** Smoothed costs (ms): game-thread tick, its patch building, physics pre-tick, physics thread a frame. */
	virtual void GetCosts(float& OutTick, float& OutPatches, float& OutPreTick, float& OutPhysics) const override { OutTick = TickMs; OutPatches = PatchMs; OutPreTick = PreTickMs; OutPhysics = PhysicsMsPerFrame; }
	/** The same, with this frame's unsmoothed costs and the readback's age (FFluidNinjaBuoyancyCosts). */
	void GetCosts(FFluidNinjaBuoyancyCosts& Out) const;
	virtual int32 GetWakePoints() const override { return LastWakePoints; }
	virtual int32 GetCapturedPrimitives() const override { return LastCapturedPrimitives; }
	virtual int32 GetSurfacePrimitives() const override { return SurfacePrimitives.Num(); }
	/** The drawn water surface's primitives (the Surface Mirror shows only these). */
	virtual const TArray<TWeakObjectPtr<UPrimitiveComponent>>& GetSurfacePrimitiveComponents() const override { return SurfacePrimitives; }
	/** FluidNinja Buoyancy's side of FluidNinja Waves in this world (FluidNinjaBuoyancyWaves.h), or null before Begin Play. */
	UFluidNinjaBuoyancyWaves* GetWaves() const;
	/** The waves as the bench reads them (FluidNinjaBuoyancyBench.h): a facade over GetWaves, null where that is. */
	virtual IFluidNinjaBuoyancyBenchWaves* GetBenchWaves() override;
	virtual int32 GetGatherBodies() const override { return LastGatherBodies; }
	virtual int32 GetGatherPoints() const override { return LastGatherSamples; }
	/** The importance scheduler last frame: bodies whose turn it was but that the gather budget (fnb.Gather.BudgetPerFrame) left on their
	 * last patch, and bodies it was not the turn of (their level of detail's cadence). */
	int32 GetGatherWaiting() const { return LastGatherWaiting; }
	int32 GetGatherDeferred() const { return LastGatherDeferred; }
	/** Every GPU allocation of this world's water (the Surface Mirror, the gather and copy readbacks, the current probes; FluidNinja Waves'
	 * targets when its report is enabled), as fnb.Memory prints them. Returns the total in megabytes. */
	double ReportMemory(TArray<FFluidNinjaBuoyancyMemoryItem>& OutItems) const;
	/** Makes the test bench of a world: the FluidNinjaBuoyancyDeveloper module binds it at startup; unbound, no world gets a bench. */
	static FFluidNinjaBuoyancyBenchFactory& BenchFactory();
	/** The test bench of this world (game worlds, in builds carrying the FluidNinjaBuoyancyDeveloper module), or null. */
	IFluidNinjaBuoyancyBench* GetBench() const { return Bench.Get(); }


	/** Takes Mesh as a swimmer (FFluidNinjaSwimmerSource) until UnregisterSwimmer or its end; registering it again changes its source. */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Buoyancy")
	void RegisterSwimmer(USkeletalMeshComponent* Mesh, FFluidNinjaSwimmerSource Source);

	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Buoyancy")
	void UnregisterSwimmer(USkeletalMeshComponent* Mesh);

	/** What Mesh's body parts did to the water: a registered swimmer, or the player's pawn's mesh. False for any other mesh. */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Buoyancy")
	bool GetSwimmerStats(const USkeletalMeshComponent* Mesh, FFluidNinjaSwimmerStats& OutStats) const;

	/** An impulse pressing the water down at Location (kg·cm/s, Unreal's units; negative lifts it) over AreaCm2 seen from above, into the
	 * FluidNinja Waves simulation there at the next frame (scaled by Wave Impulse Scale, as the hulls' impulses are). */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Buoyancy")
	void AddWaterImpulse(FVector Location, float ImpulseDown, float AreaCm2);

	/** A Gaussian pressure head HeightCm high (negative: suction) and SigmaCm wide pressing on the water at WorldXY for one of the
	 * simulation's ticks, at the next frame: an impulse into the waves (a wave maker presses one every tick). */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Buoyancy")
	void AddWavePoke(FVector2D WorldXY, float HeightCm, float SigmaCm);

	/**
	 * The water's current around Location: the mean of FluidNinja's velocity (world, cm/s; Z is 0) over a ring around it, from Inner Radius
	 * Cm to Outer Radius Cm out, at most Max Flow Speed. A ring, because a swimmer's own wake lies right under it: read there, a swimmer would
	 * feel the water it pushes and be carried along by it. Inner Radius Cm 0 reads the whole disc (for a point nothing stirs). A negative
	 * radius takes the Swimmers settings' ring (Swimmer Current Ring Inner / Outer Cm, 70 / 160 cm).
	 *
	 * The velocity is read back from the GPU. Each call also asks for a fresh reading there, and returns the newest one that has arrived,
	 * Out Age Seconds old (usually two or three frames). Call it every frame for a moving swimmer; calls a frame apart at nearby places,
	 * and calls in the same frame within 25 cm of each other, share one reading.
	 *
	 * Returns false until the first reading at a new place has arrived (a few frames after the first call), where no FluidNinja simulation
	 * that the drawn water shows covers Location, and while FluidNinja Buoyancy is off. The current moves nothing by itself: what it does to
	 * a swimmer is the project's own movement code.
	 */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Buoyancy", meta = (AdvancedDisplay = "InnerRadiusCm,OuterRadiusCm"))
	bool GetCurrentAtLocation(FVector Location, FVector& OutCurrent, float& OutAgeSeconds, float InnerRadiusCm = -1.f, float OuterRadiusCm = -1.f);

	/**
	 * The water at Location this frame, one answer for swim code (FFluidNinjaWaterSample, FluidNinjaBuoyancyWaterQuery.h): the still
	 * water the bodies feel (the nearest body's patch, else the far field), the free wave height η = B − A read at the current probe,
	 * the surface's normal from η's slope, and the current on the swimmers' ring. Each call also asks for a fresh probe reading there:
	 * call it every frame for a moving swimmer (the first calls at a new place return the still water alone). False where no water is
	 * known. The swim mode floats on SurfaceZ and drifts with the low-passed, dead-banded current
	 * (UFluidNinjaBuoyancyWaterLibrary::FilterDriftCurrent).
	 */
	bool SampleWater(const FVector& Location, FFluidNinjaWaterSample& OutSample);

	/** The swimmers' wake points written into FluidNinja last frame (Swimmers → Swimmer Wakes). */
	int32 GetSwimmerWakePoints() const { return LastSwimmerWakePoints; }

	/** FluidNinja's own tracking: whether FluidNinja tracks this body (it generates overlap events or carries a NinjaLive's tracking
	 * tag), and whether this plugin handed it over (its Interaction asks for it and FluidNinja tracks relative to the water). */
	void GetTracking(const UFluidNinjaBuoyancyComponent* Component, bool& bOutTracked, bool& bOutHandedOver) const;
	/** Why bodies are or are not handed to FluidNinja's tracking (one line per NinjaLive). */
	const FString& GetTrackingReport() const { return TrackingReport; }
	/** Looks at the NinjaLive actors again and hands bodies over or takes them back (also done every second). */
	void RefreshTracking();

	/** Test bench: switches every floating body and every NinjaLive in the world to one way of stirring FluidNinja (see
	 * EFluidNinjaBuoyancyStirApproach). The first switch remembers each NinjaLive's own settings, which the wakes approach keeps. The NinjaLive
	 * changes are made during play: FluidNinja picks most up at once, its density setting within its next parameter update. */
	virtual void SetStirApproach(EFluidNinjaBuoyancyStirApproach Approach) override;
	virtual EFluidNinjaBuoyancyStirApproach GetStirApproach() const override { return StirApproach; }
	/** False until the first switch: the bodies and NinjaLives still have the level's own settings. */
	virtual bool IsStirApproachSet() const override { return bStirApproachSet; }
	static const TCHAR* StirApproachName(EFluidNinjaBuoyancyStirApproach Approach);
	/** One line per NinjaLive (tag, object types, switches, and how many of the points FluidNinja tracks lie on floating bodies), then
	 * the bodies' routes. */
	virtual FString DescribeStirApproach() const override;

protected:
	/** Where FluidNinja's parameter collection puts the simulation following the player (TraceMeshPos / TraceMeshSize). */
	void UpdateSimSquare();
	/** The local player's pawn and the water at it, for the swimmers' body parts and FluidNinja Waves' swimmer. */
	virtual void UpdateSwimmer(float DeltaSeconds);
	/** The swimmers' body parts against the water (FFluidNinjaBuoyancySwimmers), after UpdateSwimmer. */
	virtual void TickSwimmerParts(float DeltaSeconds);
	/** A swimmer dived (FFluidNinjaBuoyancySwimmers::OnEntry): the whole body's entry splash. */
	void OnSwimmerDive(const USkeletalMeshComponent* Mesh, const FVector& Where, float SpeedCmS, float MassShareKg);
	/** The swimmers' container, made on first use, with the dive splash bound to it. */
	FFluidNinjaBuoyancySwimmers& EnsureSwimmers();
	FDelegateHandle SwimmerEntryHandle;
	/** The swimmers' wakes into FluidNinja's velocity field (the Wake Data Channel): the player's pawn, after UpdateSwimmer, and the
	 * registered swimmers (FluidNinjaBuoyancyCurrent.cpp). */
	virtual void WriteSwimmerWakes(float DeltaSeconds);
	/** The drawn surfaces and the simulations brought up to date for the current reads, once a frame whatever else ran: RefreshSources
	 * when due, UpdateSimSquare (the simulation following the player moves every frame), UpdateFlowSources. */
	void UpdateFlowSourcesForCurrent();
	/** The probe for a read of the current around Location on the ring Inner to Outer (found, moved there or made) with its copy launched
	 * once a frame; INDEX_NONE when none can be had. */
	int32 RequestCurrent(const FVector2D& Location, float InnerCm, float OuterCm);
	/** Once a frame, before the swimmers: finished copies read out on the render thread, the newest into its probe; probes nobody asked
	 * for in a second dropped. */
	void CollectCurrentProbes();
	/** FluidNinja Waves' frame, after the Surface Mirror and the gather: the bodies, the Surface Mirror, the simulations, the swimmer and
	 * the physics impulses (bIdle: no floating bodies, so the surfaces and simulations were not refreshed by the capture, and the waves'
	 * BeginFrame runs here). */
	void TickWaves(float DeltaSeconds, bool bIdle);
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
		float EntryWatts = 0.f;   // the frame's entry power (FStepResult::EntryPower), kg·cm²/s³ like SlamWatts until the readout
		TSharedPtr<FFluidNinjaBuoyancyBodyOutput> LastOutput;
		// The copied squares keep one size per body (a readback reuses its staging texture, which must not change size).
		int32 HeightSide = 0;
		int32 FlowSide = 0;
		int32 Level = 0;
		/** 0 near, 1 beyond the first level-of-detail distance, 2 beyond the second (whatever levels the hull has; the distances scaled by
		 * fnb.Lod.DistanceScale). The gather's spacing and cadence follow it; the hull's level adds fnb.Lod.Bias. */
		int32 DistanceLevel = 0;
		/** From the camera (cm) and over the water (cm/s), as UpdateLevelAndSleep last found them: the gather's importance. */
		double CameraDistanceCm = 0.0;
		double SpeedCmS = 0.0;
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
		/** fnb.Lod.DormantDrift, settling on flat water with no current: the window is open, and the body's horizontal velocity (cm/s) and
		 * place (world XY, cm) when it opened. It settles once it keeps that velocity, within the settle band, for Settle Seconds. */
		bool bSettleOpen = false;
		FVector2D SettleVelocity = FVector2D::ZeroVector;
		FVector2D SettleLocation = FVector2D::ZeroVector;
		/** Asleep, it drifts (DriftDormant): at its mean velocity over the settle window (cm/s, world XY; its drift relative to the water,
		 * which has no current there), carried from the wind it settled in (cm/s, world XY; kept by every sleeping body, for
		 * DormantWindOutgrown) to the wind of the moment. The drift not yet applied (cm), and when it next is (world seconds). The throttle
		 * it fell asleep at: game code changing it wakes the body. */
		bool bDormantDrifting = false;
		FVector2D DormantDrift = FVector2D::ZeroVector;
		FVector2D DormantWind = FVector2D::ZeroVector;
		float DormantThrottle = 0.f;
		FVector2D DormantPending = FVector2D::ZeroVector;
		double NextDormantMove = 0.0;
		/** When its water was last asked of the GPU (the gather); -1 before the first time. Far bodies take turns by frames
		 * (fnb.Gather.FarEveryNFrames) and this keeps the gap between two of theirs within Max Extrapolation Seconds. */
		double LastGatherTime = -1.0;
		/** Its wave impulse pieces last frame, so this frame's are collected with one allocation. */
		int32 LastImpulsePieces = 0;
		// FluidNinja's own tracking: FluidNinja tracks the body; this plugin handed it over (and what to give back).
		bool bNinjaTracked = false;
		bool bHandedOver = false;
		bool bOverlapBefore = false;
		TArray<FName> AddedTags;
		bool bTrackingWarned = false;
		/** Why the body was last reported as not handed over (logged again when the reason changes). */
		FString TrackingNote;
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
		/** FluidNinja Waves: the simulation's pressure buffer (RT_PressureDivergence, the surface material's Pressure Buffer Parameter). */
		TWeakObjectPtr<UTextureRenderTarget2D> Pressure;
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
	/** The slow path's look for finished readbacks (fnb.Gather.FastReadback 0): a render command, queued here, reads out on the render thread
	 * what the GPU has finished of the copies made under that path. The fast path's copies are found at the end of each render frame
	 * (FFluidNinjaBuoyancyReadbackPoller) and need none. */
	void PollReadbacks();
	/** The finished readbacks (Stage 2) become the bodies' patches: the gathers oldest first, then each body's newest copy. In TickPrePhysics
	 * with fnb.Gather.FastReadback, in Tick without. */
	void CollectReadbacks();
	void CaptureAndCopy();
	int32 CopyFootprints(const TArray<FFootprint>& Wanted, double Now);
	/** The importance scheduler and the gather dispatch: which of the bodies the capture serves are gathered this frame (their level of
	 * detail's cadence, the per-frame budget, nearest and fastest first, never past Max Extrapolation Seconds while the budget allows),
	 * and one compute pass per simulation for them. */
	int32 GatherFootprints(const TArray<FFootprint>& Wanted, double Now);
	void BuildPatch(FSlot& Slot, FFluidNinjaBuoyancyReadback& Data);
	/** Verify mode: a body's copied water against its gathered water of the same frame. */
	void CompareWithGather(FSlot& Slot, const FluidNinjaBuoyancy::FWaterPatch& Copied);
	void BuildGatherPatches(const FFluidNinjaBuoyancyGatherReadback& Data);
	/** Rate of rise, the current around the body, the water under its centre; then the patch is the body's. */
	void FinishPatch(FSlot& Slot, const TSharedRef<FluidNinjaBuoyancy::FWaterPatch, ESPMode::ThreadSafe>& Patch);
	void UpdateFlowProbe();
	virtual void UpdateWind(float DeltaSeconds);
	/** The still water outside every capture under Bounds (the far field): a water volume's top. A project may answer from its own
	 * published water instead. */
	virtual bool FarFieldTop(const FBox& Bounds, float& OutTop) const;
	/** Each source's square (its surface material, else the parameter collection) and flow scale, this frame. */
	void UpdateFlowSources();
	/** The source whose surface lies under Point (the smallest where they overlap), else whose square holds it; INDEX_NONE if none. */
	int32 FindFlowSource(const FVector2D& Point) const;
	const FFlowSource* FindFlowSourceFor(const UTextureRenderTarget2D* Buffer) const;
	/** Wave Follow (FluidNinjaBuoyancyWaveFollow.cpp): the wave height of the simulation under the view copied into the Wave Follow Target
	 * and its square published in the Wave Follow Collection, for MF_FNB_WaveFollow; at the end of every waves' frame. */
	void UpdateWaveFollow();
	void DrawDebug();
	FSlot* FindSlot(int32 Id);
	void UpdateLevelAndSleep(FSlot& Slot, const FVector& Camera, float DeltaSeconds);
	/** fnb.Lod.DormantDrift: the wind now is more than twice the one the sleeping body settled in (or 1 m/s or more after a calm), or turned
	 * more than 45 degrees from it: its drift is no longer known, so it wakes to settle again. */
	bool DormantWindOutgrown(const FSlot& Slot) const;
	/** fnb.Lod.DormantDrift: moves a sleeping, drifting body on by its drift carried to the wind of the moment, in swept steps a quarter of
	 * a second apart (no water read, no physics), and keeps it asleep. False when it must wake: it reached a shore or another body, or the
	 * flat water it settled on ends there. */
	bool DriftDormant(FSlot& Slot, UPrimitiveComponent& Primitive, float DeltaSeconds, double Now);
	/** At the end of a tick: the frame's counters and costs to the stats system and the CSV profiler (FluidNinjaBuoyancyStats.h). */
	void PublishFrameStats();

	TArray<FSlot> Slots;
	int32 NextId = 1;

	TArray<TSharedPtr<FFluidNinjaBuoyancyGatherReadback, ESPMode::ThreadSafe>> GatherRing;
	int32 GatherCursor = 0;
	int32 LastGatherBodies = 0;
	int32 LastGatherSamples = 0;
	/** The importance scheduler's counts last frame (GetGatherWaiting, GetGatherDeferred), and the bodies by distance level. */
	int32 LastGatherWaiting = 0;
	int32 LastGatherDeferred = 0;
	int32 LastBodiesByLevel[3] = {0, 0, 0};
	/** The total of ReportMemory as last built for the status (once a second at most). */
	double LastMemoryMB = 0.0;
	double NextMemoryReport = 0.0;

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

	TSharedPtr<IFluidNinjaBuoyancyBench> Bench;
	/** GetBenchWaves' facade over this world's UFluidNinjaBuoyancyWaves (made on first ask). */
	TUniquePtr<IFluidNinjaBuoyancyBenchWaves> BenchWaves;
	/** The swimmers' body-part contacts and the registered swimmers (FluidNinjaBuoyancySwimmers.h). */
	TSharedPtr<FFluidNinjaBuoyancySwimmers> Swimmers;
	/** The player's pawn's body mesh, as last found. */
	TWeakObjectPtr<USkeletalMeshComponent> PlayerSwimmerMesh;

	// What FluidNinja Buoyancy costs (milliseconds, smoothed): the game thread's tick and the part of it building patches, the physics
	// pre-tick, and the physics thread's steps per frame and per step.
	float TickMs = 0.f;
	float PatchMs = 0.f;
	float PreTickMs = 0.f;
	float PhysicsMsPerFrame = 0.f;
	float PhysicsMsPerStep = 0.f;
	// The same, this frame's own (the CSV profiler takes these; the physics pre-tick's and the patches' are cleared once published, so a
	// frame the physics scene did not tick, or no patch was built in, reads 0).
	float FrameTickMs = 0.f;
	float FramePatchMs = 0.f;
	float FramePreTickMs = 0.f;
	float FramePhysicsMs = 0.f;
	/** What TickPrePhysics cost this frame: counted into FrameTickMs with the tick's own, so the game thread's total reads the same whichever
	 * path builds the patches. */
	float FramePrePhysicsMs = 0.f;
	/** The frame the newest readback consumed so far was queued in (0 before any), and how many frames ago that is. */
	uint64 LastReadbackFrame = 0;
	int32 ReadbackAgeFrames = 0;

	FFluidNinjaBuoyancyCallback* Callback = nullptr;
	FDelegateHandle PreTickHandle;
	/** The pre-physics tick (TickPrePhysics), registered at Begin Play and unregistered at Deinitialize. */
	FFluidNinjaBuoyancyPrePhysicsTickFunction PrePhysicsTick;
	/** fnb.Gather.FastReadback: the render thread's end-of-frame look for finished readbacks. Shared with the render commands that bind and
	 * unbind it there; null where nothing ever renders (the slow path stays). */
	TSharedPtr<FFluidNinjaBuoyancyReadbackPoller, ESPMode::ThreadSafe> ReadbackPoller;

	UPROPERTY(Transient)
	TObjectPtr<ASceneCapture2D> Capture;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> CaptureTarget;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialParameterCollection> NinjaCollection;

	/** Wave Follow's render target and parameter collection (the settings' Wave Follow Target and Collection), loaded once. */
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> WaveFollowTarget;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialParameterCollection> WaveFollowCollection;
	bool bWaveFollowLoaded = false;
	/** The pressure buffer Wave Follow copied last frame (null: the window published invalid). */
	TWeakObjectPtr<UTextureRenderTarget2D> WaveFollowSource;
	bool bWaveFollowPublished = false;

	/** FluidNinja's global point input, where the wakes go. */
	UPROPERTY(Transient)
	TObjectPtr<UNiagaraDataChannelAsset> WakeChannel;
	int32 LastWakePoints = 0;

	TArray<TWeakObjectPtr<UPrimitiveComponent>> SurfacePrimitives;
	int32 SurfaceLeftOut = 0;
	TArray<FFlowSource> FlowSources;
	/** Each drawn surface primitive and the flow source it shows (its material's velocity buffer, else the one under it), for FluidNinja
	 * Waves' WaveNormalScale. */
	TArray<TPair<TWeakObjectPtr<UPrimitiveComponent>, int32>> SurfaceSources;
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
	/** The status text is built this frame: the debug view or the test bench reads it every frame, else once a second (GetStatus
	 * returns the last one built). */
	bool bBuildStatus = false;
	double NextStatusBuild = 0.0;
	uint32 PatchesBuilt = 0;
	/** The drawn water's actors handed to the waves last frame (the list is reserved to it). */
	int32 LastSurfaceActors = 0;
	FString TrackingReport;
	double NextTrackingRefresh = 0.0;

	// The test bench's stir approach, and what the NinjaLives had before the first switch.
	struct FStirNinja
	{
		TWeakObjectPtr<AActor> Actor;
		TArray<TPair<TWeakObjectPtr<UObject>, FName>> Tags;
		TArray<TPair<TWeakObjectPtr<UObject>, TArray<int64>>> Filters;
		TArray<TPair<TWeakObjectPtr<UObject>, int64>> Densities;
		TArray<TPair<TWeakObjectPtr<UNiagaraComponent>, float>> Relatives;
	};
	EFluidNinjaBuoyancyStirApproach StirApproach = EFluidNinjaBuoyancyStirApproach::Wakes;
	bool bStirApproachSet = false;
	TArray<FStirNinja> StirNinjas;
	double StirReportAt = 0.0;

	// The swimmer (the local player's pawn) and the water at it.
	TWeakObjectPtr<APawn> Swimmer;
	bool bSwimmerInWater = false;
	/** As UpdateSwimmer last found it: its Character Movement swimming, or wading (water at its feet). */
	bool bSwimmerSwimming = false;
	bool bSwimmerWading = false;

	/** A place the water's current is read at (GetCurrentAtLocation, the swimmers' wakes): a small square of one simulation's velocity
	 * buffer around it, copied and read back every frame it is asked for. */
	struct FCurrentProbe
	{
		FVector2D Where = FVector2D::ZeroVector;
		float InnerCm = 0.f;
		float OuterCm = 0.f;
		double LastRequest = 0.0;
		uint64 RequestFrame = 0;
		uint64 LaunchFrame = 0;
		/** The buffer the square is copied from and its side in texels (a change makes new readbacks: they keep their staging size). */
		TWeakObjectPtr<UTextureRenderTarget2D> Buffer;
		int32 Side = 0;
		TArray<TSharedPtr<FFluidNinjaBuoyancyCurrentReadback, ESPMode::ThreadSafe>> Ring;
		// The newest reading: the mean flow on the ring and right at the centre (cm/s), where and when it was copied.
		FVector2D Current = FVector2D::ZeroVector;
		FVector2D AtCentre = FVector2D::ZeroVector;
		FVector2D ReadWhere = FVector2D::ZeroVector;
		double ReadTime = -1.0;
		/** No simulation covered Where at the last request. */
		bool bOutside = false;
		/** FluidNinja Waves at the probe (SampleWater): the free height η = B − A at Where and its slope, from the pressure square read
		 * with the same copy; PressureSide keyed like Side (a change makes new readbacks). */
		int32 PressureSide = 0;
		float WaveHeight = 0.f;
		FVector2f WaveGradient = FVector2f::ZeroVector;   // dη/dx, dη/dy (cm per cm)
		bool bWave = false;
	};
	TArray<FCurrentProbe> CurrentProbes;
	bool bCurrentProbeCapLogged = false;
	uint64 CurrentSourcesFrame = 0;

	/** A registered swimmer last frame: its mesh's place and its hips' (its velocity), how fast it fell (its plunge), whether it was in. */
	struct FSwimmerWake
	{
		TWeakObjectPtr<USkeletalMeshComponent> Mesh;
		FVector Previous = FVector::ZeroVector;
		FVector HipsPrevious = FVector::ZeroVector;
		double FallSpeed = 0.0;
		bool bHasPrevious = false;
		bool bWasIn = false;
	};
	TArray<FSwimmerWake> SwimmerWakes;
	/** The player's pawn swam last frame; its velocity while it did not (a plunge's speed: swimming damps it on the frame it starts). */
	bool bSwimmerWasSwimming = false;
	FVector SwimmerAirVelocity = FVector::ZeroVector;
	int32 LastSwimmerWakePoints = 0;
	/** The water surface at the swimmer as UpdateSwimmer last found it, and the frame it did (the ripples' swimmer source). */
	float SwimmerSurfaceZ = 0.f;
	bool bSwimmerSurfaceKnown = false;
	uint64 SwimmerSurfaceFrame = 0;

	/** FluidNinja Buoyancy's side of FluidNinja Waves (created at Begin Play in game worlds). */
	UPROPERTY(Transient)
	TObjectPtr<UFluidNinjaBuoyancyWaves> Waves;
	/** Every popped physics step's wave impulses since the last frame, per body id (world x, y, I_down, area), handed to the waves in Tick. */
	TMap<int32, TArray<FVector4f>> PendingWaveImpulses;
	bool bWaveImpulseCapLogged = false;
	/** The Surface Mirror was drawn this frame (the waves raster the hulls against it). */
	bool bCapturedThisFrame = false;
};
