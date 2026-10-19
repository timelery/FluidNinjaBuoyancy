#include "FluidNinjaBuoyancySubsystem.h"

#include "Components/CapsuleComponent.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/CollisionProfile.h"
#include "Engine/Engine.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Algo/Sort.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "FluidNinjaBuoyancyComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PhysicsVolume.h"
#include "FluidNinjaBuoyancyCallback.h"
#include "FluidNinjaBuoyancyCore.h"
#include "FluidNinjaBuoyancyGather.h"
#include "FluidNinjaBuoyancyLog.h"
#include "FluidNinjaBuoyancySettings.h"
#include "FluidNinjaBuoyancyStats.h"
#include "FluidNinjaBuoyancySwimmerProvider.h"
#include "FluidNinjaBuoyancySwimmers.h"
#include "FluidNinjaBuoyancyWaterQuery.h"
#include "FluidNinjaBuoyancyWaves.h"
#include "NiagaraFunctionLibrary.h"
#include "Async/ParallelFor.h"
#include "Misc/ScopeExit.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Stats/Stats.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "NiagaraActor.h"
#include "NiagaraComponent.h"
#include "NiagaraDataChannelAccessContext.h"
#include "NiagaraDataChannelAccessor.h"
#include "NiagaraDataChannelAsset.h"
#include "NiagaraDataChannelFunctionLibrary.h"
#include "NiagaraDataInterfaceArrayFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "PBDRigidsSolver.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "TextureResource.h"
#include "UObject/EnumProperty.h"

#include <atomic>
#include <limits>

namespace FluidNinjaBuoyancyCVars
{
	// fnb.Wake.AlongOnly and its counters (A/B prototype), defined in FluidNinjaBuoyancyCurrent.cpp.
	extern int32 WakeAlongOnly;
	extern float WakeAlongOnlyMinCmS;
	extern std::atomic<int64> WakeAlongOnlyHullWritten;
	extern std::atomic<int64> WakeAlongOnlyHullSkipped;
}

/** One body's water, copied out of the capture and the velocity buffer and read back; shared with the render thread. */
struct FFluidNinjaBuoyancyReadback
{
	FRHIGPUTextureReadback Height{FName(TEXT("FluidNinjaBuoyancyHeight"))};
	FRHIGPUTextureReadback Flow{FName(TEXT("FluidNinjaBuoyancyFlow"))};
	std::atomic<int32> Stage{0};   // 0 free, 1 copy queued, 2 read into the arrays below
	/** Counts the copies queued into this readback: a read the render thread was asked for earlier only proceeds if no newer copy
	 * has been queued since (the readback is reused, possibly larger, once its last copy is read). */
	std::atomic<uint32> Generation{0};

	// Set by the game thread with the copy.
	FIntPoint HeightMin = FIntPoint::ZeroValue;
	FIntPoint HeightSize = FIntPoint::ZeroValue;
	FIntPoint FlowMin = FIntPoint::ZeroValue;
	FIntPoint FlowSize = FIntPoint::ZeroValue;
	bool bFlow = false;
	EPixelFormat FlowFormat = PF_Unknown;
	FVector2D CaptureOrigin = FVector2D::ZeroVector;
	double CaptureTop = 0.0;
	double CaptureRange = 0.0;
	double CaptureTexel = 1.0;
	int32 CaptureResolution = 0;
	FVector2D SimCentre = FVector2D::ZeroVector;
	double SimSizeCm = 0.0;
	int32 FlowResolution = 0;
	float FlowScale = 0.f;
	double Time = 0.0;
	/** The game thread's frame the copy was queued in (the readback's age in frames when it is consumed). */
	uint64 Frame = 0;
	/** Queued under fnb.Gather.FastReadback: the render thread's end-of-frame look (FFluidNinjaBuoyancyReadbackPoller) reads this copy out, not
	 * the command the slow path queues from the game thread (PollReadbacks leaves it alone). */
	bool bFastPoll = false;

	// Filled on the render thread.
	TArray<float> HeightTexels;
	TArray<FVector2f> FlowTexels;
};

/** Every body's water from one compute pass (FluidNinjaBuoyancyGather.usf), read back in one piece; shared with the render thread. */
struct FFluidNinjaBuoyancyGatherReadback
{
	FRHIGPUBufferReadback Buffer{FName(TEXT("FluidNinjaBuoyancyGather"))};
	std::atomic<int32> Stage{0};   // 0 free, 1 dispatched, 2 read into Samples
	/** As FFluidNinjaBuoyancyReadback::Generation. */
	std::atomic<uint32> Generation{0};

	struct FBody
	{
		int32 Id = 0;
		FVector2D Origin = FVector2D::ZeroVector;   // world XY of grid point (0, 0)
		double Spacing = 1.0;
		int32 Grid = 0;
		int32 First = 0;
		bool bFlow = false;
		/** FluidNinja Waves: its samples' w is B - B_self. */
		bool bWave = false;
	};
	TArray<FBody> Bodies;
	int32 TotalSamples = 0;
	float FlowScale = 0.f;
	double Time = 0.0;
	/** As FFluidNinjaBuoyancyReadback::Frame and bFastPoll. */
	uint64 Frame = 0;
	bool bFastPoll = false;

	// Filled on the render thread.
	TArray<FVector4f> Samples;
};

/** fnb.Buoyancy.CalibrateFlow: one square of the simulation's velocity buffer, read back on consecutive frames. */
struct FFluidNinjaBuoyancyFlowProbe
{
	static constexpr int32 Shots = 12;
	/** Shots compared this many frames apart: the dye moves several texels between them (a sub-texel move biases the peak). */
	static constexpr int32 Gap = 6;
	static constexpr int32 Side = 256;

	struct FShot
	{
		FRHIGPUTextureReadback Readback{FName(TEXT("FluidNinjaBuoyancyFlowProbe"))};
		std::atomic<int32> Stage{0};   // 0 not taken, 1 copy queued, 2 read into Texels
		double Time = 0.0;
		uint64 Frame = 0;
		FVector2D SimCentre = FVector2D::ZeroVector;
		float SmoothedDeltaTime = 0.f;
		TArray<FVector4f> Texels;      // u, v, density, wet
	};
	FShot Shot[Shots];
	int32 Taken = 0;
	TWeakObjectPtr<UTextureRenderTarget2D> Buffer;
	FIntPoint Min = FIntPoint::ZeroValue;
	EPixelFormat Format = PF_Unknown;
	int32 Resolution = 0;
	double SimSizeCm = 0.0;
	float FlowScale = 0.f;
	float Multiplier = 1.f;
	float SimSpeed = 1.f;
	float SamplingFPS = 60.f;
	/** NinjaLiveCore's fixed tick rate; 0 when it steps once a frame. */
	float FixedStepsPerSecond = 0.f;
	bool bApply = false;
	double Started = 0.0;
};

namespace
{
	constexpr int32 RingSize = 3;
	constexpr double SourceRefreshSeconds = 2.0;
	constexpr double FlatQuerySeconds = 0.25;
	constexpr float RangeExtraCm = 3000.f;
	/** The footprint leads a moving body by at most this. */
	constexpr double MaxLeadCm = 200.0;
	/** Barely moving: below these for SettleSeconds, out of sight and far, a body goes to sleep. */
	constexpr float SettledSpeedCmS = 3.f;
	constexpr float SettledSpinRadS = 0.03f;
	constexpr float SettleSeconds = 2.f;
	/** fnb.Lod.DormantDrift. A wind under 1 m/s drifts a floater by a few cm/s at most (a ball by about 3.5 % of the wind), inside the settle
	 * band: a body that settled in it learned no drift, lies still asleep, and wakes when a stronger wind blows. */
	constexpr double DormantDriftMinWindCmS = 100.0;
	/** The fastest drift a settling body may have, as a share of the wind (passive floaters drift at 1 to 10 % of it, a ball at about 3.5 %):
	 * faster, or against the wind, something else moves it (game code, a tow), and it settles only when still. */
	constexpr double DormantDriftMaxWindShare = 0.1;
	/** A settled drift under 1 cm/s is the settle window's noise, not a drift (it would carry a body pinned on a bank into the bank). */
	constexpr double DormantDriftMinCmS = 1.0;
	/** cos 45°: a wind turned further than this from the one a body settled in wakes it (DormantWindOutgrown). */
	constexpr double DormantDriftMaxTurnCos = 0.70710678118654752;
	/** A dormant body is out of sight, so its drift is applied in steps (each a swept move and a sleep request) this far apart (s) and
	 * at least this long (cm). */
	constexpr double DormantMoveSeconds = 0.25;
	constexpr double DormantMinMoveCm = 1.0;
	/** The water volume's top under a drifting dormant body may differ by this much from the one it fell asleep on (cm). */
	constexpr double DormantMaxFlatChangeCm = 1.0;
	/** A water height counts for a body only within this of it: a river passing high over a pool is not the pool's water, and must
	 * neither lift the capture's near plane over it (which would then capture that river and keep it there) nor stand in as the
	 * body's far-field water. */
	constexpr double WaterReachCm = 300.0;

	/** fnb.Gather.FarEveryNFrames: far bodies take their water from the gather less often (GatherFootprints, the importance scheduler). */
	int32 GatherFarEveryNFrames = 2;
	FAutoConsoleVariableRef CVarGatherFarEveryNFrames(TEXT("fnb.Gather.FarEveryNFrames"), GatherFarEveryNFrames,
		TEXT("The gather's cadence by level of detail: bodies beyond their first level-of-detail distance (Lod Distances Cm X, scaled by fnb.Lod.DistanceScale) have their ")
		TEXT("water gathered every this many frames, bodies beyond the second every twice as many, nearer ones every frame; between gathers a body's patch carries ")
		TEXT("forward (the physics extrapolates it by its rate), never for longer than Max Extrapolation Seconds. Raising it saves gather work on far bodies; their ")
		TEXT("water is older. 1 (or less): every body every frame. Default 2. Set by sg.WaterQuality (4 / 3 / 2 / 2 / 2, Low to Cinematic)."),
		ECVF_Scalability);

	/** fnb.Gather.BudgetPerFrame: the importance scheduler's budget, bodies gathered in one frame at most (0: Max Gather Bodies alone). Over it,
	 * the bodies that would carry past Max Extrapolation Seconds go first (the longest waiting of them first), then the nearest and fastest; the
	 * rest keep their last patch and extrapolate. */
	int32 GatherBudgetPerFrame = 0;
	FAutoConsoleVariableRef CVarGatherBudgetPerFrame(TEXT("fnb.Gather.BudgetPerFrame"), GatherBudgetPerFrame,
		TEXT("At most this many bodies have their water gathered in one frame (0 or less, the default: no budget beyond Max Gather Bodies; above it, Max Gather Bodies ")
		TEXT("rules). Over the budget, bodies whose patch would go past Max Extrapolation Seconds come first (the longest waiting first), then the nearest and fastest; ")
		TEXT("the rest keep their last patch and extrapolate (stat FluidNinjaBuoyancy: Bodies waiting). Lower it to bound one frame's gather pass and readback in a ")
		TEXT("crowd. Set by sg.WaterQuality (16 / 64 / 128 / 0 / 0, Low to Cinematic)."),
		ECVF_Scalability);

	/** fnb.Gather.MaxGrid: a cap on Max Gather Grid (0: the setting). */
	int32 GatherMaxGrid = 0;
	FAutoConsoleVariableRef CVarGatherMaxGrid(TEXT("fnb.Gather.MaxGrid"), GatherMaxGrid,
		TEXT("Caps Max Gather Grid (Project Settings → FluidNinja Buoyancy → Surface Mirror), the grid points across a body's water: a value above 0 is clamped to 8-256 ")
		TEXT("and can only lower the setting (a big body is then sampled more coarsely). 0 or less (the default) leaves the setting. Set by sg.WaterQuality (32 / 64 / 96 / 0 / 0, ")
		TEXT("Low to Cinematic)."),
		ECVF_Scalability);

	/** fnb.Capture.Scale and fnb.Capture.EveryNFrames: the Surface Mirror's resolution (a scale on Capture Resolution) and how often it is
	 * redrawn (between redraws the gather reads the last one). */
	float CaptureScale = 1.f;
	FAutoConsoleVariableRef CVarCaptureScale(TEXT("fnb.Capture.Scale"), CaptureScale,
		TEXT("Scales the Surface Mirror's Capture Resolution (Project Settings → FluidNinja Buoyancy → Surface Mirror): 0.5 halves it, 2 doubles it. The result is ")
		TEXT("clamped to 64-4096 texels across, and values under 0.01 count as 0.01. Lower is cheaper (fewer gather points a body, fewer pixels drawn) and reads the water ")
		TEXT("more coarsely. Default 1. Set by sg.WaterQuality (0.25 / 0.5 / 0.75 / 1 / 2, Low to Cinematic)."),
		ECVF_Scalability);
	int32 CaptureEveryNFrames = 1;
	FAutoConsoleVariableRef CVarCaptureEveryNFrames(TEXT("fnb.Capture.EveryNFrames"), CaptureEveryNFrames,
		TEXT("The Surface Mirror (the depth capture of the drawn water) is redrawn every this many frames; between redraws the bodies' water is gathered from the last one, ")
		TEXT("whose square and near plane are kept, so the water's rate comes in steps. 1 (or less, the default): every frame. Set by sg.WaterQuality (3 / 2 / 1 / 1 / 1, ")
		TEXT("Low to Cinematic)."),
		ECVF_Scalability);

	/** fnb.Lod.*: the distances every body's Lod Distances Cm and Sleep Distance Cm stand for are scaled, and the hull's level of detail is
	 * pushed Bias levels coarser (UpdateLevelAndSleep). */
	float LodDistanceScale = 1.f;
	FAutoConsoleVariableRef CVarLodDistanceScale(TEXT("fnb.Lod.DistanceScale"), LodDistanceScale,
		TEXT("Scales every body's Lod Distances Cm (the hull's levels of detail and the gather's spacing and cadence): 0.5 brings the coarser levels twice as near, 2 pushes them twice as far. ")
		TEXT("Default 1; values under 0.01 count as 0.01. Set by sg.WaterQuality (0.5 / 0.75 / 1 / 1 / 1.5, Low to Cinematic)."),
		ECVF_Scalability);
	int32 LodBias = 0;
	FAutoConsoleVariableRef CVarLodBias(TEXT("fnb.Lod.Bias"), LodBias,
		TEXT("Every body's hull is this many levels of detail coarser than its distance asks (0, 1 or 2; the triangles model's levels; clamped to 0-2 and to the hull's ")
		TEXT("coarsest level). Coarser hulls cost less and float less exactly. The gather's spacing and cadence follow the distance alone. Default 0. ")
		TEXT("Set by sg.WaterQuality (2 / 1 / 0 / 0 / 0, Low to Cinematic)."),
		ECVF_Scalability);
	float LodSleepDistanceScale = 1.f;
	FAutoConsoleVariableRef CVarLodSleepDistanceScale(TEXT("fnb.Lod.SleepDistanceScale"), LodSleepDistanceScale,
		TEXT("Scales every body's Sleep Distance Cm: 0.5 lets settled, unseen bodies sleep half as far from the camera, 2 keeps them awake twice as far. ")
		TEXT("Default 1; values under 0.01 count as 0.01. Set by sg.WaterQuality (0.5 / 0.75 / 0.85 / 1 / 1.5, Low to Cinematic)."),
		ECVF_Scalability);
	/** fnb.Lod.DormantDrift: 1 a passive floater on flat water with no current (the far field, or controlled water) may fall asleep while it
	 * drifts steadily with the wind, and asleep it keeps drifting (UpdateLevelAndSleep, DriftDormant); every sleeping body wakes when the wind
	 * outgrows the one it settled in (DormantWindOutgrown). 0 a body sleeps only when still and lies frozen whatever the wind does, as builds
	 * before 2026-10-18 did (for A/B). */
	int32 LodDormantDrift = 1;
	FAutoConsoleVariableRef CVarLodDormantDrift(TEXT("fnb.Lod.DormantDrift"), LodDormantDrift,
		TEXT("1 (the default; any value but 0): on flat water with no current (the far field beyond the Surface Mirror, or controlled water) a passive floater may fall asleep while it drifts ")
		TEXT("steadily with the wind, and asleep it keeps drifting at that drift, scaled with the wind's speed (no water read, no physics); it wakes when ")
		TEXT("the wind blows more than twice as hard as when it fell asleep (or past 1 m/s after a calm) or turns more than 45 degrees, when its throttle ")
		TEXT("changes, at a shore or another body, or where that water ends. 0: a body sleeps only when still and lies frozen whatever the wind does (the ")
		TEXT("older behaviour, for A/B)."));

	/** fnb.Wake.MaxPoints and fnb.Waves.MaxImpulses: caps on Max Wake Points and Max Wave Impulses (0: the setting). */
	int32 WakeMaxPoints = 0;
	FAutoConsoleVariableRef CVarWakeMaxPoints(TEXT("fnb.Wake.MaxPoints"), WakeMaxPoints,
		TEXT("Caps Max Wake Points (Project Settings → FluidNinja Buoyancy → Wake), the floating bodies' wake points written into FluidNinja in one frame (taken first ")
		TEXT("come until the cap): a value above 0 can only lower the setting; 0 or less (the default) leaves it. Set by sg.WaterQuality (64 / 128 / 192 / 0 / 0, Low to Cinematic)."),
		ECVF_Scalability);
	int32 WavesMaxImpulses = 0;
	FAutoConsoleVariableRef CVarWavesMaxImpulses(TEXT("fnb.Waves.MaxImpulses"), WavesMaxImpulses,
		TEXT("Caps Max Wave Impulses (Project Settings → FluidNinja Buoyancy → Waves), the impulse pieces sent to the waves in one frame (past it, each body's neighbouring pieces are merged, ")
		TEXT("keeping the frame's whole impulse): a value above 0 can only lower the setting; 0 or less (the default) leaves it. ")
		TEXT("Set by sg.WaterQuality (1024 / 4096 / 16384 / 0 / 0, Low to Cinematic)."),
		ECVF_Scalability);

	// The importance scheduler's counters (stat FluidNinjaBuoyancy), beside the group's own in FluidNinjaBuoyancyStats.h.
	DECLARE_DWORD_COUNTER_STAT(TEXT("Bodies LOD 0"), STAT_FNB_BodiesLod0, STATGROUP_FluidNinjaBuoyancy);
	DECLARE_DWORD_COUNTER_STAT(TEXT("Bodies LOD 1"), STAT_FNB_BodiesLod1, STATGROUP_FluidNinjaBuoyancy);
	DECLARE_DWORD_COUNTER_STAT(TEXT("Bodies LOD 2"), STAT_FNB_BodiesLod2, STATGROUP_FluidNinjaBuoyancy);
	DECLARE_DWORD_COUNTER_STAT(TEXT("Bodies gathered"), STAT_FNB_BodiesGathered, STATGROUP_FluidNinjaBuoyancy);
	DECLARE_DWORD_COUNTER_STAT(TEXT("Bodies waiting (over budget)"), STAT_FNB_BodiesWaiting, STATGROUP_FluidNinjaBuoyancy);
	DECLARE_DWORD_COUNTER_STAT(TEXT("Bodies deferred (cadence)"), STAT_FNB_BodiesDeferred, STATGROUP_FluidNinjaBuoyancy);

	/** fnb.Gather.FastReadback: 1 the finished readbacks are found at the end of every render frame (FFluidNinjaBuoyancyReadbackPoller) and their
	 * patches built before the next frame's physics (TickPrePhysics); 0 the game thread queues a command to look for them, a frame after the
	 * dispatch, and builds the patches in its tick, after the physics. Both paths work at once, each copy on the path it was queued under, so
	 * the switch is safe at any time (for A/B measurement: stat FluidNinjaBuoyancy's Readback age, the CSV's FNB/ReadbackAgeFrames). */
	int32 GatherFastReadback = 1;
	FAutoConsoleVariableRef CVarGatherFastReadback(TEXT("fnb.Gather.FastReadback"), GatherFastReadback,
		TEXT("1 (the default; any value but 0): the water read back from the GPU is taken at the end of the render frame it finishes in and reaches the bodies' physics at the start of the next game frame ")
		TEXT("(about two frames after the gather); 0: a render command queued a frame late looks for it and the patches are built after the physics (about four frames). ")
		TEXT("Safe to switch at any time. For A/B measurement: stat FluidNinjaBuoyancy (Readback age), CSV FNB/ReadbackAgeFrames."));

	/** fnb.Waves.RefreshRestingDraft: 1 a body's resting draft (which caps its static head at 1.2 times it) follows its mass every frame;
	 * 0 keeps the draft from Begin Play, as builds before 2026-10-17 did (for A/B measurement of the waves a mass override makes). */
	int32 WavesRefreshRestingDraft = 1;
	FAutoConsoleVariableRef CVarWavesRefreshRestingDraft(TEXT("fnb.Waves.RefreshRestingDraft"), WavesRefreshRestingDraft,
		TEXT("1 (the default; any value but 0): a body's resting draft, which caps its static head in FluidNinja Waves (at 1.2 times it), follows its mass every frame ")
		TEXT("(a mass override or cargo); 0: the draft from Begin Play stays (the older behaviour, for A/B)."));

	/** Solves the 3x3 system S x = R (Cramer's rule); false when it is singular. */
	bool Solve3(const double S[3][3], const double R[3], double Out[3])
	{
		auto Det = [](const double M[3][3])
		{
			return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
		};
		const double D = Det(S);
		if (FMath::Abs(D) < 1e-6)
		{
			return false;
		}
		for (int32 Column = 0; Column < 3; ++Column)
		{
			double M[3][3];
			for (int32 I = 0; I < 3; ++I)
			{
				for (int32 J = 0; J < 3; ++J)
				{
					M[I][J] = J == Column ? R[I] : S[I][J];
				}
			}
			Out[Column] = Det(M) / D;
		}
		return true;
	}

	/** Afloat, a body never falls asleep on its own: Chaos would put a nearly still body to sleep every step and zero what a light,
	 * steady force (the wind) had given it, so it would never start to drift. FluidNinja Buoyancy's dormancy puts bodies to sleep itself. */
	void SetNeverSleep(FBodyInstance* Body, bool bNever)
	{
		if (Chaos::FSingleParticlePhysicsProxy* Proxy = Body ? Body->GetPhysicsActor() : nullptr)
		{
			const Chaos::ESleepType Wanted = bNever ? Chaos::ESleepType::NeverSleep : Chaos::ESleepType::MaterialSleep;
			if (Proxy->GetGameThreadAPI().SleepType() != Wanted)
			{
				Proxy->GetGameThreadAPI().SetSleepType(Wanted);
			}
		}
	}

	/** The far field: the top of a UE water volume (Physics Volume with Water Volume on) under the body's centre, the one
	 * whose top is nearest it. */
	bool WaterVolumeTop(const UWorld* World, const FBox& Bounds, float& OutTop)
	{
		const FVector Centre = Bounds.GetCenter();
		bool bFound = false;
		double Nearest = TNumericLimits<double>::Max();
		for (auto It = World->GetNonDefaultPhysicsVolumeIterator(); It; ++It)
		{
			const APhysicsVolume* Volume = It->Get();
			if (!Volume || !Volume->bWaterVolume)
			{
				continue;
			}
			const FBox Box = Volume->GetComponentsBoundingBox(true);
			if (Centre.X < Box.Min.X || Centre.X > Box.Max.X || Centre.Y < Box.Min.Y || Centre.Y > Box.Max.Y)
			{
				continue;
			}
			const double Distance = FMath::Abs(Box.Max.Z - Centre.Z);
			if (Distance < Nearest)
			{
				Nearest = Distance;
				OutTop = static_cast<float>(Box.Max.Z);
				bFound = true;
			}
		}
		return bFound;
	}

	double ReadNumber(const UObject* Object, FName Name, double Default)
	{
		if (Object)
		{
			if (const FNumericProperty* Property = FindFProperty<FNumericProperty>(Object->GetClass(), Name))
			{
				const void* Value = Property->ContainerPtrToValuePtr<void>(Object);
				return Property->IsFloatingPoint() ? Property->GetFloatingPointPropertyValue(Value) : static_cast<double>(Property->GetSignedIntPropertyValue(Value));
			}
		}
		return Default;
	}

	/** FluidNinja's simulation system (NinjaLiveCore) on the actor that owns the NinjaLive component. */
	UNiagaraComponent* FindNinjaCore(const UObject* Ninja)
	{
		const AActor* Owner = Cast<AActor>(Ninja);
		if (!Owner && Ninja)
		{
			Owner = Ninja->GetTypedOuter<AActor>();
		}
		if (Owner)
		{
			TInlineComponentArray<UNiagaraComponent*> Systems(Owner);
			for (UNiagaraComponent* System : Systems)
			{
				if (System->GetAsset() && System->GetAsset()->GetName() == TEXT("NinjaLiveCore"))
				{
					return System;
				}
			}
		}
		return nullptr;
	}

	/** Render thread: the copied texels into the arrays (once the GPU is done). */
	void ReadOut(FFluidNinjaBuoyancyReadback& Data)
	{
		const int32 W = Data.HeightSize.X;
		const int32 H = Data.HeightSize.Y;
		Data.HeightTexels.SetNumUninitialized(W * H);
		int32 Pitch = 0;
		if (const float* Texels = static_cast<const float*>(Data.Height.Lock(Pitch)))
		{
			for (int32 Row = 0; Row < H; ++Row)
			{
				FMemory::Memcpy(&Data.HeightTexels[Row * W], Texels + static_cast<int64>(Row) * Pitch, W * sizeof(float));
			}
			Data.Height.Unlock();
		}
		else
		{
			for (float& Value : Data.HeightTexels)
			{
				Value = 0.f;
			}
		}
		if (Data.bFlow)
		{
			const int32 FW = Data.FlowSize.X;
			const int32 FH = Data.FlowSize.Y;
			Data.FlowTexels.SetNumZeroed(FW * FH);
			int32 FlowPitch = 0;
			if (const uint8* Raw = static_cast<const uint8*>(Data.Flow.Lock(FlowPitch)))
			{
				const int32 Bytes = GPixelFormats[Data.FlowFormat].BlockBytes;
				for (int32 Row = 0; Row < FH; ++Row)
				{
					for (int32 Column = 0; Column < FW; ++Column)
					{
						const uint8* Texel = Raw + (static_cast<int64>(Row) * FlowPitch + Column) * Bytes;
						FVector2f Value = FVector2f::ZeroVector;
						switch (Data.FlowFormat)
						{
						case PF_FloatRGBA:
						case PF_G16R16F:
						case PF_G16R16F_FILTER:
						{
							const FFloat16* Half = reinterpret_cast<const FFloat16*>(Texel);
							Value = FVector2f(Half[0].GetFloat(), Half[1].GetFloat());
							break;
						}
						case PF_A32B32G32R32F:
						case PF_G32R32F:
						{
							const float* Full = reinterpret_cast<const float*>(Texel);
							Value = FVector2f(Full[0], Full[1]);
							break;
						}
						default:
							break;
						}
						Data.FlowTexels[Row * FW + Column] = Value;
					}
				}
				Data.Flow.Unlock();
			}
		}
		Data.Stage = 2;
	}

	/** Render thread: every body's gathered water into the array. */
	void ReadOut(FFluidNinjaBuoyancyGatherReadback& Data)
	{
		Data.Samples.SetNumUninitialized(Data.TotalSamples);
		const uint32 Bytes = static_cast<uint32>(Data.TotalSamples) * sizeof(FVector4f);
		if (const void* Raw = Data.Buffer.Lock(Bytes))
		{
			FMemory::Memcpy(Data.Samples.GetData(), Raw, Bytes);
			Data.Buffer.Unlock();
		}
		else
		{
			Data.Samples.Reset();
		}
		Data.Stage = 2;
	}

	/** Render thread: one calibration shot into its texels. */
	void ReadOut(const FFluidNinjaBuoyancyFlowProbe& Probe, FFluidNinjaBuoyancyFlowProbe::FShot& Shot)
	{
		constexpr int32 N = FFluidNinjaBuoyancyFlowProbe::Side;
		Shot.Texels.SetNumZeroed(N * N);
		int32 Pitch = 0;
		if (const uint8* Raw = static_cast<const uint8*>(Shot.Readback.Lock(Pitch)))
		{
			const int32 Bytes = GPixelFormats[Probe.Format].BlockBytes;
			for (int32 Row = 0; Row < N; ++Row)
			{
				for (int32 Column = 0; Column < N; ++Column)
				{
					const uint8* Texel = Raw + (static_cast<int64>(Row) * Pitch + Column) * Bytes;
					FVector4f& Value = Shot.Texels[Row * N + Column];
					if (Probe.Format == PF_FloatRGBA)
					{
						const FFloat16* Half = reinterpret_cast<const FFloat16*>(Texel);
						Value = FVector4f(Half[0].GetFloat(), Half[1].GetFloat(), Half[2].GetFloat(), Half[3].GetFloat());
					}
					else
					{
						const float* Full = reinterpret_cast<const float*>(Texel);
						Value = FVector4f(Full[0], Full[1], Full[2], Full[3]);
					}
				}
			}
			Shot.Readback.Unlock();
		}
		Shot.Stage = 2;
	}

	/** Normalised cross-correlation of the window around CA in A with the one around CB in B (every Stride-th texel). */
	double Correlate(const TArray<float>& A, const TArray<float>& B, int32 N, FIntPoint CA, FIntPoint CB, int32 Half, int32 Stride)
	{
		double SA = 0.0, SB = 0.0, SAA = 0.0, SBB = 0.0, SAB = 0.0;
		int32 Count = 0;
		for (int32 Y = -Half; Y < Half; Y += Stride)
		{
			const float* RowA = &A[(CA.Y + Y) * N + CA.X];
			const float* RowB = &B[(CB.Y + Y) * N + CB.X];
			for (int32 X = -Half; X < Half; X += Stride)
			{
				const double VA = RowA[X];
				const double VB = RowB[X];
				SA += VA;
				SB += VB;
				SAA += VA * VA;
				SBB += VB * VB;
				SAB += VA * VB;
				++Count;
			}
		}
		const double VarA = SAA - SA * SA / Count;
		const double VarB = SBB - SB * SB / Count;
		return VarA > 1e-12 && VarB > 1e-12 ? (SAB - SA * SB / Count) / FMath::Sqrt(VarA * VarB) : -1.0;
	}

	/** Where the window around Centre in A went in B: every other shift and texel first, then every one near the best, then the
	 * sub-texel peak (a parabola through the neighbours). Centre must lie Search + 3 + Half inside the square. */
	bool Track(const TArray<float>& A, const TArray<float>& B, int32 N, FIntPoint Centre, int32 Half, int32 Search, FVector2D& OutShift, double& OutScore)
	{
		FIntPoint Best(0, 0);
		double BestScore = -2.0;
		for (int32 DY = -Search; DY <= Search; DY += 2)
		{
			for (int32 DX = -Search; DX <= Search; DX += 2)
			{
				const double Score = Correlate(A, B, N, Centre, Centre + FIntPoint(DX, DY), Half, 2);
				if (Score > BestScore)
				{
					BestScore = Score;
					Best = FIntPoint(DX, DY);
				}
			}
		}
		const FIntPoint Coarse = Best;
		BestScore = -2.0;
		for (int32 DY = -2; DY <= 2; ++DY)
		{
			for (int32 DX = -2; DX <= 2; ++DX)
			{
				const FIntPoint Shift = Coarse + FIntPoint(DX, DY);
				if (FMath::Abs(Shift.X) > Search + 2 || FMath::Abs(Shift.Y) > Search + 2)
				{
					continue;
				}
				const double Score = Correlate(A, B, N, Centre, Centre + Shift, Half, 1);
				if (Score > BestScore)
				{
					BestScore = Score;
					Best = Shift;
				}
			}
		}
		if (BestScore <= -1.0)
		{
			return false;
		}
		auto At = [&](FIntPoint Shift) { return Correlate(A, B, N, Centre, Centre + Shift, Half, 1); };
		auto Vertex = [](double Minus, double Middle, double Plus)
		{
			const double Curve = Minus - 2.0 * Middle + Plus;
			return Curve < -1e-9 ? FMath::Clamp(0.5 * (Minus - Plus) / Curve, -0.5, 0.5) : 0.0;
		};
		OutShift = FVector2D(Best.X + Vertex(At(Best - FIntPoint(1, 0)), BestScore, At(Best + FIntPoint(1, 0))),
			Best.Y + Vertex(At(Best - FIntPoint(0, 1)), BestScore, At(Best + FIntPoint(0, 1))));
		OutScore = BestScore;
		return true;
	}

	/** The subsystem a console command is for: the command's world's, else (typed into the editor's console) the world in Play's. */
	UFluidNinjaBuoyancySubsystem* FindConsoleSubsystem(UWorld* World)
	{
		UFluidNinjaBuoyancySubsystem* Hydro = World ? World->GetSubsystem<UFluidNinjaBuoyancySubsystem>() : nullptr;
		if (!Hydro && GEngine)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				UWorld* Candidate = Context.World();
				if (Candidate && (Context.WorldType == EWorldType::PIE || Context.WorldType == EWorldType::Game))
				{
					if ((Hydro = Candidate->GetSubsystem<UFluidNinjaBuoyancySubsystem>()) != nullptr)
					{
						break;
					}
				}
			}
		}
		return Hydro;
	}

	FAutoConsoleCommandWithWorldAndArgs CalibrateFlowCommand(TEXT("fnb.Buoyancy.CalibrateFlow"),
		TEXT("In Play: measures how fast FluidNinja's dye moves against its velocity buffer around the player's camera (stand by fast water), or around the first ")
		TEXT("floating body when the camera is over no simulation, and logs the Flow Scale Multiplier (Project Settings → FluidNinja Buoyancy) that matches it. ")
		TEXT("Usage: fnb.Buoyancy.CalibrateFlow [apply]. 'apply' sets it only when the result is trusted (enough matches, the dye moving with the buffer, at least ")
		TEXT("1.5 texels of movement), and saves it to the project's config in the editor."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UFluidNinjaBuoyancySubsystem* Hydro = FindConsoleSubsystem(World);
			if (!Hydro)
			{
				UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("fnb.Buoyancy.CalibrateFlow: run it in Play (no FluidNinja Buoyancy in this world)."));
				return;
			}
			Hydro->CalibrateFlow(Args.ContainsByPredicate([](const FString& Arg) { return Arg.Equals(TEXT("apply"), ESearchCase::IgnoreCase); }));
		}));

	/** fnb.Memory: every GPU allocation FluidNinja Buoyancy holds in the world in Play, one line each and the total (ReportMemory). */
	FAutoConsoleCommandWithWorldAndArgs MemoryCommand(TEXT("fnb.Memory"),
		TEXT("In Play: lists the GPU memory FluidNinja Buoyancy holds (the Surface Mirror, the gather and copy readbacks, the current probes, FluidNinja Waves' ")
		TEXT("source textures, aux targets, self-field atlas and pools), one line each with its size and format, and the total in MB. No arguments."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			const UFluidNinjaBuoyancySubsystem* Hydro = FindConsoleSubsystem(World);
			if (!Hydro)
			{
				UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("fnb.Memory: run it in Play (no FluidNinja Buoyancy in this world)."));
				return;
			}
			TArray<FFluidNinjaBuoyancyMemoryItem> Items;
			const double Total = Hydro->ReportMemory(Items);
			UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy GPU memory (%s tier):"), *FluidNinjaBuoyancyQuality::Describe());
			for (const FFluidNinjaBuoyancyMemoryItem& Item : Items)
			{
				UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("  %-48s %6d x %-6d %-12s %8.2f MB"), *Item.Name, Item.Size.X, Item.Size.Y, *Item.Format, Item.MegaBytes);
			}
			UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("  %-48s %26s %8.2f MB"), TEXT("total"), TEXT(""), Total);
		}));
}

/**
 * fnb.Gather.FastReadback: finds the finished readbacks at the end of every render frame (FCoreDelegates::OnEndFrameRT), where the render
 * thread is anyway, instead of by a command the game thread queues a frame after the dispatch (PollReadbacks), which can only read out at
 * the frame after that. The gather and copy commands add their readbacks here as they queue them; each is read out (ReadOut, Stage 2) at
 * the first end of frame its GPU fence has passed, and the game thread takes it at its next pre-physics tick (TickPrePhysics). The hand-off
 * is the readbacks' own Stage: the render thread writes the samples, then Stage 2; the game thread reads Stage 2, then the samples, and
 * frees the readback with Stage 0 (the protocol the slow path's command has always used with the game thread).
 *
 * Render thread only, but for the shared pointers that keep it alive: the subsystem holds one until Deinitialize, the commands that bind
 * and unbind it hold another while they wait, so when a world goes away with copies in flight the readbacks it still holds are released on
 * the render thread, after the commands already queued (the frame's gather may still add to it).
 */
class FFluidNinjaBuoyancyReadbackPoller
{
public:
	/** Render thread: listens to the end of every render frame (through a weak pointer to itself: an unbind that never came would leave no
	 * call to a freed poller). */
	void Bind(const TSharedRef<FFluidNinjaBuoyancyReadbackPoller, ESPMode::ThreadSafe>& Self)
	{
		Handle = FCoreDelegates::OnEndFrameRT.AddSP(Self, &FFluidNinjaBuoyancyReadbackPoller::Poll);
	}

	/** Render thread: stops listening and lets go of the readbacks still in flight (their staging buffers go with them, here). */
	void Unbind()
	{
		FCoreDelegates::OnEndFrameRT.Remove(Handle);
		Handle.Reset();
		Gathers.Reset();
		Copies.Reset();
	}

	/** Render thread: a readback whose copy was just queued, with the generation that copy is (a newer copy queued into the same readback
	 * since would be someone else's to read). */
	void Add(const TSharedPtr<FFluidNinjaBuoyancyGatherReadback, ESPMode::ThreadSafe>& Data, uint32 Generation)
	{
		Gathers.Emplace(Data, Generation);
	}

	void Add(const TSharedPtr<FFluidNinjaBuoyancyReadback, ESPMode::ThreadSafe>& Data, uint32 Generation)
	{
		Copies.Emplace(Data, Generation);
	}

private:
	/** Render thread, at the end of every frame: what the GPU has finished is read out; what the game thread freed or reused meanwhile is
	 * dropped; the rest waits for the next end of frame. */
	void Poll()
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FluidNinjaBuoyancy_ReadbackPoll);
		for (int32 Index = Gathers.Num() - 1; Index >= 0; --Index)
		{
			FFluidNinjaBuoyancyGatherReadback& Data = *Gathers[Index].Key;
			const bool bOurs = Data.Stage.load() == 1 && Data.Generation.load() == Gathers[Index].Value;
			if (bOurs && !Data.Buffer.IsReady())
			{
				continue;
			}
			if (bOurs)
			{
				ReadOut(Data);
			}
			Gathers.RemoveAtSwap(Index);
		}
		for (int32 Index = Copies.Num() - 1; Index >= 0; --Index)
		{
			FFluidNinjaBuoyancyReadback& Data = *Copies[Index].Key;
			const bool bOurs = Data.Stage.load() == 1 && Data.Generation.load() == Copies[Index].Value;
			if (bOurs && !(Data.Height.IsReady() && (!Data.bFlow || Data.Flow.IsReady())))
			{
				continue;
			}
			if (bOurs)
			{
				ReadOut(Data);
			}
			Copies.RemoveAtSwap(Index);
		}
	}

	TArray<TPair<TSharedPtr<FFluidNinjaBuoyancyGatherReadback, ESPMode::ThreadSafe>, uint32>> Gathers;
	TArray<TPair<TSharedPtr<FFluidNinjaBuoyancyReadback, ESPMode::ThreadSafe>, uint32>> Copies;
	FDelegateHandle Handle;
};

void FFluidNinjaBuoyancyPrePhysicsTickFunction::ExecuteTick(float DeltaTime, ELevelTick TickType, ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
{
	if (Target && TickType != LEVELTICK_ViewportsOnly)
	{
		Target->TickPrePhysics(DeltaTime);
	}
}

FString FFluidNinjaBuoyancyPrePhysicsTickFunction::DiagnosticMessage()
{
	return TEXT("UFluidNinjaBuoyancySubsystem::TickPrePhysics");
}

FName FFluidNinjaBuoyancyPrePhysicsTickFunction::DiagnosticContext(bool bDetailed)
{
	return FName(TEXT("FluidNinjaBuoyancyPrePhysics"));
}

UFluidNinjaBuoyancySubsystem::UFluidNinjaBuoyancySubsystem() = default;
UFluidNinjaBuoyancySubsystem::~UFluidNinjaBuoyancySubsystem() = default;

FFluidNinjaBuoyancyBenchFactory& UFluidNinjaBuoyancySubsystem::BenchFactory()
{
	static FFluidNinjaBuoyancyBenchFactory Factory;
	return Factory;
}

UWorld* UFluidNinjaBuoyancySubsystem::GetBenchWorld() const
{
	return GetWorld();
}

namespace
{
	/** The bench's view of FluidNinja Waves (IFluidNinjaBuoyancyBenchWaves): forwards to the world's UFluidNinjaBuoyancyWaves, whose
	 * header stays private to this module. Owned by the subsystem, which outlives it; handed out only while the waves exist. */
	class FFluidNinjaBuoyancyBenchWavesFacade final : public IFluidNinjaBuoyancyBenchWaves
	{
	public:
		explicit FFluidNinjaBuoyancyBenchWavesFacade(const UFluidNinjaBuoyancySubsystem& InHost) : Host(&InHost) {}

		virtual bool IsActive() const override { return Waves() && Waves()->IsActive(); }
		virtual FString GetStatusLine() const override { return Waves() ? Waves()->GetStatusLine() : FString(); }
		virtual void Poke(const FVector2D& WorldXY, float HeightCm, float SigmaCm) override { if (UFluidNinjaBuoyancyWaves* W = Waves()) { W->Poke(WorldXY, HeightCm, SigmaCm); } }
		virtual void SetProbes(const TArray<FVector2D>& WorldPoints) override { if (UFluidNinjaBuoyancyWaves* W = Waves()) { W->SetProbes(WorldPoints); } }
		virtual bool GetProbeResults(double& OutTime, TArray<float>& OutHeights) const override { return Waves() && Waves()->GetProbeResults(OutTime, OutHeights); }
		virtual void RequestStats(bool bOn) override { if (UFluidNinjaBuoyancyWaves* W = Waves()) { W->RequestStats(bOn); } }
		virtual bool GetStats(double& OutTime, double& OutVolumeCm3, double& OutPotentialJ, double& OutKineticJ, float& OutMaxAbsCm) const override
		{
			return Waves() && Waves()->GetStats(OutTime, OutVolumeCm3, OutPotentialJ, OutKineticJ, OutMaxAbsCm);
		}
		virtual bool GetStats(double& OutTime, double& OutVolumeCm3, double& OutPotentialJ, double& OutKineticJ, float& OutMaxAbsCm, float& OutKineticDepthCm) const override
		{
			return Waves() && Waves()->GetStats(OutTime, OutVolumeCm3, OutPotentialJ, OutKineticJ, OutMaxAbsCm, OutKineticDepthCm);
		}
		virtual bool GetSourceVolume(double& OutTime, double& OutSourceVolumeCm3) const override { return Waves() && Waves()->GetSourceVolume(OutTime, OutSourceVolumeCm3); }
		virtual void RequestClear() override { if (UFluidNinjaBuoyancyWaves* W = Waves()) { W->RequestClear(); } }
		virtual FVector2D GetWindowOrigin() const override { return Waves() ? Waves()->GetWindowOrigin() : FVector2D::ZeroVector; }
		virtual double GetWindowCm() const override { return Waves() ? Waves()->GetWindowCm() : 0.0; }
		virtual double GetCellCm() const override { return Waves() ? Waves()->GetCellCm() : 0.0; }
		virtual double GetStepsPerSecond() const override { return Waves() ? Waves()->GetStepsPerSecond() : 60.0; }
		virtual void SetBenchFocus(const FVector2D& WorldXY) override { if (UFluidNinjaBuoyancyWaves* W = Waves()) { W->SetBenchFocus(WorldXY); } }
		virtual void ClearBenchFocus() override { if (UFluidNinjaBuoyancyWaves* W = Waves()) { W->ClearBenchFocus(); } }
		virtual void GetBenchWindows(TArray<FBox2D>& Out) const override { if (const UFluidNinjaBuoyancyWaves* W = Waves()) { W->GetBenchWindows(Out); } else { Out.Reset(); } }
		virtual bool IsBenchEdgeOpen() const override { return Waves() && Waves()->IsBenchEdgeOpen(); }
		virtual bool ReadBenchAux(TArray<FLinearColor>& OutCells, FIntPoint& OutSize) const override { return Waves() && Waves()->ReadBenchAux(OutCells, OutSize); }

	private:
		UFluidNinjaBuoyancyWaves* Waves() const { return Host.IsValid() ? Host->GetWaves() : nullptr; }
		TWeakObjectPtr<const UFluidNinjaBuoyancySubsystem> Host;
	};
}

IFluidNinjaBuoyancyBenchWaves* UFluidNinjaBuoyancySubsystem::GetBenchWaves()
{
	if (!Waves)
	{
		return nullptr;   // as GetWaves: null before Begin Play, and in a world without the waves object
	}
	if (!BenchWaves.IsValid())
	{
		BenchWaves = MakeUnique<FFluidNinjaBuoyancyBenchWavesFacade>(*this);
	}
	return BenchWaves.Get();
}

bool UFluidNinjaBuoyancySubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}
	// A project's derived subsystem (its customization layer) replaces this one: the base steps aside so one water system runs.
	if (GetClass() == StaticClass())
	{
		TArray<UClass*> Derived;
		GetDerivedClasses(StaticClass(), Derived, true);
		for (const UClass* Class : Derived)
		{
			if (!Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				return false;
			}
		}
	}
	return true;
}

bool UFluidNinjaBuoyancySubsystem::FarFieldTop(const FBox& Bounds, float& OutTop) const
{
	return WaterVolumeTop(GetWorld(), Bounds, OutTop);
}

bool UFluidNinjaBuoyancySubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UFluidNinjaBuoyancySubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFluidNinjaBuoyancySubsystem, STATGROUP_Tickables);
}

void UFluidNinjaBuoyancySubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	// Test values left from an earlier Play would otherwise steer the first boat.
	FluidNinjaBuoyancyCVars::BoatThrottle = 0.f;
	FluidNinjaBuoyancyCVars::BoatRudder = 0.f;
	AppliedConsoleThrottle = AppliedConsoleRudder = 0.f;
	if (FPhysScene* Scene = InWorld.GetPhysicsScene())
	{
		if (Scene->GetSolver())
		{
			Callback = Scene->GetSolver()->CreateAndRegisterSimCallbackObject_External<FFluidNinjaBuoyancyCallback>();
			PreTickHandle = Scene->OnPhysScenePreTick.AddUObject(this, &UFluidNinjaBuoyancySubsystem::PhysicsPreTick);
		}
	}
	NinjaCollection = Cast<UMaterialParameterCollection>(UFluidNinjaBuoyancySettings::Get().NinjaParameterCollection.TryLoad());
	WakeChannel = Cast<UNiagaraDataChannelAsset>(UFluidNinjaBuoyancySettings::Get().WakeDataChannel.TryLoad());
	// fnb.Gather.FastReadback: the readbacks are looked for at the end of every render frame and taken before the physics of the next game
	// frame (TickPrePhysics). Where nothing ever renders (a dedicated server) that end of frame never comes, so the slow path stays.
	if (FApp::CanEverRender())
	{
		ReadbackPoller = MakeShared<FFluidNinjaBuoyancyReadbackPoller, ESPMode::ThreadSafe>();
		ENQUEUE_RENDER_COMMAND(FluidNinjaBuoyancyReadbackPollerBind)([Poller = ReadbackPoller.ToSharedRef()](FRHICommandListImmediate&)
		{
			Poller->Bind(Poller);
		});
	}
	// The pre-physics tick, first in its group: the patches are built before anything else of the frame looks at the water.
	PrePhysicsTick.Target = this;
	PrePhysicsTick.TickGroup = TG_PrePhysics;
	PrePhysicsTick.EndTickGroup = TG_PrePhysics;
	PrePhysicsTick.bCanEverTick = true;
	PrePhysicsTick.bStartWithTickEnabled = true;
	PrePhysicsTick.bHighPriority = true;
	PrePhysicsTick.bAllowTickOnDedicatedServer = true;
	PrePhysicsTick.RegisterTickFunction(InWorld.PersistentLevel);
	// FluidNinja Waves: real water waves from the floating bodies and the swimmer in FluidNinja's own simulation (none, with an error, on a
	// stock NinjaLiveCore).
	if (InWorld.IsGameWorld())
	{
		Waves = NewObject<UFluidNinjaBuoyancyWaves>(this);
		Waves->Initialize(*this);
	}
}

void UFluidNinjaBuoyancySubsystem::Deinitialize()
{
	PrePhysicsTick.UnRegisterTickFunction();
	PrePhysicsTick.Target = nullptr;
	if (ReadbackPoller)
	{
		// On the render thread, behind the commands already queued (this frame's gather may still add to it): it stops listening and lets go
		// of the readbacks still in flight there. The subsystem's own references go with the slots and the ring below.
		ENQUEUE_RENDER_COMMAND(FluidNinjaBuoyancyReadbackPollerUnbind)([Poller = ReadbackPoller](FRHICommandListImmediate&)
		{
			Poller->Unbind();
		});
		ReadbackPoller.Reset();
	}
	if (UWorld* World = GetWorld())
	{
		if (FPhysScene* Scene = World->GetPhysicsScene())
		{
			Scene->OnPhysScenePreTick.Remove(PreTickHandle);
			if (Callback && Scene->GetSolver())
			{
				Scene->GetSolver()->UnregisterAndFreeSimCallbackObject_External(Callback);
			}
		}
	}
	Callback = nullptr;
	if (Capture)
	{
		Capture->Destroy();
		Capture = nullptr;
	}
	if (Waves)
	{
		Waves->Shutdown();   // its GPU state is released on the render thread, every core's User.WaveSimEnabled goes back to false
		Waves = nullptr;
	}
	PendingWaveImpulses.Reset();
	if (Bench)
	{
		Bench->Remove();
		Bench.Reset();
	}
	Slots.Reset();   // a render command still holding a readback releases it there
	GatherRing.Reset();
	FlowProbe.Reset();
	CurrentProbes.Reset();
	SwimmerWakes.Reset();
	Super::Deinitialize();
}

void UFluidNinjaBuoyancySubsystem::Register(UFluidNinjaBuoyancyComponent* Component)
{
	if (!Component || Slots.ContainsByPredicate([Component](const FSlot& Slot) { return Slot.Component == Component; }))
	{
		return;
	}
	FSlot& Slot = Slots.AddDefaulted_GetRef();
	Slot.Component = Component;
	Slot.Id = NextId++;
	for (int32 Index = 0; Index < RingSize; ++Index)
	{
		Slot.Ring.Add(MakeShared<FFluidNinjaBuoyancyReadback, ESPMode::ThreadSafe>());
	}
}

void UFluidNinjaBuoyancySubsystem::Unregister(UFluidNinjaBuoyancyComponent* Component)
{
	Slots.RemoveAll([Component](const FSlot& Slot) { return Slot.Component == Component || !Slot.Component.IsValid(); });
}

UFluidNinjaBuoyancySubsystem::FSlot* UFluidNinjaBuoyancySubsystem::FindSlot(int32 Id)
{
	return Slots.FindByPredicate([Id](const FSlot& Slot) { return Slot.Id == Id; });
}

namespace
{
	/** A live component of a NinjaLive actor: Blueprint reinstancing can leave unregistered TRASH_ / REINST_ copies behind. */
	bool IsLiveComponent(const UActorComponent* Component)
	{
		if (!Component || !Component->IsRegistered())
		{
			return false;
		}
		const FString Name = Component->GetName();
		return !Name.StartsWith(TEXT("TRASH_")) && !Name.StartsWith(TEXT("REINST_"));
	}

	/** A NinjaLive's say on whether its tracking paints velocity relative to the water: a bool or number property named Switch on the
	 * actor or a component, or a float user parameter of its simulation; on above 0.5. */
	bool ReadTrackingSwitch(const AActor& Actor, FName Switch, FString& OutHow)
	{
		TArray<const UObject*> Holders = {&Actor};
		for (const UActorComponent* Component : Actor.GetComponents())
		{
			if (IsLiveComponent(Component))
			{
				Holders.Add(Component);
			}
		}
		for (const UObject* Holder : Holders)
		{
			if (const FBoolProperty* Flag = FindFProperty<FBoolProperty>(Holder->GetClass(), Switch))
			{
				OutHow = FString::Printf(TEXT("%s.%s"), *Holder->GetName(), *Switch.ToString());
				return Flag->GetPropertyValue_InContainer(Holder);
			}
			if (const FNumericProperty* Number = FindFProperty<FNumericProperty>(Holder->GetClass(), Switch))
			{
				OutHow = FString::Printf(TEXT("%s.%s"), *Holder->GetName(), *Switch.ToString());
				const void* Value = Number->ContainerPtrToValuePtr<void>(Holder);
				return (Number->IsFloatingPoint() ? Number->GetFloatingPointPropertyValue(Value) : static_cast<double>(Number->GetSignedIntPropertyValue(Value))) > 0.5;
			}
		}
		const FNiagaraVariable Parameter(FNiagaraTypeDefinition::GetFloatDef(), FName(*(TEXT("User.") + Switch.ToString())));
		for (const UActorComponent* Component : Actor.GetComponents())
		{
			if (const UNiagaraComponent* Simulation = IsLiveComponent(Component) ? Cast<UNiagaraComponent>(Component) : nullptr)
			{
				if (Simulation->GetOverrideParameters().IndexOf(Parameter) != INDEX_NONE)
				{
					OutHow = FString::Printf(TEXT("%s user parameter %s"), *Simulation->GetName(), *Switch.ToString());
					return Simulation->GetOverrideParameters().GetParameterValue<float>(Parameter) > 0.5f;
				}
			}
		}
		OutHow = FString::Printf(TEXT("no %s switch"), *Switch.ToString());
		return false;
	}

	/** The object types a NinjaLive tracks by (FluidNinja's Overlap Filter Inclusive Obj Type): an array property named Filter on the
	 * actor or, failing that, a component. bOutFound is false when the NinjaLive has no such setting. */
	TArray<ECollisionChannel> ReadObjectTypeFilter(const AActor& Actor, FName Filter, bool& bOutFound)
	{
		TArray<const UObject*> Holders = {&Actor};
		for (const UActorComponent* Component : Actor.GetComponents())
		{
			if (IsLiveComponent(Component))
			{
				Holders.Add(Component);
			}
		}
		TArray<ECollisionChannel> Types;
		for (const UObject* Holder : Holders)
		{
			const FArrayProperty* Array = FindFProperty<FArrayProperty>(Holder->GetClass(), Filter);
			if (!Array)
			{
				continue;
			}
			FScriptArrayHelper Elements(Array, Array->ContainerPtrToValuePtr<void>(Holder));
			for (int32 Index = 0; Index < Elements.Num(); ++Index)
			{
				const void* Element = Elements.GetRawPtr(Index);
				int64 Query = -1;
				if (const FNumericProperty* Number = CastField<FNumericProperty>(Array->Inner))
				{
					Query = Number->GetSignedIntPropertyValue(Element);
				}
				else if (const FEnumProperty* Enum = CastField<FEnumProperty>(Array->Inner))
				{
					Query = Enum->GetUnderlyingProperty()->GetSignedIntPropertyValue(Element);
				}
				if (Query >= 0 && Query < ObjectTypeQuery_MAX)
				{
					Types.AddUnique(UEngineTypes::ConvertToCollisionChannel(static_cast<EObjectTypeQuery>(Query)));
				}
			}
			bOutFound = true;
			return Types;
		}
		bOutFound = false;
		return Types;
	}

	FString ObjectTypeName(ECollisionChannel Channel)
	{
		return UCollisionProfile::Get()->ReturnChannelNameFromContainerIndex(Channel).ToString();
	}

	/** A NinjaLive's say on whether tracked bodies paint velocity but no density: FluidNinja's Selectively Kill Brush Density Keep
	 * Velocity, a number or enum property named Switch on the actor or a component, or an int user parameter User.Painter.Switch or
	 * User.Switch of its simulation. 1 (ARRAY: primitives and bones), 4 (ARRAY and CHAOS) and 5 (ALL) leave tracked pivots' density out.
	 * bOutFound is false when the NinjaLive has no such setting at all. */
	bool ReadTrackedDensityOff(const AActor& Actor, FName Switch, FString& OutHow, bool& bOutFound)
	{
		bOutFound = true;
		const auto LeavesDensityOut = [](int64 Choice) { return Choice == 1 || Choice == 4 || Choice == 5; };
		TArray<const UObject*> Holders = {&Actor};
		for (const UActorComponent* Component : Actor.GetComponents())
		{
			if (IsLiveComponent(Component))
			{
				Holders.Add(Component);
			}
		}
		for (const UObject* Holder : Holders)
		{
			int64 Choice = 0;
			bool bFound = false;
			if (const FNumericProperty* Number = FindFProperty<FNumericProperty>(Holder->GetClass(), Switch))
			{
				const void* Value = Number->ContainerPtrToValuePtr<void>(Holder);
				Choice = Number->IsFloatingPoint() ? FMath::RoundToInt64(Number->GetFloatingPointPropertyValue(Value)) : Number->GetSignedIntPropertyValue(Value);
				bFound = true;
			}
			else if (const FEnumProperty* Enum = FindFProperty<FEnumProperty>(Holder->GetClass(), Switch))
			{
				Choice = Enum->GetUnderlyingProperty()->GetSignedIntPropertyValue(Enum->ContainerPtrToValuePtr<void>(Holder));
				bFound = true;
			}
			if (bFound)
			{
				OutHow = FString::Printf(TEXT("%s.%s %lld"), *Holder->GetName(), *Switch.ToString(), Choice);
				return LeavesDensityOut(Choice);
			}
		}
		for (const TCHAR* Prefix : {TEXT("User.Painter."), TEXT("User.")})
		{
			const FNiagaraVariable Parameter(FNiagaraTypeDefinition::GetIntDef(), FName(*(Prefix + Switch.ToString())));
			for (const UActorComponent* Component : Actor.GetComponents())
			{
				if (const UNiagaraComponent* Simulation = IsLiveComponent(Component) ? Cast<UNiagaraComponent>(Component) : nullptr)
				{
					if (Simulation->GetOverrideParameters().IndexOf(Parameter) != INDEX_NONE)
					{
						const int32 Choice = Simulation->GetOverrideParameters().GetParameterValue<int32>(Parameter);
						OutHow = FString::Printf(TEXT("%s user parameter %s %d"), *Simulation->GetName(), *Parameter.GetName().ToString(), Choice);
						return LeavesDensityOut(Choice);
					}
				}
			}
		}
		OutHow = FString::Printf(TEXT("no %s setting"), *Switch.ToString());
		bOutFound = false;
		return false;
	}
}

void UFluidNinjaBuoyancySubsystem::RefreshTracking()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	// Nothing to decide while no body asks for FluidNinja's tracking, is handed over, or generates overlap events itself (the warning
	// below): skip the walk over every actor in the world (a large level has thousands), which would otherwise run every second for nothing.
	const bool bNeeded = Slots.ContainsByPredicate([](const FSlot& Slot)
		{
			const UFluidNinjaBuoyancyComponent* Component = Slot.Component.Get();
			const UPrimitiveComponent* Primitive = Component ? Component->GetSimulatedPrimitive() : nullptr;
			return Primitive && (Component->Interaction != EFluidNinjaBuoyancyInteraction::Wakes || Slot.bHandedOver || Primitive->GetGenerateOverlapEvents());
		});
	if (!bNeeded)
	{
		for (FSlot& Slot : Slots)
		{
			Slot.bNinjaTracked = false;
		}
		TrackingReport = TEXT("not checked: no body asks for FluidNinja's tracking or generates overlap events\n");
		return;
	}
	// The NinjaLive actors: the tag each tracks, whether each paints tracked velocity relative to the water, and whether it leaves
	// tracked bodies' density out (FluidNinja weights each tick's push of its river driver and brushes by the local density, so
	// painted density makes the current push harder wherever floaters have been).
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	TArray<FName> Tags;
	// Per NinjaLive: where it tracks (its interaction volume's bounds), the object types it selects there, and its tag.
	struct FNinjaSelection
	{
		FBox Area = FBox(ForceInit);
		TArray<ECollisionChannel> Types;
		FName Tag;
	};
	TArray<FNinjaSelection> Selections;
	int32 Ninjas = 0;
	bool bAllRelative = true;
	bool bAllDensityOff = true;
	bool bAnyDensityMissing = false;
	bool bAnyDensityWrong = false;
	FString Report;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		bool bHasSetting = false;
		FName Tag = NAME_None;
		TArray<const UObject*> Holders = {*It};
		for (const UActorComponent* Component : It->GetComponents())
		{
			if (IsLiveComponent(Component))
			{
				Holders.Add(Component);
			}
		}
		for (const UObject* Holder : Holders)
		{
			if (const FNameProperty* Property = FindFProperty<FNameProperty>(Holder->GetClass(), Settings.TrackingTagProperty))
			{
				bHasSetting = true;
				const FName Value = Property->GetPropertyValue_InContainer(Holder);
				Tag = Value.IsNone() ? Tag : Value;
			}
		}
		if (!bHasSetting)
		{
			continue;
		}
		++Ninjas;
		FString How;
		const bool bRelative = ReadTrackingSwitch(**It, Settings.RelativeTrackingSwitch, How);
		bAllRelative &= bRelative;
		FString DensityHow;
		bool bDensityFound = false;
		const bool bDensityOff = ReadTrackedDensityOff(**It, Settings.TrackedDensitySwitch, DensityHow, bDensityFound);
		bAllDensityOff &= bDensityOff;
		bAnyDensityMissing |= !bDensityFound;
		bAnyDensityWrong |= bDensityFound && !bDensityOff;
		if (!Tag.IsNone())
		{
			Tags.AddUnique(Tag);
		}
		// Where it tracks, and the object types it selects bodies by there, besides its tag.
		FNinjaSelection& Selection = Selections.AddDefaulted_GetRef();
		Selection.Tag = Tag;
		bool bFilterFound = false;
		Selection.Types = ReadObjectTypeFilter(**It, Settings.ObjectTypeFilterProperty, bFilterFound);
		TArray<FString> TypeNames;
		for (const ECollisionChannel Type : Selection.Types)
		{
			TypeNames.Add(ObjectTypeName(Type));
		}
		for (const UActorComponent* Component : It->GetComponents())
		{
			const UPrimitiveComponent* Volume = IsLiveComponent(Component) && Component->GetFName() == Settings.InteractionVolumeComponent ? Cast<UPrimitiveComponent>(Component) : nullptr;
			if (Volume)
			{
				Selection.Area = Volume->Bounds.GetBox();
				break;
			}
		}
		const FString Types = !bFilterFound ? FString::Printf(TEXT("no '%s' setting"), *Settings.ObjectTypeFilterProperty.ToString())
			: (TypeNames.IsEmpty() ? FString(TEXT("none")) : FString::Join(TypeNames, TEXT(", ")));
		const FString Area = Selection.Area.IsValid ? FString::Printf(TEXT("%.0f x %.0f m"), Selection.Area.GetSize().X / 100.0, Selection.Area.GetSize().Y / 100.0)
			: FString::Printf(TEXT("none (no '%s' component, so it selects nothing)"), *Settings.InteractionVolumeComponent.ToString());
		Report += FString::Printf(TEXT("%s: tracking tag %s; object types %s; area %s; paints tracked %s (%s), %s (%s)\n"), *It->GetName(), Tag.IsNone() ? TEXT("none (object types only)") : *Tag.ToString(),
			*Types, *Area, bRelative ? TEXT("velocity relative to the water") : TEXT("world velocity"), *How, bDensityOff ? TEXT("no density") : TEXT("density too"), *DensityHow);
	}
	const int32 Mode = FluidNinjaBuoyancyCVars::Tracking;
	const bool bSafe = Ninjas > 0 && bAllRelative && bAllDensityOff;
	const bool bHandOver = Ninjas > 0 && (Mode == 2 || (Mode == 1 && bSafe));
	TrackingReport = Ninjas > 0 ? Report : FString(TEXT("no NinjaLive in the level\n"));
	// Every reason bodies are not handed over, and what a forced hand-over paints.
	TArray<FString> Unmet;
	if (Ninjas == 0)
	{
		Unmet.Add(TEXT("there is no NinjaLive"));
	}
	else if (Mode != 1 && Mode != 2)
	{
		Unmet.Add(Mode == 0 ? FString(TEXT("fnb.Buoyancy.Tracking is 0")) : FString::Printf(TEXT("fnb.Buoyancy.Tracking is %d (use 0, 1 or 2)"), Mode));
	}
	else
	{
		if (!bAllRelative)
		{
			Unmet.Add(TEXT("a NinjaLive paints tracked bodies' world velocity (its switch is off or missing)"));
		}
		if (bAnyDensityMissing)
		{
			Unmet.Add(FString::Printf(TEXT("a NinjaLive has no '%s' setting"), *Settings.TrackedDensitySwitch.ToString()));
		}
		if (bAnyDensityWrong)
		{
			Unmet.Add(TEXT("a NinjaLive lets tracked bodies paint density, which makes FluidNinja's current push harder where they go, so floaters would speed it up"));
		}
	}
	const FString Reason = FString::Join(Unmet, TEXT("; "));
	const TCHAR* Forced = !bAllRelative && !bAllDensityOff ? TEXT("(forced by fnb.Buoyancy.Tracking 2: FluidNinja paints its world velocity and density)")
		: (!bAllRelative ? TEXT("(forced by fnb.Buoyancy.Tracking 2: FluidNinja paints its world velocity)")
		: TEXT("(forced by fnb.Buoyancy.Tracking 2: FluidNinja paints its density, which speeds a current up)"));

	for (FSlot& Slot : Slots)
	{
		UFluidNinjaBuoyancyComponent* Component = Slot.Component.Get();
		UPrimitiveComponent* Primitive = Component ? Component->GetSimulatedPrimitive() : nullptr;
		if (!Primitive)
		{
			continue;
		}
		const bool bWants = Component->Interaction != EFluidNinjaBuoyancyInteraction::Wakes;
		// A NinjaLive tracks the body only where its interaction volume overlaps it, and only if it selects it there: by object type, or
		// by the tag the hand-over adds. (Bounds are compared as axis-aligned boxes.)
		const ECollisionChannel ObjectType = Primitive->GetCollisionObjectType();
		const FBox Body = Primitive->Bounds.GetBox();
		bool bInArea = false;
		bool bSelectable = false;
		bool bTracked = false;
		for (const FNinjaSelection& Selection : Selections)
		{
			if (!Selection.Area.IsValid || !Selection.Area.Intersect(Body))
			{
				continue;
			}
			bInArea = true;
			const bool bByType = Selection.Types.Contains(ObjectType);
			bSelectable |= bByType || !Selection.Tag.IsNone();
			bTracked |= bByType || (!Selection.Tag.IsNone() && Primitive->ComponentHasTag(Selection.Tag));
		}
		const bool bGo = bHandOver && bSelectable;
		if (bWants && bGo && !Slot.bHandedOver)
		{
			// FluidNinja's tracking sees primitives that generate overlap events (by object type, or by its tracking tag).
			Slot.bOverlapBefore = Primitive->GetGenerateOverlapEvents();
			Slot.AddedTags.Reset();
			for (const FName& Tag : Tags)
			{
				if (!Primitive->ComponentTags.Contains(Tag))
				{
					Primitive->ComponentTags.Add(Tag);
					Slot.AddedTags.Add(Tag);
				}
			}
			Primitive->SetGenerateOverlapEvents(true);
			Primitive->UpdateOverlaps();
			Slot.bHandedOver = true;
			Slot.TrackingNote.Reset();
			UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy on %s: handed to FluidNinja's own tracking %s, %s."), *GetNameSafe(Component->GetOwner()),
				bSafe ? TEXT("(relative to the water, no density)") : Forced,
				Component->Interaction == EFluidNinjaBuoyancyInteraction::Both ? TEXT("wakes as well") : TEXT("no wakes"));
		}
		else if (Slot.bHandedOver && (!bWants || !bGo))
		{
			for (const FName& Tag : Slot.AddedTags)
			{
				Primitive->ComponentTags.Remove(Tag);
			}
			Slot.AddedTags.Reset();
			Primitive->SetGenerateOverlapEvents(Slot.bOverlapBefore);
			Primitive->UpdateOverlaps();
			Slot.bHandedOver = false;
			UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy on %s: taken back from FluidNinja's tracking."), *GetNameSafe(Component->GetOwner()));
		}
		// FluidNinja's tracking needs overlap events, and a NinjaLive whose area holds the body selecting it by object type or by tag.
		Slot.bNinjaTracked = Primitive->GetGenerateOverlapEvents() && bTracked;
		const FString Note = !Reason.IsEmpty() ? Reason : (bSelectable ? FString() : (bInArea ? FString::Printf(TEXT("unselected %s"), *ObjectTypeName(ObjectType)) : FString(TEXT("outside"))));
		if (Reason.IsEmpty() && bWants && !bInArea && Slot.TrackingNote != Note)
		{
			// Not an error: outside every NinjaLive's area there is no simulation to stir. It is handed over once it enters one.
			Slot.TrackingNote = Note;
			UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy on %s: not handed to FluidNinja's tracking: it is outside every NinjaLive's '%s'."),
				*GetNameSafe(Component->GetOwner()), *Settings.InteractionVolumeComponent.ToString());
		}
		if (bWants && !Slot.bHandedOver && (!Slot.bNinjaTracked || Ninjas == 0) && !Note.IsEmpty() && Slot.TrackingNote != Note)
		{
			// No fallback: this build needs the modified FluidNinja. (A body with overlap events of its own, in a level with a NinjaLive,
			// gets the warning below.)
			Slot.TrackingNote = Note;
			const bool bWakesToo = Component->Interaction == EFluidNinjaBuoyancyInteraction::Both && Component->bWake;
			if (!Reason.IsEmpty())
			{
				UE_LOG(LogFluidNinjaBuoyancy, Error, TEXT("FluidNinja Buoyancy on %s: not tracked by FluidNinja, so %s. Its Interaction includes FluidNinja's tracking, which needs the modified FluidNinja: ")
					TEXT("every NinjaLive turning the Relative Tracking Switch '%s' on, and setting its Tracked Density Switch '%s' to 1, 4 or 5 (no density from tracked bodies). But %s."),
					*GetNameSafe(Component->GetOwner()), bWakesToo ? TEXT("it stirs the water only with its wakes") : TEXT("it does not stir the water"),
					*Settings.RelativeTrackingSwitch.ToString(), *Settings.TrackedDensitySwitch.ToString(), *Reason);
			}
			else
			{
				UE_LOG(LogFluidNinjaBuoyancy, Error, TEXT("FluidNinja Buoyancy on %s: not tracked by FluidNinja, so %s. No NinjaLive selects it where it is: its object type %s is not in the '%s' of the NinjaLives ")
					TEXT("whose '%s' it is in, and none of them tracks by tag ('%s'). Add %s to that NinjaLive's Overlap Filter Inclusive Obj Type, or give it a tracking tag (the plugin adds it to the body)."),
					*GetNameSafe(Component->GetOwner()), bWakesToo ? TEXT("it stirs the water only with its wakes") : TEXT("it does not stir the water"),
					*ObjectTypeName(ObjectType), *Settings.ObjectTypeFilterProperty.ToString(), *Settings.InteractionVolumeComponent.ToString(), *Settings.TrackingTagProperty.ToString(), *ObjectTypeName(ObjectType));
			}
		}
		if (Slot.bNinjaTracked && !Slot.bHandedOver && Ninjas > 0 && !bSafe && !Slot.bTrackingWarned)
		{
			Slot.bTrackingWarned = true;
			UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("FluidNinja Buoyancy on %s: %s has Generate Overlap Events on, so FluidNinja's own tracking may paint its world velocity or its density. ")
				TEXT("Carried by a current, it then speeds that current up. Leave overlap events off and use Wake (see Docs/UserGuide.md, Interacting with FluidNinja)."),
				*GetNameSafe(Component->GetOwner()), *Primitive->GetName());
		}
	}
}

void UFluidNinjaBuoyancySubsystem::GetTracking(const UFluidNinjaBuoyancyComponent* Component, bool& bOutTracked, bool& bOutHandedOver) const
{
	const FSlot* Slot = Slots.FindByPredicate([Component](const FSlot& Candidate) { return Candidate.Component == Component; });
	bOutTracked = Slot && Slot->bNinjaTracked;
	bOutHandedOver = Slot && Slot->bHandedOver;
}

namespace
{
	/** A NinjaLive actor and its live components: where its settings are. */
	TArray<UObject*> NinjaHolders(AActor& Actor)
	{
		TArray<UObject*> Holders = {&Actor};
		for (UActorComponent* Component : Actor.GetComponents())
		{
			if (IsLiveComponent(Component))
			{
				Holders.Add(Component);
			}
		}
		return Holders;
	}

	/** A NinjaLive's simulations: its live Niagara components running NinjaLiveCore. */
	TArray<UNiagaraComponent*> NinjaSimulations(const AActor& Actor)
	{
		TArray<UNiagaraComponent*> Simulations;
		for (UActorComponent* Component : Actor.GetComponents())
		{
			UNiagaraComponent* Simulation = IsLiveComponent(Component) ? Cast<UNiagaraComponent>(Component) : nullptr;
			if (Simulation && Simulation->GetAsset() && Simulation->GetAsset()->GetName() == TEXT("NinjaLiveCore"))
			{
				Simulations.Add(Simulation);
			}
		}
		return Simulations;
	}

	TArray<int64> ReadIntArray(const FArrayProperty& Array, const UObject& Holder)
	{
		TArray<int64> Values;
		FScriptArrayHelper Elements(&Array, Array.ContainerPtrToValuePtr<void>(&Holder));
		for (int32 Index = 0; Index < Elements.Num(); ++Index)
		{
			const void* Element = Elements.GetRawPtr(Index);
			if (const FNumericProperty* Number = CastField<FNumericProperty>(Array.Inner))
			{
				Values.Add(Number->GetSignedIntPropertyValue(Element));
			}
			else if (const FEnumProperty* Enum = CastField<FEnumProperty>(Array.Inner))
			{
				Values.Add(Enum->GetUnderlyingProperty()->GetSignedIntPropertyValue(Element));
			}
		}
		return Values;
	}

	void WriteIntArray(const FArrayProperty& Array, UObject& Holder, const TArray<int64>& Values)
	{
		FScriptArrayHelper Elements(&Array, Array.ContainerPtrToValuePtr<void>(&Holder));
		Elements.Resize(Values.Num());
		for (int32 Index = 0; Index < Values.Num(); ++Index)
		{
			void* Element = Elements.GetRawPtr(Index);
			if (const FNumericProperty* Number = CastField<FNumericProperty>(Array.Inner))
			{
				Number->SetIntPropertyValue(Element, Values[Index]);
			}
			else if (const FEnumProperty* Enum = CastField<FEnumProperty>(Array.Inner))
			{
				Enum->GetUnderlyingProperty()->SetIntPropertyValue(Element, Values[Index]);
			}
		}
	}

	int64 ReadInt(const FProperty& Property, const UObject& Holder)
	{
		const void* Value = Property.ContainerPtrToValuePtr<void>(&Holder);
		if (const FNumericProperty* Number = CastField<FNumericProperty>(&Property))
		{
			return Number->IsFloatingPoint() ? FMath::RoundToInt64(Number->GetFloatingPointPropertyValue(Value)) : Number->GetSignedIntPropertyValue(Value);
		}
		if (const FEnumProperty* Enum = CastField<FEnumProperty>(&Property))
		{
			return Enum->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value);
		}
		return 0;
	}

	void WriteInt(const FProperty& Property, UObject& Holder, int64 NewValue)
	{
		void* Value = Property.ContainerPtrToValuePtr<void>(&Holder);
		if (const FNumericProperty* Number = CastField<FNumericProperty>(&Property))
		{
			if (Number->IsFloatingPoint())
			{
				Number->SetFloatingPointPropertyValue(Value, static_cast<double>(NewValue));
			}
			else
			{
				Number->SetIntPropertyValue(Value, NewValue);
			}
		}
		else if (const FEnumProperty* Enum = CastField<FEnumProperty>(&Property))
		{
			Enum->GetUnderlyingProperty()->SetIntPropertyValue(Value, NewValue);
		}
	}

	bool IsNumeric(const FProperty* Property)
	{
		return CastField<FNumericProperty>(Property) || CastField<FEnumProperty>(Property);
	}
}

const TCHAR* UFluidNinjaBuoyancySubsystem::StirApproachName(EFluidNinjaBuoyancyStirApproach Approach)
{
	switch (Approach)
	{
	case EFluidNinjaBuoyancyStirApproach::TrackingByTag: return TEXT("FluidNinja tracking by tag");
	case EFluidNinjaBuoyancyStirApproach::TrackingByType: return TEXT("FluidNinja tracking by object type");
	case EFluidNinjaBuoyancyStirApproach::WakesAndTracking: return TEXT("wakes and FluidNinja tracking (by tag)");
	default: return TEXT("wakes");
	}
}

void UFluidNinjaBuoyancySubsystem::SetStirApproach(EFluidNinjaBuoyancyStirApproach Approach)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	static const FName StirTag(TEXT("FluidNinjaTracked"));
	const FName PainterDensity(*(TEXT("Painter.") + Settings.TrackedDensitySwitch.ToString()));
	// The first switch remembers what the NinjaLives have (the wakes approach keeps it; tracking changes it from there).
	if (!bStirApproachSet)
	{
		StirNinjas.Reset();
		const FNiagaraVariable Relative(FNiagaraTypeDefinition::GetFloatDef(), FName(*(TEXT("User.") + Settings.RelativeTrackingSwitch.ToString())));
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			FStirNinja Ninja;
			Ninja.Actor = *It;
			for (UObject* Holder : NinjaHolders(**It))
			{
				if (const FNameProperty* Tag = FindFProperty<FNameProperty>(Holder->GetClass(), Settings.TrackingTagProperty))
				{
					Ninja.Tags.Add({Holder, Tag->GetPropertyValue_InContainer(Holder)});
				}
				if (const FArrayProperty* Filter = FindFProperty<FArrayProperty>(Holder->GetClass(), Settings.ObjectTypeFilterProperty))
				{
					Ninja.Filters.Add({Holder, ReadIntArray(*Filter, *Holder)});
				}
				const FProperty* Density = FindFProperty<FProperty>(Holder->GetClass(), Settings.TrackedDensitySwitch);
				if (IsNumeric(Density))
				{
					Ninja.Densities.Add({Holder, ReadInt(*Density, *Holder)});
				}
			}
			if (Ninja.Tags.IsEmpty())
			{
				continue;   // not a NinjaLive
			}
			for (UNiagaraComponent* Simulation : NinjaSimulations(**It))
			{
				const FNiagaraParameterStore& Overrides = Simulation->GetOverrideParameters();
				Ninja.Relatives.Add({Simulation, Overrides.IndexOf(Relative) != INDEX_NONE ? Overrides.GetParameterValue<float>(Relative) : 0.f});
			}
			StirNinjas.Add(MoveTemp(Ninja));
		}
	}
	StirApproach = Approach;
	bStirApproachSet = true;
	const bool bTracking = Approach != EFluidNinjaBuoyancyStirApproach::Wakes;
	const bool bByTag = Approach == EFluidNinjaBuoyancyStirApproach::TrackingByTag || Approach == EFluidNinjaBuoyancyStirApproach::WakesAndTracking;

	for (const FSlot& Slot : Slots)
	{
		UFluidNinjaBuoyancyComponent* Component = Slot.Component.Get();
		if (!Component)
		{
			continue;
		}
		switch (Approach)
		{
		case EFluidNinjaBuoyancyStirApproach::TrackingByTag:
		case EFluidNinjaBuoyancyStirApproach::TrackingByType:
			Component->Interaction = EFluidNinjaBuoyancyInteraction::FluidNinjaTracking;
			break;
		case EFluidNinjaBuoyancyStirApproach::WakesAndTracking:
			Component->Interaction = EFluidNinjaBuoyancyInteraction::Both;
			Component->bWake = true;
			break;
		default:
			Component->Interaction = EFluidNinjaBuoyancyInteraction::Wakes;
			Component->bWake = true;
			break;
		}
	}

	// The NinjaLives: tracking turns the relative switch on, leaves tracked density out (1, unless it is already 1, 4 or 5), and selects
	// bodies by the tag FluidNinjaTracked with PhysicsBody out of the filter, or by object type with it in. Wakes give each its own settings.
	const int64 PhysicsBody = static_cast<int64>(UEngineTypes::ConvertToObjectType(ECC_PhysicsBody));
	for (const FStirNinja& Ninja : StirNinjas)
	{
		for (const TPair<TWeakObjectPtr<UObject>, FName>& Was : Ninja.Tags)
		{
			UObject* Holder = Was.Key.Get();
			if (const FNameProperty* Tag = Holder ? FindFProperty<FNameProperty>(Holder->GetClass(), Settings.TrackingTagProperty) : nullptr)
			{
				Tag->SetPropertyValue_InContainer(Holder, bTracking ? (bByTag ? StirTag : NAME_None) : Was.Value);
			}
		}
		for (const TPair<TWeakObjectPtr<UObject>, TArray<int64>>& Was : Ninja.Filters)
		{
			UObject* Holder = Was.Key.Get();
			const FArrayProperty* Filter = Holder ? FindFProperty<FArrayProperty>(Holder->GetClass(), Settings.ObjectTypeFilterProperty) : nullptr;
			if (!Filter)
			{
				continue;
			}
			TArray<int64> Values = Was.Value;
			if (bTracking)
			{
				Values.Remove(PhysicsBody);
				if (!bByTag)
				{
					Values.Add(PhysicsBody);
				}
			}
			WriteIntArray(*Filter, *Holder, Values);
		}
		int64 Density = 0;
		for (const TPair<TWeakObjectPtr<UObject>, int64>& Was : Ninja.Densities)
		{
			UObject* Holder = Was.Key.Get();
			const FProperty* Property = Holder ? FindFProperty<FProperty>(Holder->GetClass(), Settings.TrackedDensitySwitch) : nullptr;
			if (IsNumeric(Property))
			{
				Density = bTracking && Was.Value != 1 && Was.Value != 4 && Was.Value != 5 ? 1 : Was.Value;
				WriteInt(*Property, *Holder, Density);
			}
		}
		for (const TPair<TWeakObjectPtr<UNiagaraComponent>, float>& Was : Ninja.Relatives)
		{
			if (UNiagaraComponent* Simulation = Was.Key.Get())
			{
				Simulation->SetVariableFloat(Settings.RelativeTrackingSwitch, bTracking ? 1.f : Was.Value);
				// NinjaLive passes its density setting on at its next parameter update; this takes effect now.
				Simulation->SetVariableInt(PainterDensity, static_cast<int32>(Density));
			}
		}
	}
	RefreshTracking();
	StirReportAt = World->GetTimeSeconds() + 3.0;
	UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy test: stir approach %s (%d bodies, %d NinjaLives)."), StirApproachName(Approach), Slots.Num(), StirNinjas.Num());
}

FString UFluidNinjaBuoyancySubsystem::DescribeStirApproach() const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return FString();
	}
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	TArray<FBox> Bodies;
	int32 HandedOver = 0;
	int32 Waking = 0;
	for (const FSlot& Slot : Slots)
	{
		const UFluidNinjaBuoyancyComponent* Component = Slot.Component.Get();
		const UPrimitiveComponent* Primitive = Component ? Component->GetSimulatedPrimitive() : nullptr;
		if (!Primitive)
		{
			continue;
		}
		Bodies.Add(Primitive->Bounds.GetBox().ExpandBy(30.0));
		HandedOver += Slot.bHandedOver ? 1 : 0;
		Waking += Component->bWake && Component->Interaction != EFluidNinjaBuoyancyInteraction::FluidNinjaTracking ? 1 : 0;
	}
	FString Text = FString::Printf(TEXT("Stir     %s: %d of %d bodies on FluidNinja's tracking, %d with wakes\n"),
		bStirApproachSet ? StirApproachName(StirApproach) : TEXT("level settings"), HandedOver, Bodies.Num(), Waking);
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		bool bNinja = false;
		FName Tag = NAME_None;
		for (UObject* Holder : NinjaHolders(**It))
		{
			if (const FNameProperty* Property = FindFProperty<FNameProperty>(Holder->GetClass(), Settings.TrackingTagProperty))
			{
				bNinja = true;
				const FName Value = Property->GetPropertyValue_InContainer(Holder);
				Tag = Value.IsNone() ? Tag : Value;
			}
		}
		if (!bNinja)
		{
			continue;
		}
		bool bFilterFound = false;
		TArray<FString> Types;
		for (const ECollisionChannel Type : ReadObjectTypeFilter(**It, Settings.ObjectTypeFilterProperty, bFilterFound))
		{
			Types.Add(ObjectTypeName(Type));
		}
		FString How;
		const bool bRelative = ReadTrackingSwitch(**It, Settings.RelativeTrackingSwitch, How);
		bool bDensityFound = false;
		const bool bDensityOff = ReadTrackedDensityOff(**It, Settings.TrackedDensitySwitch, How, bDensityFound);
		// What FluidNinja really tracks: its tracked points, and how many of them lie on floating bodies.
		int32 Points = 0;
		int32 OnBodies = 0;
		for (UNiagaraComponent* Simulation : NinjaSimulations(**It))
		{
			for (const FVector& Position : UNiagaraDataInterfaceArrayFunctionLibrary::GetNiagaraArrayPosition(Simulation, TEXT("User.TrackedPositions")))
			{
				++Points;
				OnBodies += Bodies.ContainsByPredicate([&Position](const FBox& Box) { return Box.IsInsideOrOn(Position); }) ? 1 : 0;
			}
		}
		Text += FString::Printf(TEXT("  %s: tag %s | %s | %s, %s | tracks %d points, %d on floating bodies\n"), *It->GetActorNameOrLabel(),
			Tag.IsNone() ? TEXT("none") : *Tag.ToString(), Types.IsEmpty() ? TEXT("no object types") : *FString::Join(Types, TEXT(", ")),
			bRelative ? TEXT("relative") : TEXT("world velocity"), bDensityOff ? TEXT("no density") : TEXT("density"), Points, OnBodies);
	}
	Text.RemoveFromEnd(TEXT("\n"));
	return Text;
}

void UFluidNinjaBuoyancySubsystem::RefreshSources()
{
	// The drawn water surface: the spline pieces and flat meshes of the tagged actors, and a Niagara water sheet. A
	// waterfall's face is left out (a top-down capture keeps the highest surface, so a falling sheet would stand as a wall over its pool).
	UWorld* World = GetWorld();
	SurfacePrimitives.Reset();
	SurfaceLeftOut = 0;
	TArray<AActor*> Actors;
	for (const FName& Tag : UFluidNinjaBuoyancySettings::Get().SurfaceTags)
	{
		TArray<AActor*> Tagged;
		UGameplayStatics::GetAllActorsWithTag(World, Tag, Tagged);
		for (AActor* Actor : Tagged)
		{
			Actors.AddUnique(Actor);
		}
	}
	for (AActor* Actor : Actors)
	{
		const bool bNiagaraSurface = Actor->IsA<ANiagaraActor>();
		TArray<UPrimitiveComponent*> Primitives;
		Actor->GetComponents(Primitives);
		for (UPrimitiveComponent* Primitive : Primitives)
		{
			if (!Primitive->IsVisible() || Primitive->bHiddenInGame)
			{
				continue;
			}
			bool bKeep = false;
			if (const USplineMeshComponent* Piece = Cast<USplineMeshComponent>(Primitive))
			{
				const FTransform& Transform = Piece->GetComponentTransform();
				const FVector Run = Transform.TransformPosition(Piece->GetEndPosition()) - Transform.TransformPosition(Piece->GetStartPosition());
				bKeep = FMath::Abs(Run.Z) <= Run.Size2D();
			}
			else if (Primitive->IsA<UStaticMeshComponent>())
			{
				const FVector Extent = Primitive->Bounds.BoxExtent;
				bKeep = Extent.Z <= FMath::Max(Extent.X, Extent.Y);
			}
			else if (bNiagaraSurface && Primitive->IsA<UNiagaraComponent>())
			{
				bKeep = true;
			}
			if (bKeep)
			{
				SurfacePrimitives.Add(Primitive);
			}
			else
			{
				++SurfaceLeftOut;
			}
		}
	}

	// The simulations' velocity buffers: each texture the drawn water samples (so its current is the one the surface shows), the
	// FluidNinja component that owns it (its speed, sampling rate and resolution give the buffer's units), and where its surfaces lie.
	const FName Parameter = UFluidNinjaBuoyancySettings::Get().VelocityBufferParameter;
	const FName PressureParameter = UFluidNinjaBuoyancySettings::Get().PressureBufferParameter;
	TArray<FFlowSource> Found;
	for (const TWeakObjectPtr<UPrimitiveComponent>& Weak : SurfacePrimitives)
	{
		UPrimitiveComponent* Primitive = Weak.Get();
		UMaterialInterface* Material = Primitive && !Primitive->IsA<UNiagaraComponent>() ? Primitive->GetMaterial(0) : nullptr;
		UTexture* Texture = nullptr;
		UTextureRenderTarget2D* Target = Material && Material->GetTextureParameterValue(FHashedMaterialParameterInfo(Parameter), Texture)
			? Cast<UTextureRenderTarget2D>(Texture) : nullptr;
		if (!Target)
		{
			continue;
		}
		FFlowSource* Source = Found.FindByPredicate([Target](const FFlowSource& Candidate) { return Candidate.Buffer.Get() == Target; });
		if (!Source)
		{
			Source = &Found.AddDefaulted_GetRef();
			Source->Buffer = Target;
			Source->Ninja = Target->GetOuter();
			Source->Material = Material;
			if (const UObject* Ninja = Source->Ninja.Get())
			{
				Source->SimSpeed = static_cast<float>(ReadNumber(Ninja, TEXT("SimSpeed"), 1.0));
				Source->SamplingFPS = static_cast<float>(ReadNumber(Ninja, TEXT("SamplingFPS"), 60.0));
				Source->Resolution = static_cast<int32>(ReadNumber(Ninja, TEXT("ResolutionX"), Target->SizeX));
			}
		}
		// FluidNinja Waves: the same material's pressure buffer (B and the static head), from the first surface that has it.
		UTexture* PressureTexture = nullptr;
		if (!Source->Pressure.IsValid() && Material->GetTextureParameterValue(FHashedMaterialParameterInfo(PressureParameter), PressureTexture))
		{
			Source->Pressure = Cast<UTextureRenderTarget2D>(PressureTexture);
		}
		const FBox Box = Primitive->Bounds.GetBox();
		Source->SurfaceArea += FBox2D(FVector2D(Box.Min), FVector2D(Box.Max));
	}
	FlowSources = MoveTemp(Found);
	UpdateFlowSources();
	// Which simulation each surface shows: its material's velocity buffer, else (a Niagara sheet) the one whose surface lies under it.
	SurfaceSources.Reset(SurfacePrimitives.Num());
	for (const TWeakObjectPtr<UPrimitiveComponent>& Weak : SurfacePrimitives)
	{
		UPrimitiveComponent* Primitive = Weak.Get();
		if (!Primitive)
		{
			continue;
		}
		UMaterialInterface* Material = !Primitive->IsA<UNiagaraComponent>() ? Primitive->GetMaterial(0) : nullptr;
		UTexture* Texture = nullptr;
		const UTextureRenderTarget2D* Target = Material && Material->GetTextureParameterValue(FHashedMaterialParameterInfo(Parameter), Texture)
			? Cast<UTextureRenderTarget2D>(Texture) : nullptr;
		int32 Shown = Target ? FlowSources.IndexOfByPredicate([Target](const FFlowSource& Candidate) { return Candidate.Buffer.Get() == Target; }) : INDEX_NONE;
		if (Shown == INDEX_NONE)
		{
			Shown = FindFlowSource(FVector2D(Primitive->Bounds.Origin));
		}
		SurfaceSources.Emplace(Primitive, Shown);
	}
}

void UFluidNinjaBuoyancySubsystem::UpdateFlowSources()
{
	const float FrameRate = 1.f / FMath::Max(SmoothedDeltaTime, 1e-3f);
	const float Multiplier = UFluidNinjaBuoyancySettings::Get().FlowScaleMultiplier;
	FlowScale = 0.f;
	for (FFlowSource& Source : FlowSources)
	{
		// Where it lies: the surface material's own TraceMeshPos / TraceMeshSize (metres) when the simulation sets them there, else
		// FluidNinja's parameter collection.
		Source.bValid = Source.bFromMaterial = false;
		FLinearColor Position;
		FLinearColor Size;
		FLinearColor Offset = FLinearColor::Transparent;
		const UMaterialInterface* Material = Source.Material.Get();
		if (Material && Material->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("TraceMeshPos")), Position)
			&& Material->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("TraceMeshSize")), Size) && Size.R * 100.0 > 100.0)
		{
			Material->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("TraceMeshPosLWCOffset")), Offset);
			Source.Centre = FVector2D(Position.R + Offset.R, Position.G + Offset.G);
			Source.SizeCm = Size.R * 100.0;
			Source.bValid = Source.bFromMaterial = true;
		}
		else if (bSimValid)
		{
			Source.Centre = SimCentre;
			Source.SizeCm = SimSizeCm;
			Source.bValid = true;
		}
		// How often it steps: a fixed tick delta on NinjaLiveCore makes Niagara substep it at that rate whatever the frame rate (two
		// steps a frame at 30 fps for FluidNinja's 1/60 s); without one it steps once a tick, at SamplingFPS at most the frame rate.
		if (!Source.Core.IsValid())
		{
			Source.Core = FindNinjaCore(Source.Ninja.Get());
		}
		const UNiagaraComponent* Core = Source.Core.Get();
		const UNiagaraSystem* System = Core ? Core->GetAsset() : nullptr;
		Source.bFixedTick = System && System->HasFixedTickDelta() && System->GetFixedTickDeltaTime() > 0.f;
		Source.StepsPerSecond = Source.bFixedTick ? 1.f / System->GetFixedTickDeltaTime() : FMath::Min(Source.SamplingFPS, FrameRate);
		const UTextureRenderTarget2D* Buffer = Source.Buffer.Get();
		const int32 Cells = Source.Resolution > 0 ? Source.Resolution : (Buffer ? Buffer->SizeX : 0);
		Source.FlowScale = Source.bValid && Buffer && Cells > 0
			? Source.SimSpeed * static_cast<float>(Source.SizeCm / Cells) * Source.StepsPerSecond * Multiplier : 0.f;
		if (FlowScale == 0.f)
		{
			FlowScale = Source.FlowScale;
		}
	}
}

int32 UFluidNinjaBuoyancySubsystem::FindFlowSource(const FVector2D& Point) const
{
	int32 Best = INDEX_NONE;
	double BestArea = TNumericLimits<double>::Max();
	for (int32 Index = 0; Index < FlowSources.Num(); ++Index)
	{
		const FFlowSource& Source = FlowSources[Index];
		const double Half = Source.SizeCm * 0.5;
		if (!Source.bValid || !Source.Buffer.IsValid() || FMath::Abs(Point.X - Source.Centre.X) >= Half || FMath::Abs(Point.Y - Source.Centre.Y) >= Half)
		{
			continue;   // its current exists only inside its square
		}
		// Prefer the simulation whose surface lies under the point (the smaller where surfaces overlap).
		const bool bOnSurface = Source.SurfaceArea.bIsValid && Source.SurfaceArea.IsInside(Point);
		const double Area = bOnSurface ? Source.SurfaceArea.GetArea() : 1e30 + Source.SizeCm * Source.SizeCm;
		if (Area < BestArea)
		{
			BestArea = Area;
			Best = Index;
		}
	}
	return Best;
}

const UFluidNinjaBuoyancySubsystem::FFlowSource* UFluidNinjaBuoyancySubsystem::FindFlowSourceFor(const UTextureRenderTarget2D* Buffer) const
{
	return Buffer ? FlowSources.FindByPredicate([Buffer](const FFlowSource& Source) { return Source.Buffer.Get() == Buffer; }) : nullptr;
}

void UFluidNinjaBuoyancySubsystem::Tick(float DeltaTime)
{
	UWorld* World = GetWorld();
	if (!World || !World->HasBegunPlay())
	{
		return;
	}
	SCOPE_CYCLE_COUNTER(STAT_FNB_Tick);
	const double TickStart = FPlatformTime::Seconds();
	ON_SCOPE_EXIT
	{
		// With the pre-physics tick's share (the patches, under fnb.Gather.FastReadback), so the game thread's total compares across the paths.
		FrameTickMs = static_cast<float>((FPlatformTime::Seconds() - TickStart) * 1000.0) + FramePrePhysicsMs;
		FramePrePhysicsMs = 0.f;
		TickMs = FMath::Lerp(TickMs, FrameTickMs, 0.1f);
		PublishFrameStats();
	};
	SmoothedDeltaTime = FMath::Lerp(SmoothedDeltaTime, FMath::Clamp(DeltaTime, 1e-3f, 0.25f), 0.1f);
	Slots.RemoveAll([](const FSlot& Slot) { return !Slot.Component.IsValid(); });
	bCapturedThisFrame = false;
	// The status text (GetStatus, the debug view) is a few hundred characters of Printf a frame: built only when something reads it every
	// frame (the debug view, the test bench's panel or a test run), else once a second, so the steady path does not pay for it.
	const double Now = World->GetTimeSeconds();
	bBuildStatus = UFluidNinjaBuoyancySettings::IsDebugView() || (Bench.IsValid() && Bench->WantsStatusEveryFrame()) || Now >= NextStatusBuild;
	if (bBuildStatus)
	{
		NextStatusBuild = Now + 1.0;
	}

	// Test controls: a change of fnb.Boat.* reaches every propulsion component that takes console control.
	if (FluidNinjaBuoyancyCVars::BoatThrottle != AppliedConsoleThrottle || FluidNinjaBuoyancyCVars::BoatRudder != AppliedConsoleRudder)
	{
		AppliedConsoleThrottle = FluidNinjaBuoyancyCVars::BoatThrottle;
		AppliedConsoleRudder = FluidNinjaBuoyancyCVars::BoatRudder;
		for (FSlot& Slot : Slots)
		{
			UFluidNinjaBuoyancyComponent* Component = Slot.Component.Get();
			if (Component && Component->bPropulsion && Component->bConsoleControl)
			{
				Component->Throttle = FMath::Clamp(AppliedConsoleThrottle, -1.f, 1.f);
				Component->Rudder = FMath::Clamp(AppliedConsoleRudder, -1.f, 1.f);
			}
		}
	}

	UpdateWind(DeltaTime);
	if (!Bench && BenchFactory().IsBound())
	{
		// The FluidNinjaBuoyancyDeveloper module's test bench; in builds without it (shipping), no factory and no bench.
		Bench = BenchFactory().Execute(*this);
	}
	if (Bench)
	{
		Bench->Tick(DeltaTime);
	}
	// The current read back where it was asked for (GetCurrentAtLocation, the swimmers' wakes), before anyone asks again this frame.
	CollectCurrentProbes();
	// The player's swimmer, floating bodies or not.
	UpdateSwimmer(DeltaTime);
	// Swimmers' body parts (the player's while it swims or jumps in, and registered swimmers): splashes and rings where they go in.
	TickSwimmerParts(DeltaTime);
	// Swimmers' wakes: their speed through the water written into FluidNinja's velocity field.
	WriteSwimmerWakes(DeltaTime);

	if (Slots.IsEmpty() || !UFluidNinjaBuoyancySettings::Get().IsEnabled())
	{
		if (bBuildStatus)
		{
			Status = TEXT("FluidNinja Buoyancy: idle");
		}
		// The waves run without floating bodies too (the swimmer, the test bench's pokes).
		TickWaves(DeltaTime, true);
		return;
	}
	if (Now >= NextSourceRefresh)
	{
		NextSourceRefresh = Now + SourceRefreshSeconds;
		RefreshSources();
	}
	if (Now >= NextTrackingRefresh)
	{
		NextTrackingRefresh = Now + 1.0;
		RefreshTracking();
	}
	// The water read back from the GPU. Under fnb.Gather.FastReadback it was found at the end of the render frame and became the patches
	// before this frame's physics (TickPrePhysics); here only the slow path's look for its own copies, and its patches after the physics.
	PollReadbacks();
	if (GatherFastReadback == 0)
	{
		CollectReadbacks();
	}
	// Before the gather: where Niagara ticks after FluidNinja Buoyancy, the bodies' own fields take the ticks that consumed last frame's write, so the
	// gather reads them and the simulations' B for the same instant. The waves' frame is pinned to this tick's place among the tick groups
	// (UFluidNinjaBuoyancyWaves::ReadTicks), so the capture and the gather stay here after it.
	if (Waves)
	{
		Waves->BeginFrame();
	}
	CaptureAndCopy();
	// Right after the Surface Mirror is drawn (and the gather has read last frame's waves): the waves raster the hulls against it.
	TickWaves(DeltaTime, false);
	UpdateFlowProbe();

	// Bodies held at the start: set them on their water once it is read (a water volume's top after a second, if that is all
	// there is), or let them go after a few seconds without any.
	for (FSlot& Slot : Slots)
	{
		UFluidNinjaBuoyancyComponent* Component = Slot.Component.Get();
		UPrimitiveComponent* Primitive = Component && Component->IsAwaitingWater() ? Component->GetSimulatedPrimitive() : nullptr;
		if (!Primitive)
		{
			continue;
		}
		// At least a second: FluidNinja's surface starts up with jumps. Then the mean water over the body's footprint.
		const double Waited = Now - Component->GetAwaitingSince();
		if (bControlledWater)
		{
			Component->SettleOnWater(ControlledHeight);
			continue;
		}
		if (Waited < 1.0)
		{
			continue;
		}
		double Sum = 0.0;
		int32 Count = 0;
		if (const FluidNinjaBuoyancy::FWaterPatch* Patch = Slot.bInCapture ? Slot.Patch.Get() : nullptr)
		{
			const FBox Box = Primitive->Bounds.GetBox();
			for (double X = Box.Min.X; X <= Box.Max.X; X += 25.0)
			{
				for (double Y = Box.Min.Y; Y <= Box.Max.Y; Y += 25.0)
				{
					float Height = 0.f;
					float Rate = 0.f;
					FVector2f Flow;
					if (Patch->Sample(X, Y, Height, Rate, Flow))
					{
						Sum += Height;
						++Count;
					}
				}
			}
		}
		if (Count > 0)
		{
			Component->SettleOnWater(static_cast<float>(Sum / Count));
		}
		else if (Slot.bFlat && (!Slot.bInCapture || Waited > 2.0))
		{
			Component->SettleOnWater(Slot.FlatHeight);
		}
		else if (Waited > 4.0)
		{
			Component->ReleaseHold();
		}
	}
	if (UFluidNinjaBuoyancySettings::IsDebugView())
	{
		DrawDebug();
	}
}

void UFluidNinjaBuoyancySubsystem::TickPrePhysics(float DeltaSeconds)
{
	// fnb.Gather.FastReadback: the readbacks the render thread read out by the end of its last frame become the bodies' patches now, before
	// this frame's physics pre-tick (TG_StartPhysics) hands the physics its water. The tickable tick, where the slow path builds them, runs
	// after the physics, which is why that path's water reaches the bodies a frame later. Gated as the tick's own consumption is.
	UWorld* World = GetWorld();
	if (GatherFastReadback == 0 || !World || !World->HasBegunPlay() || Slots.IsEmpty() || !UFluidNinjaBuoyancySettings::Get().IsEnabled())
	{
		return;
	}
	const double Start = FPlatformTime::Seconds();
	CollectReadbacks();
	FramePrePhysicsMs = static_cast<float>((FPlatformTime::Seconds() - Start) * 1000.0);
}

void UFluidNinjaBuoyancySubsystem::PollReadbacks()
{
	// Render thread: lock and copy out what the GPU has finished, of the copies queued under fnb.Gather.FastReadback 0 (the others are read
	// out at the end of the render frame, FFluidNinjaBuoyancyReadbackPoller, and a second reader would only race it for the same readback).
	// Each with the copy it is for: by the time the render thread gets to it, that copy may have been read by an earlier request and
	// the readback reused for a newer (larger) copy, which this request must leave alone.
	TArray<TPair<TSharedPtr<FFluidNinjaBuoyancyReadback, ESPMode::ThreadSafe>, uint32>> Pending;
	for (FSlot& Slot : Slots)
	{
		for (const TSharedPtr<FFluidNinjaBuoyancyReadback, ESPMode::ThreadSafe>& Data : Slot.Ring)
		{
			if (Data->Stage.load() == 1 && !Data->bFastPoll)
			{
				Pending.Emplace(Data, Data->Generation.load());
			}
		}
	}
	TArray<TPair<TSharedPtr<FFluidNinjaBuoyancyGatherReadback, ESPMode::ThreadSafe>, uint32>> PendingGathers;
	for (const TSharedPtr<FFluidNinjaBuoyancyGatherReadback, ESPMode::ThreadSafe>& Data : GatherRing)
	{
		if (Data->Stage.load() == 1 && !Data->bFastPoll)
		{
			PendingGathers.Emplace(Data, Data->Generation.load());
		}
	}
	if (!Pending.IsEmpty() || !PendingGathers.IsEmpty())
	{
		ENQUEUE_RENDER_COMMAND(FluidNinjaBuoyancyRead)([Pending, PendingGathers](FRHICommandListImmediate&)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FluidNinjaBuoyancy_ReadbackLock);
			for (const TPair<TSharedPtr<FFluidNinjaBuoyancyReadback, ESPMode::ThreadSafe>, uint32>& Entry : Pending)
			{
				FFluidNinjaBuoyancyReadback& Data = *Entry.Key;
				if (Data.Stage.load() == 1 && Data.Generation.load() == Entry.Value && Data.Height.IsReady() && (!Data.bFlow || Data.Flow.IsReady()))
				{
					ReadOut(Data);
				}
			}
			for (const TPair<TSharedPtr<FFluidNinjaBuoyancyGatherReadback, ESPMode::ThreadSafe>, uint32>& Entry : PendingGathers)
			{
				FFluidNinjaBuoyancyGatherReadback& Data = *Entry.Key;
				if (Data.Stage.load() == 1 && Data.Generation.load() == Entry.Value && Data.Buffer.IsReady())
				{
					ReadOut(Data);
				}
			}
		});
	}
}

void UFluidNinjaBuoyancySubsystem::CollectReadbacks()
{
	SCOPE_CYCLE_COUNTER(STAT_FNB_Patches);
	const double PatchStart = FPlatformTime::Seconds();
	ON_SCOPE_EXIT
	{
		FramePatchMs = static_cast<float>((FPlatformTime::Seconds() - PatchStart) * 1000.0);
		PatchMs = FMath::Lerp(PatchMs, FramePatchMs, 0.1f);
	};
	// Game thread: finished gathers, oldest first (bodies may take turns), each body's water from them.
	TArray<FFluidNinjaBuoyancyGatherReadback*, TInlineAllocator<4>> Gathered;
	for (const TSharedPtr<FFluidNinjaBuoyancyGatherReadback, ESPMode::ThreadSafe>& Data : GatherRing)
	{
		if (Data->Stage.load() == 2)
		{
			Gathered.Add(Data.Get());
		}
	}
	Gathered.Sort([](const FFluidNinjaBuoyancyGatherReadback& A, const FFluidNinjaBuoyancyGatherReadback& B) { return A.Time < B.Time; });
	for (FFluidNinjaBuoyancyGatherReadback* Data : Gathered)
	{
		BuildGatherPatches(*Data);
		Data->Stage = 0;
	}
	// The newest finished copy of each body becomes its water.
	for (FSlot& Slot : Slots)
	{
		FFluidNinjaBuoyancyReadback* Newest = nullptr;
		for (const TSharedPtr<FFluidNinjaBuoyancyReadback, ESPMode::ThreadSafe>& Data : Slot.Ring)
		{
			if (Data->Stage.load() == 2)
			{
				if (!Newest || Data->Time > Newest->Time)
				{
					if (Newest)
					{
						Newest->Stage = 0;
					}
					Newest = Data.Get();
				}
				else
				{
					Data->Stage = 0;
				}
			}
		}
		if (Newest)
		{
			BuildPatch(Slot, *Newest);
			Newest->Stage = 0;
		}
	}
}

void UFluidNinjaBuoyancySubsystem::BuildPatch(FSlot& Slot, FFluidNinjaBuoyancyReadback& Data)
{
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	const int32 W = Data.HeightSize.X;
	const int32 H = Data.HeightSize.Y;
	if (W < 1 || H < 1 || Data.HeightTexels.Num() != W * H)
	{
		return;
	}
	TSharedRef<FluidNinjaBuoyancy::FWaterPatch, ESPMode::ThreadSafe> Patch = MakeShared<FluidNinjaBuoyancy::FWaterPatch, ESPMode::ThreadSafe>();
	// Capture texel (column c, row r) sits at x = origin.x + (R/2 - r - 0.5) texel, y = origin.y + (c + 0.5 - R/2) texel (looking down,
	// texture right = +Y, texture up = +X). The patch grid runs i along +X (rows reversed), j along +Y (columns).
	const double Texel = Data.CaptureTexel;
	const double HalfRes = Data.CaptureResolution * 0.5;
	const int32 LastRow = Data.HeightMin.Y + H - 1;
	Patch->NX = H;
	Patch->NY = W;
	Patch->Texel = Texel;
	Patch->Time = Data.Time;
	Patch->Origin = FVector2D(Data.CaptureOrigin.X + (HalfRes - LastRow - 0.5) * Texel, Data.CaptureOrigin.Y + (Data.HeightMin.X + 0.5 - HalfRes) * Texel);
	Patch->Height.SetNumUninitialized(W * H);
	Patch->Rate.SetNumZeroed(W * H);
	constexpr float NaN = std::numeric_limits<float>::quiet_NaN();
	const float Far = static_cast<float>(Data.CaptureRange) - 1.f;
	for (int32 Row = 0; Row < H; ++Row)
	{
		for (int32 Column = 0; Column < W; ++Column)
		{
			const float Depth = Data.HeightTexels[Row * W + Column];
			const int32 I = H - 1 - Row;
			const int32 J = Column;
			Patch->Height[J * H + I] = Depth > 0.f && Depth < Far ? static_cast<float>(Data.CaptureTop - Depth) : NaN;
		}
	}

	// The current, resampled from the simulation's velocity buffer onto the patch grid (u = +X, v = +Y over the simulation square).
	if (Data.bFlow && Data.FlowTexels.Num() == Data.FlowSize.X * Data.FlowSize.Y && Data.SimSizeCm > 0.0)
	{
		Patch->bHasFlow = true;
		Patch->Flow.SetNumZeroed(W * H);
		const double Res = Data.FlowResolution;
		const float MaxSpeed = Settings.MaxFlowSpeedCmS;
		for (int32 I = 0; I < H; ++I)
		{
			for (int32 J = 0; J < W; ++J)
			{
				const double X = Patch->Origin.X + I * Texel;
				const double Y = Patch->Origin.Y + J * Texel;
				const double Col = ((X - Data.SimCentre.X) / Data.SimSizeCm + 0.5) * Res - 0.5 - Data.FlowMin.X;
				const double RowF = ((Y - Data.SimCentre.Y) / Data.SimSizeCm + 0.5) * Res - 0.5 - Data.FlowMin.Y;
				if (Col < -0.5 || RowF < -0.5 || Col > Data.FlowSize.X - 0.5 || RowF > Data.FlowSize.Y - 0.5)
				{
					continue;
				}
				const double CC = FMath::Clamp(Col, 0.0, Data.FlowSize.X - 1.0);
				const double RR = FMath::Clamp(RowF, 0.0, Data.FlowSize.Y - 1.0);
				const int32 C0 = FMath::FloorToInt32(CC);
				const int32 R0 = FMath::FloorToInt32(RR);
				const int32 C1 = FMath::Min(C0 + 1, Data.FlowSize.X - 1);
				const int32 R1 = FMath::Min(R0 + 1, Data.FlowSize.Y - 1);
				const float A = static_cast<float>(CC - C0);
				const float B = static_cast<float>(RR - R0);
				const int32 FW = Data.FlowSize.X;
				const FVector2f Value = Data.FlowTexels[R0 * FW + C0] * ((1.f - A) * (1.f - B)) + Data.FlowTexels[R0 * FW + C1] * (A * (1.f - B))
					+ Data.FlowTexels[R1 * FW + C0] * ((1.f - A) * B) + Data.FlowTexels[R1 * FW + C1] * (A * B);
				FVector2f Flow = Value * Data.FlowScale;
				if (Flow.SizeSquared() > MaxSpeed * MaxSpeed)
				{
					Flow = Flow.GetSafeNormal() * MaxSpeed;
				}
				Patch->Flow[J * H + I] = Flow;
			}
		}
	}

	if (bVerifyGather && IsGathering())
	{
		CompareWithGather(Slot, *Patch);
		return;
	}
	LastReadbackFrame = FMath::Max(LastReadbackFrame, Data.Frame);
	FinishPatch(Slot, Patch);
	++PatchesBuilt;
}

void UFluidNinjaBuoyancySubsystem::CompareWithGather(FSlot& Slot, const FluidNinjaBuoyancy::FWaterPatch& Copied)
{
	const FluidNinjaBuoyancy::FWaterPatch* Gathered = Slot.Patch.Get();
	if (!Gathered || !FMath::IsNearlyEqual(Gathered->Time, Copied.Time, 1e-6))
	{
		return;   // not the same frame
	}
	for (int32 I = 0; I < Gathered->NX; ++I)
	{
		for (int32 J = 0; J < Gathered->NY; ++J)
		{
			const int32 K = J * Gathered->NX + I;
			const double X = Gathered->Origin.X + I * Gathered->Texel;
			const double Y = Gathered->Origin.Y + J * Gathered->Texel;
			float Height = 0.f;
			float Rate = 0.f;
			FVector2f Flow;
			if (FMath::IsNaN(Gathered->Height[K]) || !Copied.Contains(X, Y) || !Copied.Sample(X, Y, Height, Rate, Flow))
			{
				continue;
			}
			const float Difference = FMath::Abs(Height - Gathered->Height[K]);
			Slot.VerifySum += Difference;
			Slot.VerifyMax = FMath::Max(Slot.VerifyMax, Difference);
			Slot.VerifyFar += Difference > 2.f;
			if (Copied.bHasFlow && Gathered->bHasFlow)
			{
				if (Flow.IsNearlyZero() && !Gathered->Flow[K].IsNearlyZero(1.f))
				{
					++Slot.VerifyFlowOnlyGathered;
				}
				else
				{
					Slot.VerifyFlowSum += (Gathered->Flow[K] - Flow).Size();
				}
			}
			++Slot.VerifyCount;
		}
	}
}

void UFluidNinjaBuoyancySubsystem::SetVerifyGather(bool bOn)
{
	bVerifyGather = bOn;
	for (FSlot& Slot : Slots)
	{
		Slot.VerifyCount = Slot.VerifyFar = Slot.VerifyFlowOnlyGathered = 0;
		Slot.VerifySum = Slot.VerifyFlowSum = 0.0;
		Slot.VerifyMax = 0.f;
	}
}

void UFluidNinjaBuoyancySubsystem::BuildGatherPatches(const FFluidNinjaBuoyancyGatherReadback& Data)
{
	const float MaxSpeed = UFluidNinjaBuoyancySettings::Get().MaxFlowSpeedCmS;
	// Each body's slot, found first: the slot list is shared, and a body without one (unregistered since the dispatch), with a grid
	// too small or beyond the samples read back, is left out.
	TArray<FSlot*, TInlineAllocator<32>> Targets;
	Targets.SetNumZeroed(Data.Bodies.Num());
	int32 Built = 0;
	for (int32 Index = 0; Index < Data.Bodies.Num(); ++Index)
	{
		const FFluidNinjaBuoyancyGatherReadback::FBody& Body = Data.Bodies[Index];
		FSlot* Slot = FindSlot(Body.Id);
		if (Slot && Body.Grid >= 2 && Body.First + Body.Grid * Body.Grid <= Data.Samples.Num())
		{
			Targets[Index] = Slot;
			++Built;
		}
	}
	if (Built == 0)
	{
		return;
	}
	LastReadbackFrame = FMath::Max(LastReadbackFrame, Data.Frame);
	// The bodies in parallel: each one's copy out of the readback (which nothing writes until it is freed, after this), its rate against
	// its own previous patch and its ambient fit (FinishPatch) read the readback and its own slot and write only its own slot, so no two
	// bodies touch the same memory. The one shared count, the patches built, is added once after.
	ParallelFor(Data.Bodies.Num(), [this, &Data, &Targets, MaxSpeed](int32 Index)
	{
		FSlot* Slot = Targets[Index];
		if (!Slot)
		{
			return;
		}
		const FFluidNinjaBuoyancyGatherReadback::FBody& Body = Data.Bodies[Index];
		const int32 Count = Body.Grid * Body.Grid;
		constexpr float NaN = std::numeric_limits<float>::quiet_NaN();
		TSharedRef<FluidNinjaBuoyancy::FWaterPatch, ESPMode::ThreadSafe> Patch = MakeShared<FluidNinjaBuoyancy::FWaterPatch, ESPMode::ThreadSafe>();
		Patch->NX = Body.Grid;
		Patch->NY = Body.Grid;
		Patch->Texel = Body.Spacing;
		Patch->Origin = Body.Origin;
		Patch->Time = Data.Time;
		Patch->Height.SetNumUninitialized(Count);
		Patch->Rate.SetNumZeroed(Count);
		Patch->bHasFlow = Body.bFlow;
		if (Body.bFlow)
		{
			Patch->Flow.SetNumUninitialized(Count);
		}
		Patch->bHasWave = Body.bWave;
		if (Body.bWave)
		{
			Patch->Wave.SetNumUninitialized(Count);
		}
		// Sample (i, j) is at first + j * grid + i, as the patch's own index.
		const FVector4f* Samples = Data.Samples.GetData() + Body.First;
		for (int32 K = 0; K < Count; ++K)
		{
			const FVector4f& Sample = Samples[K];
			Patch->Height[K] = Sample.X > -1e29f ? Sample.X : NaN;
			if (Body.bWave)
			{
				Patch->Wave[K] = FMath::IsFinite(Sample.W) ? Sample.W : 0.f;
			}
			if (Body.bFlow)
			{
				FVector2f Flow = FVector2f(Sample.Y, Sample.Z) * Data.FlowScale;
				if (Flow.SizeSquared() > MaxSpeed * MaxSpeed)
				{
					Flow = Flow.GetSafeNormal() * MaxSpeed;
				}
				Patch->Flow[K] = Flow;
			}
		}
		FinishPatch(*Slot, Patch);
	});
	PatchesBuilt += Built;
}

void UFluidNinjaBuoyancySubsystem::FinishPatch(FSlot& Slot, const TSharedRef<FluidNinjaBuoyancy::FWaterPatch, ESPMode::ThreadSafe>& Patch)
{
	// Runs for several bodies at once (BuildGatherPatches): reads the patch, the body's own slot and its primitive's bounds, writes only
	// the slot. Nothing shared may be written here.
	const int32 H = Patch->NX;   // i along +X
	const int32 W = Patch->NY;   // j along +Y
	const double Texel = Patch->Texel;

	// How fast the surface rises or falls: against the previous patch at the same points, smoothed (FluidNinja may not step every
	// frame, so single differences come in bursts). The same grid shifted by whole points is read directly.
	if (const FluidNinjaBuoyancy::FWaterPatch* Previous = Slot.Patch.Get())
	{
		const double Elapsed = Patch->Time - Previous->Time;
		const double ShiftI = (Patch->Origin.X - Previous->Origin.X) / Texel;
		const double ShiftJ = (Patch->Origin.Y - Previous->Origin.Y) / Texel;
		const bool bSameGrid = FMath::IsNearlyEqual(Previous->Texel, Texel, Texel * 1e-4) && FMath::Abs(ShiftI - FMath::RoundToDouble(ShiftI)) < 1e-3
			&& FMath::Abs(ShiftJ - FMath::RoundToDouble(ShiftJ)) < 1e-3;
		const int32 DI = FMath::RoundToInt32(ShiftI);
		const int32 DJ = FMath::RoundToInt32(ShiftJ);
		if (Elapsed > 1e-4)
		{
			for (int32 I = 0; I < H; ++I)
			{
				for (int32 J = 0; J < W; ++J)
				{
					const int32 K = J * H + I;
					if (FMath::IsNaN(Patch->Height[K]))
					{
						continue;
					}
					float OldHeight = 0.f;
					float OldRate = 0.f;
					bool bOld = false;
					if (bSameGrid)
					{
						const int32 PrevI = I + DI;
						const int32 PrevJ = J + DJ;
						if (PrevI >= 0 && PrevJ >= 0 && PrevI < Previous->NX && PrevJ < Previous->NY)
						{
							const int32 PrevK = PrevJ * Previous->NX + PrevI;
							OldHeight = Previous->Height[PrevK];
							OldRate = Previous->Rate[PrevK];
							bOld = !FMath::IsNaN(OldHeight);
						}
					}
					else
					{
						FVector2f OldFlow;
						bOld = Previous->Sample(Patch->Origin.X + I * Texel, Patch->Origin.Y + J * Texel, OldHeight, OldRate, OldFlow);
					}
					if (bOld)
					{
						const float Raw = FMath::Clamp(static_cast<float>((Patch->Height[K] - OldHeight) / Elapsed), -60.f, 60.f);
						Patch->Rate[K] = FMath::Lerp(OldRate, Raw, 0.3f);
					}
				}
			}
		}
	}

	// The highest the water can be anywhere on this patch, with the most the rate's carry-forward and the waves can add: a hull whose
	// lowest point is above it skips its vertex sampling (FluidNinjaBuoyancy::Integrate).
	{
		const float MaxExtrapolation = UFluidNinjaBuoyancySettings::Get().MaxExtrapolationSeconds;
		float Ceiling = -TNumericLimits<float>::Max();
		bool bAny = false;
		for (int32 K = 0; K < H * W; ++K)
		{
			const float Height = Patch->Height[K];
			if (FMath::IsNaN(Height))
			{
				continue;
			}
			float Top = Height + FMath::Max(Patch->Rate[K], 0.f) * MaxExtrapolation;
			if (Patch->bHasWave)
			{
				Top += FMath::Abs(Patch->Wave[K]);
			}
			Ceiling = FMath::Max(Ceiling, Top);
			bAny = true;
		}
		Patch->HeightCeiling = bAny ? Ceiling : TNumericLimits<float>::Max();
	}

	// The current around the body: a ring of water outside its own reach, fitted with a mean and a linear gradient. A body that
	// makes its own wake is carried and dragged by this, not by the water it has just pushed (which would feed back on itself).
	if (const UFluidNinjaBuoyancyComponent* Component = Slot.Component.Get(); Component && Patch->bHasFlow)
	{
		if (const UPrimitiveComponent* Primitive = Component->GetSimulatedPrimitive())
		{
			const FVector2D Centre(Primitive->Bounds.Origin);
			const double Inner = Primitive->Bounds.SphereRadius * 1.05 + 50.0;
			double S[3][3] = {};
			double RX[3] = {};
			double RY[3] = {};
			FVector2D Mean = FVector2D::ZeroVector;
			int32 Count = 0;
			for (int32 I = 0; I < H; ++I)
			{
				for (int32 J = 0; J < W; ++J)
				{
					const int32 K = J * H + I;
					if (FMath::IsNaN(Patch->Height[K]))
					{
						continue;
					}
					const FVector2D Offset = Patch->Origin + FVector2D(I * Texel, J * Texel) - Centre;
					if (Offset.Size() < Inner)
					{
						continue;
					}
					const double Row[3] = {1.0, Offset.X, Offset.Y};
					for (int32 A = 0; A < 3; ++A)
					{
						for (int32 B = 0; B < 3; ++B)
						{
							S[A][B] += Row[A] * Row[B];
						}
						RX[A] += Row[A] * Patch->Flow[K].X;
						RY[A] += Row[A] * Patch->Flow[K].Y;
					}
					Mean += FVector2D(Patch->Flow[K].X, Patch->Flow[K].Y);
					++Count;
				}
			}
			if (Count >= 12)
			{
				double FitX[3];
				double FitY[3];
				Patch->bAmbient = true;
				Patch->AmbientCentre = Centre;
				if (Solve3(S, RX, FitX) && Solve3(S, RY, FitY))
				{
					constexpr double MaxGradient = 1.5;   // 1/s
					Patch->AmbientFlow = FVector2f(static_cast<float>(FitX[0]), static_cast<float>(FitY[0]));
					Patch->AmbientGradient = FVector4f(static_cast<float>(FMath::Clamp(FitX[1], -MaxGradient, MaxGradient)), static_cast<float>(FMath::Clamp(FitX[2], -MaxGradient, MaxGradient)),
						static_cast<float>(FMath::Clamp(FitY[1], -MaxGradient, MaxGradient)), static_cast<float>(FMath::Clamp(FitY[2], -MaxGradient, MaxGradient)));
				}
				else
				{
					Mean /= Count;
					Patch->AmbientFlow = FVector2f(static_cast<float>(Mean.X), static_cast<float>(Mean.Y));
				}
			}
		}
	}

	// The water under the body's centre keeps the capture's near plane above it.
	if (const UFluidNinjaBuoyancyComponent* Component = Slot.Component.Get())
	{
		if (const UPrimitiveComponent* Primitive = Component->GetSimulatedPrimitive())
		{
			const FVector Centre = Primitive->Bounds.Origin;
			float Height = 0.f;
			float Rate = 0.f;
			FVector2f Flow;
			if (Patch->Sample(Centre.X, Centre.Y, Height, Rate, Flow))
			{
				Slot.LastWaterZ = Height;
				Slot.bHasWaterZ = true;
			}
			// How far the drawn water sits from the water volume's top here, for when the body leaves the capture: the mean over the
			// whole patch (its waves average out), eased over a few seconds.
			if (Slot.bVolume)
			{
				double Sum = 0.0;
				int32 Count = 0;
				for (const float Value : Patch->Height)
				{
					if (!FMath::IsNaN(Value))
					{
						Sum += Value;
						++Count;
					}
				}
				const float Offset = Count > 0 ? static_cast<float>(Sum / Count) - Slot.VolumeHeight : 0.f;
				if (Count > 0 && FMath::Abs(Offset) < 100.f)
				{
					const double Elapsed = Slot.Patch.IsValid() ? FMath::Max(Patch->Time - Slot.Patch->Time, 0.0) : 0.0;
					const float Ease = 1.f - FMath::Exp(-static_cast<float>(Elapsed) / 3.f);
					Slot.FarFieldCorrection = Slot.bFarFieldLearned ? FMath::Lerp(Slot.FarFieldCorrection, Offset, Ease) : Offset;
					Slot.bFarFieldLearned = true;
				}
			}
		}
	}
	Slot.Patch = Patch;
}

void UFluidNinjaBuoyancySubsystem::CaptureAndCopy()
{
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	UWorld* World = GetWorld();
	const double Now = World->GetTimeSeconds();

	// Where a simulation that follows the player is (FluidNinja's parameter collection, such as the river template's).
	bSimValid = false;
	FLinearColor Offset = FLinearColor::Transparent;
	if (NinjaCollection)
	{
		if (UMaterialParameterCollectionInstance* Instance = World->GetParameterCollectionInstance(NinjaCollection))
		{
			FLinearColor Position;
			FLinearColor Size;
			if (Instance->GetVectorParameterValue(TEXT("TraceMeshPos"), Position) && Instance->GetVectorParameterValue(TEXT("TraceMeshSize"), Size))
			{
				Instance->GetVectorParameterValue(TEXT("TraceMeshPosLWCOffset"), Offset);
				SimCentre = FVector2D(Position.R + Offset.R, Position.G + Offset.G);
				SimSizeCm = Size.R * 100.0;
				bSimValid = SimSizeCm > 100.0;
			}
		}
	}
	UpdateFlowSources();

	// The capture's square: on the simulation following the player, else on the middle of all the bodies (a pool and a river beside
	// it fit in one square). Bodies spread wider than the square (separate tanks far apart): on the
	// simulations nearest the camera, as many of them as fit in it together (the nearest always), so the bodies there float on the
	// water as drawn and the rest on the published water.
	const double Width = Settings.CaptureWidthCm;
	// The tier's scale on the resolution (fnb.Capture.Scale), and whether the mirror is redrawn this frame (fnb.Capture.EveryNFrames): between
	// redraws the gather reads the last capture, so its square, near plane and texel are kept (a change of resolution redraws at once).
	const int32 Resolution = FMath::Clamp(FMath::RoundToInt32(Settings.CaptureResolution * FMath::Max(CaptureScale, 0.01f)), 64, 4096);
	const double Texel = Width / Resolution;
	const bool bHaveCapture = Capture && CaptureTarget && CaptureResolution == Resolution && CaptureTexel > 0.0;
	const bool bCaptureDue = !bHaveCapture || CaptureEveryNFrames <= 1 || (GFrameCounter % static_cast<uint64>(CaptureEveryNFrames)) == 0;
	FVector2D Centre = SimCentre;
	if (!bSimValid)
	{
		FBox2D Spread(ForceInit);
		for (const FSlot& Slot : Slots)
		{
			if (const UPrimitiveComponent* Primitive = Slot.Component.IsValid() ? Slot.Component->GetSimulatedPrimitive() : nullptr)
			{
				Spread += FVector2D(Primitive->Bounds.Origin);
			}
		}
		if (Spread.bIsValid)
		{
			Centre = Spread.GetCenter();
			const APlayerController* Controller = World->GetFirstPlayerController();
			const FVector2D Size = Spread.GetSize();
			if (FMath::Max(Size.X, Size.Y) > Width * 0.8 && Controller && Controller->PlayerCameraManager)
			{
				const FVector2D Here(Controller->PlayerCameraManager->GetCameraLocation());
				const auto Off = [&Here](const FFlowSource* Source)
				{
					return FMath::Max(FMath::Abs(Here.X - Source->Centre.X), FMath::Abs(Here.Y - Source->Centre.Y)) - Source->SizeCm * 0.5;
				};
				TArray<const FFlowSource*, TInlineAllocator<16>> Near;
				for (const FFlowSource& Source : FlowSources)
				{
					if (Source.bValid)
					{
						Near.Add(&Source);
					}
				}
				Algo::SortBy(Near, Off);
				FBox2D Together(ForceInit);
				for (const FFlowSource* Source : Near)
				{
					const FBox2D Square(Source->Centre - FVector2D(Source->SizeCm * 0.5), Source->Centre + FVector2D(Source->SizeCm * 0.5));
					const FBox2D Grown = Together.bIsValid ? Together + Square : Square;
					if (Together.bIsValid && FMath::Max(Grown.GetSize().X, Grown.GetSize().Y) > Width * 0.9)
					{
						break;
					}
					Together = Grown;
				}
				if (Together.bIsValid)
				{
					Centre = Together.GetCenter();
				}
			}
		}
	}
	// Between redraws the bodies are judged against the square the last capture was drawn over.
	const FVector2D Origin = bCaptureDue ? FVector2D(FMath::RoundToDouble(Centre.X / Texel) * Texel, FMath::RoundToDouble(Centre.Y / Texel) * Texel) : CaptureOrigin;
	const double Half = Width * 0.5;

	// Which bodies it serves, the footprint each needs, and how high its near plane must be.
	TArray<FFootprint> Wanted;
	double Top = -TNumericLimits<double>::Max();
	for (FSlot& Slot : Slots)
	{
		UFluidNinjaBuoyancyComponent* Component = Slot.Component.Get();
		UPrimitiveComponent* Primitive = Component ? Component->GetSimulatedPrimitive() : nullptr;
		Slot.bInCapture = false;
		if (!Primitive || !Component->GetHull().IsValid() || Slot.bDormant)
		{
			continue;
		}
		const FBox Bounds = Primitive->Bounds.GetBox();
		// Far field: the top of the water volume under the body (a few times a second).
		if (Now >= Slot.NextFlatQuery)
		{
			Slot.NextFlatQuery = Now + FlatQuerySeconds;
			float Surface = 0.f;
			const bool bVolume = FarFieldTop(Bounds, Surface);
			Slot.bFlat = bVolume;
			Slot.bVolume = bVolume;
			Slot.VolumeHeight = Surface;
			// The drawn water can sit off the volume's top (a churned surface): what this body last saw there.
			Slot.FlatHeight = Surface + (Slot.bFarFieldLearned ? Slot.FarFieldCorrection : 0.f);
			Slot.bFlat = Slot.bFlat && Slot.FlatHeight <= Bounds.Max.Z + WaterReachCm && Slot.FlatHeight >= Bounds.Min.Z - WaterReachCm;
		}
		const double Margin = Settings.FootprintMarginCm + FMath::Min(Primitive->GetPhysicsLinearVelocity().Size2D() * Settings.FootprintLeadSeconds, MaxLeadCm);
		const FBox2D Area(FVector2D(Bounds.Min) - FVector2D(Margin), FVector2D(Bounds.Max) + FVector2D(Margin));
		const FVector2D BodyCentre = Area.GetCenter();
		if (FMath::Abs(BodyCentre.X - Origin.X) > Half || FMath::Abs(BodyCentre.Y - Origin.Y) > Half)
		{
			continue;   // beyond the capture: the far field only
		}
		Slot.bInCapture = true;
		Wanted.Add({&Slot, Area, Margin, FindFlowSource(BodyCentre)});
		// The near plane: above the body, and above its water when that is near it (a body pushed under). Never from a water height
		// far above it, or the capture would keep showing that water instead of the body's own.
		double BodyTop = Bounds.Max.Z;
		if (Slot.bHasWaterZ && Slot.LastWaterZ <= Bounds.Max.Z + WaterReachCm)
		{
			BodyTop = FMath::Max(BodyTop, static_cast<double>(Slot.LastWaterZ));
		}
		if (Slot.bFlat)
		{
			BodyTop = FMath::Max(BodyTop, static_cast<double>(Slot.FlatHeight));
		}
		Top = FMath::Max(Top, BodyTop);
	}
	if (Wanted.IsEmpty() || SurfacePrimitives.IsEmpty())
	{
		if (bBuildStatus)
		{
			Status = FString::Printf(TEXT("FluidNinja Buoyancy: no capture (%d bodies, %d surface primitives)"), Slots.Num(), SurfacePrimitives.Num());
		}
		return;
	}
	Top += Settings.NearPlaneAboveBodiesCm;
	const double Range = Width + RangeExtraCm;

	if (bCaptureDue)
	{
		if (!Capture)
		{
			FActorSpawnParameters Params;
			Params.ObjectFlags |= RF_Transient;
			Capture = World->SpawnActor<ASceneCapture2D>(Params);
			CaptureTarget = NewObject<UTextureRenderTarget2D>(this);
			if (!Capture || !CaptureTarget)
			{
				if (bBuildStatus)
				{
					Status = TEXT("FluidNinja Buoyancy: the Surface Mirror capture could not be made");   // a fresh line each time (the ripples add theirs)
				}
				return;
			}
			CaptureTarget->RenderTargetFormat = RTF_R32f;
			CaptureTarget->ClearColor = FLinearColor(static_cast<float>(Range), 0.f, 0.f, 0.f);
			CaptureTarget->Filter = TF_Nearest;
			CaptureTarget->AddressX = TA_Clamp;
			CaptureTarget->AddressY = TA_Clamp;
			CaptureTarget->InitAutoFormat(Resolution, Resolution);
			CaptureTarget->UpdateResourceImmediate(true);
			USceneCaptureComponent2D* Component = Capture->GetCaptureComponent2D();
			Component->ProjectionType = ECameraProjectionMode::Orthographic;
			Component->bAutoCalculateOrthoPlanes = false;
			Component->bOverride_CustomNearClippingPlane = true;
			Component->CustomNearClippingPlane = 0.f;
			Component->CaptureSource = SCS_SceneDepth;
			Component->bCaptureEveryFrame = false;
			Component->bCaptureOnMovement = false;
			Component->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
			Component->TextureTarget = CaptureTarget;
			// The finest meshes, as the player sees them close by: the waves are displaced per vertex.
			Component->LODDistanceFactor = 0.01f;
			Component->ShowFlags.SetLighting(false);
			Component->ShowFlags.SetDynamicShadows(false);
			Component->ShowFlags.SetPostProcessing(false);
			Component->ShowFlags.SetAtmosphere(false);
			Component->ShowFlags.SetFog(false);
			Component->ShowFlags.SetVolumetricFog(false);
			Component->ShowFlags.SetMotionBlur(false);
			Component->ShowFlags.SetTemporalAA(false);
			Component->ShowFlags.SetAntiAliasing(false);
		}
		if (CaptureTarget->SizeX != Resolution || !FMath::IsNearlyEqual(CaptureTarget->ClearColor.R, static_cast<float>(Range)))
		{
			CaptureTarget->ClearColor = FLinearColor(static_cast<float>(Range), 0.f, 0.f, 0.f);
			CaptureTarget->ResizeTarget(Resolution, Resolution);
		}
		USceneCaptureComponent2D* Component = Capture->GetCaptureComponent2D();
		// Only the surface pieces under some body's footprint are drawn (at their finest level of detail, so the rest of the water
		// would cost the GPU for nothing).
		TArray<FBox2D, TInlineAllocator<16>> Reaches;
		for (const FFootprint& Want : Wanted)
		{
			const UPrimitiveComponent* Primitive = Want.Slot->Component->GetSimulatedPrimitive();
			const double Reach = Primitive->Bounds.SphereRadius + Want.Margin + MaxLeadCm + 4.0 * Texel;
			const FVector2D Middle = Want.Area.GetCenter();
			Reaches.Add(FBox2D(Middle - FVector2D(Reach), Middle + FVector2D(Reach)));
		}
		Component->ShowOnlyComponents.Reset();
		for (const TWeakObjectPtr<UPrimitiveComponent>& Weak : SurfacePrimitives)
		{
			const UPrimitiveComponent* Primitive = Weak.Get();
			if (!Primitive)
			{
				continue;
			}
			const FBox Box = Primitive->Bounds.GetBox();
			const FBox2D Flat(FVector2D(Box.Min), FVector2D(Box.Max));
			if (Reaches.ContainsByPredicate([&Flat](const FBox2D& Reach) { return Reach.Intersect(Flat); }))
			{
				Component->ShowOnlyComponents.Add(Weak);
			}
		}
		LastCapturedPrimitives = Component->ShowOnlyComponents.Num();
		Capture->SetActorLocationAndRotation(FVector(Origin.X, Origin.Y, Top), FRotator(-90.0, 0.0, 0.0));
		Component->OrthoWidth = static_cast<float>(Width);
		Component->MaxViewDistanceOverride = static_cast<float>(Range);
		// FluidNinja Waves: the mirror shows FluidNinja's own surface W (the drawn waves' height scaled to 0 for the capture alone; the scalar
		// reaches the render thread in order, before and after the capture's own command), and the bodies feel the waves through the gather.
		{
			SCOPE_CYCLE_COUNTER(STAT_FNB_Capture);
			if (Waves)
			{
				Waves->SetMirrorWavesHidden(true, Component->ShowOnlyComponents);
			}
			Component->CaptureScene();
			if (Waves)
			{
				Waves->SetMirrorWavesHidden(false, Component->ShowOnlyComponents);
			}
		}
		bCapturedThisFrame = true;
		CaptureOrigin = Origin;
		CaptureTop = Top;
		CaptureRange = Range;
		CaptureTexel = Texel;
		CaptureResolution = Resolution;
	}

	// Each body's water out of the capture and the velocity buffer: all in one pass, or copied body by body.
	const bool bGather = Settings.GatherEnabled();
	int32 Served = 0;
	{
		SCOPE_CYCLE_COUNTER(STAT_FNB_GatherBuild);
		Served = bGather ? GatherFootprints(Wanted, Now) : CopyFootprints(Wanted, Now);
		if (bGather && bVerifyGather)
		{
			CopyFootprints(Wanted, Now);   // compared with the gather, never used
		}
	}
	if (!bGather)
	{
		LastGatherBodies = LastGatherSamples = 0;
	}
	if (!bBuildStatus)
	{
		return;   // the status text below is not read this frame
	}
	FString Sims;
	for (const FFlowSource& Source : FlowSources)
	{
		const UTextureRenderTarget2D* Buffer = Source.Buffer.Get();
		Sims += FString::Printf(TEXT("\n  sim %s: %s centre (%.0f, %.0f) size %.0f m, SimSpeed %.2f, %.0f steps/s (%s), %d cells -> flow %.0f cm/s per unit"),
			Source.Ninja.IsValid() && Source.Ninja->GetOuter() ? *Source.Ninja->GetOuter()->GetName() : TEXT("?"),
			Source.bValid ? (Source.bFromMaterial ? TEXT("material,") : TEXT("collection,")) : TEXT("NOT PLACED,"), Source.Centre.X, Source.Centre.Y,
			Source.SizeCm / 100.0, Source.SimSpeed, Source.StepsPerSecond,
			Source.bFixedTick ? TEXT("NinjaLiveCore fixed tick") : *FString::Printf(TEXT("SamplingFPS %.0f, at most the frame rate"), Source.SamplingFPS),
			Source.Resolution > 0 ? Source.Resolution : (Buffer ? Buffer->SizeX : 0), Source.FlowScale);
	}
	if (Sims.IsEmpty())
	{
		Sims = TEXT("\n  no simulation velocity buffer on the surfaces (no current)");
	}
	const FString Mode = bGather
		? FString::Printf(TEXT("gather %d of %d bodies (%d waiting, %d not due), %d points (%.0f KB)"), Served, Wanted.Num(), LastGatherWaiting, LastGatherDeferred, LastGatherSamples, LastGatherSamples * sizeof(FVector4f) / 1024.0)
		: FString::Printf(TEXT("%d copies"), Served);
	// The GPU memory total (fnb.Memory lists it): a walk over a few arrays, once a second.
	if (Now >= NextMemoryReport)
	{
		NextMemoryReport = Now + 1.0;
		TArray<FFluidNinjaBuoyancyMemoryItem> Items;
		LastMemoryMB = ReportMemory(Items);
	}

	Status = FString::Printf(TEXT("FluidNinja Buoyancy: tier %s; capture %.0f m at %d px (%.1f cm)%s, top %.0f, %d surface primitives (%d drawn, %d left out), %s, %u patches, %d wake points; ")
		TEXT("bodies by LOD %d / %d / %d; GPU memory %.1f MB (fnb.Memory)")
		TEXT("%s (flow multiplier %.2f)\n")
		TEXT("  wind %.1f m/s toward %.0f deg (%s)%s\n")
		TEXT("  cost: game thread %.2f ms (patches %.2f), physics pre-tick %.2f ms, physics thread %.2f ms a frame (%.3f a step)"),
		*FluidNinjaBuoyancyQuality::Describe(), Width / 100.0, Resolution, Texel, CaptureEveryNFrames > 1 ? *FString::Printf(TEXT(" every %d frames"), CaptureEveryNFrames) : TEXT(""),
		CaptureTop, SurfacePrimitives.Num(), LastCapturedPrimitives, SurfaceLeftOut, *Mode, PatchesBuilt, LastWakePoints,
		LastBodiesByLevel[0], LastBodiesByLevel[1], LastBodiesByLevel[2], LastMemoryMB,
		*Sims, Settings.FlowScaleMultiplier,
		Wind.SpeedMs, Wind.DirectionDegrees, Wind.bOverride ? TEXT("test override") : TEXT("project setting"),
		bControlledWater ? *FString::Printf(TEXT("; CONTROLLED WATER flat at %.0f, still"), ControlledHeight) : TEXT(""),
		TickMs, PatchMs, PreTickMs, PhysicsMsPerFrame, PhysicsMsPerStep);
	if (!CalibrationReport.IsEmpty())
	{
		Status += TEXT("\n  ") + CalibrationReport;
	}
}

int32 UFluidNinjaBuoyancySubsystem::GatherFootprints(const TArray<FFootprint>& Wanted, double Now)
{
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	FTextureRenderTargetResource* CaptureResource = CaptureTarget->GameThread_GetRenderTargetResource();
	const double Texel = CaptureTexel;
	const double HalfRes = CaptureResolution * 0.5;
	const FVector2D Origin = CaptureOrigin;
	// The tier may cap the grid (fnb.Gather.MaxGrid).
	int32 MaxGrid = FMath::Clamp(Settings.MaxGatherGrid, 8, 256);
	if (GatherMaxGrid > 0)
	{
		MaxGrid = FMath::Min(MaxGrid, FMath::Clamp(GatherMaxGrid, 8, 256));
	}

	// The importance scheduler. The cadence by level of detail (fnb.Gather.FarEveryNFrames): a body within its first level-of-detail distance
	// every frame, beyond it every N frames, beyond the second every 2N, spread over the frames by their ids; between gathers a body's patch
	// carries forward (the physics extrapolates it by its rate). A body is due whatever its turn before it has a patch, and whenever waiting
	// another frame would put the gap between two of its gathers past Max Extrapolation Seconds (urgent): the readback's own few frames of
	// latency are what the extrapolation already bridges, so it is the gap the cadence adds that is held to the limit.
	const int32 FarEvery = FMath::Max(1, GatherFarEveryNFrames);
	struct FDue
	{
		const FFootprint* Want = nullptr;
		/** Lower goes first: its distance from the camera, shortened by its speed (a body at 1 m/s counts as half as far). */
		double Score = 0.0;
		bool bUrgent = false;
	};
	TArray<FDue, TInlineAllocator<64>> Due;
	Due.Reserve(Wanted.Num());
	int32 Deferred = 0;
	for (const FFootprint& Want : Wanted)
	{
		const FSlot& Slot = *Want.Slot;
		const int32 Every = Slot.DistanceLevel >= 2 ? 2 * FarEvery : (Slot.DistanceLevel >= 1 ? FarEvery : 1);
		const bool bHasPatch = Slot.Patch.IsValid() && Slot.LastGatherTime >= 0.0;
		const bool bUrgent = !bHasPatch || Now - Slot.LastGatherTime + SmoothedDeltaTime >= Settings.MaxExtrapolationSeconds;
		// A turn that comes less than half the cadence after the body's last gather is skipped: an urgent gather does not move the body's
		// turn, so with a cadence longer than Max Extrapolation Seconds the turn gathered it again a frame or two later (gaps 4, 2 at 60 fps).
		const bool bJustGathered = bHasPatch && Now - Slot.LastGatherTime < 0.5 * Every * SmoothedDeltaTime;
		if (Every > 1 && !bUrgent && ((GFrameCounter + static_cast<uint64>(Slot.Id)) % Every != 0 || bJustGathered))
		{
			++Deferred;   // not its frame: its patch carries forward
			continue;
		}
		Due.Add({&Want, Slot.CameraDistanceCm / (1.0 + Slot.SpeedCmS * 0.01), bUrgent});
	}
	LastGatherDeferred = Deferred;

	// With more bodies due than the frame takes: under the tier's budget (fnb.Gather.BudgetPerFrame), the urgent bodies first (the longest
	// waiting of them first, so none starves), then the nearest and fastest, and the rest keep their last patch and extrapolate (never past
	// Max Extrapolation Seconds: the physics clamps there, so an over-subscribed budget shows as older water, not as a drop); without a
	// budget, over Max Gather Bodies alone, they take turns as before.
	const int32 MaxBodies = FMath::Max(1, Settings.MaxGatherBodies);
	const int32 Budget = GatherBudgetPerFrame > 0 ? FMath::Min(MaxBodies, GatherBudgetPerFrame) : MaxBodies;
	const int32 Count = FMath::Min(Due.Num(), Budget);
	int32 Start = 0;
	if (Count < Due.Num())
	{
		if (GatherBudgetPerFrame > 0)
		{
			Algo::Sort(Due, [](const FDue& A, const FDue& B)
			{
				if (A.bUrgent != B.bUrgent)
				{
					return A.bUrgent;
				}
				if (A.bUrgent)
				{
					return A.Want->Slot->LastGatherTime < B.Want->Slot->LastGatherTime;
				}
				return A.Score < B.Score;
			});
			GatherCursor = 0;
		}
		else
		{
			Start = GatherCursor % Due.Num();
			GatherCursor = Start + Count;
		}
	}
	else
	{
		GatherCursor = 0;
	}
	LastGatherWaiting = Due.Num() - Count;

	// One pass per simulation (each reads its own velocity buffer); bodies over none get heights only.
	TMap<int32, TArray<const FFootprint*>> Groups;
	for (int32 N = 0; N < Count; ++N)
	{
		const FFootprint* Want = Due[(Start + N) % Due.Num()].Want;
		Groups.FindOrAdd(Want->Source).Add(Want);
	}
	int32 Served = 0;
	LastGatherBodies = LastGatherSamples = 0;
	for (const TPair<int32, TArray<const FFootprint*>>& Group : Groups)
	{
		TSharedPtr<FFluidNinjaBuoyancyGatherReadback, ESPMode::ThreadSafe> Data;
		for (const TSharedPtr<FFluidNinjaBuoyancyGatherReadback, ESPMode::ThreadSafe>& Candidate : GatherRing)
		{
			if (Candidate->Stage.load() == 0)
			{
				Data = Candidate;
				break;
			}
		}
		if (!Data)
		{
			if (GatherRing.Num() >= RingSize * FMath::Max(1, Groups.Num()))
			{
				continue;   // all still in flight
			}
			Data = GatherRing.Add_GetRef(MakeShared<FFluidNinjaBuoyancyGatherReadback, ESPMode::ThreadSafe>());
		}
		const FFlowSource* Source = FlowSources.IsValidIndex(Group.Key) ? &FlowSources[Group.Key] : nullptr;
		UTextureRenderTarget2D* Velocity = Source ? Source->Buffer.Get() : nullptr;
		FTextureRenderTargetResource* FlowResource = Velocity && Source->bValid ? Velocity->GameThread_GetRenderTargetResource() : nullptr;
		const int32 FlowResolution = FlowResource ? Velocity->SizeX : 0;
		const FVector2D SourceCentre = Source ? Source->Centre : FVector2D::ZeroVector;
		const double SourceSize = Source ? Source->SizeCm : 0.0;

		FluidNinjaBuoyancyGather::FRequest Request;
		Request.HeightSize = FIntPoint(CaptureResolution, CaptureResolution);
		Request.FlowSize = Velocity ? FIntPoint(Velocity->SizeX, Velocity->SizeY) : FIntPoint(1, 1);
		Request.CaptureTop = static_cast<float>(CaptureTop);
		Request.CaptureFar = static_cast<float>(CaptureRange) - 1.f;
		Request.Slots.Reserve(Group.Value.Num() * FluidNinjaBuoyancyGather::SlotStride);
		// FluidNinja Waves: this simulation's B and the bodies' own fields, for the bodies that feel the waves.
		UTextureRenderTarget2D* WaveBuffer = nullptr;
		UTextureRenderTarget2D* SelfAtlas = Waves ? Waves->GetSelfAtlas() : nullptr;
		Data->Bodies.Reset(Group.Value.Num());
		int32 Total = 0;
		int32 Largest = 0;
		for (const FFootprint* Want : Group.Value)
		{
			const UPrimitiveComponent* Primitive = Want->Slot->Component->GetSimulatedPrimitive();
			// The body's square: its bounding sphere and margin (with its lead) all round, so the ring of water around it is in. Points
			// one capture texel apart (on texel centres: the heights are read exactly), two and four apart beyond the level-of-detail
			// distances, and never more than MaxGrid across.
			const FVector2D BodyCentre = Want->Area.GetCenter();
			const double Half = Primitive->Bounds.SphereRadius + Want->Margin;
			double Spacing = Texel * (1 << FMath::Clamp(Want->Slot->DistanceLevel, 0, 2));
			int32 Grid = FMath::CeilToInt32(2.0 * Half / Spacing) + 1;
			if (Grid > MaxGrid)
			{
				Grid = MaxGrid;
				Spacing = 2.0 * Half / (MaxGrid - 1);
			}
			const FVector2D GridOrigin(Origin.X + (FMath::RoundToDouble((BodyCentre.X - Half - Origin.X) / Texel - 0.5) + 0.5) * Texel,
				Origin.Y + (FMath::RoundToDouble((BodyCentre.Y - Half - Origin.Y) / Texel - 0.5) + 0.5) * Texel);
			// Capture texel (column, row) of grid point (0, 0) and the step between points; velocity texels likewise.
			const double HeightColumn = (GridOrigin.Y - Origin.Y) / Texel + HalfRes - 0.5;
			const double HeightRow = HalfRes - (GridOrigin.X - Origin.X) / Texel - 0.5;
			bool bFlow = false;
			FVector4f FlowSlot = FVector4f::Zero();
			if (FlowResolution > 0 && SourceSize > 0.0)
			{
				const double Column = ((BodyCentre.X - SourceCentre.X) / SourceSize + 0.5) * FlowResolution - 0.5;
				const double Row = ((BodyCentre.Y - SourceCentre.Y) / SourceSize + 0.5) * FlowResolution - 0.5;
				bFlow = Column >= 0.0 && Row >= 0.0 && Column < FlowResolution && Row < FlowResolution;   // a body inside the simulation
				FlowSlot = FVector4f(static_cast<float>(((GridOrigin.X - SourceCentre.X) / SourceSize + 0.5) * FlowResolution - 0.5),
					static_cast<float>(((GridOrigin.Y - SourceCentre.Y) / SourceSize + 0.5) * FlowResolution - 0.5),
					static_cast<float>(Spacing * FlowResolution / SourceSize), bFlow ? 1.f : 0.f);
			}
			// The waves the body feels: B at the grid's points in the pressure buffer's texels, B_self at the same points in its own field's tile
			// (the wave grid's cells: the pressure buffer's over the simulation's scale-down), each bilinear in its own texture. One simulation
			// per group, so one B for all.
			FVector4f WaveSlot = FVector4f::Zero();
			FVector4f SelfSlot = FVector4f::Zero();
			FVector4f TileSlot = FVector4f::Zero();
			FFluidNinjaWaveGatherSlot Wave;
			if (SelfAtlas && Waves->GetGatherSlot(Want->Slot->Id, Wave) && (!WaveBuffer || WaveBuffer == Wave.Pressure) && Wave.CellCm > 0.0 && Wave.SelfCellCm > 0.0)
			{
				WaveBuffer = Wave.Pressure;
				Request.WaveSize = Wave.PressureCells;
				const double Step = Spacing / Wave.CellCm;             // B: the pressure buffer's texels
				const double SelfStep = Spacing / Wave.SelfCellCm;     // B_self: the body's own field's cells (the wave grid's)
				WaveSlot = FVector4f(static_cast<float>((GridOrigin.X - Wave.SimOrigin.X) / Wave.CellCm - 0.5), static_cast<float>((GridOrigin.Y - Wave.SimOrigin.Y) / Wave.CellCm - 0.5),
					static_cast<float>(Step), 1.f);
				SelfSlot = FVector4f(static_cast<float>(Wave.Tile.X + (GridOrigin.X - Wave.SelfOrigin.X) / Wave.SelfCellCm - 0.5),
					static_cast<float>(Wave.Tile.Y + (GridOrigin.Y - Wave.SelfOrigin.Y) / Wave.SelfCellCm - 0.5), static_cast<float>(SelfStep), static_cast<float>(Wave.SelfCells));
				TileSlot = FVector4f(static_cast<float>(Wave.Tile.X), static_cast<float>(Wave.Tile.Y), Wave.SpongeCells, 0.f);
			}
			Request.Slots.Add(FVector4f(static_cast<float>(HeightColumn), static_cast<float>(HeightRow), static_cast<float>(Spacing / Texel), static_cast<float>(Grid)));
			Request.Slots.Add(FlowSlot);
			Request.Slots.Add(FVector4f(static_cast<float>(Total), 0.f, 0.f, 0.f));
			Request.Slots.Add(WaveSlot);
			Request.Slots.Add(SelfSlot);
			Request.Slots.Add(TileSlot);
			FFluidNinjaBuoyancyGatherReadback::FBody& Body = Data->Bodies.AddDefaulted_GetRef();
			Body.Id = Want->Slot->Id;
			Body.Origin = GridOrigin;
			Body.Spacing = Spacing;
			Body.Grid = Grid;
			Body.First = Total;
			Body.bFlow = bFlow;
			Body.bWave = WaveSlot.W > 0.5f;
			Total += Grid * Grid;
			Largest = FMath::Max(Largest, Grid);
		}
		if (Total == 0)
		{
			continue;
		}
		Request.NumSlots = Group.Value.Num();
		Request.MaxGrid = Largest;
		Request.TotalSamples = Total;
		Data->TotalSamples = Total;
		Data->FlowScale = Source ? Source->FlowScale : 0.f;
		Data->Time = Now;
		Data->Frame = GFrameCounter;
		// Which path reads it out (fnb.Gather.FastReadback): the render thread's end-of-frame look, told of it by the command below, or the slow
		// path's command. Settled before Stage 1, which publishes it with the rest.
		TSharedPtr<FFluidNinjaBuoyancyReadbackPoller, ESPMode::ThreadSafe> Poller;
		if (GatherFastReadback != 0)
		{
			Poller = ReadbackPoller;
		}
		Data->bFastPoll = Poller.IsValid();
		const uint32 Generation = Data->Generation.fetch_add(1) + 1;
		Data->Stage = 1;
		Served += Group.Value.Num();
		LastGatherBodies += Group.Value.Num();
		LastGatherSamples += Total;
		for (const FFootprint* Want : Group.Value)
		{
			Want->Slot->LastGatherTime = Now;
		}
		FTextureRenderTargetResource* WaveResource = WaveBuffer && SelfAtlas ? WaveBuffer->GameThread_GetRenderTargetResource() : nullptr;
		FTextureRenderTargetResource* SelfResource = WaveResource ? SelfAtlas->GameThread_GetRenderTargetResource() : nullptr;
		ENQUEUE_RENDER_COMMAND(FluidNinjaBuoyancyGather)([Data, Request = MoveTemp(Request), CaptureResource, FlowResource, WaveResource, SelfResource, Poller, Generation](FRHICommandListImmediate& RHICmdList) mutable
		{
			Request.Height = CaptureResource ? CaptureResource->GetRenderTargetTexture() : nullptr;
			Request.Flow = FlowResource ? FlowResource->GetRenderTargetTexture() : nullptr;
			Request.Wave = WaveResource && SelfResource ? WaveResource->GetRenderTargetTexture() : nullptr;
			Request.Self = WaveResource && SelfResource ? SelfResource->GetRenderTargetTexture() : nullptr;
			if (!Request.Height)
			{
				Data->Stage = 0;
				return;
			}
			FluidNinjaBuoyancyGather::Enqueue(RHICmdList, Request, Data->Buffer);
			if (Poller)
			{
				Poller->Add(Data, Generation);
			}
		});
	}
	return Served;
}

int32 UFluidNinjaBuoyancySubsystem::CopyFootprints(const TArray<FFootprint>& Wanted, double Now)
{
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	const FVector2D Origin = CaptureOrigin;
	const double Top = CaptureTop;
	const double Range = CaptureRange;
	const double Texel = CaptureTexel;
	const int32 Resolution = CaptureResolution;
	FTextureRenderTargetResource* CaptureResource = CaptureTarget->GameThread_GetRenderTargetResource();
	int32 Copies = 0;
	for (const FFootprint& Want : Wanted)
	{
		// The body's own simulation (its velocity buffer and square).
		const FFlowSource* BodySource = FlowSources.IsValidIndex(Want.Source) ? &FlowSources[Want.Source] : nullptr;
		UTextureRenderTarget2D* Velocity = BodySource && BodySource->bValid ? BodySource->Buffer.Get() : nullptr;
		FTextureRenderTargetResource* FlowResource = Velocity ? Velocity->GameThread_GetRenderTargetResource() : nullptr;
		const int32 FlowResolution = Velocity ? Velocity->SizeX : 0;
		const EPixelFormat FlowFormat = Velocity ? Velocity->GetFormat() : PF_Unknown;
		const bool bFlowReadable = FlowResource && (FlowFormat == PF_FloatRGBA || FlowFormat == PF_G16R16F || FlowFormat == PF_G16R16F_FILTER
			|| FlowFormat == PF_A32B32G32R32F || FlowFormat == PF_G32R32F);
		const FVector2D SourceCentre = BodySource ? BodySource->Centre : FVector2D::ZeroVector;
		const double SourceSize = BodySource ? BodySource->SizeCm : 0.0;
		TSharedPtr<FFluidNinjaBuoyancyReadback, ESPMode::ThreadSafe> Data;
		for (const TSharedPtr<FFluidNinjaBuoyancyReadback, ESPMode::ThreadSafe>& Candidate : Want.Slot->Ring)
		{
			if (Candidate->Stage.load() == 0)
			{
				Data = Candidate;
				break;
			}
		}
		if (!Data)
		{
			continue;   // all three still in flight
		}
		// One square size per body (its bounding sphere, margin and lead): the readbacks reuse their staging textures, which must keep
		// their size. The square slides with the body and stays inside the texture.
		const UPrimitiveComponent* Primitive = Want.Slot->Component->GetSimulatedPrimitive();
		const double Reach = Primitive->Bounds.SphereRadius + Settings.FootprintMarginCm + MaxLeadCm;
		const int32 HeightSide = FMath::Min(Resolution, FMath::CeilToInt32(2.0 * Reach / Texel) + 2);
		const int32 FlowSide = FlowResolution > 0 && SourceSize > 0.0 ? FMath::Min(FlowResolution, FMath::CeilToInt32(2.0 * Reach / (SourceSize / FlowResolution)) + 2) : 0;
		if (HeightSide != Want.Slot->HeightSide || FlowSide != Want.Slot->FlowSide)
		{
			Want.Slot->HeightSide = HeightSide;
			Want.Slot->FlowSide = FlowSide;
			for (TSharedPtr<FFluidNinjaBuoyancyReadback, ESPMode::ThreadSafe>& Entry : Want.Slot->Ring)
			{
				Entry = MakeShared<FFluidNinjaBuoyancyReadback, ESPMode::ThreadSafe>();   // one still in flight finishes on its own
			}
			Data = Want.Slot->Ring[0];
		}
		const FVector2D BodyCentre = Want.Area.GetCenter();
		const double CentreColumn = (BodyCentre.Y - Origin.Y) / Texel + Resolution * 0.5 - 0.5;
		const double CentreRow = Resolution * 0.5 - (BodyCentre.X - Origin.X) / Texel - 0.5;
		const int32 Col0 = FMath::Clamp(FMath::RoundToInt32(CentreColumn - HeightSide * 0.5), 0, Resolution - HeightSide);
		const int32 Row0 = FMath::Clamp(FMath::RoundToInt32(CentreRow - HeightSide * 0.5), 0, Resolution - HeightSide);
		const int32 Col1 = Col0 + HeightSide - 1;
		const int32 Row1 = Row0 + HeightSide - 1;
		Data->HeightMin = FIntPoint(Col0, Row0);
		Data->HeightSize = FIntPoint(Col1 - Col0 + 1, Row1 - Row0 + 1);
		Data->CaptureOrigin = Origin;
		Data->CaptureTop = Top;
		Data->CaptureRange = Range;
		Data->CaptureTexel = Texel;
		Data->CaptureResolution = Resolution;
		Data->Time = Now;
		Data->Frame = GFrameCounter;
		Data->bFlow = false;
		if (bFlowReadable && FlowResolution > 0)
		{
			// Velocity texels: column from X, row from Y, over the simulation square; only for a body inside it.
			const double FlowColumn = ((BodyCentre.X - SourceCentre.X) / SourceSize + 0.5) * FlowResolution - 0.5;
			const double FlowRow = ((BodyCentre.Y - SourceCentre.Y) / SourceSize + 0.5) * FlowResolution - 0.5;
			if (FlowSide > 1 && FlowColumn >= 0.0 && FlowRow >= 0.0 && FlowColumn < FlowResolution && FlowRow < FlowResolution)
			{
				Data->bFlow = true;
				Data->FlowMin = FIntPoint(FMath::Clamp(FMath::RoundToInt32(FlowColumn - FlowSide * 0.5), 0, FlowResolution - FlowSide),
					FMath::Clamp(FMath::RoundToInt32(FlowRow - FlowSide * 0.5), 0, FlowResolution - FlowSide));
				Data->FlowSize = FIntPoint(FlowSide, FlowSide);
				Data->FlowFormat = FlowFormat;
				Data->SimCentre = SourceCentre;
				Data->SimSizeCm = SourceSize;
				Data->FlowResolution = FlowResolution;
				Data->FlowScale = BodySource->FlowScale;
			}
		}
		// Which path reads it out, as GatherFootprints settles it.
		TSharedPtr<FFluidNinjaBuoyancyReadbackPoller, ESPMode::ThreadSafe> Poller;
		if (GatherFastReadback != 0)
		{
			Poller = ReadbackPoller;
		}
		Data->bFastPoll = Poller.IsValid();
		const uint32 Generation = Data->Generation.fetch_add(1) + 1;
		Data->Stage = 1;
		++Copies;
		ENQUEUE_RENDER_COMMAND(FluidNinjaBuoyancyCopy)([Data, CaptureResource, FlowResource, Poller, Generation](FRHICommandListImmediate& RHICmdList)
		{
			FRHITexture* Source = CaptureResource ? CaptureResource->GetRenderTargetTexture() : nullptr;
			if (!Source)
			{
				Data->Stage = 0;
				return;
			}
			Data->Height.EnqueueCopy(RHICmdList, Source, FIntVector(Data->HeightMin.X, Data->HeightMin.Y, 0), 0, FIntVector(Data->HeightSize.X, Data->HeightSize.Y, 1));
			if (Data->bFlow)
			{
				// FluidNinja's velocity buffer can still be its 1 x 1 placeholder on the GPU while its SizeX already says the full
				// size (a simulation that starts with play): copy only a block that lies inside the texture as it is now.
				FRHITexture* FlowSource = FlowResource ? FlowResource->GetRenderTargetTexture() : nullptr;
				const FIntVector FlowExtent = FlowSource ? FlowSource->GetSizeXYZ() : FIntVector::ZeroValue;
				if (FlowSource && Data->FlowMin.X >= 0 && Data->FlowMin.Y >= 0 && Data->FlowMin.X + Data->FlowSize.X <= FlowExtent.X
					&& Data->FlowMin.Y + Data->FlowSize.Y <= FlowExtent.Y)
				{
					Data->Flow.EnqueueCopy(RHICmdList, FlowSource, FIntVector(Data->FlowMin.X, Data->FlowMin.Y, 0), 0, FIntVector(Data->FlowSize.X, Data->FlowSize.Y, 1));
				}
				else
				{
					Data->bFlow = false;
				}
			}
			if (Poller)
			{
				Poller->Add(Data, Generation);
			}
		});
	}

	return Copies;
}

namespace
{
	/**
	 * Brings a frame's ripple impulse pieces (world x, world y, I_down kg·cm/s, area seen from above cm²) down to at most Cap by merging
	 * neighbours rather than dropping the last ones: a hull's pieces come in triangle order, so the pieces past a cap would always be the
	 * same part of it (its stern, say) and the ring would lean towards the rest. Pieces in the same square, pressing the same way, become
	 * one with their summed impulse and area at their impulse-weighted centre. The square starts at one ripple cell (the solver's splat
	 * blurs pieces closer than that together anyway) and doubles until the pieces fit. Every piece's impulse stays in the frame, and so
	 * does where it lands to within a square: with one sign per square, each merged centre lies among its own pieces.
	 */
	void FluidNinjaBuoyancyMergeRippleImpulses(TArray<FVector4f>& Pieces, double CellCm, int32 Cap)
	{
		struct FRippleImpulseBin
		{
			double Impulse = 0.0;   // kg·cm/s
			double Area = 0.0;      // cm²
			double Weight = 0.0;    // Σ|I|
			double OffsetX = 0.0;   // Σ|I|·x and Σ|I|·y, relative to the first piece
			double OffsetY = 0.0;
		};
		if (Cap <= 0)
		{
			Pieces.Reset();
			return;
		}
		if (Pieces.Num() <= Cap)
		{
			return;
		}
		// Pieces that press nothing (or are not numbers) only take room.
		Pieces.RemoveAll([](const FVector4f& Piece)
		{
			return Piece.Z == 0.f || !FMath::IsFinite(Piece.Z) || !FMath::IsFinite(Piece.W) || !FMath::IsFinite(Piece.X) || !FMath::IsFinite(Piece.Y);
		});
		if (Pieces.Num() <= Cap)
		{
			return;
		}
		// Relative to the first piece, in double (world coordinates are large); the squares are centred on it, so once a square is wider
		// than the spread of the pieces every piece pressing one way falls in the same one (and the loop ends with at most two).
		const double BaseX = Pieces[0].X;
		const double BaseY = Pieces[0].Y;
		TMap<FIntVector, FRippleImpulseBin> Bins;
		for (double BinCm = FMath::Max(CellCm, 1.0); Pieces.Num() > FMath::Max(Cap, 2); BinCm *= 2.0)
		{
			Bins.Reset();
			for (const FVector4f& Piece : Pieces)
			{
				const double PieceX = static_cast<double>(Piece.X) - BaseX;
				const double PieceY = static_cast<double>(Piece.Y) - BaseY;
				const FIntVector Key(static_cast<int32>(FMath::Clamp(FMath::RoundToDouble(PieceX / BinCm), -1.0e9, 1.0e9)),
					static_cast<int32>(FMath::Clamp(FMath::RoundToDouble(PieceY / BinCm), -1.0e9, 1.0e9)), Piece.Z > 0.f ? 1 : -1);
				FRippleImpulseBin& Bin = Bins.FindOrAdd(Key);
				const double PieceWeight = FMath::Abs(static_cast<double>(Piece.Z));
				Bin.Impulse += Piece.Z;
				Bin.Area += Piece.W;
				Bin.Weight += PieceWeight;
				Bin.OffsetX += PieceWeight * PieceX;
				Bin.OffsetY += PieceWeight * PieceY;
			}
			// A merged piece's |I| is its pieces' summed |I| (they press the same way), so merging it again keeps the centre exact.
			Pieces.Reset(Bins.Num());
			for (const TPair<FIntVector, FRippleImpulseBin>& Entry : Bins)
			{
				const FRippleImpulseBin& Bin = Entry.Value;
				Pieces.Add(FVector4f(static_cast<float>(BaseX + Bin.OffsetX / Bin.Weight), static_cast<float>(BaseY + Bin.OffsetY / Bin.Weight),
					static_cast<float>(Bin.Impulse), static_cast<float>(Bin.Area)));
			}
		}
		if (Pieces.Num() > Cap)
		{
			// A cap of one with pieces pressing both ways: the net impulse, at the centre of them all.
			FRippleImpulseBin Net;
			for (const FVector4f& Piece : Pieces)
			{
				const double PieceWeight = FMath::Abs(static_cast<double>(Piece.Z));
				Net.Impulse += Piece.Z;
				Net.Area += Piece.W;
				Net.Weight += PieceWeight;
				Net.OffsetX += PieceWeight * (static_cast<double>(Piece.X) - BaseX);
				Net.OffsetY += PieceWeight * (static_cast<double>(Piece.Y) - BaseY);
			}
			Pieces.Reset(1);
			Pieces.Add(FVector4f(static_cast<float>(BaseX + Net.OffsetX / Net.Weight), static_cast<float>(BaseY + Net.OffsetY / Net.Weight),
				static_cast<float>(Net.Impulse), static_cast<float>(Net.Area)));
		}
	}
}

void UFluidNinjaBuoyancySubsystem::PhysicsPreTick(FPhysScene_Chaos* Scene, float DeltaSeconds)
{
	UWorld* World = GetWorld();
	if (!Callback || !World)
	{
		return;
	}
	SCOPE_CYCLE_COUNTER(STAT_FNB_PreTick);
	const double PreTickStart = FPlatformTime::Seconds();
	ON_SCOPE_EXIT
	{
		FramePreTickMs = static_cast<float>((FPlatformTime::Seconds() - PreTickStart) * 1000.0);
		PreTickMs = FMath::Lerp(PreTickMs, FramePreTickMs, 0.1f);
	};
	// What the physics thread found (and what its steps cost). The waves take the impulses of every step, not only the last.
	float StepSeconds = 0.f;
	int32 Steps = 0;
	const bool bTakeWaveImpulses = Waves && Waves->WantsImpulses();
	// Max Wave Impulses, capped by the tier (fnb.Waves.MaxImpulses).
	int32 WaveImpulseCap = FMath::Max(0, UFluidNinjaBuoyancySettings::Get().MaxWaveImpulses);
	if (WavesMaxImpulses > 0)
	{
		WaveImpulseCap = FMath::Min(WaveImpulseCap, WavesMaxImpulses);
	}
	while (Chaos::TSimCallbackOutputHandle<FFluidNinjaBuoyancyOutput> Output = Callback->PopOutputData_External())
	{
		StepSeconds += Output->StepSeconds;
		++Steps;
		for (FFluidNinjaBuoyancyBodyOutput& Body : Output->Bodies)
		{
			// Moved out first, so the copy into LastOutput (which needs only the last step) stays small.
			TArray<FVector4f> StepImpulses = MoveTemp(Body.Result.RippleImpulses);
			if (FSlot* Slot = FindSlot(Body.Id))
			{
				const UFluidNinjaBuoyancyComponent* Waving = Slot->Component.Get();
				if (bTakeWaveImpulses && WaveImpulseCap > 0 && !StepImpulses.IsEmpty() && Waving && Waving->bWaves)
				{
					TArray<FVector4f>& Pieces = PendingWaveImpulses.FindOrAdd(Body.Id);
					if (Pieces.IsEmpty())
					{
						Pieces.Reserve(FMath::Max(Slot->LastImpulsePieces, StepImpulses.Num()));   // last frame's count: one allocation a frame, not one a step
					}
					Pieces.Append(MoveTemp(StepImpulses));
				}
				if (!Slot->LastOutput.IsValid())
				{
					Slot->LastOutput = MakeShared<FFluidNinjaBuoyancyBodyOutput>();
				}
				*Slot->LastOutput = Body;
				if (Body.Result.SlamPower > Slot->SlamWatts)
				{
					Slot->SlamWatts = Body.Result.SlamPower;
					Slot->SlamLocation = Body.Result.SlamLocation;
				}
				Slot->EntryWatts = FMath::Max(Slot->EntryWatts, Body.Result.EntryPower);
			}
		}
	}
	// More pieces than Max Wave Impulses: each body's neighbouring ones are merged, keeping its impulse and where it lands (dropping the
	// last ones would always drop the same part of a hull), and every body keeps its own (its own field takes them alone). Each body gets
	// its share of the cap by how many pieces it has.
	int32 PendingPieces = 0;
	for (const TPair<int32, TArray<FVector4f>>& Entry : PendingWaveImpulses)
	{
		PendingPieces += Entry.Value.Num();
	}
	if (PendingPieces > WaveImpulseCap)
	{
		if (WaveImpulseCap > 0 && !bWaveImpulseCapLogged)
		{
			UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("FluidNinja Buoyancy waves: %d impulse pieces in a frame, more than Max Wave Impulses (%d); each body's neighbouring pieces are merged (their impulse and where it lands are kept). Logged once."),
				PendingPieces, WaveImpulseCap);
			bWaveImpulseCapLogged = true;
		}
		const double MergeCell = Waves ? Waves->GetMergeCellCm() : 1.0;
		for (TPair<int32, TArray<FVector4f>>& Entry : PendingWaveImpulses)
		{
			const int32 Share = WaveImpulseCap > 0 ? FMath::Max(1, static_cast<int32>(static_cast<int64>(WaveImpulseCap) * Entry.Value.Num() / PendingPieces)) : 0;
			FluidNinjaBuoyancyMergeRippleImpulses(Entry.Value, MergeCell, Share);
		}
	}
	FramePhysicsMs = StepSeconds * 1000.f;
	if (Steps > 0)
	{
		PhysicsMsPerFrame = FMath::Lerp(PhysicsMsPerFrame, FramePhysicsMs, 0.1f);
		PhysicsMsPerStep = FMath::Lerp(PhysicsMsPerStep, FramePhysicsMs / Steps, 0.1f);
	}
	const double Now = World->GetTimeSeconds();
	for (FSlot& Slot : Slots)
	{
		UFluidNinjaBuoyancyComponent* Component = Slot.Component.Get();
		if (!Component || !Slot.LastOutput.IsValid())
		{
			continue;
		}
		const FluidNinjaBuoyancy::FStepResult& Result = Slot.LastOutput->Result;
		FFluidNinjaBuoyancyReadout Readout;
		Readout.bInWater = Result.bTouchesWater;
		Readout.DisplacedLitres = Result.DisplacedVolume / 1000.f;
		Readout.WettedAreaM2 = Result.WettedArea / 10000.f;
		Readout.ForceN = Result.Force / 100.0;
		Readout.WindForceN = Result.AirForce / 100.0;
		Readout.MassKg = Slot.LastOutput->Mass;
		Readout.SlamWatts = Slot.SlamWatts * 1e-4f;
		Readout.EntryWatts = Slot.EntryWatts * 1e-4f;
		Readout.WaterAgeSeconds = Slot.Patch.IsValid() && Slot.bInCapture ? static_cast<float>(Now - Slot.Patch->Time) : -1.f;
		Readout.HullLevel = Slot.Level;
		Readout.bDormant = Slot.bDormant;
		if (const FluidNinjaBuoyancy::FWaterPatch* Patch = Slot.Patch.Get())
		{
			float Height = 0.f;
			float Rate = 0.f;
			FVector2f Flow = FVector2f::ZeroVector;
			if (Patch->Sample(Slot.LastOutput->CenterOfMass.X, Slot.LastOutput->CenterOfMass.Y, Height, Rate, Flow))
			{
				Readout.FlowCmS = FVector2D(Flow.X, Flow.Y);
			}
		}
		Component->ReceiveReadout(Readout, Readout.SlamWatts, Slot.SlamLocation);
		Slot.SlamWatts = 0.f;
		Slot.EntryWatts = 0.f;
	}

	// The water line each body last had: its contacts (wet line, spray, sound), and the pieces driving into the water written into
	// FluidNinja as moving brush points (bow wave, wake, foam). Points move with the hull: PositionPrevious is where that piece of
	// hull was a frame ago, and the velocity is the hull's there.
	const UFluidNinjaBuoyancySettings& HydroSettings = UFluidNinjaBuoyancySettings::Get();
	const bool bWakes = HydroSettings.WakesEnabled() && WakeChannel && !bControlledWater;
	// Max Wake Points, capped by the tier (fnb.Wake.MaxPoints).
	const int32 MaxWakePoints = WakeMaxPoints > 0 ? FMath::Min(HydroSettings.MaxWakePoints, WakeMaxPoints) : HydroSettings.MaxWakePoints;
	struct FWakePoint
	{
		FVector Position;
		FVector Previous;
		FVector4 VelocityAndSize;
	};
	TArray<FWakePoint, TInlineAllocator<64>> Wakes;
	Wakes.Reserve(LastWakePoints);   // about as many as last frame
	TArray<FFluidNinjaWaterlineContact, TInlineAllocator<FluidNinjaBuoyancy::WaterlineSectors>> Contacts;
	for (FSlot& Slot : Slots)
	{
		UFluidNinjaBuoyancyComponent* Component = Slot.Component.Get();
		if (!Component || !Slot.LastOutput.IsValid())
		{
			continue;
		}
		const FluidNinjaBuoyancy::FStepResult& Result = Slot.LastOutput->Result;
		Contacts.Reset();
		if (!Slot.bDormant)
		{
			for (const FluidNinjaBuoyancy::FWaterlineSample& Sample : Result.Sectors)
			{
				if (Sample.bValid)
				{
					Contacts.Add({Sample.Position, Sample.HullVelocity, Sample.Outward, Sample.Push, Sample.Plunge});
				}
			}
		}
		Component->ReceiveContacts(Contacts, Result.Wash, Result.WaterlineLength);
		if (!bWakes || !Component->bWake || Component->Interaction == EFluidNinjaBuoyancyInteraction::FluidNinjaTracking)
		{
			continue;   // no wakes: FluidNinja's own tracking is its only route into the water
		}
		// Each point gives the water a share of what it lacks along the hull's outward normal: the hull's speed there minus the water's
		// own (read where the point is). FluidNinja adds brush velocity every frame, so this closes on the hull's speed, never past it.
		const FluidNinjaBuoyancy::FWaterPatch* Patch = Slot.bInCapture ? Slot.Patch.Get() : nullptr;
		for (const FFluidNinjaWaterlineContact& Contact : Contacts)
		{
			if (Wakes.Num() >= MaxWakePoints)
			{
				break;
			}
			float Height = 0.f;
			float Rate = 0.f;
			FVector2f Local = FVector2f::ZeroVector;
			if (!Patch || !Patch->Sample(Contact.Position.X, Contact.Position.Y, Height, Rate, Local))
			{
				continue;   // no water read here
			}
			// The hull's push through the water around it, sideways plus the water it displaces as it sinks (a drop or a dunk sends a
			// ring out); the water at the hull should move on with the current plus a share of that (Wake Strength), and gets a share
			// of what it still lacks (Wake Gain) each frame.
			const FVector2f Around = Patch->bAmbient ? Patch->AmbientAt(Contact.Position.X, Contact.Position.Y) : Local;
			const FVector Outward = Contact.Outward;
			const float AroundOut = static_cast<float>(FVector::DotProduct(FVector(Around.X, Around.Y, 0.0), Outward));
			const float LocalOut = static_cast<float>(FVector::DotProduct(FVector(Local.X, Local.Y, 0.0), Outward));
			const float Through = static_cast<float>(FVector::DotProduct(Contact.HullVelocity - FVector(Around.X, Around.Y, 0.0), Outward)) + Contact.Plunge;
			if (Through >= Component->WakeMinPushCmS)
			{
				const float Target = AroundOut + Component->WakeStrength * Through;
				const float Lack = Target - LocalOut;
				// fnb.Wake.AlongOnly (A/B): no side wake where the hull's outward normal faces into the current around it.
				const bool bAgainst = FluidNinjaBuoyancyCVars::WakeAlongOnly != 0 && Lack > 0.f && AroundOut < -FluidNinjaBuoyancyCVars::WakeAlongOnlyMinCmS;
				if (FluidNinjaBuoyancyCVars::WakeAlongOnly != 0 && Lack > 0.f)
				{
					(bAgainst ? FluidNinjaBuoyancyCVars::WakeAlongOnlyHullSkipped : FluidNinjaBuoyancyCVars::WakeAlongOnlyHullWritten) += 1;
				}
				if (Lack > 0.f && !bAgainst)
				{
					const FVector Velocity = Outward * (Lack * Component->WakeGain);
					const float Size = Component->WakeBrushSizeCm * FMath::Clamp(Through / 200.f, 0.4f, 2.f);
					Wakes.Add({Contact.Position, Contact.Position - Contact.HullVelocity * DeltaSeconds, FVector4(Velocity.X, Velocity.Y, Velocity.Z, Size)});
				}
			}
			// The impact burst, on its own: where the hull sinks in, the water it displaces runs outward across the water line at about
			// the sinking speed (Plunge Gain 1 is that speed), and the water there gets Plunge Response of what it lacks of it each frame.
			// A radial burst carries no net momentum, so it cannot build a current.
			if (Contact.Plunge > Component->WakeMinPushCmS && Wakes.Num() < MaxWakePoints)
			{
				const float PlungeTarget = AroundOut + Component->PlungeGain * Contact.Plunge;
				const float PlungeLack = PlungeTarget - LocalOut;
				if (PlungeLack > 0.f)
				{
					const FVector Burst = Outward * (PlungeLack * Component->PlungeResponse);
					const float BurstSize = Component->WakeBrushSizeCm * FMath::Clamp(Contact.Plunge / 200.f, 0.4f, 2.f);
					Wakes.Add({Contact.Position, Contact.Position - Contact.HullVelocity * DeltaSeconds, FVector4(Burst.X, Burst.Y, Burst.Z, BurstSize)});
				}
			}
		}
	}
	LastWakePoints = Wakes.Num();
	if (!Wakes.IsEmpty())
	{
		if (UNiagaraDataChannelWriter* Writer = UNiagaraDataChannelLibrary::WriteToNiagaraDataChannel(World, WakeChannel, FNiagaraDataChannelSearchParameters(),
			Wakes.Num(), false, true, true, TEXT("FluidNinja Buoyancy wakes")))
		{
			static const FName PositionName(TEXT("Position"));
			static const FName PreviousName(TEXT("PositionPrevious"));
			static const FName VelocityName(TEXT("VelocityAndSize"));
			for (int32 Index = 0; Index < Wakes.Num(); ++Index)
			{
				Writer->WritePosition(PositionName, Index, Wakes[Index].Position);
				Writer->WritePosition(PreviousName, Index, Wakes[Index].Previous);
				Writer->WriteVector4(VelocityName, Index, Wakes[Index].VelocityAndSize);
			}
		}
	}

	// Level of detail and sleep, from the camera.
	FVector Camera = FVector::ZeroVector;
	if (const APlayerController* Controller = World->GetFirstPlayerController())
	{
		if (Controller->PlayerCameraManager)
		{
			Camera = Controller->PlayerCameraManager->GetCameraLocation();
		}
	}
	for (FSlot& Slot : Slots)
	{
		UpdateLevelAndSleep(Slot, Camera, DeltaSeconds);
	}

	// What it needs for the coming steps (the frame, so the steps that share this input know how far into it they are).
	FFluidNinjaBuoyancyInput* Input = Callback->GetProducerInputData_External();
	Input->Reset();
	Input->Frame = GFrameCounter;
	if (!UFluidNinjaBuoyancySettings::Get().IsEnabled())
	{
		return;
	}
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	for (FSlot& Slot : Slots)
	{
		UFluidNinjaBuoyancyComponent* Component = Slot.Component.Get();
		UPrimitiveComponent* Primitive = Component ? Component->GetSimulatedPrimitive() : nullptr;
		FBodyInstance* Body = Primitive ? Primitive->GetBodyInstance() : nullptr;
		if (!Body || !Body->IsInstanceSimulatingPhysics() || !Component->GetHull().IsValid() || Slot.bDormant || Slot.bHeld)
		{
			continue;
		}
		Chaos::FSingleParticlePhysicsProxy* Proxy = Body->GetPhysicsActor();
		if (!Proxy)
		{
			continue;
		}
		SetNeverSleep(Body, Slot.LastOutput.IsValid() ? Slot.LastOutput->Result.bTouchesWater : true);
		FFluidNinjaBuoyancyBodyInput& BodyInput = Input->Bodies.AddDefaulted_GetRef();
		BodyInput.Proxy = Proxy;
		BodyInput.Id = Slot.Id;
		BodyInput.Hull = Component->GetHull(Slot.Level);
		BodyInput.FinestHull = Component->GetHull(0);
		if (bControlledWater)
		{
			BodyInput.bFlat = true;   // still and flat: no patch, no current
			BodyInput.FlatHeight = ControlledHeight;
		}
		else
		{
			BodyInput.Patch = Slot.bInCapture ? Slot.Patch : nullptr;
			// The patch's age at the frame's start; each step of the frame adds its own time into the frame, under the same cap (the callback).
			BodyInput.Extrapolate = BodyInput.Patch.IsValid() ? FMath::Clamp(static_cast<float>(Now - BodyInput.Patch->Time), 0.f, Settings.MaxExtrapolationSeconds) : 0.f;
			BodyInput.MaxExtrapolate = Settings.MaxExtrapolationSeconds;
			BodyInput.bFlat = Slot.bFlat;
			BodyInput.FlatHeight = Slot.FlatHeight;
			// A body that pushes the water itself is carried by the current around it, not by the water it has just pushed (which
			// would carry it on with its own speed): one that makes wakes, or one FluidNinja paints in itself (its overlap tracking
			// sees primitives that generate overlap events).
			BodyInput.bAmbientFlow = (Component->bWake && Settings.WakesEnabled()) || Primitive->GetGenerateOverlapEvents();
		}
		Component->FillParams(BodyInput.Params);
		// The coarse hull levels are cruder shells inside the true surface and displace less at the same waterline: the level in use lifts
		// by V_0 / V_Level more (1 for level 0 and for a level not built), so the body rests at the finest hull's draft at every distance.
		BodyInput.Params.BuoyancyMultiplier *= Component->GetLevelBuoyancyScale(Slot.Level);
		BodyInput.Params.Rho = Settings.WaterDensity * 1e-6f;
		BodyInput.Params.RhoAir = Settings.AirDensity * 1e-6f;
		BodyInput.Params.Gravity = FMath::Max(1.f, -World->GetGravityZ());
		BodyInput.Params.Wind = Wind.Velocity;
		BodyInput.bWaterline = UFluidNinjaBuoyancySettings::IsDebugView();
		BodyInput.bStableSteps = Settings.StableStepsEnabled();
		// FluidNinja Waves: each wet piece's impulse into the water, every step (controlled water too: the test bench measures them there);
		// how much of the others' waves the body feels, and the radiation damping of its own.
		BodyInput.bWaveImpulses = bTakeWaveImpulses && Component->bWaves;
		FFluidNinjaWaveBodyFeel Feel;
		if (Waves && Waves->GetBodyFeel(Slot.Id, Feel))
		{
			BodyInput.WaveFeel = Feel.Feel;
			BodyInput.RadiationDamping = Feel.Radiation;
			BodyInput.RadiationRollAxis = Feel.RollAxis;
			BodyInput.RadiationPitchAxis = Feel.PitchAxis;
			BodyInput.RestWettedArea = Feel.RestWettedArea;
			BodyInput.AddedMass = Feel.AddedMass;
		}
	}
}

void UFluidNinjaBuoyancySubsystem::UpdateLevelAndSleep(FSlot& Slot, const FVector& Camera, float DeltaSeconds)
{
	UFluidNinjaBuoyancyComponent* Component = Slot.Component.Get();
	UPrimitiveComponent* Primitive = Component ? Component->GetSimulatedPrimitive() : nullptr;
	if (!Primitive)
	{
		return;
	}
	const double Distance = FVector::Distance(Camera, Primitive->Bounds.Origin);
	Slot.CameraDistanceCm = Distance;
	Slot.SpeedCmS = Primitive->IsSimulatingPhysics() ? Primitive->GetPhysicsLinearVelocity().Size() : 0.0;

	// Hull level of detail (triangles model), with a 10 % band so it does not flicker at a boundary. The tier scales the distances
	// (fnb.Lod.DistanceScale) and may push the hull a level or two coarser than its distance asks (fnb.Lod.Bias); the gather's spacing and
	// cadence follow the distance level alone.
	const FVector2D Limits = Component->LodDistancesCm * FMath::Max(LodDistanceScale, 0.01f);
	const int32 Levels = Component->GetHullLevels();
	int32 Level = Distance > Limits.Y ? 2 : Distance > Limits.X ? 1 : 0;
	if (Level != Slot.DistanceLevel)
	{
		const double Boundary = FMath::Max(Level, Slot.DistanceLevel) == 2 ? Limits.Y : Limits.X;
		if (FMath::Abs(Distance - Boundary) < Boundary * 0.1)
		{
			Level = Slot.DistanceLevel;
		}
	}
	Slot.DistanceLevel = Level;   // the gather's spacing follows it for every hull model
	Slot.Level = FMath::Clamp(Level + FMath::Clamp(LodBias, 0, 2), 0, FMath::Max(Levels - 1, 0));

	// Sleep: settled, unseen and far. It wakes when seen, when the camera comes near, or when something wakes its body (a hit).
	FBodyInstance* Body = Primitive->GetBodyInstance();
	if (!Body || !Body->IsInstanceSimulatingPhysics())
	{
		return;
	}

	// Held: afloat a moment ago, now out of the capture (the simulation follows the player) with no water volume there. It would
	// sink through water it can no longer read, so it is held asleep where it is until its water is known again.
	const bool bWaterKnown = bControlledWater || Slot.bInCapture || Slot.bFlat;
	if (Slot.bHeld)
	{
		if (bWaterKnown)
		{
			Slot.bHeld = false;
			Primitive->WakeRigidBody();
		}
		else if (Body->IsInstanceAwake())
		{
			Primitive->PutRigidBodyToSleep();
		}
		return;
	}
	if (!bWaterKnown && !Slot.bDormant && !Component->IsAwaitingWater() && Slot.LastOutput.IsValid() && Slot.LastOutput->Result.bTouchesWater)
	{
		Slot.bHeld = true;
		SetNeverSleep(Body, false);
		Primitive->PutRigidBodyToSleep();
		UE_LOG(LogFluidNinjaBuoyancy, Log, TEXT("FluidNinja Buoyancy: %s held (out of the capture, no water volume there)"), *GetNameSafe(Component->GetOwner()));
		return;
	}

	const bool bSeen = Primitive->WasRecentlyRendered(0.5f);
	const bool bFar = Distance > Component->SleepDistanceCm * FMath::Max(LodSleepDistanceScale, 0.01f);
	const double Now = GetWorld()->GetTimeSeconds();
	if (Slot.bDormant)
	{
		// Woken by something else (a hit, a force from game code): at once when it moves, since asleep it gets no water forces and
		// would fall. Awake but still, it may only be the sleep request not yet through, for the first half second.
		const bool bWoken = Body->IsInstanceAwake() && (Now - Slot.DormantSince > 0.5 || Primitive->GetPhysicsLinearVelocity().Size() > 2.f * SettledSpeedCmS);
		bool bWake = !bFar || bSeen || !Component->bSleepWhenSettled || bWoken;
		if (!bWake && LodDormantDrift != 0)
		{
			// fnb.Lod.DormantDrift: a wind past the one it settled in wakes it to find its drift again, and a throttle other than the one it
			// fell asleep at (game code steering it) wakes it to be driven; else a drifting body moves on. The hit is looked for first, so a
			// move's own sleep request never hides one, and DormantSince stays when it fell asleep.
			bWake = (Component->bPropulsion && Component->Throttle != Slot.DormantThrottle) || DormantWindOutgrown(Slot)
				|| (Slot.bDormantDrifting && !DriftDormant(Slot, *Primitive, DeltaSeconds, Now));
		}
		if (bWake)
		{
			Slot.bDormant = false;
			Slot.SettledSeconds = 0.f;
			Primitive->WakeRigidBody();
		}
		return;
	}

	// Settled. With fnb.Lod.DormantDrift, on flat water with no current (controlled water, or the far field beyond the capture and every
	// FluidNinja simulation: the physics gives that water no current either), a passive floater (not under power) may settle moving: level,
	// not turning, its horizontal velocity within the settle band of what it was when the window opened, for Settle Seconds, and in a wind
	// of 1 m/s or more no faster than that wind drifts a floater, nor against it. Still is a steady drift of 0. Elsewhere (FluidNinja's
	// water, whose current only the gather knows), under power, or with the switch off, it settles only when still, as before.
	const FVector Velocity = Primitive->GetPhysicsLinearVelocity();
	const bool bNotTurning = Primitive->GetPhysicsAngularVelocityInRadians().Size() < SettledSpinRadS;
	const bool bCandidate = Component->bSleepWhenSettled && bFar && !bSeen;
	const FVector2D Location(Primitive->GetComponentLocation());
	const bool bDriftWater = bCandidate && LodDormantDrift != 0 && !(Component->bPropulsion && Component->Throttle != 0.f)
		&& (bControlledWater || (!Slot.bInCapture && Slot.bFlat && FindFlowSource(FVector2D(Primitive->Bounds.Origin)) == INDEX_NONE));
	const FVector2D Horizontal(Velocity);
	const FVector2D WindNow(Wind.Velocity);
	const auto IsWindDrift = [&WindNow](const FVector2D& Motion)
	{
		const double Speed = Motion.Size();
		const double WindSpeed = WindNow.Size();
		return Speed < SettledSpeedCmS || (WindSpeed >= DormantDriftMinWindCmS && Speed <= DormantDriftMaxWindShare * WindSpeed && (Motion | WindNow) >= 0.0);
	};
	bool bSettling = false;
	if (bDriftWater)
	{
		bSettling = Slot.bSettleOpen && IsWindDrift(Horizontal) && bNotTurning && FMath::Abs(Velocity.Z) < SettledSpeedCmS
			&& FVector2D::Distance(Horizontal, Slot.SettleVelocity) < SettledSpeedCmS;
		if (!bSettling)
		{
			// The window (re)opens here.
			Slot.bSettleOpen = true;
			Slot.SettleVelocity = Horizontal;
			Slot.SettleLocation = Location;
		}
	}
	else
	{
		Slot.bSettleOpen = false;
		bSettling = bCandidate && Velocity.Size() < SettledSpeedCmS && bNotTurning;
	}
	Slot.SettledSeconds = bSettling ? Slot.SettledSeconds + DeltaSeconds : 0.f;
	if (Slot.SettledSeconds > SettleSeconds)
	{
		// Asleep it drifts at its mean velocity over the window (the water has no current, so that is its drift relative to the water),
		// carried to the wind of the moment (DriftDormant). The mean must agree with its velocity now and pass as a wind's drift: a jump
		// within the window (a teleport that kept the velocity, a window left open from an earlier place) shows in the mean alone, and opens
		// the window again here instead of sending the body off at the jump's speed.
		const FVector2D Drift = Slot.bSettleOpen ? (Location - Slot.SettleLocation) / Slot.SettledSeconds : FVector2D::ZeroVector;
		if (Slot.bSettleOpen && (FVector2D::Distance(Drift, Horizontal) >= SettledSpeedCmS || !IsWindDrift(Drift)))
		{
			Slot.SettleVelocity = Horizontal;
			Slot.SettleLocation = Location;
			Slot.SettledSeconds = 0.f;
			return;
		}
		Slot.bDormant = true;
		Slot.DormantSince = Now;
		// Under 1 cm/s, or settled in a wind under 1 m/s (which teaches no drift), it lies still. Every sleeping body keeps the wind it
		// settled in and its throttle, so a wind it outgrows (DormantWindOutgrown) or a new throttle wakes it.
		Slot.DormantWind = WindNow;
		Slot.DormantThrottle = Component->Throttle;
		Slot.bDormantDrifting = Drift.Size() >= DormantDriftMinCmS && WindNow.Size() >= DormantDriftMinWindCmS;
		Slot.DormantDrift = Slot.bDormantDrifting ? Drift : FVector2D::ZeroVector;
		Slot.DormantPending = FVector2D::ZeroVector;
		Slot.NextDormantMove = Now + DormantMoveSeconds;
		Slot.bSettleOpen = false;
		SetNeverSleep(Body, false);
		Primitive->PutRigidBodyToSleep();
	}
}

bool UFluidNinjaBuoyancySubsystem::DormantWindOutgrown(const FSlot& Slot) const
{
	// A floater's drift follows the wind's speed (DriftDormant), but the drift was measured within the settle band (3 cm/s), and carried to
	// a wind U from the U0 it settled in its error grows by U / U0: past twice U0 (or past 1 m/s after a calm, which taught no drift) the body
	// wakes and settles again at its new drift. So does a wind turned more than 45 degrees: a hull's leeway, or the bank a body lies pinned
	// on, depends on where the wind comes from, which a turned drift cannot follow.
	const FVector2D WindNow(Wind.Velocity);
	const double U = WindNow.Size();
	const double U0 = Slot.DormantWind.Size();
	if (U > FMath::Max(2.0 * U0, DormantDriftMinWindCmS))
	{
		return true;
	}
	return U >= DormantDriftMinWindCmS && U0 >= DormantDriftMinWindCmS && (WindNow | Slot.DormantWind) < U * U0 * DormantDriftMaxTurnCos;
}

bool UFluidNinjaBuoyancySubsystem::DriftDormant(FSlot& Slot, UPrimitiveComponent& Primitive, float DeltaSeconds, double Now)
{
	// A floater whose windage meets the water's drag, both quadratic, drifts at v = U √(ρ_air C_air A_air / (ρ C_water A_water)) relative to
	// the water: in proportion to the wind's speed U, along it (give or take its leeway). So the drift v0 it settled at in the wind U0 is
	// carried to the wind of the moment as v = R(θ) v0 |U| / |U0|, θ the wind's turn since (within 45 degrees, DormantWindOutgrown):
	// v = [U·U0, −(U0×U); U0×U, U·U0] v0 / |U0|², no trigonometry. The far field has no current (the physics gives the flat water none), so
	// none is added. A drifting body settled in 1 m/s or more (UpdateLevelAndSleep), so |U0|² is never small.
	const FVector2D WindNow(Wind.Velocity);
	const double Along = WindNow | Slot.DormantWind;
	const double Turn = Slot.DormantWind ^ WindNow;
	const FVector2D& V0 = Slot.DormantDrift;
	Slot.DormantPending += FVector2D(Along * V0.X - Turn * V0.Y, Turn * V0.X + Along * V0.Y) * (DeltaSeconds / Slot.DormantWind.SizeSquared());
	// Unseen, it moves in steps (each a swept move and a sleep request): DormantMoveSeconds apart, a centimetre or longer.
	if (Now < Slot.NextDormantMove)
	{
		return true;
	}
	Slot.NextDormantMove = Now + DormantMoveSeconds;
	if (Slot.DormantPending.SizeSquared() < FMath::Square(DormantMinMoveCm))
	{
		return true;
	}
	const FVector Step(Slot.DormantPending, 0.0);
	Slot.DormantPending = FVector2D::ZeroVector;

	// Where it goes must be the same flat water: the far field's top within a centimetre of the one in its slot (it fell asleep on that
	// one, and CaptureAndCopy leaves a sleeping body's far field alone) and within reach of the hull, as CaptureAndCopy judges it, and no
	// FluidNinja simulation there (its water is live). Else it wakes where it is, on the water its slot still describes, and the physics
	// takes it on (held, where no water is known). Controlled water lies everywhere.
	if (!bControlledWater)
	{
		const FBox Bounds = Primitive.Bounds.GetBox().ShiftBy(Step);
		float Surface = 0.f;
		if (!FarFieldTop(Bounds, Surface) || FindFlowSource(FVector2D(Bounds.GetCenter())) != INDEX_NONE)
		{
			return false;
		}
		const float FlatHeight = Surface + (Slot.bFarFieldLearned ? Slot.FarFieldCorrection : 0.f);
		if (FMath::Abs(FlatHeight - Slot.FlatHeight) > DormantMaxFlatChangeCm || FlatHeight > Bounds.Max.Z + WaterReachCm || FlatHeight < Bounds.Min.Z - WaterReachCm)
		{
			return false;
		}
	}

	// Swept, so it stops at a shore, a pier or another body and wakes there (the physics takes the contact). Chaos wakes a sleeping body
	// that is set somewhere; put back to sleep in the same frame, the move and the sleep reach the solver together and it moves asleep
	// (PushToPhysicsStateImp).
	FHitResult Hit;
	Primitive.SetWorldLocation(Primitive.GetComponentLocation() + Step, true, &Hit, ETeleportType::TeleportPhysics);
	if (Hit.bBlockingHit)
	{
		return false;
	}
	Primitive.PutRigidBodyToSleep();
	return true;
}

void UFluidNinjaBuoyancySubsystem::UpdateWind(float DeltaSeconds)
{
	// The test override, else the project's Wind.
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	FFluidNinjaBuoyancyWind Next;
	FVector Blowing = FVector::ZeroVector;
	if (bWindOverride)
	{
		Next.bOverride = true;
		Blowing = FRotator(0.f, OverrideWindDegrees, 0.f).Vector() * (OverrideWindMs * 100.f);
	}
	else
	{
		Blowing = Settings.Wind;
	}
	Next.Velocity = Blowing;
	Next.SpeedMs = static_cast<float>(Blowing.Size2D() / 100.0);
	Next.DirectionDegrees = Blowing.SizeSquared2D() > 1.0 ? FRotator::ClampAxis(static_cast<float>(FMath::RadiansToDegrees(FMath::Atan2(Blowing.Y, Blowing.X)))) : 0.f;
	Wind = Next;
}

void UFluidNinjaBuoyancySubsystem::SetWindOverride(bool bOn, float SpeedMs, float DirectionDegrees)
{
	bWindOverride = bOn;
	OverrideWindMs = FMath::Max(0.f, SpeedMs);
	OverrideWindDegrees = DirectionDegrees;
	UpdateWind(0.f);
}

void UFluidNinjaBuoyancySubsystem::SetControlledWater(bool bOn, float Height)
{
	bControlledWater = bOn;
	ControlledHeight = Height;
	for (FSlot& Slot : Slots)
	{
		// Bodies asleep or held wake to the new water.
		if (UPrimitiveComponent* Primitive = Slot.Component.IsValid() ? Slot.Component->GetSimulatedPrimitive() : nullptr)
		{
			if (Slot.bDormant || Slot.bHeld)
			{
				Slot.bDormant = Slot.bHeld = false;
				Slot.SettledSeconds = 0.f;
				Primitive->WakeRigidBody();
			}
		}
	}
}

TArray<UFluidNinjaBuoyancyComponent*> UFluidNinjaBuoyancySubsystem::GetBodies() const
{
	TArray<UFluidNinjaBuoyancyComponent*> Out;
	for (const FSlot& Slot : Slots)
	{
		if (UFluidNinjaBuoyancyComponent* Component = Slot.Component.Get())
		{
			Out.Add(Component);
		}
	}
	return Out;
}

bool UFluidNinjaBuoyancySubsystem::GetBodyDebug(const UFluidNinjaBuoyancyComponent* Component, FFluidNinjaBuoyancyBodyDebug& Out) const
{
	const FSlot* Slot = Slots.FindByPredicate([Component](const FSlot& Candidate) { return Candidate.Component.Get() == Component; });
	if (!Slot)
	{
		return false;
	}
	Out = FFluidNinjaBuoyancyBodyDebug();
	Out.Id = Slot->Id;
	Out.Level = Slot->Level;
	Out.bDormant = Slot->bDormant;
	Out.bHeld = Slot->bHeld;
	Out.bInCapture = Slot->bInCapture;
	Out.bFlat = Slot->bFlat;
	Out.bHasWaterZ = bControlledWater || Slot->bHasWaterZ || Slot->bFlat;
	Out.WaterZ = bControlledWater ? ControlledHeight : Slot->bHasWaterZ && Slot->bInCapture ? Slot->LastWaterZ : Slot->FlatHeight;
	Out.FarFieldCorrection = Slot->FarFieldCorrection;
	Out.bFarFieldLearned = Slot->bFarFieldLearned;
	Out.VerifyCount = Slot->VerifyCount;
	Out.VerifyMeanCm = Slot->VerifyCount > 0 ? static_cast<float>(Slot->VerifySum / Slot->VerifyCount) : 0.f;
	Out.VerifyMaxCm = Slot->VerifyMax;
	Out.VerifyFlowMeanCmS = Slot->VerifyCount > Slot->VerifyFlowOnlyGathered ? static_cast<float>(Slot->VerifyFlowSum / (Slot->VerifyCount - Slot->VerifyFlowOnlyGathered)) : 0.f;
	Out.VerifyFar = Slot->VerifyFar;
	Out.VerifyFlowOnlyGathered = Slot->VerifyFlowOnlyGathered;
	if (const FluidNinjaBuoyancy::FWaterPatch* Patch = Slot->Patch.Get())
	{
		Out.PatchAge = static_cast<float>(GetWorld()->GetTimeSeconds() - Patch->Time);
		Out.PatchPoints = Patch->NX * Patch->NY;
		Out.PatchSpacing = static_cast<float>(Patch->Texel);
	}
	return true;
}

void UFluidNinjaBuoyancySubsystem::Reregister(UFluidNinjaBuoyancyComponent* Component)
{
	// The hand-over to FluidNinja's tracking belongs to the body, not to its slot: keep it, so it can still be given back.
	FSlot Kept;
	if (const FSlot* Old = Slots.FindByPredicate([Component](const FSlot& Slot) { return Slot.Component.Get() == Component; }))
	{
		Kept.bHandedOver = Old->bHandedOver;
		Kept.bOverlapBefore = Old->bOverlapBefore;
		Kept.AddedTags = Old->AddedTags;
		Kept.bTrackingWarned = Old->bTrackingWarned;
		Kept.TrackingNote = Old->TrackingNote;
	}
	Slots.RemoveAll([Component](const FSlot& Slot) { return Slot.Component.Get() == Component; });
	Register(Component);
	if (FSlot* New = Slots.FindByPredicate([Component](const FSlot& Slot) { return Slot.Component.Get() == Component; }))
	{
		New->bHandedOver = Kept.bHandedOver;
		New->bOverlapBefore = Kept.bOverlapBefore;
		New->AddedTags = MoveTemp(Kept.AddedTags);
		New->bTrackingWarned = Kept.bTrackingWarned;
		New->TrackingNote = MoveTemp(Kept.TrackingNote);
	}
}

bool UFluidNinjaBuoyancySubsystem::IsGathering() const
{
	return UFluidNinjaBuoyancySettings::Get().GatherEnabled();
}

void UFluidNinjaBuoyancySubsystem::CalibrateFlow(bool bApply)
{
	// Where the player looks from (stand by fast water to measure it), else the first floating body in a simulation.
	FVector Where(SimCentre.X, SimCentre.Y, 0.0);
	if (const APlayerController* Controller = GetWorld()->GetFirstPlayerController(); Controller && Controller->PlayerCameraManager)
	{
		Where = Controller->PlayerCameraManager->GetCameraLocation();
	}
	if (FindFlowSource(FVector2D(Where)) == INDEX_NONE)
	{
		for (const FSlot& Slot : Slots)
		{
			if (const UPrimitiveComponent* Primitive = Slot.Component.IsValid() ? Slot.Component->GetSimulatedPrimitive() : nullptr)
			{
				Where = Primitive->Bounds.Origin;
				break;
			}
		}
	}
	CalibrateFlowAt(Where, bApply);
}

void UFluidNinjaBuoyancySubsystem::CalibrateFlowAt(const FVector& Where, bool bApply)
{
	const int32 SourceIndex = FindFlowSource(FVector2D(Where));
	const FFlowSource* Source = FlowSources.IsValidIndex(SourceIndex) ? &FlowSources[SourceIndex] : nullptr;
	UTextureRenderTarget2D* Velocity = Source ? Source->Buffer.Get() : nullptr;
	const EPixelFormat Format = Velocity ? Velocity->GetFormat() : PF_Unknown;
	if (!Velocity || Velocity->SizeX < FFluidNinjaBuoyancyFlowProbe::Side || Velocity->SizeY < FFluidNinjaBuoyancyFlowProbe::Side)
	{
		CalibrationReport = TEXT("Flow calibration: no FluidNinja simulation found (it needs a floating body in Play on FluidNinja water).");
	}
	else if (Format != PF_FloatRGBA && Format != PF_A32B32G32R32F)
	{
		CalibrationReport = TEXT("Flow calibration: the velocity buffer has no density channel to follow (RGBA float expected).");
	}
	else
	{
		TSharedPtr<FFluidNinjaBuoyancyFlowProbe, ESPMode::ThreadSafe> Probe = MakeShared<FFluidNinjaBuoyancyFlowProbe, ESPMode::ThreadSafe>();
		constexpr int32 Side = FFluidNinjaBuoyancyFlowProbe::Side;
		const double Column = ((Where.X - Source->Centre.X) / Source->SizeCm + 0.5) * Velocity->SizeX - 0.5;
		const double Row = ((Where.Y - Source->Centre.Y) / Source->SizeCm + 0.5) * Velocity->SizeY - 0.5;
		Probe->Min = FIntPoint(FMath::Clamp(FMath::RoundToInt32(Column - Side * 0.5), 0, Velocity->SizeX - Side),
			FMath::Clamp(FMath::RoundToInt32(Row - Side * 0.5), 0, Velocity->SizeY - Side));
		Probe->Buffer = Velocity;
		Probe->Format = Format;
		Probe->Resolution = Velocity->SizeX;
		Probe->SimSizeCm = Source->SizeCm;
		Probe->FlowScale = Source->FlowScale;
		Probe->Multiplier = UFluidNinjaBuoyancySettings::Get().FlowScaleMultiplier;
		Probe->SimSpeed = Source->SimSpeed;
		Probe->SamplingFPS = Source->SamplingFPS;
		Probe->FixedStepsPerSecond = Source->bFixedTick ? Source->StepsPerSecond : 0.f;
		Probe->bApply = bApply;
		Probe->Started = FPlatformTime::Seconds();
		FlowProbe = Probe;
		CalibrationReport = FString::Printf(TEXT("Flow calibration: measuring around (%.0f, %.0f)..."), Where.X, Where.Y);
	}
	UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("%s"), *CalibrationReport);
}

void UFluidNinjaBuoyancySubsystem::UpdateFlowProbe()
{
	if (!FlowProbe)
	{
		return;
	}
	FFluidNinjaBuoyancyFlowProbe& Probe = *FlowProbe;
	constexpr int32 Shots = FFluidNinjaBuoyancyFlowProbe::Shots;
	constexpr int32 Gap = FFluidNinjaBuoyancyFlowProbe::Gap;
	constexpr int32 N = FFluidNinjaBuoyancyFlowProbe::Side;
	auto Finish = [this](const FString& Report)
	{
		CalibrationReport = Report;
		UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("%s"), *CalibrationReport);
		FlowProbe.Reset();
	};
	if (FPlatformTime::Seconds() - Probe.Started > 30.0)
	{
		Finish(TEXT("Flow calibration: gave up (the readbacks did not come back in 30 s)."));
		return;
	}
	// Read what the GPU has finished (render thread), take one more shot a frame.
	bool bPending = false;
	for (const FFluidNinjaBuoyancyFlowProbe::FShot& Shot : Probe.Shot)
	{
		bPending |= Shot.Stage.load() == 1;
	}
	if (bPending)
	{
		ENQUEUE_RENDER_COMMAND(FluidNinjaBuoyancyProbeRead)([Hold = FlowProbe](FRHICommandListImmediate&)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FluidNinjaBuoyancy_FlowProbeLock);
			for (FFluidNinjaBuoyancyFlowProbe::FShot& Shot : Hold->Shot)
			{
				if (Shot.Stage.load() == 1 && Shot.Readback.IsReady())
				{
					ReadOut(*Hold, Shot);
				}
			}
		});
	}
	UTextureRenderTarget2D* Velocity = Probe.Buffer.Get();
	const FFlowSource* ProbeSource = FindFlowSourceFor(Velocity);
	if (Probe.Taken < Shots)
	{
		if (!Velocity || !ProbeSource || Velocity->SizeX != Probe.Resolution)
		{
			Finish(TEXT("Flow calibration: the velocity buffer went away."));
			return;
		}
		const int32 Index = Probe.Taken++;
		FFluidNinjaBuoyancyFlowProbe::FShot& Shot = Probe.Shot[Index];
		Shot.Time = GetWorld()->GetTimeSeconds();
		Shot.Frame = GFrameCounter;
		Shot.SimCentre = ProbeSource->Centre;
		Shot.SmoothedDeltaTime = SmoothedDeltaTime;
		Shot.Stage = 1;
		FTextureRenderTargetResource* Resource = Velocity->GameThread_GetRenderTargetResource();
		ENQUEUE_RENDER_COMMAND(FluidNinjaBuoyancyProbeCopy)([Hold = FlowProbe, Index, Resource](FRHICommandListImmediate& RHICmdList)
		{
			FFluidNinjaBuoyancyFlowProbe::FShot& Shot = Hold->Shot[Index];
			// As the gather: FluidNinja's buffer may still be its 1 x 1 placeholder on the GPU.
			FRHITexture* Source = Resource ? Resource->GetRenderTargetTexture() : nullptr;
			const FIntVector Extent = Source ? Source->GetSizeXYZ() : FIntVector::ZeroValue;
			if (!Source || Hold->Min.X < 0 || Hold->Min.Y < 0 || Hold->Min.X + FFluidNinjaBuoyancyFlowProbe::Side > Extent.X
				|| Hold->Min.Y + FFluidNinjaBuoyancyFlowProbe::Side > Extent.Y)
			{
				Shot.Stage = -1;
				return;
			}
			Shot.Readback.EnqueueCopy(RHICmdList, Source, FIntVector(Hold->Min.X, Hold->Min.Y, 0), 0, FIntVector(FFluidNinjaBuoyancyFlowProbe::Side, FFluidNinjaBuoyancyFlowProbe::Side, 1));
		});
	}
	for (const FFluidNinjaBuoyancyFlowProbe::FShot& Shot : Probe.Shot)
	{
		if (Shot.Stage.load() < 0)
		{
			Finish(TEXT("Flow calibration: the velocity buffer could not be copied."));
			return;
		}
		if (Shot.Stage.load() != 2)
		{
			return;   // still coming
		}
	}

	// The tracer: FluidNinja's dye (density); where there is none to follow, the pattern of speeds.
	const double Cell = Probe.SimSizeCm / Probe.Resolution;
	constexpr int32 Half = 24;
	constexpr int32 Search = 48;
	constexpr int32 Margin = Search + 3 + Half;
	TArray<float> Tracer[Shots];
	auto Fill = [&](bool bDensity)
	{
		for (int32 S = 0; S < Shots; ++S)
		{
			Tracer[S].SetNumUninitialized(N * N);
			for (int32 K = 0; K < N * N; ++K)
			{
				const FVector4f& Texel = Probe.Shot[S].Texels[K];
				Tracer[S][K] = bDensity ? Texel.Z : FVector2f(Texel.X, Texel.Y).Size();
			}
		}
	};
	struct FWindow
	{
		FIntPoint Centre;
		double Spread = 0.0;
		double Speed = 0.0;
	};
	auto FindWindows = [&]()
	{
		TArray<FWindow> Out;
		for (int32 Y = Margin; Y < N - Margin; Y += 16)
		{
			for (int32 X = Margin; X < N - Margin; X += 16)
			{
				double Sum = 0.0;
				double SumSq = 0.0;
				FVector2D Velocity2 = FVector2D::ZeroVector;
				int32 Count = 0;
				for (int32 DY = -Half; DY < Half; DY += 2)
				{
					for (int32 DX = -Half; DX < Half; DX += 2)
					{
						const int32 K = (Y + DY) * N + X + DX;
						const double Value = Tracer[0][K];
						Sum += Value;
						SumSq += Value * Value;
						Velocity2 += FVector2D(Probe.Shot[0].Texels[K].X, Probe.Shot[0].Texels[K].Y);
						++Count;
					}
				}
				Out.Add({FIntPoint(X, Y), FMath::Sqrt(FMath::Max(0.0, SumSq / Count - FMath::Square(Sum / Count))), (Velocity2 / Count).Size()});
			}
		}
		return Out;
	};
	Fill(true);
	TArray<FWindow> Windows = FindWindows();
	double MostSpread = 0.0;
	for (const FWindow& Window : Windows)
	{
		MostSpread = FMath::Max(MostSpread, Window.Spread);
	}
	const TCHAR* TracerName = TEXT("dye");
	if (MostSpread < 1e-3)
	{
		Fill(false);
		Windows = FindWindows();
		TracerName = TEXT("speed pattern");
	}
	// Structure where the water moves: the six with the most, among windows at least a fifth as fast as the fastest.
	double Fastest = 0.0;
	for (const FWindow& Window : Windows)
	{
		Fastest = FMath::Max(Fastest, Window.Speed);
	}
	Windows.RemoveAll([Fastest](const FWindow& Window) { return Window.Speed < Fastest * 0.2; });
	Windows.Sort([](const FWindow& A, const FWindow& B) { return A.Spread > B.Spread; });
	Windows.SetNum(FMath::Min(Windows.Num(), 6));

	// Each window followed across each pair of consecutive shots, against the buffer's velocity there.
	double ShiftDotVelocity = 0.0;
	double VelocityDotVelocity = 0.0;
	double SpeedDotVelocity = 0.0;
	double AlongU[2] = {};
	double AlongV[2] = {};
	double CosineSum = 0.0;
	double ScoreSum = 0.0;
	double ElapsedSum = 0.0;
	int32 Cosines = 0;
	int32 Matches = 0;
	int32 Tried = 0;
	int32 Pairs = 0;
	double ShiftSum = 0.0;
	for (int32 S = 0; S + Gap < Shots; ++S)
	{
		const FFluidNinjaBuoyancyFlowProbe::FShot& A = Probe.Shot[S];
		const FFluidNinjaBuoyancyFlowProbe::FShot& B = Probe.Shot[S + Gap];
		const double Elapsed = B.Time - A.Time;
		if (Elapsed <= 1e-4)
		{
			continue;
		}
		++Pairs;
		ElapsedSum += Elapsed;
		// The simulation recentres on the player: its content keeps its place in the world.
		const FVector2D Recentre = (B.SimCentre - A.SimCentre) / Cell;
		for (const FWindow& Window : Windows)
		{
			++Tried;
			FVector2D Shift;
			double Score = 0.0;
			if (!Track(Tracer[S], Tracer[S + Gap], N, Window.Centre, Half, Search, Shift, Score) || Score < 0.6)
			{
				continue;
			}
			Shift += Recentre;
			FVector2D Velocity2 = FVector2D::ZeroVector;
			int32 Count = 0;
			for (int32 DY = -Half; DY < Half; DY += 2)
			{
				for (int32 DX = -Half; DX < Half; DX += 2)
				{
					const int32 K = (Window.Centre.Y + DY) * N + Window.Centre.X + DX;
					Velocity2 += FVector2D(A.Texels[K].X, A.Texels[K].Y) + FVector2D(B.Texels[K].X, B.Texels[K].Y);
					Count += 2;
				}
			}
			Velocity2 /= Count;
			ShiftDotVelocity += Shift | Velocity2;
			VelocityDotVelocity += Velocity2 | Velocity2;
			SpeedDotVelocity += (Shift * Cell / Elapsed) | Velocity2;
			AlongU[0] += Shift.X * Velocity2.X;
			AlongU[1] += Velocity2.X * Velocity2.X;
			AlongV[0] += Shift.Y * Velocity2.Y;
			AlongV[1] += Velocity2.Y * Velocity2.Y;
			if (Shift.Size() > 0.3 && Velocity2.Size() > 1e-3)
			{
				CosineSum += (Shift.GetSafeNormal() | Velocity2.GetSafeNormal());
				++Cosines;
			}
			ScoreSum += Score;
			ShiftSum += Shift.Size();
			++Matches;
		}
	}
	if (Matches == 0 || VelocityDotVelocity <= 1e-12 || Pairs == 0)
	{
		Finish(FString::Printf(TEXT("Flow calibration: nothing to follow (%d of %d windows matched; the water around (%d, %d) in the buffer may be still or featureless)."),
			Matches, Tried, Probe.Min.X + N / 2, Probe.Min.Y + N / 2));
		return;
	}
	const double FrameSeconds = ElapsedSum / Pairs / Gap;
	const double CellsPerFrame = ShiftDotVelocity / VelocityDotVelocity / Gap;      // per buffer unit
	const double MeasuredPerUnit = SpeedDotVelocity / VelocityDotVelocity;          // cm/s per buffer unit
	const double Cycles = Probe.FixedStepsPerSecond > 0.f ? static_cast<double>(Probe.FixedStepsPerSecond)
		: FMath::Min(static_cast<double>(Probe.SamplingFPS), 1.0 / FrameSeconds);   // simulation steps per second, as UpdateFlowSources
	const double ModelPerUnit = Probe.SimSpeed * Cell * Cycles;                    // without the multiplier
	const double ModelCellsPerFrame = Probe.SimSpeed * Cycles * FrameSeconds;
	const double Suggested = ModelPerUnit > 0.0 ? MeasuredPerUnit / ModelPerUnit : 0.0;
	const double Agreement = Cosines > 0 ? CosineSum / Cosines : 0.0;
	const double MeanShift = ShiftSum / Matches;   // texels between compared shots
	const double RatioU = AlongU[1] > 0.0 ? AlongU[0] / AlongU[1] / Gap : 0.0;
	const double RatioV = AlongV[1] > 0.0 ? AlongV[0] / AlongV[1] / Gap : 0.0;
	// Both axes are judged only where the water moves along both (a ratio along a barely moving axis is noise).
	const bool bBothAxes = AlongU[1] > 0.2 * VelocityDotVelocity && AlongV[1] > 0.2 * VelocityDotVelocity;
	const bool bAxesAgree = !bBothAxes || FMath::Abs(RatioU - RatioV) < 0.25 * FMath::Max(FMath::Abs(RatioU), FMath::Abs(RatioV));
	FString Why;
	if (Matches < 6) { Why = TEXT("too few matches"); }
	else if (Agreement <= 0.8) { Why = TEXT("the dye does not move the way the buffer says"); }
	else if (MeanShift < 1.5) { Why = TEXT("the dye moved less than 1.5 texels (still water: measure where it runs)"); }
	else if (!bAxesAgree) { Why = TEXT("the two axes disagree"); }
	else if (Suggested <= 0.05 || Suggested >= 20.0) { Why = TEXT("implausible result"); }
	const bool bGood = Why.IsEmpty();
	FString Report = FString::Printf(TEXT("Flow calibration (%s, %d of %d matches, correlation %.2f, direction agreement %.2f, moved %.1f texels in %d frames, axes u %.2f v %.2f cells/frame per unit): ")
		TEXT("moves %.2f cells a frame per buffer unit (model %.2f) at %.0f fps, %.0f simulation steps/s%s = %.0f cm/s per unit (model %.0f). Flow Scale Multiplier %.2f matches it (now %.2f)"),
		TracerName, Matches, Tried, ScoreSum / Matches, Agreement, MeanShift, Gap, RatioU, RatioV,
		CellsPerFrame, ModelCellsPerFrame, 1.0 / FrameSeconds, Cycles, Probe.FixedStepsPerSecond > 0.f ? TEXT(" (NinjaLiveCore fixed tick)") : TEXT(""),
		MeasuredPerUnit, ModelPerUnit, Suggested, Probe.Multiplier);
	if (!bGood)
	{
		Report += FString::Printf(TEXT("; NOT TRUSTED: %s"), *Why);
	}
	else if (Probe.bApply)
	{
		UFluidNinjaBuoyancySettings* Settings = &UFluidNinjaBuoyancySettings::GetMutable();
		Settings->FlowScaleMultiplier = static_cast<float>(FMath::RoundToDouble(Suggested * 100.0) / 100.0);
#if WITH_EDITOR
		if (const FProperty* Property = UFluidNinjaBuoyancySettings::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UFluidNinjaBuoyancySettings, FlowScaleMultiplier)))
		{
			Settings->UpdateSinglePropertyInConfigFile(Property, Settings->GetDefaultConfigFilename());
		}
#endif
		Report += FString::Printf(TEXT("; applied %.2f"), Settings->FlowScaleMultiplier);
	}
	Finish(Report);
}

void UFluidNinjaBuoyancySubsystem::DrawDebug()
{
	UWorld* World = GetWorld();
	FString Text = Status;
	for (const FSlot& Slot : Slots)
	{
		const UFluidNinjaBuoyancyComponent* Component = Slot.Component.Get();
		if (!Component)
		{
			continue;
		}
		Text += TEXT("\n") + Component->GetStatus();
		if (const FluidNinjaBuoyancy::FWaterPatch* Patch = Slot.Patch.Get())
		{
			const FVector Min(Patch->Origin.X, Patch->Origin.Y, Slot.LastWaterZ);
			const FVector Max(Patch->Origin.X + (Patch->NX - 1) * Patch->Texel, Patch->Origin.Y + (Patch->NY - 1) * Patch->Texel, Slot.LastWaterZ);
			DrawDebugBox(World, (Min + Max) * 0.5, (Max - Min) * 0.5 + FVector(0.0, 0.0, 1.0), FColor(60, 160, 255), false, -1.f, 0, 1.5f);
		}
		if (const FFluidNinjaBuoyancyBodyOutput* Output = Slot.LastOutput.Get())
		{
			const TArray<FVector>& Line = Output->Result.Waterline;
			for (int32 Index = 0; Index + 1 < Line.Num(); Index += 2)
			{
				DrawDebugLine(World, Line[Index], Line[Index + 1], FColor(0, 255, 255), false, -1.f, 0, 2.f);
			}
			const float Weight = FMath::Max(Output->Mass * 980.f, 1.f);
			DrawDebugDirectionalArrow(World, Output->CenterOfMass, Output->CenterOfMass + Output->Result.Force / Weight * 100.0, 20.f, FColor::Yellow, false, -1.f, 0, 2.f);
			if (Output->Result.DisplacedVolume > 0.f)
			{
				DrawDebugPoint(World, Output->Result.BuoyancyCentre, 10.f, FColor::Green);
			}
			DrawDebugPoint(World, Output->CenterOfMass, 10.f, FColor::Red);
			for (const FFluidNinjaWaterlineContact& Contact : Component->GetContacts())
			{
				const float Drive = Contact.Push + Contact.Plunge;
				const bool bWaking = Drive >= Component->WakeMinPushCmS;
				DrawDebugLine(World, Contact.Position, Contact.Position + Contact.Outward * FMath::Clamp(Drive, 0.f, 300.f) * 0.3,
					bWaking ? FColor::Orange : FColor(90, 90, 90), false, -1.f, 0, bWaking ? 3.f : 1.f);
			}
		}
	}
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(static_cast<uint64>(0x1F5A000), 0.f, FColor(150, 210, 255), Text);
	}
}

void UFluidNinjaBuoyancySubsystem::UpdateSimSquare()
{
	// Where the river simulation is (FluidNinja's parameter collection; it follows the player).
	bSimValid = false;
	UWorld* World = GetWorld();
	UMaterialParameterCollectionInstance* Instance = NinjaCollection && World ? World->GetParameterCollectionInstance(NinjaCollection) : nullptr;
	FLinearColor Position;
	FLinearColor Size;
	if (Instance && Instance->GetVectorParameterValue(TEXT("TraceMeshPos"), Position) && Instance->GetVectorParameterValue(TEXT("TraceMeshSize"), Size))
	{
		FLinearColor Offset = FLinearColor::Transparent;
		Instance->GetVectorParameterValue(TEXT("TraceMeshPosLWCOffset"), Offset);
		SimCentre = FVector2D(Position.R + Offset.R, Position.G + Offset.G);
		SimSizeCm = Size.R * 100.0;
		bSimValid = SimSizeCm > 100.0;
	}
}

FFluidNinjaBuoyancySwimmers& UFluidNinjaBuoyancySubsystem::EnsureSwimmers()
{
	if (!Swimmers)
	{
		Swimmers = MakeShared<FFluidNinjaBuoyancySwimmers>(*this);
		SwimmerEntryHandle = Swimmers->OnEntry.AddUObject(this, &UFluidNinjaBuoyancySubsystem::OnSwimmerDive);
	}
	return *Swimmers;
}

void UFluidNinjaBuoyancySubsystem::TickSwimmerParts(float DeltaSeconds)
{
	FFluidNinjaBuoyancySwimmers& Parts = EnsureSwimmers();
	// The player's pawn: its swimmer provider when it has one (the pawn or a component implementing FluidNinjaSwimmerProvider), else the
	// default pose from its body mesh and Character Movement — while it swims, or is in the air over its water (jumping in).
	FFluidNinjaSwimmerPose Pose;
	const FFluidNinjaSwimmerPose* PlayerPose = nullptr;
	APawn* Pawn = Swimmer.Get();
	if (Pawn && bSwimmerSurfaceKnown && SwimmerSurfaceFrame == GFrameCounter
		&& UFluidNinjaSwimmerProviderLibrary::GetPoseFor(Pawn, PlayerSwimmerMesh, Pose) && (Pose.bSwimming || Pose.bAirborne))
	{
		PlayerPose = &Pose;
	}
	Parts.Tick(DeltaSeconds, PlayerPose, SwimmerSurfaceZ);
}

void UFluidNinjaBuoyancySubsystem::OnSwimmerDive(const USkeletalMeshComponent* Mesh, const FVector& Where, float SpeedCmS, float MassShareKg)
{
	// The dive's splash: the whole body's entry power (its mass share going in at the fall speed over about a body's depth of entry,
	// m·v³ / 2h with h = 0.6 m), on the project's splash scale, as a hull's entry splash is sized. The parts' entries already made their
	// own splashes and rings; this is the one big splash a dive adds. The effects side binds the rest of the entry package (cavity,
	// crown, bubbles, sound) to the same event, on the FluidNinja Swimmer component's On Entry.
	UWorld* World = GetWorld();
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	if (!World || MassShareKg <= 0.f || SpeedCmS <= 0.f)
	{
		return;
	}
	const double SpeedMs = SpeedCmS * 0.01;
	const double Watts = MassShareKg * SpeedMs * SpeedMs * SpeedMs / (2.0 * 0.6);
	const float Strength = Settings.SplashStrength(static_cast<float>(Watts)) * FMath::Max(Settings.SwimmerSplashScale, 0.f);
	if (Strength < 0.2f)
	{
		return;
	}
	if (UNiagaraSystem* Splash = Cast<UNiagaraSystem>(Settings.SplashSystem.TryLoad()))
	{
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(World, Splash, Where, FRotator::ZeroRotator, FVector(Strength), true, true, ENCPoolMethod::AutoRelease);
	}
}

bool UFluidNinjaBuoyancySubsystem::SampleWater(const FVector& Location, FFluidNinjaWaterSample& OutSample)
{
	OutSample = FFluidNinjaWaterSample();
	const UWorld* World = GetWorld();
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	if (!World || Location.ContainsNaN() || !Settings.IsEnabled())
	{
		return false;
	}
	if (bControlledWater)
	{
		// The test bench's still water: flat, no waves, no current, as the floating bodies feel it.
		OutSample.bWater = true;
		OutSample.StillZ = OutSample.SurfaceZ = ControlledHeight;
		OutSample.Depth = ControlledHeight - static_cast<float>(Location.Z);
		return true;
	}
	const double Now = World->GetTimeSeconds();

	// The still water: the nearest body's patch holding the point (FluidNinja's surface as drawn, carried forward by its rate, exactly
	// what that body floats on), else the far field (a water volume's top; a project's FarFieldTop may answer from its published water).
	double BestDistance = TNumericLimits<double>::Max();
	for (const FSlot& Slot : Slots)
	{
		const FluidNinjaBuoyancy::FWaterPatch* Patch = Slot.bInCapture ? Slot.Patch.Get() : nullptr;
		if (!Patch || !Patch->Contains(Location.X, Location.Y))
		{
			continue;
		}
		const UFluidNinjaBuoyancyComponent* Component = Slot.Component.Get();
		const UPrimitiveComponent* Primitive = Component ? Component->GetSimulatedPrimitive() : nullptr;
		const double Distance = Primitive ? FVector::DistSquared2D(Primitive->Bounds.Origin, Location) : TNumericLimits<double>::Max() * 0.5;
		if (Distance >= BestDistance)
		{
			continue;
		}
		float Height = 0.f;
		float Rate = 0.f;
		FVector2f Flow;
		if (Patch->Sample(Location.X, Location.Y, Height, Rate, Flow))
		{
			BestDistance = Distance;
			OutSample.StillZ = Height + Rate * FMath::Clamp(static_cast<float>(Now - Patch->Time), 0.f, Settings.MaxExtrapolationSeconds);
			OutSample.bWater = true;
			OutSample.bFromPatch = true;
		}
	}
	if (!OutSample.bWater)
	{
		float Top = 0.f;
		if (FarFieldTop(FBox(Location - FVector(1.0, 1.0, 400.0), Location + FVector(1.0, 1.0, 400.0)), Top))
		{
			OutSample.StillZ = Top;
			OutSample.bWater = true;
		}
	}
	if (!OutSample.bWater)
	{
		return false;
	}

	// The current and the waves around the point, from the probe there (asked fresh each call; the reading is two or three frames old).
	// The waves are the free height η = B − A: the drawn wave height less every body's static head, so a swimmer rides a hull's waves
	// without standing in its hollow.
	const int32 Index = RequestCurrent(FVector2D(Location), Settings.SwimmerCurrentRingInnerCm, Settings.SwimmerCurrentRingOuterCm);
	if (const FCurrentProbe* Probe = CurrentProbes.IsValidIndex(Index) ? &CurrentProbes[Index] : nullptr; Probe && !Probe->bOutside && Probe->ReadTime >= 0.0)
	{
		OutSample.AgeSeconds = static_cast<float>(Now - Probe->ReadTime);
		OutSample.Current = FVector(Probe->Current.X, Probe->Current.Y, 0.0);
		OutSample.bHasCurrent = true;
		if (Probe->bWave)
		{
			OutSample.WaveHeight = Probe->WaveHeight;
			OutSample.Normal = FVector(-Probe->WaveGradient.X, -Probe->WaveGradient.Y, 1.0).GetSafeNormal();
			OutSample.bHasWaves = true;
		}
	}
	OutSample.SurfaceZ = OutSample.StillZ + OutSample.WaveHeight;
	OutSample.Depth = OutSample.SurfaceZ - static_cast<float>(Location.Z);
	return true;
}

void UFluidNinjaBuoyancySubsystem::RegisterSwimmer(USkeletalMeshComponent* Mesh, FFluidNinjaSwimmerSource Source)
{
	EnsureSwimmers().Register(Mesh, Source);
}

void UFluidNinjaBuoyancySubsystem::UnregisterSwimmer(USkeletalMeshComponent* Mesh)
{
	if (Swimmers)
	{
		Swimmers->Unregister(Mesh);
	}
}

bool UFluidNinjaBuoyancySubsystem::GetSwimmerStats(const USkeletalMeshComponent* Mesh, FFluidNinjaSwimmerStats& OutStats) const
{
	return Swimmers && Swimmers->GetStats(Mesh, OutStats);
}

void UFluidNinjaBuoyancySubsystem::AddWaterImpulse(FVector Location, float ImpulseDown, float AreaCm2)
{
	// As a pressure head pressing for one of the simulation's ticks over a Gaussian as the hull impulses are splatted (three times the σ
	// of a disc of its area, at least Wave Source Smooth and a wave cell wide): the impulse ρ·g·H·Δt·2πσ² it puts in is ImpulseDown.
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	if (!Waves || !FMath::IsFinite(ImpulseDown) || ImpulseDown == 0.f || !Settings.bWaveImpulse)
	{
		return;
	}
	const double Cell = Waves->GetCellCm() > 0.0 ? Waves->GetCellCm() : 5.0;
	const double Sigma = FMath::Max3(3.0 * 0.5 * FMath::Sqrt(FMath::Max(static_cast<double>(AreaCm2), 1.0) / UE_DOUBLE_PI),
		static_cast<double>(FMath::Max(Settings.WaveSourceSmoothCm, 0.f)), Cell);
	const double TickSeconds = 1.0 / FMath::Max(Waves->GetStepsPerSecond(), 1.0);
	const double Density = Settings.WaterDensity * 1.0e-6;   // kg/m³ to kg/cm³
	const double Gravity = GetWorld() ? FMath::Max(-GetWorld()->GetGravityZ(), 1.0) : 980.0;
	const double Head = ImpulseDown * FMath::Max(Settings.WaveImpulseScale, 0.f) / (Density * Gravity * TickSeconds * UE_DOUBLE_TWO_PI * Sigma * Sigma);
	Waves->Poke(FVector2D(Location), static_cast<float>(Head), static_cast<float>(Sigma));
}

void UFluidNinjaBuoyancySubsystem::AddWavePoke(FVector2D WorldXY, float HeightCm, float SigmaCm)
{
	if (Waves && FMath::IsFinite(HeightCm) && HeightCm != 0.f)
	{
		Waves->Poke(WorldXY, HeightCm, SigmaCm);
	}
}

void UFluidNinjaBuoyancySubsystem::UpdateSwimmer(float DeltaSeconds)
{
	// The local player's pawn and the water at it (a water volume's top): the swimmers' body parts and FluidNinja Waves' swimmer read it.
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	UWorld* World = GetWorld();
	const APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
	APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
	if (Pawn != Swimmer.Get())
	{
		Swimmer = Pawn;
		bSwimmerInWater = false;
	}
	bSwimmerSurfaceKnown = false;
	if (!Pawn || !Settings.IsEnabled())
	{
		bSwimmerInWater = bSwimmerSwimming = bSwimmerWading = false;
		return;
	}
	// In the water: swimming (its Character Movement swims), or wading with water at the feet.
	const UCharacterMovementComponent* Movement = Pawn->FindComponentByClass<UCharacterMovementComponent>();
	const bool bSwimming = Movement && Movement->IsSwimming();
	const FVector Location = Pawn->GetActorLocation();
	const UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(Pawn->GetRootComponent());
	const double HalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : Pawn->GetSimpleCollisionHalfHeight();
	const double FeetZ = Location.Z - HalfHeight;
	float SurfaceZ = 0.f;
	const bool bWater = FarFieldTop(FBox(Location - FVector(1.0, 1.0, HalfHeight + 300.0), Location + FVector(1.0)), SurfaceZ);
	SwimmerSurfaceZ = SurfaceZ;
	bSwimmerSurfaceKnown = bWater;
	SwimmerSurfaceFrame = GFrameCounter;
	constexpr double WadeMinDepthCm = 20.0;
	const bool bWading = !bSwimming && bWater && SurfaceZ - FeetZ >= WadeMinDepthCm && SurfaceZ < Location.Z + HalfHeight;
	bSwimmerInWater = bSwimming || bWading;
	bSwimmerSwimming = bSwimming;
	bSwimmerWading = bWading;
}

UFluidNinjaBuoyancyWaves* UFluidNinjaBuoyancySubsystem::GetWaves() const
{
	return Waves;
}

void UFluidNinjaBuoyancySubsystem::TickWaves(float DeltaSeconds, bool bIdle)
{
	UWorld* World = GetWorld();
	if (!Waves || !World)
	{
		PendingWaveImpulses.Reset();
		return;
	}
	const double Now = World->GetTimeSeconds();
	// Without floating bodies the capture did not refresh the drawn surfaces or where the simulations are: the swimmer's and the test
	// bench's waves need both (the simulations they are made in; the Surface Mirror they would be measured against is not drawn then).
	if (bIdle && UFluidNinjaBuoyancySettings::Get().bWaves)
	{
		if (Now >= NextSourceRefresh)
		{
			NextSourceRefresh = Now + SourceRefreshSeconds;
			RefreshSources();
		}
		UpdateSimSquare();
		UpdateFlowSources();
		Waves->BeginFrame();
	}

	FFluidNinjaWaveWorldInput Input;
	// The simulations FluidNinja Buoyancy found: each one's NinjaLiveCore, its pressure buffer and its square.
	TMap<int32, int32> SimIndex;   // flow source -> its index in Input.Sims
	SimIndex.Reserve(FlowSources.Num());
	Input.Sims.Reserve(FlowSources.Num());
	for (int32 Index = 0; Index < FlowSources.Num(); ++Index)
	{
		const FFlowSource& Source = FlowSources[Index];
		if (!Source.Ninja.IsValid())
		{
			continue;
		}
		if (!Source.Core.IsValid())
		{
			FlowSources[Index].Core = FindNinjaCore(Source.Ninja.Get());
		}
		FFluidNinjaWaveSimInput& Sim = Input.Sims.AddDefaulted_GetRef();
		Sim.Ninja = Source.Ninja;
		Sim.Core = Source.Core;
		Sim.Pressure = Source.Pressure;
		Sim.Centre = Source.Centre;
		Sim.SizeCm = Source.SizeCm;
		Sim.bValid = Source.bValid;
		SimIndex.Add(Index, Input.Sims.Num() - 1);
	}
	// The bodies: pose, hull, velocity, the simulation under each, and the water each is measured against where the Surface Mirror shows
	// none under it.
	Input.Bodies.Reserve(Slots.Num());
	for (const FSlot& Slot : Slots)
	{
		UFluidNinjaBuoyancyComponent* Component = Slot.Component.Get();
		const UPrimitiveComponent* Primitive = Component ? Component->GetSimulatedPrimitive() : nullptr;
		if (!Primitive)
		{
			continue;
		}
		if (WavesRefreshRestingDraft != 0)
		{
			Component->RefreshRestingDraft();   // the waves cap a hull's static head by its resting draft: at the body's mass now
		}
		FFluidNinjaWaveBodyInput& Body = Input.Bodies.AddDefaulted_GetRef();
		Body.Id = Slot.Id;
		Body.Component = Component;
		Body.Hull = Component->GetWaveHull();
		Body.Transform = FTransform(Primitive->GetComponentQuat(), Primitive->GetComponentLocation());
		Body.Velocity = Primitive->GetPhysicsLinearVelocity();
		Body.AngularVelocity = Primitive->GetPhysicsAngularVelocityInRadians();
		Body.Radius = Primitive->Bounds.SphereRadius;
		if (bControlledWater)
		{
			Body.WaterZ = ControlledHeight;
			Body.WaterSource = EFluidNinjaWaveWater::Controlled;
		}
		else if (Slot.bInCapture && Slot.bHasWaterZ)
		{
			Body.WaterZ = Slot.LastWaterZ;
			Body.WaterSource = EFluidNinjaWaveWater::Captured;
		}
		else if (Slot.bFlat)
		{
			Body.WaterZ = Slot.FlatHeight;
			Body.WaterSource = EFluidNinjaWaveWater::Published;
		}
		Body.bWaves = Component->bWaves;
		Body.bAsleep = Slot.bDormant || Slot.bHeld;
		Body.TeleportCounter = Component->GetTeleportCounter();
		const int32 Source = FindFlowSource(FVector2D(Primitive->Bounds.Origin));
		const int32* Found = SimIndex.Find(Source);
		Body.Sim = Found ? *Found : INDEX_NONE;
	}

	// The Surface Mirror, when it was drawn this frame (with the waves' height left out of it: FluidNinja's own surface W).
	if (bCapturedThisFrame && CaptureTarget)
	{
		Input.Mirror.Target = CaptureTarget;
		Input.Mirror.Origin = CaptureOrigin;
		Input.Mirror.Top = CaptureTop;
		Input.Mirror.Far = CaptureRange - 1.0;
		Input.Mirror.Texel = CaptureTexel;
		Input.Mirror.Resolution = CaptureResolution;
	}
	Input.bControlled = bControlledWater;
	Input.ControlledHeight = ControlledHeight;
	// The surfaces' actors, each once, in the order first met (a river has hundreds of spline pieces on a few actors: a set keeps this
	// from a search of the list per piece).
	TSet<const AActor*, DefaultKeyFuncs<const AActor*>, TInlineSetAllocator<32>> SeenActors;
	SeenActors.Reserve(LastSurfaceActors);
	Input.SurfaceActors.Reserve(LastSurfaceActors);
	for (const TWeakObjectPtr<UPrimitiveComponent>& Surface : SurfacePrimitives)
	{
		if (const UPrimitiveComponent* SurfacePrimitive = Surface.Get())
		{
			AActor* Owner = SurfacePrimitive->GetOwner();
			bool bSeen = false;
			SeenActors.Add(Owner, &bSeen);
			if (!bSeen)
			{
				Input.SurfaceActors.Add(Owner);
			}
		}
	}
	LastSurfaceActors = Input.SurfaceActors.Num();
	Input.Surfaces.Reserve(SurfaceSources.Num());
	for (const TPair<TWeakObjectPtr<UPrimitiveComponent>, int32>& Shown : SurfaceSources)
	{
		const int32* Found = SimIndex.Find(Shown.Value);
		Input.Surfaces.Emplace(Shown.Key, Found ? *Found : INDEX_NONE);
	}

	// The camera (the test bench's window is the simulation around it), and the player's pawn: its capsule pushes the water aside. A
	// capsule without collision (seated in a boat, a boarding clip) is not in the water as a body of its own.
	const APlayerController* Controller = World->GetFirstPlayerController();
	if (Controller && Controller->PlayerCameraManager)
	{
		Input.Camera = Controller->PlayerCameraManager->GetCameraLocation();
		Input.bHasCamera = true;
	}
	APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
	const UCapsuleComponent* Capsule = Pawn ? Cast<UCapsuleComponent>(Pawn->GetRootComponent()) : nullptr;
	if (Capsule && Capsule->IsCollisionEnabled())
	{
		Input.Swimmer.bValid = true;
		Input.Swimmer.Pawn = Pawn;
		Input.Swimmer.Centre = Capsule->GetComponentLocation();
		Input.Swimmer.Radius = Capsule->GetScaledCapsuleRadius();
		Input.Swimmer.HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
		Input.Swimmer.Velocity = Pawn->GetVelocity();
		// The water UpdateSwimmer found this frame. When it stopped before looking (no swim settings published, say), none: the swimmer is
		// then measured against the Surface Mirror alone, where that shows water under it.
		if (SwimmerSurfaceFrame == GFrameCounter)
		{
			Input.Swimmer.WaterZ = SwimmerSurfaceZ;
			Input.Swimmer.bHasWaterZ = bSwimmerSurfaceKnown;
		}
	}

	// Registered swimmers' head, chest and hips (FFluidNinjaBuoyancySwimmers, ticked before this): spheres pushing the water aside.
	if (Swimmers)
	{
		Input.Puppets.Reserve(Swimmers->GetSpheres().Num());
		for (const FFluidNinjaSwimmerSphere& Sphere : Swimmers->GetSpheres())
		{
			FFluidNinjaWaveSwimmerInput& Puppet = Input.Puppets.AddDefaulted_GetRef();
			Puppet.bValid = true;
			Puppet.Source = Sphere.Mesh.Get();
			Puppet.Part = Sphere.Part;
			Puppet.Centre = Sphere.Centre;
			Puppet.Radius = Sphere.Radius;
			Puppet.HalfHeight = Sphere.Radius;
			Puppet.Velocity = Sphere.Velocity;
			Puppet.WaterZ = Sphere.WaterZ;
			Puppet.bHasWaterZ = true;
		}
	}

	// Every physics step's impulses since last frame, per body (and how many each had, for next frame's collection).
	Input.Impulses.Reserve(PendingWaveImpulses.Num());
	for (TPair<int32, TArray<FVector4f>>& Entry : PendingWaveImpulses)
	{
		if (FSlot* Slot = FindSlot(Entry.Key))
		{
			Slot->LastImpulsePieces = Entry.Value.Num();
		}
		FFluidNinjaWaveBodyImpulses& Pieces = Input.Impulses.AddDefaulted_GetRef();
		Pieces.Id = Entry.Key;
		Pieces.Pieces = MoveTemp(Entry.Value);
	}
	PendingWaveImpulses.Reset();

	Waves->Tick(DeltaSeconds, Input);
	if (bBuildStatus)
	{
		Status += TEXT("\n  ") + Waves->GetStatusLine();
	}
}

void UFluidNinjaBuoyancySubsystem::GetCosts(FFluidNinjaBuoyancyCosts& Out) const
{
	Out.TickMs = TickMs;
	Out.PatchMs = PatchMs;
	Out.PreTickMs = PreTickMs;
	Out.PhysicsMs = PhysicsMsPerFrame;
	Out.PhysicsMsPerStep = PhysicsMsPerStep;
	Out.FrameTickMs = FrameTickMs;
	Out.FramePatchMs = FramePatchMs;
	Out.FramePreTickMs = FramePreTickMs;
	Out.FramePhysicsMs = FramePhysicsMs;
	Out.ReadbackAgeFrames = ReadbackAgeFrames;
}

double UFluidNinjaBuoyancySubsystem::ReportMemory(TArray<FFluidNinjaBuoyancyMemoryItem>& OutItems) const
{
	constexpr double MB = 1.0 / (1024.0 * 1024.0);
	const auto Add = [&OutItems](const FString& Name, const FIntPoint& Size, const FString& Format, double Bytes)
	{
		OutItems.Add({Name, Size, Format, Bytes * MB});
	};
	// The Surface Mirror: the depth capture (R32F) the gather and the copies read the water's height from.
	if (CaptureTarget)
	{
		Add(TEXT("Surface Mirror capture"), FIntPoint(CaptureTarget->SizeX, CaptureTarget->SizeY), TEXT("R32F"), static_cast<double>(CaptureTarget->SizeX) * CaptureTarget->SizeY * 4.0);
	}
	// The gather's readbacks: one staging buffer each, the size of the last dispatch read into it (float4 per sample).
	for (int32 Index = 0; Index < GatherRing.Num(); ++Index)
	{
		const FFluidNinjaBuoyancyGatherReadback& Data = *GatherRing[Index];
		if (Data.TotalSamples > 0)
		{
			Add(FString::Printf(TEXT("Gather readback %d (%d bodies)"), Index, Data.Bodies.Num()), FIntPoint(Data.TotalSamples, 1), TEXT("float4"), static_cast<double>(Data.TotalSamples) * sizeof(FVector4f));
		}
	}
	// The per-body copies (fnb.Buoyancy.Gather 0, or verify mode): each body's ring of readbacks keeps a height square (R32F) and a flow
	// square of the velocity buffer's format, summed over the bodies.
	int32 CopyBodies = 0;
	int32 CopyReadbacks = 0;
	double CopyBytes = 0.0;
	for (const FSlot& Slot : Slots)
	{
		if (Slot.HeightSide <= 0)
		{
			continue;
		}
		++CopyBodies;
		for (const TSharedPtr<FFluidNinjaBuoyancyReadback, ESPMode::ThreadSafe>& Data : Slot.Ring)
		{
			if (!Data.IsValid() || Data->HeightSize.X <= 0)
			{
				continue;   // never copied into
			}
			++CopyReadbacks;
			CopyBytes += static_cast<double>(Data->HeightSize.X) * Data->HeightSize.Y * 4.0;
			if (Data->bFlow && Data->FlowFormat != PF_Unknown)
			{
				CopyBytes += static_cast<double>(Data->FlowSize.X) * Data->FlowSize.Y * GPixelFormats[Data->FlowFormat].BlockBytes;
			}
		}
	}
	if (CopyReadbacks > 0)
	{
		Add(FString::Printf(TEXT("Per-body copy readbacks (%d bodies)"), CopyBodies), FIntPoint(CopyReadbacks, 1), TEXT("R32F + flow"), CopyBytes);
	}
	// The current probes (GetCurrentAtLocation, the swimmers' wakes): a ring of squares of the velocity buffer each.
	int32 ProbeReadbacks = 0;
	double ProbeBytes = 0.0;
	for (const FCurrentProbe& Probe : CurrentProbes)
	{
		const UTextureRenderTarget2D* Buffer = Probe.Buffer.Get();
		if (Probe.Side <= 0 || !Buffer)
		{
			continue;
		}
		ProbeReadbacks += Probe.Ring.Num();
		ProbeBytes += static_cast<double>(Probe.Ring.Num()) * Probe.Side * Probe.Side * GPixelFormats[Buffer->GetFormat()].BlockBytes;
	}
	if (ProbeReadbacks > 0)
	{
		Add(FString::Printf(TEXT("Current probes (%d)"), CurrentProbes.Num()), FIntPoint(ProbeReadbacks, 1), TEXT("velocity squares"), ProbeBytes);
	}
	// fnb.Buoyancy.CalibrateFlow while it measures: its shots of the velocity buffer.
	if (FlowProbe.IsValid() && FlowProbe->Format != PF_Unknown)
	{
		constexpr int32 Side = FFluidNinjaBuoyancyFlowProbe::Side;
		Add(TEXT("Flow calibration shots"), FIntPoint(Side, Side), TEXT("velocity"), static_cast<double>(FFluidNinjaBuoyancyFlowProbe::Shots) * Side * Side * GPixelFormats[FlowProbe->Format].BlockBytes);
	}
	// FluidNinja Waves' targets: each simulation's source texture and Wave Aux target, the self-field atlas and pools, the bench's readbacks.
	if (Waves)
	{
		TArray<FFluidNinjaWaveMemoryItem> WaveItems;
		Waves->ReportMemory(WaveItems);
		for (const FFluidNinjaWaveMemoryItem& Item : WaveItems)
		{
			OutItems.Add({Item.Name, Item.Size, Item.Format, Item.MegaBytes});
		}
	}
	double Total = 0.0;
	for (const FFluidNinjaBuoyancyMemoryItem& Item : OutItems)
	{
		Total += Item.MegaBytes;
	}
	return Total;
}

void UFluidNinjaBuoyancySubsystem::PublishFrameStats()
{
	// The bodies by state, the water's age, and the frame's work, to stat FluidNinjaBuoyancy and the CSV profiler's FNB category (the
	// costs as measured this frame; the smoothed ones are for the status and the test bench).
	int32 Awake = 0;
	int32 Asleep = 0;
	int32 InCapture = 0;
	int32 ByLevel[3] = {0, 0, 0};
	for (const FSlot& Slot : Slots)
	{
		if (Slot.bDormant || Slot.bHeld)
		{
			++Asleep;
		}
		else
		{
			++Awake;
		}
		InCapture += Slot.bInCapture ? 1 : 0;
		++ByLevel[FMath::Clamp(Slot.DistanceLevel, 0, 2)];
	}
	for (int32 Level = 0; Level < 3; ++Level)
	{
		LastBodiesByLevel[Level] = ByLevel[Level];
	}
	// Frames from the newest consumed readback's gather to this one. Under fnb.Gather.FastReadback the patches were built before this frame's
	// physics, so this is the age of the water the physics used this frame; under the slow path they were built in this tick, after the
	// physics, and reach the physics next frame (one more than counted).
	ReadbackAgeFrames = LastReadbackFrame > 0 && GFrameCounter >= LastReadbackFrame ? static_cast<int32>(FMath::Min<uint64>(GFrameCounter - LastReadbackFrame, 100000)) : 0;
	const int32 WakePointsWritten = LastWakePoints + LastSwimmerWakePoints;
	SET_DWORD_STAT(STAT_FNB_BodiesAwake, Awake);
	SET_DWORD_STAT(STAT_FNB_BodiesAsleep, Asleep);
	SET_DWORD_STAT(STAT_FNB_BodiesInCapture, InCapture);
	SET_DWORD_STAT(STAT_FNB_GatherPoints, LastGatherSamples);
	SET_DWORD_STAT(STAT_FNB_WakePoints, WakePointsWritten);
	SET_DWORD_STAT(STAT_FNB_ReadbackAgeFrames, ReadbackAgeFrames);
	// The importance scheduler: the bodies by distance level, and what the gather did with them this frame.
	SET_DWORD_STAT(STAT_FNB_BodiesLod0, ByLevel[0]);
	SET_DWORD_STAT(STAT_FNB_BodiesLod1, ByLevel[1]);
	SET_DWORD_STAT(STAT_FNB_BodiesLod2, ByLevel[2]);
	SET_DWORD_STAT(STAT_FNB_BodiesGathered, LastGatherBodies);
	SET_DWORD_STAT(STAT_FNB_BodiesWaiting, LastGatherWaiting);
	SET_DWORD_STAT(STAT_FNB_BodiesDeferred, LastGatherDeferred);
	CSV_CUSTOM_STAT(FNB, BodiesLod0, ByLevel[0], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(FNB, BodiesLod1, ByLevel[1], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(FNB, BodiesLod2, ByLevel[2], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(FNB, BodiesGathered, LastGatherBodies, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(FNB, BodiesWaiting, LastGatherWaiting, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(FNB, TickMs, FrameTickMs, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(FNB, PatchMs, FramePatchMs, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(FNB, PreTickMs, FramePreTickMs, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(FNB, PhysicsMs, FramePhysicsMs, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(FNB, GatherPoints, LastGatherSamples, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(FNB, WakePoints, WakePointsWritten, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(FNB, ReadbackAgeFrames, ReadbackAgeFrames, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(FNB, BodiesAwake, Awake, ECsvCustomStatOp::Set);
	// The physics pre-tick runs before this tick when the physics scene steps, and the patches are built before it too (TickPrePhysics)
	// under fnb.Gather.FastReadback: their costs are this frame's only once, then cleared, so a frame without a step, or without a patch,
	// reads 0 rather than the last one's.
	FramePreTickMs = 0.f;
	FramePhysicsMs = 0.f;
	FramePatchMs = 0.f;
}
