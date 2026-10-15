#include "FluidNinjaBuoyancySubsystem.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "FluidNinjaBuoyancyComponent.h"
#include "GameFramework/PhysicsVolume.h"
#include "FluidNinjaBuoyancyCallback.h"
#include "FluidNinjaBuoyancyCore.h"
#include "FluidNinjaBuoyancyGather.h"
#include "FluidNinjaBuoyancyLog.h"
#include "FluidNinjaBuoyancySettings.h"
#include "FluidNinjaBuoyancyTestBench.h"
#include "Misc/ScopeExit.h"
#include "Stats/Stats.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "NiagaraActor.h"
#include "NiagaraComponent.h"
#include "NiagaraDataChannelAccessContext.h"
#include "NiagaraDataChannelAccessor.h"
#include "NiagaraDataChannelAsset.h"
#include "NiagaraDataChannelFunctionLibrary.h"
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
	};
	TArray<FBody> Bodies;
	int32 TotalSamples = 0;
	float FlowScale = 0.f;
	double Time = 0.0;

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
	/** A water height counts for a body only within this of it: a river passing high over a pool is not the pool's water, and must
	 * neither lift the capture's near plane over it (which would then capture that river and keep it there) nor stand in as the
	 * body's far-field water. */
	constexpr double WaterReachCm = 300.0;

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

	FAutoConsoleCommandWithWorldAndArgs CalibrateFlowCommand(TEXT("fnb.Buoyancy.CalibrateFlow"),
		TEXT("In Play: measures how fast FluidNinja's dye moves against its velocity buffer around the first floating body (or the camera), ")
		TEXT("and the Flow Scale Multiplier (Project Settings → FluidNinja Buoyancy) that matches it. 'apply' sets and saves it when the match is good."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UFluidNinjaBuoyancySubsystem* Hydro = World ? World->GetSubsystem<UFluidNinjaBuoyancySubsystem>() : nullptr;
			if (!Hydro && GEngine)
			{
				// Typed into the editor's console: the world in Play.
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
			if (!Hydro)
			{
				UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("fnb.Buoyancy.CalibrateFlow: run it in Play (no FluidNinja Buoyancy in this world)."));
				return;
			}
			Hydro->CalibrateFlow(Args.ContainsByPredicate([](const FString& Arg) { return Arg.Equals(TEXT("apply"), ESearchCase::IgnoreCase); }));
		}));
}

UFluidNinjaBuoyancySubsystem::UFluidNinjaBuoyancySubsystem() = default;
UFluidNinjaBuoyancySubsystem::~UFluidNinjaBuoyancySubsystem() = default;

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
}

void UFluidNinjaBuoyancySubsystem::Deinitialize()
{
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
	if (Bench)
	{
		Bench->Remove();
		Bench.Reset();
	}
	Slots.Reset();   // a render command still holding a readback releases it there
	GatherRing.Reset();
	FlowProbe.Reset();
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
	// The NinjaLive actors: the tag each tracks, whether each paints tracked velocity relative to the water, and whether it leaves
	// tracked bodies' density out (FluidNinja weights each tick's push of its river driver and brushes by the local density, so
	// painted density makes the current push harder wherever floaters have been).
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	TArray<FName> Tags;
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
		Report += FString::Printf(TEXT("%s: tracking tag %s; paints tracked %s (%s), %s (%s)\n"), *It->GetName(), Tag.IsNone() ? TEXT("none (object types only)") : *Tag.ToString(),
			bRelative ? TEXT("velocity relative to the water") : TEXT("world velocity"), *How, bDensityOff ? TEXT("no density") : TEXT("density too"), *DensityHow);
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
		if (bWants && bHandOver && !Slot.bHandedOver)
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
		else if (Slot.bHandedOver && (!bWants || !bHandOver))
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
		// FluidNinja's tracking needs overlap events, by object type or by tag.
		Slot.bNinjaTracked = Primitive->GetGenerateOverlapEvents();
		if (bWants && !Slot.bHandedOver && (!Slot.bNinjaTracked || Ninjas == 0) && Slot.TrackingNote != Reason)
		{
			// No fallback: this build needs the modified FluidNinja. (A body with overlap events of its own, in a level with a NinjaLive,
			// gets the warning below.)
			Slot.TrackingNote = Reason;
			const bool bWakesToo = Component->Interaction == EFluidNinjaBuoyancyInteraction::Both && Component->bWake;
			UE_LOG(LogFluidNinjaBuoyancy, Error, TEXT("FluidNinja Buoyancy on %s: not tracked by FluidNinja, so %s. Its Interaction includes FluidNinja's tracking, which needs the modified FluidNinja: ")
				TEXT("every NinjaLive turning the Relative Tracking Switch '%s' on, and setting its Tracked Density Switch '%s' to 1, 4 or 5 (no density from tracked bodies). But %s."),
				*GetNameSafe(Component->GetOwner()), bWakesToo ? TEXT("it stirs the water only with its wakes") : TEXT("it does not stir the water"),
				*Settings.RelativeTrackingSwitch.ToString(), *Settings.TrackedDensitySwitch.ToString(), *Reason);
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
		const FBox Box = Primitive->Bounds.GetBox();
		Source->SurfaceArea += FBox2D(FVector2D(Box.Min), FVector2D(Box.Max));
	}
	FlowSources = MoveTemp(Found);
	UpdateFlowSources();
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
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FluidNinjaBuoyancy_Tick);
	const double TickStart = FPlatformTime::Seconds();
	ON_SCOPE_EXIT
	{
		TickMs = FMath::Lerp(TickMs, static_cast<float>((FPlatformTime::Seconds() - TickStart) * 1000.0), 0.1f);
	};
	SmoothedDeltaTime = FMath::Lerp(SmoothedDeltaTime, FMath::Clamp(DeltaTime, 1e-3f, 0.25f), 0.1f);
	Slots.RemoveAll([](const FSlot& Slot) { return !Slot.Component.IsValid(); });

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
	if (!Bench)
	{
		Bench = FFluidNinjaBuoyancyTestBench::Create(*this);
	}
	if (Bench)
	{
		Bench->Tick(DeltaTime);
	}

	if (Slots.IsEmpty() || !UFluidNinjaBuoyancySettings::Get().bEnabled || FluidNinjaBuoyancyCVars::Enable == 0)
	{
		Status = TEXT("FluidNinja Buoyancy: idle");
		return;
	}
	const double Now = World->GetTimeSeconds();
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
	CollectReadbacks();
	CaptureAndCopy();
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
	if (FluidNinjaBuoyancyCVars::Enable >= 2)
	{
		DrawDebug();
	}
}

void UFluidNinjaBuoyancySubsystem::CollectReadbacks()
{
	// Render thread: lock and copy out what the GPU has finished.
	// Each with the copy it is for: by the time the render thread gets to it, that copy may have been read by an earlier request and
	// the readback reused for a newer (larger) copy, which this request must leave alone.
	TArray<TPair<TSharedPtr<FFluidNinjaBuoyancyReadback, ESPMode::ThreadSafe>, uint32>> Pending;
	for (FSlot& Slot : Slots)
	{
		for (const TSharedPtr<FFluidNinjaBuoyancyReadback, ESPMode::ThreadSafe>& Data : Slot.Ring)
		{
			if (Data->Stage.load() == 1)
			{
				Pending.Emplace(Data, Data->Generation.load());
			}
		}
	}
	TArray<TPair<TSharedPtr<FFluidNinjaBuoyancyGatherReadback, ESPMode::ThreadSafe>, uint32>> PendingGathers;
	for (const TSharedPtr<FFluidNinjaBuoyancyGatherReadback, ESPMode::ThreadSafe>& Data : GatherRing)
	{
		if (Data->Stage.load() == 1)
		{
			PendingGathers.Emplace(Data, Data->Generation.load());
		}
	}
	if (!Pending.IsEmpty() || !PendingGathers.IsEmpty())
	{
		ENQUEUE_RENDER_COMMAND(FluidNinjaBuoyancyRead)([Pending, PendingGathers](FRHICommandListImmediate&)
		{
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

	const double PatchStart = FPlatformTime::Seconds();
	ON_SCOPE_EXIT
	{
		PatchMs = FMath::Lerp(PatchMs, static_cast<float>((FPlatformTime::Seconds() - PatchStart) * 1000.0), 0.1f);
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
	FinishPatch(Slot, Patch);
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
	constexpr float NaN = std::numeric_limits<float>::quiet_NaN();
	for (const FFluidNinjaBuoyancyGatherReadback::FBody& Body : Data.Bodies)
	{
		FSlot* Slot = FindSlot(Body.Id);
		const int32 Count = Body.Grid * Body.Grid;
		if (!Slot || Body.Grid < 2 || Body.First + Count > Data.Samples.Num())
		{
			continue;
		}
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
		// Sample (i, j) is at first + j * grid + i, as the patch's own index.
		const FVector4f* Samples = Data.Samples.GetData() + Body.First;
		for (int32 K = 0; K < Count; ++K)
		{
			const FVector4f& Sample = Samples[K];
			Patch->Height[K] = Sample.X > -1e29f ? Sample.X : NaN;
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
	}
}

void UFluidNinjaBuoyancySubsystem::FinishPatch(FSlot& Slot, const TSharedRef<FluidNinjaBuoyancy::FWaterPatch, ESPMode::ThreadSafe>& Patch)
{
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
	++PatchesBuilt;
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
	// it fit in one square).
	const double Width = Settings.CaptureWidthCm;
	const int32 Resolution = FMath::Clamp(Settings.CaptureResolution, 64, 4096);
	const double Texel = Width / Resolution;
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
		}
	}
	const FVector2D Origin(FMath::RoundToDouble(Centre.X / Texel) * Texel, FMath::RoundToDouble(Centre.Y / Texel) * Texel);
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
			const bool bVolume = WaterVolumeTop(World, Bounds, Surface);
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
		Status = FString::Printf(TEXT("FluidNinja Buoyancy: no capture (%d bodies, %d surface primitives)"), Slots.Num(), SurfacePrimitives.Num());
		return;
	}
	Top += Settings.NearPlaneAboveBodiesCm;
	const double Range = Width + RangeExtraCm;

	if (!Capture)
	{
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		Capture = World->SpawnActor<ASceneCapture2D>(Params);
		CaptureTarget = NewObject<UTextureRenderTarget2D>(this);
		if (!Capture || !CaptureTarget)
		{
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
	Component->CaptureScene();
	CaptureOrigin = Origin;
	CaptureTop = Top;
	CaptureRange = Range;
	CaptureTexel = Texel;
	CaptureResolution = Resolution;

	// Each body's water out of the capture and the velocity buffer: all in one pass, or copied body by body.
	const bool bGather = Settings.bGPUGather && FluidNinjaBuoyancyCVars::Gather != 0;
	const int32 Served = bGather ? GatherFootprints(Wanted, Now) : CopyFootprints(Wanted, Now);
	if (bGather && bVerifyGather)
	{
		CopyFootprints(Wanted, Now);   // compared with the gather, never used
	}
	if (!bGather)
	{
		LastGatherBodies = LastGatherSamples = 0;
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
		? FString::Printf(TEXT("gather %d of %d bodies, %d points (%.0f KB)"), Served, Wanted.Num(), LastGatherSamples, LastGatherSamples * sizeof(FVector4f) / 1024.0)
		: FString::Printf(TEXT("%d copies"), Served);

	Status = FString::Printf(TEXT("FluidNinja Buoyancy: capture %.0f m at %d px (%.1f cm), top %.0f, %d surface primitives (%d drawn, %d left out), %s, %u patches, %d wake points")
		TEXT("%s (flow multiplier %.2f)\n")
		TEXT("  wind %.1f m/s toward %.0f deg (%s)%s\n")
		TEXT("  cost: game thread %.2f ms (patches %.2f), physics pre-tick %.2f ms, physics thread %.2f ms a frame (%.3f a step)"),
		Width / 100.0, Resolution, Texel, Top, SurfacePrimitives.Num(), LastCapturedPrimitives, SurfaceLeftOut, *Mode, PatchesBuilt, LastWakePoints,
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
	const int32 MaxGrid = FMath::Clamp(Settings.MaxGatherGrid, 8, 256);

	// With more bodies than one gather takes, they take turns.
	const int32 MaxBodies = FMath::Max(1, Settings.MaxGatherBodies);
	const int32 Count = FMath::Min(Wanted.Num(), MaxBodies);
	const int32 Start = Wanted.Num() > MaxBodies ? GatherCursor % Wanted.Num() : 0;
	GatherCursor = Wanted.Num() > MaxBodies ? Start + Count : 0;

	// One pass per simulation (each reads its own velocity buffer); bodies over none get heights only.
	TMap<int32, TArray<const FFootprint*>> Groups;
	for (int32 N = 0; N < Count; ++N)
	{
		const FFootprint& Want = Wanted[(Start + N) % Wanted.Num()];
		Groups.FindOrAdd(Want.Source).Add(&Want);
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
		Request.Slots.Reserve(Group.Value.Num() * 3);
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
			Request.Slots.Add(FVector4f(static_cast<float>(HeightColumn), static_cast<float>(HeightRow), static_cast<float>(Spacing / Texel), static_cast<float>(Grid)));
			Request.Slots.Add(FlowSlot);
			Request.Slots.Add(FVector4f(static_cast<float>(Total), 0.f, 0.f, 0.f));
			FFluidNinjaBuoyancyGatherReadback::FBody& Body = Data->Bodies.AddDefaulted_GetRef();
			Body.Id = Want->Slot->Id;
			Body.Origin = GridOrigin;
			Body.Spacing = Spacing;
			Body.Grid = Grid;
			Body.First = Total;
			Body.bFlow = bFlow;
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
		Data->Generation.fetch_add(1);
		Data->Stage = 1;
		Served += Group.Value.Num();
		LastGatherBodies += Group.Value.Num();
		LastGatherSamples += Total;
		ENQUEUE_RENDER_COMMAND(FluidNinjaBuoyancyGather)([Data, Request = MoveTemp(Request), CaptureResource, FlowResource](FRHICommandListImmediate& RHICmdList) mutable
		{
			Request.Height = CaptureResource ? CaptureResource->GetRenderTargetTexture() : nullptr;
			Request.Flow = FlowResource ? FlowResource->GetRenderTargetTexture() : nullptr;
			if (!Request.Height)
			{
				Data->Stage = 0;
				return;
			}
			FluidNinjaBuoyancyGather::Enqueue(RHICmdList, Request, Data->Buffer);
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
		Data->Generation.fetch_add(1);
		Data->Stage = 1;
		++Copies;
		ENQUEUE_RENDER_COMMAND(FluidNinjaBuoyancyCopy)([Data, CaptureResource, FlowResource](FRHICommandListImmediate& RHICmdList)
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
				FRHITexture* FlowSource = FlowResource ? FlowResource->GetRenderTargetTexture() : nullptr;
				if (FlowSource)
				{
					Data->Flow.EnqueueCopy(RHICmdList, FlowSource, FIntVector(Data->FlowMin.X, Data->FlowMin.Y, 0), 0, FIntVector(Data->FlowSize.X, Data->FlowSize.Y, 1));
				}
				else
				{
					Data->bFlow = false;
				}
			}
		});
	}

	return Copies;
}

void UFluidNinjaBuoyancySubsystem::PhysicsPreTick(FPhysScene_Chaos* Scene, float DeltaSeconds)
{
	UWorld* World = GetWorld();
	if (!Callback || !World)
	{
		return;
	}
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FluidNinjaBuoyancy_PhysicsPreTick);
	const double PreTickStart = FPlatformTime::Seconds();
	ON_SCOPE_EXIT
	{
		PreTickMs = FMath::Lerp(PreTickMs, static_cast<float>((FPlatformTime::Seconds() - PreTickStart) * 1000.0), 0.1f);
	};
	// What the physics thread found (and what its steps cost).
	float StepSeconds = 0.f;
	int32 Steps = 0;
	while (Chaos::TSimCallbackOutputHandle<FFluidNinjaBuoyancyOutput> Output = Callback->PopOutputData_External())
	{
		StepSeconds += Output->StepSeconds;
		++Steps;
		for (const FFluidNinjaBuoyancyBodyOutput& Body : Output->Bodies)
		{
			if (FSlot* Slot = FindSlot(Body.Id))
			{
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
			}
		}
	}
	if (Steps > 0)
	{
		PhysicsMsPerFrame = FMath::Lerp(PhysicsMsPerFrame, StepSeconds * 1000.f, 0.1f);
		PhysicsMsPerStep = FMath::Lerp(PhysicsMsPerStep, StepSeconds * 1000.f / Steps, 0.1f);
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
	}

	// The water line each body last had: its contacts (wet line, spray, sound), and the pieces driving into the water written into
	// FluidNinja as moving brush points (bow wave, wake, foam). Points move with the hull: PositionPrevious is where that piece of
	// hull was a frame ago, and the velocity is the hull's there.
	const UFluidNinjaBuoyancySettings& HydroSettings = UFluidNinjaBuoyancySettings::Get();
	const bool bWakes = HydroSettings.bWakes && FluidNinjaBuoyancyCVars::Wake != 0 && FluidNinjaBuoyancyCVars::Enable != 0 && WakeChannel && !bControlledWater;
	struct FWakePoint
	{
		FVector Position;
		FVector Previous;
		FVector4 VelocityAndSize;
	};
	TArray<FWakePoint, TInlineAllocator<64>> Wakes;
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
			if (Wakes.Num() >= HydroSettings.MaxWakePoints)
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
			const float Through = static_cast<float>(FVector::DotProduct(Contact.HullVelocity - FVector(Around.X, Around.Y, 0.0), Outward)) + Contact.Plunge;
			if (Through < Component->WakeMinPushCmS)
			{
				continue;
			}
			const float Target = static_cast<float>(FVector::DotProduct(FVector(Around.X, Around.Y, 0.0), Outward)) + Component->WakeStrength * Through;
			const float Lack = Target - static_cast<float>(FVector::DotProduct(FVector(Local.X, Local.Y, 0.0), Outward));
			if (Lack <= 0.f)
			{
				continue;
			}
			const FVector Velocity = Outward * (Lack * Component->WakeGain);
			const float Size = Component->WakeBrushSizeCm * FMath::Clamp(Through / 200.f, 0.4f, 2.f);
			Wakes.Add({Contact.Position, Contact.Position - Contact.HullVelocity * DeltaSeconds, FVector4(Velocity.X, Velocity.Y, Velocity.Z, Size)});
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

	// What it needs for the coming steps.
	FFluidNinjaBuoyancyInput* Input = Callback->GetProducerInputData_External();
	Input->Reset();
	if (!UFluidNinjaBuoyancySettings::Get().bEnabled || FluidNinjaBuoyancyCVars::Enable == 0)
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
		if (bControlledWater)
		{
			BodyInput.bFlat = true;   // still and flat: no patch, no current
			BodyInput.FlatHeight = ControlledHeight;
		}
		else
		{
			BodyInput.Patch = Slot.bInCapture ? Slot.Patch : nullptr;
			BodyInput.Extrapolate = BodyInput.Patch.IsValid() ? FMath::Clamp(static_cast<float>(Now - BodyInput.Patch->Time), 0.f, Settings.MaxExtrapolationSeconds) : 0.f;
			BodyInput.bFlat = Slot.bFlat;
			BodyInput.FlatHeight = Slot.FlatHeight;
			// A body that pushes the water itself is carried by the current around it, not by the water it has just pushed (which
			// would carry it on with its own speed): one that makes wakes, or one FluidNinja paints in itself (its overlap tracking
			// sees primitives that generate overlap events).
			BodyInput.bAmbientFlow = (Component->bWake && Settings.bWakes && FluidNinjaBuoyancyCVars::Wake != 0) || Primitive->GetGenerateOverlapEvents();
		}
		Component->FillParams(BodyInput.Params);
		BodyInput.Params.Rho = Settings.WaterDensity * 1e-6f;
		BodyInput.Params.RhoAir = Settings.AirDensity * 1e-6f;
		BodyInput.Params.Gravity = FMath::Max(1.f, -World->GetGravityZ());
		BodyInput.Params.Wind = Wind.Velocity;
		BodyInput.bWaterline = FluidNinjaBuoyancyCVars::Enable >= 2;
		BodyInput.bStableSteps = Settings.bStableSteps && FluidNinjaBuoyancyCVars::Implicit != 0;
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

	// Hull level of detail (triangles model), with a 10 % band so it does not flicker at a boundary.
	const FVector2D& Limits = Component->LodDistancesCm;
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
	Slot.Level = FMath::Clamp(Level, 0, FMath::Max(Levels - 1, 0));

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
	const bool bFar = Distance > Component->SleepDistanceCm;
	const double Now = GetWorld()->GetTimeSeconds();
	if (Slot.bDormant)
	{
		// Woken by something else (a hit, a force from game code): at once when it moves, since asleep it gets no water forces and
		// would fall. Awake but still, it may only be the sleep request not yet through, for the first half second.
		const bool bWoken = Body->IsInstanceAwake() && (Now - Slot.DormantSince > 0.5 || Primitive->GetPhysicsLinearVelocity().Size() > 2.f * SettledSpeedCmS);
		if (!bFar || bSeen || !Component->bSleepWhenSettled || bWoken)
		{
			Slot.bDormant = false;
			Slot.SettledSeconds = 0.f;
			Primitive->WakeRigidBody();
		}
		return;
	}
	const bool bStill = Primitive->GetPhysicsLinearVelocity().Size() < SettledSpeedCmS && Primitive->GetPhysicsAngularVelocityInRadians().Size() < SettledSpinRadS;
	Slot.SettledSeconds = Component->bSleepWhenSettled && bFar && !bSeen && bStill ? Slot.SettledSeconds + DeltaSeconds : 0.f;
	if (Slot.SettledSeconds > SettleSeconds)
	{
		Slot.bDormant = true;
		Slot.DormantSince = Now;
		SetNeverSleep(Body, false);
		Primitive->PutRigidBodyToSleep();
	}
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
	return UFluidNinjaBuoyancySettings::Get().bGPUGather && FluidNinjaBuoyancyCVars::Gather != 0;
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
			FRHITexture* Source = Resource ? Resource->GetRenderTargetTexture() : nullptr;
			if (!Source)
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
		UFluidNinjaBuoyancySettings* Settings = GetMutableDefault<UFluidNinjaBuoyancySettings>();
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
