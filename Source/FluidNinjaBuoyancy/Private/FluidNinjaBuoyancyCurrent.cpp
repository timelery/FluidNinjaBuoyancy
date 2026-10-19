#include "FluidNinjaBuoyancySubsystem.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "FluidNinjaBuoyancyLog.h"
#include "FluidNinjaBuoyancySettings.h"
#include "FluidNinjaBuoyancySwimmers.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"
#include "NiagaraDataChannelAccessor.h"
#include "NiagaraDataChannelAsset.h"
#include "NiagaraDataChannelFunctionLibrary.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "TextureResource.h"

#include <atomic>

namespace FluidNinjaBuoyancyCVars
{
	// A/B prototype (wake law, 2026-10-17). FluidNinja's data channel cannot brake a driven current: a point written against it paints dye
	// that the sim's driver speeds up downstream. With this on, wake points pushing against the current around them are not written.
	int32 WakeAlongOnly = 0;
	static FAutoConsoleVariableRef CVarWakeAlongOnly(TEXT("fnb.Wake.AlongOnly"), WakeAlongOnly,
		TEXT("A/B: 1 skips swimmer wake points whose way through the water has a negative component along the current around the swimmer, and ")
		TEXT("floating bodies' side wake points whose outward normal does (plunge rings and bursts stay), since a point written against a driven current speeds it up ")
		TEXT("downstream. 0 off (the default); any other value on. While on, the written and skipped counts are logged every 2 s."));
	float WakeAlongOnlyMinCmS = 0.f;
	static FAutoConsoleVariableRef CVarWakeAlongOnlyMin(TEXT("fnb.Wake.AlongOnlyMinCmS"), WakeAlongOnlyMinCmS,
		TEXT("A/B, with fnb.Wake.AlongOnly 1: a point is skipped only when the current's component against it is more than this, cm/s. 0 (the default): any. ")
		TEXT("Use 0 or more: a negative value also skips points that go with the current."));
	/** Side wake points written and skipped while fnb.Wake.AlongOnly is on, logged every 2 s (A/B evidence). */
	std::atomic<int64> WakeAlongOnlyHullWritten{0};
	std::atomic<int64> WakeAlongOnlyHullSkipped{0};
	std::atomic<int64> WakeAlongOnlySwimmerWritten{0};
	std::atomic<int64> WakeAlongOnlySwimmerSkipped{0};
	// FluidNinja's data channel ignores points slower than about 40 cm/s (Docs/FluidNinjaWaves.md, Wakes and the current). A swimmer point
	// that would be slower is not written, and a swimmer whose point cannot be that fast is not probed for the current (a GPU readback per probe
	// per frame) unless it plunges: its ring (80 to 300 cm/s) needs no probe.
	float WakeMinWriteCmS = 40.f;
	static FAutoConsoleVariableRef CVarWakeMinWrite(TEXT("fnb.Wake.MinWriteCmS"), WakeMinWriteCmS,
		TEXT("Swimmer wake points slower than this, cm/s, are not written, and a swimmer whose point cannot reach it is not probed for the current ")
		TEXT("(a plunging one is): raising it saves current probes (a GPU readback each) and points FluidNinja would ignore, lowering it writes slower points too. ")
		TEXT("Swimmer Wake Min Speed still applies on top. 0 writes and probes everything; negative counts as 0. Default 40, about where FluidNinja starts to see a point."));
	/** Current probes and swimmer wake points the cut-off spared, logged every 2 s at Verbose (evidence). */
	std::atomic<int64> WakeMinWriteProbesSkipped{0};
	std::atomic<int64> WakeMinWritePointsSkipped{0};
}

/** One probe's square of a simulation's velocity buffer, copied and read back; shared with the render thread. */
struct FFluidNinjaBuoyancyCurrentReadback
{
	FRHIGPUTextureReadback Readback{FName(TEXT("FluidNinjaBuoyancyCurrent"))};
	std::atomic<int32> Stage{0};   // 0 free, 1 copy queued, 2 read into Texels
	/** Counts the copies queued into this readback (as FFluidNinjaBuoyancyReadback::Generation). */
	std::atomic<uint32> Generation{0};

	// Set by the game thread with the copy.
	FIntPoint Min = FIntPoint::ZeroValue;
	int32 Side = 0;
	EPixelFormat Format = PF_Unknown;
	FVector2D SimCentre = FVector2D::ZeroVector;
	double SimSizeCm = 0.0;
	int32 Resolution = 0;
	float FlowScale = 0.f;
	FVector2D Where = FVector2D::ZeroVector;
	double Time = 0.0;

	// Filled on the render thread.
	TArray<FVector2f> Texels;

	// FluidNinja Waves: the same square of the simulation's pressure buffer (a core-made RGBA16F: B = η + P, A = the static head P), for
	// the swimmers' wave height (UFluidNinjaBuoyancySubsystem::SampleWater). bPressure false: no wave buffer here (stock core).
	FRHIGPUTextureReadback PressureReadback{FName(TEXT("FluidNinjaBuoyancyCurrentPressure"))};
	bool bPressure = false;
	FIntPoint PressureMin = FIntPoint::ZeroValue;
	int32 PressureSide = 0;
	int32 PressureResolution = 0;
	/** η = B − A per texel (cm), PressureSide², filled on the render thread. */
	TArray<float> Etas;
};

namespace FluidNinjaBuoyancyCurrentReads
{
	constexpr int32 CurrentRingSize = 3;
	constexpr double CurrentSourceRefreshSeconds = 2.0;
	/** A probe nobody asked for in this long is dropped. */
	constexpr double CurrentIdleSeconds = 1.0;
	/** At most this many places are read at once. */
	constexpr int32 MaxCurrentProbes = 32;
	/** Calls in the same frame this near each other share a reading. */
	constexpr double CurrentShareCm = 25.0;
	/** A call takes over the nearest probe asked for in an earlier frame within this, plus as far as the fastest swimmer goes meanwhile
	 * (a moving swimmer keeps its probe and its readings). */
	constexpr double CurrentFollowCm = 150.0;
	constexpr double CurrentFollowSpeedCmS = 1000.0;
	/** A reading older than this does not count for a wake. */
	constexpr double CurrentFreshSeconds = 0.5;
	/** A registered swimmer moving further than this in a frame, or faster than SwimmerMaxSpeedCmS, was put there: no velocity (used when
	 * its pose gives no velocity of its own and its mesh's motion frame to frame stands in). */
	constexpr double SwimmerTeleportCm = 200.0;
	constexpr double SwimmerMaxSpeedCmS = 2000.0;
	/** Going into the water faster than this (straight down): a plunge, a ring pushed outward. */
	constexpr double PlungeSpeedCmS = 250.0;
	/** A swimmer whose top (its capsule's top, a registered swimmer's head sphere's centre) is this far under the surface leaves the surface alone. A swimmer
	 * at the surface floats with its top about at the water line (Character Movement holds its capsule's top there). */
	constexpr double SurfaceReachCm = 50.0;
	/** A registered swimmer's hips are in the water once they are no higher than their sphere's radius over it; this stands in when its
	 * pose has no hips sphere. */
	constexpr double HipsRadiusCm = 15.0;

	bool IsVelocityReadable(EPixelFormat Format)
	{
		return Format == PF_FloatRGBA || Format == PF_G16R16F || Format == PF_G16R16F_FILTER || Format == PF_A32B32G32R32F || Format == PF_G32R32F;
	}

	/** Render thread: the copied velocity texels into the array (once the GPU is done). */
	void ReadOutCurrent(FFluidNinjaBuoyancyCurrentReadback& Data)
	{
		const int32 N = Data.Side;
		Data.Texels.SetNumZeroed(N * N);
		int32 Pitch = 0;
		if (const uint8* Raw = static_cast<const uint8*>(Data.Readback.Lock(Pitch)))
		{
			const int32 Bytes = GPixelFormats[Data.Format].BlockBytes;
			for (int32 Row = 0; Row < N; ++Row)
			{
				for (int32 Column = 0; Column < N; ++Column)
				{
					const uint8* Texel = Raw + (static_cast<int64>(Row) * Pitch + Column) * Bytes;
					FVector2f Value = FVector2f::ZeroVector;
					switch (Data.Format)
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
					Data.Texels[Row * N + Column] = Value;
				}
			}
			Data.Readback.Unlock();
		}
		if (Data.bPressure)
		{
			const int32 M = Data.PressureSide;
			Data.Etas.SetNumZeroed(M * M);
			int32 PressurePitch = 0;
			if (const uint8* PressureRaw = static_cast<const uint8*>(Data.PressureReadback.Lock(PressurePitch)))
			{
				for (int32 Row = 0; Row < M; ++Row)
				{
					for (int32 Column = 0; Column < M; ++Column)
					{
						const FFloat16* Half = reinterpret_cast<const FFloat16*>(PressureRaw + (static_cast<int64>(Row) * PressurePitch + Column) * 8);
						Data.Etas[Row * M + Column] = Half[2].GetFloat() - Half[3].GetFloat();   // η = B − A
					}
				}
				Data.PressureReadback.Unlock();
			}
			else
			{
				Data.bPressure = false;
			}
		}
		Data.Stage = 2;
	}

	/** Cramer's rule on the 3×3 normal equations (as Subsystem.cpp's Solve3, which is file-local there). */
	bool SolvePlane(const double S[3][3], const double R[3], double Out[3])
	{
		const double D = S[0][0] * (S[1][1] * S[2][2] - S[1][2] * S[2][1]) - S[0][1] * (S[1][0] * S[2][2] - S[1][2] * S[2][0])
			+ S[0][2] * (S[1][0] * S[2][1] - S[1][1] * S[2][0]);
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
			Out[Column] = (M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
				+ M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0])) / D;
		}
		return true;
	}
}

bool UFluidNinjaBuoyancySubsystem::GetCurrentAtLocation(FVector Location, FVector& OutCurrent, float& OutAgeSeconds, float InnerRadiusCm, float OuterRadiusCm)
{
	OutCurrent = FVector::ZeroVector;
	OutAgeSeconds = 0.f;
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	const UWorld* World = GetWorld();
	if (!World || !Settings.IsEnabled() || Location.ContainsNaN())
	{
		return false;
	}
	if (bControlledWater)
	{
		return true;   // the test bench's still water: no current, as the floating bodies feel
	}
	const float Inner = InnerRadiusCm < 0.f ? Settings.SwimmerCurrentRingInnerCm : InnerRadiusCm;
	const float Outer = OuterRadiusCm < 0.f ? Settings.SwimmerCurrentRingOuterCm : OuterRadiusCm;
	const int32 Index = RequestCurrent(FVector2D(Location), Inner, Outer);
	if (!CurrentProbes.IsValidIndex(Index))
	{
		return false;
	}
	const FCurrentProbe& Probe = CurrentProbes[Index];
	if (Probe.bOutside || Probe.ReadTime < 0.0)
	{
		return false;
	}
	OutCurrent = FVector(Probe.Current.X, Probe.Current.Y, 0.0);
	OutAgeSeconds = static_cast<float>(World->GetTimeSeconds() - Probe.ReadTime);
	return true;
}

void UFluidNinjaBuoyancySubsystem::UpdateFlowSourcesForCurrent()
{
	if (CurrentSourcesFrame == GFrameCounter)
	{
		return;
	}
	CurrentSourcesFrame = GFrameCounter;
	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const double Now = World->GetTimeSeconds();
	if (Now >= NextSourceRefresh)
	{
		NextSourceRefresh = Now + FluidNinjaBuoyancyCurrentReads::CurrentSourceRefreshSeconds;
		RefreshSources();
	}
	UpdateSimSquare();
	UpdateFlowSources();
}

int32 UFluidNinjaBuoyancySubsystem::RequestCurrent(const FVector2D& Location, float InnerCm, float OuterCm)
{
	using namespace FluidNinjaBuoyancyCurrentReads;
	const UWorld* World = GetWorld();
	if (!World || Location.ContainsNaN())
	{
		return INDEX_NONE;
	}
	const double Now = World->GetTimeSeconds();
	const float Inner = FMath::Max(InnerCm, 0.f);
	const float Outer = FMath::Max(OuterCm, Inner + 10.f);

	// The probe: one already asked for nearby this frame (shared), else the nearest asked for earlier within reach (moved here), else a
	// new one.
	int32 Found = INDEX_NONE;
	double Best = TNumericLimits<double>::Max();
	for (int32 Index = 0; Index < CurrentProbes.Num(); ++Index)
	{
		const FCurrentProbe& Probe = CurrentProbes[Index];
		if (FMath::Abs(Probe.InnerCm - Inner) > 1.f || FMath::Abs(Probe.OuterCm - Outer) > 1.f)
		{
			continue;
		}
		const double Distance = FVector2D::Distance(Probe.Where, Location);
		if (Probe.RequestFrame == GFrameCounter)
		{
			if (Distance <= CurrentShareCm)
			{
				Found = Index;
				break;
			}
			continue;
		}
		const double Reach = CurrentFollowCm + CurrentFollowSpeedCmS * (Now - Probe.LastRequest);
		if (Distance <= Reach && Distance < Best)
		{
			Best = Distance;
			Found = Index;
		}
	}
	if (Found == INDEX_NONE)
	{
		if (CurrentProbes.Num() >= MaxCurrentProbes)
		{
			if (!bCurrentProbeCapLogged)
			{
				bCurrentProbeCapLogged = true;
				UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("FluidNinja Buoyancy: the current is already read at %d places; GetCurrentAtLocation returns false for more."),
					MaxCurrentProbes);
			}
			return INDEX_NONE;
		}
		Found = CurrentProbes.AddDefaulted();
		CurrentProbes[Found].InnerCm = Inner;
		CurrentProbes[Found].OuterCm = Outer;
	}
	FCurrentProbe& Probe = CurrentProbes[Found];
	if (Probe.RequestFrame != GFrameCounter)
	{
		Probe.Where = Location;
	}
	Probe.RequestFrame = GFrameCounter;
	Probe.LastRequest = Now;
	if (Probe.LaunchFrame == GFrameCounter)
	{
		return Found;   // this frame's copy is already on its way
	}
	Probe.LaunchFrame = GFrameCounter;

	// The simulation under it, wherever it is this frame, and one square of its velocity buffer around it, as big as the ring.
	UpdateFlowSourcesForCurrent();
	const int32 SourceIndex = FindFlowSource(Probe.Where);
	const FFlowSource* Source = FlowSources.IsValidIndex(SourceIndex) ? &FlowSources[SourceIndex] : nullptr;
	UTextureRenderTarget2D* Velocity = Source && Source->bValid ? Source->Buffer.Get() : nullptr;
	FTextureRenderTargetResource* Resource = Velocity ? Velocity->GameThread_GetRenderTargetResource() : nullptr;
	const EPixelFormat Format = Velocity ? Velocity->GetFormat() : PF_Unknown;
	const int32 Resolution = Velocity ? Velocity->SizeX : 0;
	if (!Resource || !IsVelocityReadable(Format) || Resolution <= 0 || Source->SizeCm <= 0.0)
	{
		Probe.bOutside = true;   // in no simulation's square: no current known here
		return Found;
	}
	Probe.bOutside = false;
	const double Cell = Source->SizeCm / Resolution;
	const int32 Side = FMath::Clamp(FMath::CeilToInt32(2.0 * Probe.OuterCm / Cell) + 2, 4, Resolution);
	// FluidNinja Waves: the same square of the pressure buffer, for the swimmers' wave height. Only a core-made RGBA16F buffer carries
	// B and A (a stock RG16F one has neither; no waves then).
	UTextureRenderTarget2D* Pressure = Source->Pressure.Get();
	FTextureRenderTargetResource* PressureResource = Pressure && Pressure->GetFormat() == PF_FloatRGBA && Pressure->SizeX > 0
		? Pressure->GameThread_GetRenderTargetResource() : nullptr;
	int32 PressureSide = 0;
	const int32 PressureResolution = PressureResource ? Pressure->SizeX : 0;
	if (PressureResource)
	{
		const double PressureCell = Source->SizeCm / PressureResolution;
		PressureSide = FMath::Clamp(FMath::CeilToInt32(2.0 * FMath::Max(Probe.OuterCm * 0.5f, 40.f) / PressureCell) + 2, 4, PressureResolution);
	}
	if (Side != Probe.Side || PressureSide != Probe.PressureSide || Velocity != Probe.Buffer.Get() || Probe.Ring.IsEmpty())
	{
		// The readbacks reuse their staging textures, which must keep their size.
		Probe.Side = Side;
		Probe.PressureSide = PressureSide;
		Probe.Buffer = Velocity;
		Probe.Ring.Reset();   // one still in flight finishes on its own
		for (int32 Index = 0; Index < CurrentRingSize; ++Index)
		{
			Probe.Ring.Add(MakeShared<FFluidNinjaBuoyancyCurrentReadback, ESPMode::ThreadSafe>());
		}
	}
	TSharedPtr<FFluidNinjaBuoyancyCurrentReadback, ESPMode::ThreadSafe> Data;
	for (const TSharedPtr<FFluidNinjaBuoyancyCurrentReadback, ESPMode::ThreadSafe>& Candidate : Probe.Ring)
	{
		if (Candidate->Stage.load() == 0)
		{
			Data = Candidate;
			break;
		}
	}
	if (!Data)
	{
		return Found;   // all still in flight
	}
	// Velocity texels: column from X, row from Y, over the simulation square. The square slides to stay inside the texture.
	const double Column = ((Probe.Where.X - Source->Centre.X) / Source->SizeCm + 0.5) * Resolution - 0.5;
	const double Row = ((Probe.Where.Y - Source->Centre.Y) / Source->SizeCm + 0.5) * Resolution - 0.5;
	Data->Min = FIntPoint(FMath::Clamp(FMath::RoundToInt32(Column - Side * 0.5), 0, Resolution - Side),
		FMath::Clamp(FMath::RoundToInt32(Row - Side * 0.5), 0, Resolution - Side));
	Data->Side = Side;
	Data->Format = Format;
	Data->SimCentre = Source->Centre;
	Data->SimSizeCm = Source->SizeCm;
	Data->Resolution = Resolution;
	Data->FlowScale = Source->FlowScale;
	Data->Where = Probe.Where;
	Data->Time = Now;
	Data->bPressure = false;
	if (PressureResource && PressureSide > 0)
	{
		const double PressureColumn = ((Probe.Where.X - Source->Centre.X) / Source->SizeCm + 0.5) * PressureResolution - 0.5;
		const double PressureRow = ((Probe.Where.Y - Source->Centre.Y) / Source->SizeCm + 0.5) * PressureResolution - 0.5;
		Data->PressureMin = FIntPoint(FMath::Clamp(FMath::RoundToInt32(PressureColumn - PressureSide * 0.5), 0, PressureResolution - PressureSide),
			FMath::Clamp(FMath::RoundToInt32(PressureRow - PressureSide * 0.5), 0, PressureResolution - PressureSide));
		Data->PressureSide = PressureSide;
		Data->PressureResolution = PressureResolution;
		Data->bPressure = true;
	}
	Data->Generation.fetch_add(1);
	Data->Stage = 1;
	ENQUEUE_RENDER_COMMAND(FluidNinjaBuoyancyCurrentCopy)([Data, Resource, PressureResource](FRHICommandListImmediate& RHICmdList)
	{
		FRHITexture* Texture = Resource ? Resource->GetRenderTargetTexture() : nullptr;
		if (!Texture)
		{
			Data->Stage = 0;
			return;
		}
		Data->Readback.EnqueueCopy(RHICmdList, Texture, FIntVector(Data->Min.X, Data->Min.Y, 0), 0, FIntVector(Data->Side, Data->Side, 1));
		FRHITexture* PressureTexture = Data->bPressure && PressureResource ? PressureResource->GetRenderTargetTexture() : nullptr;
		if (PressureTexture)
		{
			Data->PressureReadback.EnqueueCopy(RHICmdList, PressureTexture, FIntVector(Data->PressureMin.X, Data->PressureMin.Y, 0), 0,
				FIntVector(Data->PressureSide, Data->PressureSide, 1));
		}
		else
		{
			Data->bPressure = false;
		}
	});
	return Found;
}

void UFluidNinjaBuoyancySubsystem::CollectCurrentProbes()
{
	using namespace FluidNinjaBuoyancyCurrentReads;
	const UWorld* World = GetWorld();
	if (!World || CurrentProbes.IsEmpty())
	{
		return;
	}
	const double Now = World->GetTimeSeconds();
	CurrentProbes.RemoveAll([Now](const FCurrentProbe& Probe) { return Now - Probe.LastRequest > CurrentIdleSeconds; });

	// Render thread: read out what the GPU has finished, each with the copy it is for (as CollectReadbacks).
	TArray<TPair<TSharedPtr<FFluidNinjaBuoyancyCurrentReadback, ESPMode::ThreadSafe>, uint32>> Pending;
	for (const FCurrentProbe& Probe : CurrentProbes)
	{
		for (const TSharedPtr<FFluidNinjaBuoyancyCurrentReadback, ESPMode::ThreadSafe>& Data : Probe.Ring)
		{
			if (Data->Stage.load() == 1)
			{
				Pending.Emplace(Data, Data->Generation.load());
			}
		}
	}
	if (!Pending.IsEmpty())
	{
		ENQUEUE_RENDER_COMMAND(FluidNinjaBuoyancyCurrentRead)([Pending](FRHICommandListImmediate&)
		{
			for (const TPair<TSharedPtr<FFluidNinjaBuoyancyCurrentReadback, ESPMode::ThreadSafe>, uint32>& Entry : Pending)
			{
				FFluidNinjaBuoyancyCurrentReadback& Data = *Entry.Key;
				if (Data.Stage.load() == 1 && Data.Generation.load() == Entry.Value && Data.Readback.IsReady() && (!Data.bPressure || Data.PressureReadback.IsReady()))
				{
					ReadOutCurrent(Data);
				}
			}
		});
	}

	// Game thread: each probe's newest finished square gives its current, the mean flow on its ring around where it was read, and the mean
	// right at that place (within a few texels).
	const float MaxSpeed = UFluidNinjaBuoyancySettings::Get().MaxFlowSpeedCmS;
	for (FCurrentProbe& Probe : CurrentProbes)
	{
		FFluidNinjaBuoyancyCurrentReadback* Newest = nullptr;
		for (const TSharedPtr<FFluidNinjaBuoyancyCurrentReadback, ESPMode::ThreadSafe>& Data : Probe.Ring)
		{
			if (Data->Stage.load() != 2)
			{
				continue;
			}
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
		if (!Newest)
		{
			continue;
		}
		const int32 N = Newest->Side;
		if (N > 0 && Newest->Texels.Num() == N * N && Newest->Resolution > 0 && Newest->SimSizeCm > 0.0)
		{
			const double Cell = Newest->SimSizeCm / Newest->Resolution;
			const double Inner = Probe.InnerCm;
			const double Outer = Probe.OuterCm;
			const double Local = FMath::Max(FMath::Min(Inner * 0.5, 40.0), Cell * 0.75);
			FVector2D Sum = FVector2D::ZeroVector;
			int32 Count = 0;
			FVector2D LocalSum = FVector2D::ZeroVector;
			int32 LocalCount = 0;
			for (int32 Row = 0; Row < N; ++Row)
			{
				for (int32 Column = 0; Column < N; ++Column)
				{
					// Texel (column, row) of the buffer: x from the column, y from the row, over the simulation square.
					const double X = Newest->SimCentre.X + ((Newest->Min.X + Column + 0.5) / Newest->Resolution - 0.5) * Newest->SimSizeCm;
					const double Y = Newest->SimCentre.Y + ((Newest->Min.Y + Row + 0.5) / Newest->Resolution - 0.5) * Newest->SimSizeCm;
					const double Distance = FVector2D::Distance(FVector2D(X, Y), Newest->Where);
					FVector2f Flow = Newest->Texels[Row * N + Column] * Newest->FlowScale;
					if (Flow.SizeSquared() > MaxSpeed * MaxSpeed)
					{
						Flow = Flow.GetSafeNormal() * MaxSpeed;
					}
					if (Distance <= Local)
					{
						LocalSum += FVector2D(Flow);
						++LocalCount;
					}
					if (Distance >= Inner && Distance <= Outer)
					{
						Sum += FVector2D(Flow);
						++Count;
					}
				}
			}
			if (Count > 0)
			{
				Probe.Current = Sum / Count;
				Probe.AtCentre = LocalCount > 0 ? LocalSum / LocalCount : Probe.Current;
				Probe.ReadWhere = Newest->Where;
				Probe.ReadTime = Newest->Time;
				// The waves at the probe: η's mean over the centre disc and its slope (a least-squares plane over the square), for
				// SampleWater's surface and normal.
				Probe.bWave = false;
				if (Newest->bPressure && Newest->Etas.Num() == Newest->PressureSide * Newest->PressureSide && Newest->PressureResolution > 0)
				{
					const int32 M = Newest->PressureSide;
					const double PressureCell = Newest->SimSizeCm / Newest->PressureResolution;
					const double LocalWave = FMath::Max(40.0, PressureCell * 1.5);
					double EtaSum = 0.0;
					int32 EtaCount = 0;
					double PS[3][3] = {};
					double PR[3] = {};
					for (int32 Row = 0; Row < M; ++Row)
					{
						for (int32 Column = 0; Column < M; ++Column)
						{
							const double X = Newest->SimCentre.X + ((Newest->PressureMin.X + Column + 0.5) / Newest->PressureResolution - 0.5) * Newest->SimSizeCm;
							const double Y = Newest->SimCentre.Y + ((Newest->PressureMin.Y + Row + 0.5) / Newest->PressureResolution - 0.5) * Newest->SimSizeCm;
							const double Eta = Newest->Etas[Row * M + Column];
							const double Basis[3] = {1.0, X - Newest->Where.X, Y - Newest->Where.Y};
							for (int32 A = 0; A < 3; ++A)
							{
								for (int32 B = 0; B < 3; ++B)
								{
									PS[A][B] += Basis[A] * Basis[B];
								}
								PR[A] += Basis[A] * Eta;
							}
							if (FVector2D::Distance(FVector2D(X, Y), Newest->Where) <= LocalWave)
							{
								EtaSum += Eta;
								++EtaCount;
							}
						}
					}
					if (EtaCount > 0)
					{
						Probe.WaveHeight = static_cast<float>(EtaSum / EtaCount);
						double Fit[3];
						Probe.WaveGradient = SolvePlane(PS, PR, Fit) ? FVector2f(static_cast<float>(Fit[1]), static_cast<float>(Fit[2])) : FVector2f::ZeroVector;
						Probe.bWave = true;
					}
				}
			}
		}
		Newest->Stage = 0;
	}
}

void UFluidNinjaBuoyancySubsystem::WriteSwimmerWakes(float DeltaSeconds)
{
	using namespace FluidNinjaBuoyancyCurrentReads;
	LastSwimmerWakePoints = 0;
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	UWorld* World = GetWorld();

	// The player's pawn: whether it swims now, and how it moved while it did not (Character Movement damps a plunge on the frame it starts
	// to swim, so the fall is the frame before's).
	APawn* Pawn = Swimmer.Get();
	const bool bSwimming = Pawn && bSwimmerSwimming;
	const bool bEntered = bSwimming && !bSwimmerWasSwimming;
	const FVector AirVelocity = SwimmerAirVelocity;
	bSwimmerWasSwimming = bSwimming;
	if (Pawn && !bSwimming)
	{
		SwimmerAirVelocity = Pawn->GetVelocity();
	}

	TArray<TPair<USkeletalMeshComponent*, FFluidNinjaSwimmerSource>> Registered;
	if (Swimmers)
	{
		Swimmers->GetRegistered(Registered);
	}
	SwimmerWakes.RemoveAll([&Registered](const FSwimmerWake& State)
	{
		return !Registered.ContainsByPredicate([&State](const TPair<USkeletalMeshComponent*, FFluidNinjaSwimmerSource>& Entry) { return Entry.Key == State.Mesh.Get(); });
	});
	if (!World || !Settings.SwimmerWakesEnabled() || !WakeChannel
		|| bControlledWater || DeltaSeconds <= 0.f)
	{
		SwimmerWakes.Reset();
		return;
	}
	const double Now = World->GetTimeSeconds();
	if (FluidNinjaBuoyancyCVars::WakeAlongOnly != 0)
	{
		static double LastAlongOnlyLog = -1.0;
		if (Now < LastAlongOnlyLog || Now - LastAlongOnlyLog >= 2.0)
		{
			LastAlongOnlyLog = Now;
			UE_LOG(LogFluidNinjaBuoyancy, Log, TEXT("FluidNinja Buoyancy: wake along-only at %.1f s: side wake points written %lld, skipped %lld; swimmer points written %lld, skipped %lld"),
				Now, FluidNinjaBuoyancyCVars::WakeAlongOnlyHullWritten.load(), FluidNinjaBuoyancyCVars::WakeAlongOnlyHullSkipped.load(),
				FluidNinjaBuoyancyCVars::WakeAlongOnlySwimmerWritten.load(), FluidNinjaBuoyancyCVars::WakeAlongOnlySwimmerSkipped.load());
		}
	}
	const double MinWrite = FMath::Max(FluidNinjaBuoyancyCVars::WakeMinWriteCmS, 0.f);
	if (MinWrite > 0.0)
	{
		static double LastMinWriteLog = -1.0;
		if (Now < LastMinWriteLog || Now - LastMinWriteLog >= 2.0)
		{
			LastMinWriteLog = Now;
			UE_LOG(LogFluidNinjaBuoyancy, Verbose, TEXT("FluidNinja Buoyancy: wake cut-off %.0f cm/s at %.1f s: current probes spared %lld, swimmer points skipped %lld"),
				MinWrite, Now, FluidNinjaBuoyancyCVars::WakeMinWriteProbesSkipped.load(), FluidNinjaBuoyancyCVars::WakeMinWritePointsSkipped.load());
		}
	}

	struct FWakePoint
	{
		FVector Position;
		FVector Previous;
		FVector4 VelocityAndSize;
	};
	TArray<FWakePoint, TInlineAllocator<16>> Wakes;

	// One swimmer, its current read on the ring around it. Plunging in: a ring pushing outward, as hard as the fall (once, as it goes in).
	// At the surface: the body pushes the water it moves through (under the surface it leaves the surface alone). The target is the current
	// plus a share of the swimmer's speed through it (Swimmer Wake Strength); the point carries a share (Swimmer Wake Gain) of what the water
	// at the swimmer still lacks of it along the swimmer's way. FluidNinja ignores points under about 40 cm/s and paints dye with faster
	// ones, which speeds a driven current up along its flow (Docs/FluidNinjaWaves.md, Wakes and the current). Wading: at each foot instead.
	// Tuning: the swimmer's own (its provider's Wake Tuning with Override on), else the project's Swimmers settings.
	auto AddSwimmer = [&](const FVector& Location, float SurfaceZ, const FVector& Velocity, double FallSpeed, bool bAtSurface, TConstArrayView<FVector> Feet,
		const FFluidNinjaSwimmerWakeTuning& Tuning)
	{
		const double Size = Tuning.SizeCm;
		const bool bPlunge = FallSpeed > PlungeSpeedCmS;
		const int32 Points = Feet.IsEmpty() ? (bAtSurface ? 1 : 0) : Feet.Num();
		const double PointScale = Feet.IsEmpty() ? 1.0 : 0.7;   // a foot's point carries 0.7 of the push (below)
		// fnb.Wake.MinWriteCmS: the probe is spared when no point of this swimmer can reach the cut-off (a plunge needs none for its ring).
		// Before the current is read, the swimmer's own speed stands in for its speed through the water: the point is at most Swimmer Wake
		// Strength times that times Swimmer Wake Gain.
		int32 Index = INDEX_NONE;
		if (MinWrite <= 0.0 || bPlunge || (Points > 0 && Tuning.Strength * FVector2D(Velocity).Size() * Tuning.Gain * PointScale >= MinWrite))
		{
			Index = RequestCurrent(FVector2D(Location), Tuning.RingInnerCm, Tuning.RingOuterCm);
		}
		else
		{
			++FluidNinjaBuoyancyCVars::WakeMinWriteProbesSkipped;
		}
		const FCurrentProbe* Probe = CurrentProbes.IsValidIndex(Index) ? &CurrentProbes[Index] : nullptr;
		const bool bFresh = Probe && !Probe->bOutside && Probe->ReadTime >= 0.0 && Now - Probe->ReadTime < CurrentFreshSeconds;
		const FVector2D Water = bFresh ? Probe->Current : FVector2D::ZeroVector;
		const FVector2D AtSwimmer = bFresh ? Probe->AtCentre : FVector2D::ZeroVector;

		if (bPlunge)
		{
			const double Push = FMath::Clamp(FallSpeed * 0.35, 80.0, 300.0);
			if (MinWrite > 0.0 && Push < MinWrite)
			{
				FluidNinjaBuoyancyCVars::WakeMinWritePointsSkipped += 8;   // only with the cut-off raised past the ring's push
			}
			else
			{
				for (int32 Spoke = 0; Spoke < 8; ++Spoke)
				{
					const double Angle = Spoke * UE_DOUBLE_TWO_PI / 8.0;
					const FVector Outward(FMath::Cos(Angle), FMath::Sin(Angle), 0.0);
					const FVector At = FVector(Location.X, Location.Y, SurfaceZ) + Outward * 50.0;
					Wakes.Add({At, At - Outward * (Push * DeltaSeconds), FVector4(Outward.X * Push, Outward.Y * Push, 0.0, Size * 1.2)});
				}
				UE_LOG(LogFluidNinjaBuoyancy, Log, TEXT("FluidNinja Buoyancy: swimmer plunge at (%.0f, %.0f), %.0f cm/s, ring push %.0f cm/s"), Location.X, Location.Y,
					FallSpeed, Push);
			}
		}

		const FVector Through(Velocity.X - Water.X, Velocity.Y - Water.Y, 0.0);
		const double Speed = Through.Size();
		const FVector2D Way(Through.GetSafeNormal());
		const double Target = FVector2D::DotProduct(Water, Way) + Tuning.Strength * Speed;
		const double Lack = Target - FVector2D::DotProduct(AtSwimmer, Way);
		if (Speed < Tuning.MinSpeedCmS || Lack <= 0.0)
		{
			return;
		}
		// fnb.Wake.MinWriteCmS: a point FluidNinja would ignore is not written (the along-only A/B below counts only points that reach it).
		if (MinWrite > 0.0 && Lack * Tuning.Gain * PointScale < MinWrite)
		{
			FluidNinjaBuoyancyCVars::WakeMinWritePointsSkipped += Points;
			return;
		}
		// fnb.Wake.AlongOnly (A/B): no points pushing against the current around the swimmer (holding against it, swimming upstream).
		if (FluidNinjaBuoyancyCVars::WakeAlongOnly != 0 && Points > 0)
		{
			if (FVector2D::DotProduct(Way, Water) < -FluidNinjaBuoyancyCVars::WakeAlongOnlyMinCmS)
			{
				FluidNinjaBuoyancyCVars::WakeAlongOnlySwimmerSkipped += Points;
				return;
			}
			FluidNinjaBuoyancyCVars::WakeAlongOnlySwimmerWritten += Points;
		}
		const FVector2D Push = Way * (Lack * Tuning.Gain);
		if (Feet.IsEmpty())
		{
			if (bAtSurface)
			{
				const FVector At(Location.X, Location.Y, SurfaceZ);
				Wakes.Add({At, At - Velocity * DeltaSeconds, FVector4(Push.X, Push.Y, 0.0, Size * FMath::Clamp(Speed / 100.0, 0.6, 1.5))});
			}
			return;
		}
		for (const FVector& Foot : Feet)
		{
			const FVector At(Foot.X, Foot.Y, SurfaceZ);
			Wakes.Add({At, At - Velocity * DeltaSeconds, FVector4(Push.X * 0.7, Push.Y * 0.7, 0.0, Size * 2.0 / 3.0)});
		}
	};

	// The player's pawn, swimming or wading over a water volume. When its mesh is registered as a swimmer, it is that registered swimmer
	// only (one wake per body, whichever movement system it has).
	const bool bPlayerRegistered = Pawn && Registered.ContainsByPredicate([Pawn](const TPair<USkeletalMeshComponent*, FFluidNinjaSwimmerSource>& Entry)
	{
		return Entry.Key->GetOwner() == Pawn;
	});
	if (Pawn && !bPlayerRegistered && bSwimmerInWater && bSwimmerSurfaceKnown && SwimmerSurfaceFrame == GFrameCounter)
	{
		// An unregistered pawn has no provider of its own here: the project's tuning.
		const FFluidNinjaSwimmerWakeTuning Tuning = FFluidNinjaBuoyancySwimmers::ProjectWakeTuning();
		const FVector Location = Pawn->GetActorLocation();
		const FVector Velocity = Pawn->GetVelocity();
		if (bSwimming)
		{
			const double Fall = bEntered ? -FMath::Min(Velocity.Z, AirVelocity.Z) : 0.0;
			const double Top = Location.Z + Pawn->GetSimpleCollisionHalfHeight();
			AddSwimmer(Location, SwimmerSurfaceZ, Velocity, Fall, SwimmerSurfaceZ - Top < SurfaceReachCm, {}, Tuning);
		}
		else if (bSwimmerWading)
		{
			// Wading: at each foot (of the first skeletal mesh with feet; the pawn's centre without one).
			TArray<FVector, TInlineAllocator<2>> Feet;
			TInlineComponentArray<USkeletalMeshComponent*> Meshes(Pawn);
			for (const USkeletalMeshComponent* Mesh : Meshes)
			{
				if (Mesh->GetBoneIndex(TEXT("foot_l")) != INDEX_NONE && Mesh->GetBoneIndex(TEXT("foot_r")) != INDEX_NONE)
				{
					Feet.Add(Mesh->GetBoneLocation(TEXT("foot_l")));
					Feet.Add(Mesh->GetBoneLocation(TEXT("foot_r")));
					break;
				}
			}
			if (Feet.IsEmpty())
			{
				Feet.Add(Location);
			}
			AddSwimmer(Location, SwimmerSurfaceZ, Velocity, 0.0, true, Feet, Tuning);
		}
	}

	// Registered swimmers, from the pose each was ticked with this frame (TickSwimmerParts runs first): its hips sphere (else the pose's
	// reference point) against its still water says whether it is in the water, its head sphere (else its hips) whether it is at the
	// surface. Its velocity is the pose's (a provider's, a pawn's), else the mesh's own motion frame to frame; the hips' fall counts
	// for the plunge too (a dive clip may move only them). With no pose this frame (hidden, or its provider says it is no swimmer) it makes
	// no wake, and its motion starts over when it is back.
	for (const TPair<USkeletalMeshComponent*, FFluidNinjaSwimmerSource>& Entry : Registered)
	{
		USkeletalMeshComponent* Mesh = Entry.Key;
		FSwimmerWake* State = SwimmerWakes.FindByPredicate([Mesh](const FSwimmerWake& Candidate) { return Candidate.Mesh.Get() == Mesh; });
		if (!State)
		{
			State = &SwimmerWakes.AddDefaulted_GetRef();
			State->Mesh = Mesh;
		}
		FFluidNinjaSwimmerPose Pose;
		if (!Swimmers || !Swimmers->GetPose(Mesh, Pose))
		{
			State->bHasPrevious = false;
			State->bWasIn = false;
			State->FallSpeed = 0.0;
			continue;
		}
		const bool bHipsSphere = Pose.bHasSpheres && Pose.HipsRadiusCm > 0.f;
		// The body's motion: the mesh's own, as the plugin always took it (a mesh-only pose's reference point is its hips bone, which sways
		// with the swim clip).
		const FVector Location = Mesh->GetComponentLocation();
		const FVector Hips = bHipsSphere ? Pose.Hips : Pose.Location;
		const double InReach = bHipsSphere ? static_cast<double>(Pose.HipsRadiusCm) : HipsRadiusCm;
		const double FallBefore = State->FallSpeed;
		const bool bPoseVelocity = !Pose.Velocity.ContainsNaN() && !Pose.Velocity.IsNearlyZero() && Pose.Velocity.Size() < SwimmerMaxSpeedCmS;
		FVector Velocity = bPoseVelocity ? Pose.Velocity : FVector::ZeroVector;
		double HipsFall = 0.0;
		if (State->bHasPrevious)
		{
			const double Moved = FVector::Dist(Location, State->Previous);
			if (!bPoseVelocity && Moved < SwimmerTeleportCm && Moved < SwimmerMaxSpeedCmS * DeltaSeconds + 1.0)
			{
				Velocity = (Location - State->Previous) / DeltaSeconds;
			}
			const double HipsMoved = FVector::Dist(Hips, State->HipsPrevious);
			if (HipsMoved < SwimmerTeleportCm && HipsMoved < SwimmerMaxSpeedCmS * DeltaSeconds + 1.0)
			{
				HipsFall = (State->HipsPrevious.Z - Hips.Z) / DeltaSeconds;
			}
		}
		State->Previous = Location;
		State->HipsPrevious = Hips;
		State->bHasPrevious = true;
		const double Fall = FMath::Max(-Velocity.Z, HipsFall);
		State->FallSpeed = Fall;
		const float WaterZ = Entry.Value.WaterZ;
		const double Depth = WaterZ - Hips.Z;
		const bool bIn = Depth > -InReach;
		const bool bEnteredWater = bIn && !State->bWasIn;
		State->bWasIn = bIn;
		if (!bIn)
		{
			continue;
		}
		const double Top = Pose.bHasSpheres && Pose.HeadRadiusCm > 0.f ? FMath::Max(Hips.Z, Pose.Head.Z) : Hips.Z;
		AddSwimmer(Hips, WaterZ, Velocity, bEnteredWater ? FMath::Max(Fall, FallBefore) : 0.0, WaterZ - Top < SurfaceReachCm, {}, Swimmers->GetWakeTuning(Mesh));
	}

	if (Wakes.IsEmpty())
	{
		return;
	}
	if (UNiagaraDataChannelWriter* Writer = UNiagaraDataChannelLibrary::WriteToNiagaraDataChannel(World, WakeChannel, FNiagaraDataChannelSearchParameters(),
		Wakes.Num(), false, true, true, TEXT("FluidNinja Buoyancy swimmers")))
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
		LastSwimmerWakePoints = Wakes.Num();
	}
}
