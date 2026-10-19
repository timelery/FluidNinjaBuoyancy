#include "FluidNinjaBuoyancyWaveSource.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalRenderResources.h"
#include "GlobalShader.h"
#include "Misc/ScopeLock.h"
#include "PixelFormat.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RendererInterface.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "RHIStaticStates.h"
#include "ShaderCore.h"
#include "ShaderParameterStruct.h"

DECLARE_GPU_STAT_NAMED(FluidNinjaBuoyancyWaveSource, TEXT("FluidNinja Buoyancy wave sources"));
// The pass families, so stat gpu splits the total above (the engine's GPU stats come from the _STAT event scopes; RDG_GPU_STAT_SCOPE is gone in 5.8).
DECLARE_GPU_STAT_NAMED(FNB_Raster, TEXT("FNB Raster"));
DECLARE_GPU_STAT_NAMED(FNB_Splat, TEXT("FNB Splat"));
DECLARE_GPU_STAT_NAMED(FNB_Assemble, TEXT("FNB Assemble"));
DECLARE_GPU_STAT_NAMED(FNB_DeepHead, TEXT("FNB DeepHead"));
DECLARE_GPU_STAT_NAMED(FNB_SelfBegin, TEXT("FNB SelfBegin"));
DECLARE_GPU_STAT_NAMED(FNB_SelfPrepare, TEXT("FNB SelfPrepare"));
DECLARE_GPU_STAT_NAMED(FNB_SelfMip, TEXT("FNB SelfMip"));
DECLARE_GPU_STAT_NAMED(FNB_SelfStep, TEXT("FNB SelfStep"));
DECLARE_GPU_STAT_NAMED(FNB_SelfExport, TEXT("FNB SelfExport"));
DECLARE_GPU_STAT_NAMED(FNB_Probes, TEXT("FNB Probes"));
DEFINE_LOG_CATEGORY_STATIC(LogFluidNinjaBuoyancyWaveSource, Log, All);

// ---------------------------------------------------------------------------------------------------------------------------------
// Shaders (FluidNinjaBuoyancyWaveSource.usf). Every pass is a global compute shader for SM5 and up.

#define FNB_WAVE_GLOBAL_SHADER(ShaderClass) \
	DECLARE_GLOBAL_SHADER(ShaderClass); \
	SHADER_USE_PARAMETER_STRUCT(ShaderClass, FGlobalShader); \
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) \
	{ \
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5); \
	}

/** The grid, shared by most passes. */
BEGIN_SHADER_PARAMETER_STRUCT(FFluidNinjaBuoyancyWaveGrid, )
	SHADER_PARAMETER(FIntPoint, GridSize)
	SHADER_PARAMETER(float, CellCm)
END_SHADER_PARAMETER_STRUCT()

/** The raster's inputs, shared by a simulation's and the self fields' raster. */
BEGIN_SHADER_PARAMETER_STRUCT(FFluidNinjaBuoyancyWaveRasterInputs, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FFluidNinjaBuoyancyWaveGrid, Grid)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, BodyData)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, BodyInfo)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Triangles)
	SHADER_PARAMETER(int32, NumBodies)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, CaptureTexture)
	SHADER_PARAMETER(int32, HasCapture)
	SHADER_PARAMETER(float, CaptureTexel)
	SHADER_PARAMETER(int32, CaptureSize)
	SHADER_PARAMETER(float, CaptureTop)
	SHADER_PARAMETER(float, CaptureFar)
	SHADER_PARAMETER(int32, Controlled)
	SHADER_PARAMETER(float, ControlledHeight)
END_SHADER_PARAMETER_STRUCT()

/** A batch of self fields: N x N cells each, and their records (WaveSelfInfoStride int4 each). */
BEGIN_SHADER_PARAMETER_STRUCT(FFluidNinjaBuoyancyWaveSelfBatch, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FFluidNinjaBuoyancyWaveGrid, Grid)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, SelfInfo)
END_SHADER_PARAMETER_STRUCT()

class FFluidNinjaBuoyancyWaveRasterCS : public FGlobalShader
{
public:
	FNB_WAVE_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveRasterCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FFluidNinjaBuoyancyWaveRasterInputs, Raster)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, HeadOut)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, SpeedOut)
	END_SHADER_PARAMETER_STRUCT()
};

class FFluidNinjaBuoyancyWaveSelfRasterCS : public FGlobalShader
{
public:
	FNB_WAVE_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveSelfRasterCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FFluidNinjaBuoyancyWaveRasterInputs, Raster)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, HeadPool)
	END_SHADER_PARAMETER_STRUCT()
};

class FFluidNinjaBuoyancyWaveSplatCS : public FGlobalShader
{
public:
	FNB_WAVE_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveSplatCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FFluidNinjaBuoyancyWaveGrid, Grid)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Impulses)
		SHADER_PARAMETER(int32, NumImpulses)
		SHADER_PARAMETER(float, ImpulseFixedScale)
		SHADER_PARAMETER(float, SmoothCells)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, ImpulseRW)
	END_SHADER_PARAMETER_STRUCT()
};

class FFluidNinjaBuoyancyWaveSelfSplatCS : public FGlobalShader
{
public:
	FNB_WAVE_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveSelfSplatCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FFluidNinjaBuoyancyWaveGrid, Grid)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Impulses)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ImpulseSlots)
		SHADER_PARAMETER(int32, NumImpulses)
		SHADER_PARAMETER(float, ImpulseFixedScale)
		SHADER_PARAMETER(float, SmoothCells)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ImpulsePool)
	END_SHADER_PARAMETER_STRUCT()
};

class FFluidNinjaBuoyancyWavePokeCS : public FGlobalShader
{
public:
	FNB_WAVE_GLOBAL_SHADER(FFluidNinjaBuoyancyWavePokeCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FFluidNinjaBuoyancyWaveGrid, Grid)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Pokes)
		SHADER_PARAMETER(int32, NumPokes)
		SHADER_PARAMETER(float, PokeToFixed)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, ImpulseRW)
	END_SHADER_PARAMETER_STRUCT()
};

class FFluidNinjaBuoyancyWaveDeepHeadCS : public FGlobalShader
{
public:
	FNB_WAVE_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveDeepHeadCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FFluidNinjaBuoyancyWaveGrid, Grid)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, DeepData)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, DeepInfo)
		SHADER_PARAMETER(int32, NumDeep)
		SHADER_PARAMETER(float, DeepFixedScale)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, DeepOut)
	END_SHADER_PARAMETER_STRUCT()
};

class FFluidNinjaBuoyancyWaveAssembleCS : public FGlobalShader
{
public:
	FNB_WAVE_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveAssembleCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FFluidNinjaBuoyancyWaveGrid, Grid)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, HeadIn)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, SpeedIn)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, ImpulseIn)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, DeepIn)
		SHADER_PARAMETER(int32, HasDeep)
		SHADER_PARAMETER(float, DeepToCm)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, EraseBoxes)
		SHADER_PARAMETER(int32, NumErase)
		SHADER_PARAMETER(int32, EraseAll)
		SHADER_PARAMETER(float, ImpulseToHead)
		SHADER_PARAMETER(FVector4f, Header0)
		SHADER_PARAMETER(FVector4f, Header1)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, SourceOut)
	END_SHADER_PARAMETER_STRUCT()
};

class FFluidNinjaBuoyancyWaveSelfShiftCS : public FGlobalShader
{
public:
	FNB_WAVE_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveSelfShiftCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FFluidNinjaBuoyancyWaveSelfBatch, Batch)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float2>, StatePoolIn)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ImpulsePoolIn)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float2>, ShiftStateOut)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ShiftImpulseOut)
	END_SHADER_PARAMETER_STRUCT()
};

class FFluidNinjaBuoyancyWaveSelfBeginCS : public FGlobalShader
{
public:
	FNB_WAVE_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveSelfBeginCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FFluidNinjaBuoyancyWaveSelfBatch, Batch)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float2>, ShiftStateIn)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ShiftImpulseIn)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float2>, StatePool)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, HeadPool)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ImpulsePool)
	END_SHADER_PARAMETER_STRUCT()
};

class FFluidNinjaBuoyancyWaveSelfResetCS : public FGlobalShader
{
public:
	FNB_WAVE_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveSelfResetCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FFluidNinjaBuoyancyWaveSelfBatch, Batch)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, HeadPoolIn)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float2>, StatePool)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, SelfTileOut)
	END_SHADER_PARAMETER_STRUCT()
};

class FFluidNinjaBuoyancyWaveSelfPrepareCS : public FGlobalShader
{
public:
	FNB_WAVE_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveSelfPrepareCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FFluidNinjaBuoyancyWaveSelfBatch, Batch)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float2>, StatePoolIn)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, HeadPoolIn)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ImpulsePoolIn)
		SHADER_PARAMETER(float, HeadPerUnit)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, AuxTexture)
		SHADER_PARAMETER(FIntPoint, SimCells)
		SHADER_PARAMETER(int32, EdgeOpen)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, WaveOpOut)
	END_SHADER_PARAMETER_STRUCT()
};

class FFluidNinjaBuoyancyWaveSelfMipCS : public FGlobalShader
{
public:
	FNB_WAVE_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveSelfMipCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float4>, MipIn)
		SHADER_PARAMETER(FIntPoint, MipInSize)
		SHADER_PARAMETER(FIntPoint, MipOutSize)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, MipOut)
		SHADER_PARAMETER_SAMPLER(SamplerState, WaveOpSampler)
	END_SHADER_PARAMETER_STRUCT()
};

class FFluidNinjaBuoyancyWaveSelfStepCS : public FGlobalShader
{
public:
	FNB_WAVE_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveSelfStepCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FFluidNinjaBuoyancyWaveSelfBatch, Batch)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float4>, WaveOp)
		SHADER_PARAMETER_SAMPLER(SamplerState, WaveOpSampler)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, HeadPoolIn)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, AuxTexture)
		SHADER_PARAMETER(FIntPoint, SimCells)
		SHADER_PARAMETER(int32, EdgeOpen)
		SHADER_PARAMETER(float, Damping)
		SHADER_PARAMETER(float, Hyperviscosity)
		SHADER_PARAMETER(float, SimSpongeCm)
		SHADER_PARAMETER(float, WallAbsorption)
		SHADER_PARAMETER(float, WallBandCm)
		SHADER_PARAMETER(float, ShoreDepthCm)
		SHADER_PARAMETER(float, BreakingSlope)
		SHADER_PARAMETER(float, MeanRelax)
		SHADER_PARAMETER(int32, TopMip)
		SHADER_PARAMETER(float, TickSeconds)
		SHADER_PARAMETER(float, SpongeRate)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float2>, StatePool)
	END_SHADER_PARAMETER_STRUCT()
};

class FFluidNinjaBuoyancyWaveSelfExportCS : public FGlobalShader
{
public:
	FNB_WAVE_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveSelfExportCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FFluidNinjaBuoyancyWaveSelfBatch, Batch)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float2>, StatePoolIn)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, HeadPoolIn)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ImpulsePool)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, SelfTileOut)
	END_SHADER_PARAMETER_STRUCT()
};

class FFluidNinjaBuoyancyWaveProbeCS : public FGlobalShader
{
public:
	FNB_WAVE_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveProbeCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FFluidNinjaBuoyancyWaveGrid, Grid)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, PressureTexture)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float2>, ProbePoints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, ProbeSlots)
		SHADER_PARAMETER(int32, NumProbes)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, ProbeValues)
	END_SHADER_PARAMETER_STRUCT()
};

class FFluidNinjaBuoyancyWaveStatsCS : public FGlobalShader
{
public:
	FNB_WAVE_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveStatsCS);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FFluidNinjaBuoyancyWaveGrid, Grid)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, PressureTexture)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, StatsPartials)
	END_SHADER_PARAMETER_STRUCT()
};

#undef FNB_WAVE_GLOBAL_SHADER

IMPLEMENT_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveRasterCS, "/Plugin/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancyWaveSource.usf", "RasterCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveSelfRasterCS, "/Plugin/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancyWaveSource.usf", "SelfRasterCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveSplatCS, "/Plugin/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancyWaveSource.usf", "SplatCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveSelfSplatCS, "/Plugin/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancyWaveSource.usf", "SelfSplatCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FFluidNinjaBuoyancyWavePokeCS, "/Plugin/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancyWaveSource.usf", "PokeCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveDeepHeadCS, "/Plugin/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancyWaveSource.usf", "DeepHeadCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveAssembleCS, "/Plugin/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancyWaveSource.usf", "AssembleCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveSelfShiftCS, "/Plugin/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancyWaveSource.usf", "SelfShiftCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveSelfBeginCS, "/Plugin/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancyWaveSource.usf", "SelfBeginCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveSelfResetCS, "/Plugin/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancyWaveSource.usf", "SelfResetCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveSelfPrepareCS, "/Plugin/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancyWaveSource.usf", "SelfPrepareCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveSelfMipCS, "/Plugin/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancyWaveSource.usf", "SelfMipCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveSelfStepCS, "/Plugin/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancyWaveSource.usf", "SelfStepCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveSelfExportCS, "/Plugin/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancyWaveSource.usf", "SelfExportCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveProbeCS, "/Plugin/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancyWaveSource.usf", "ProbeCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FFluidNinjaBuoyancyWaveStatsCS, "/Plugin/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancyWaveSource.usf", "StatsCS", SF_Compute);

namespace FluidNinjaBuoyancyWaveSource
{
	namespace
	{
		constexpr int32 WaveGroup = 8;           // 8 x 8 threads for the per-cell passes
		constexpr int32 WaveStatsGroup = 16;     // 16 x 16 threads per stats partial sum
		constexpr int32 WaveLineGroup = 64;      // threads per group for the point passes (probes, impulses)
		constexpr float WaveSplatMaxSigmaCells = 16.f;   // the widest impulse splat, σ in cells (FNB_WAVE_SPLAT_MAX_SIGMA in the shader)
		constexpr int32 WaveRasterLanes = 64;    // threads per raster group, one cell (FNB_WAVE_RASTER_LANES in the shader)
		constexpr int32 WaveBodyStride = 7;      // float4 per body in the raster's BodyData (FluidNinjaWaveBodyStride in the shader)
		constexpr int32 WaveMaxBodies = 65535;   // thread groups along z
		constexpr int32 WaveMaxProbes = 256;
		constexpr int32 WaveMaxErase = 256;
		constexpr int32 WaveSelfInfoStride = 3;  // int4 per self field in SelfInfo (FluidNinjaWaveSelfInfoStride in the shader)
		constexpr int32 WaveDeepStride = 3;      // float4 per deep head in DeepData (FluidNinjaWaveDeepStride in the shader)
		constexpr int32 WaveMaxDeep = 1024;      // deep heads per simulation (thread groups along z)
		/** The deep heads' fixed point: units per cm of P. A head is clamped to ±1e9 units (±15 000 cm) before it is added; the cells' sums
		 * stay far inside int32 for any number of sinkers that fit in one place. 1/65536 cm is far below anything the waves resolve. */
		constexpr float WaveDeepFixedScale = 65536.f;
		constexpr int32 WaveMinSelfPool = 4;     // slots a self-field pool starts with
		// A self field's flags in SelfInfo (FluidNinjaWaveSelf* in the shader).
		constexpr int32 WaveSelfNew = 1;
		constexpr int32 WaveSelfMoved = 2;
		constexpr int32 WaveSelfSources = 4;
		constexpr int32 WaveSelfReset = 8;

		FIntVector CellGroups(const FIntPoint& Cells, int32 Slices = 1)
		{
			return FIntVector(FMath::DivideAndRoundUp(Cells.X, WaveGroup), FMath::DivideAndRoundUp(Cells.Y, WaveGroup), Slices);
		}

		/** The pool of self fields of N cells across (N one of the self sizes). */
		int32 SelfPoolIndex(int32 N)
		{
			return static_cast<int32>(FMath::FloorLog2(static_cast<uint32>(N))) - static_cast<int32>(FMath::FloorLog2(static_cast<uint32>(MinSelfCells)));
		}

		/** A float's bits, for an int4 record. */
		int32 FloatBits(float Value)
		{
			int32 Bits = 0;
			FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
			return Bits;
		}

		FRDGTextureDesc CellDesc(const FIntPoint& Cells, EPixelFormat Format)
		{
			return FRDGTextureDesc::Create2D(Cells, Format, FClearValueBinding::Black, ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV);
		}

		/** A texture to bind where a pass declares an input it will not read this frame. */
		FRDGTextureRef BlackTexture(FRDGBuilder& GraphBuilder)
		{
			return RegisterExternalTexture(GraphBuilder, GBlackTexture->TextureRHI.GetReference(), TEXT("FluidNinjaBuoyancy.Waves.Black"));
		}

		bool IsSelfSize(int32 N)
		{
			return N == 64 || N == 128 || N == 256 || N == 512;
		}

		/** The grid cells whose centres may fall in a body's box, one cell of margin; empty if outside. */
		FIntRect BodyCells(const FBody& Body, const FIntPoint& Cells, float CellCm)
		{
			const FVector4f& Box = Body.Bounds;
			if (!(Box.Z > Box.X && Box.W > Box.Y))
			{
				return FIntRect();
			}
			if (!(Box.Z >= 0.f && Box.W >= 0.f && Box.X <= Cells.X * CellCm && Box.Y <= Cells.Y * CellCm))
			{
				return FIntRect();   // outside the grid (or not a number)
			}
			// Cell i's centre is at (i + 0.5)·Δ.
			const auto FirstCell = [CellCm](float Low, int32 Count) { return FMath::Clamp(FMath::CeilToInt(FMath::Clamp(Low / CellCm - 0.5f, -2.f, Count + 2.f)) - 1, 0, Count - 1); };
			const auto LastCell = [CellCm](float High, int32 Count) { return FMath::Clamp(FMath::FloorToInt(FMath::Clamp(High / CellCm - 0.5f, -2.f, Count + 2.f)) + 1, 0, Count - 1); };
			const FIntPoint MinCell(FirstCell(Box.X, Cells.X), FirstCell(Box.Y, Cells.Y));
			const FIntPoint MaxCell(LastCell(Box.Z, Cells.X), LastCell(Box.W, Cells.Y));
			if (MaxCell.X < MinCell.X || MaxCell.Y < MinCell.Y)
			{
				return FIntRect();
			}
			return FIntRect(MinCell, MaxCell + FIntPoint(1, 1));
		}
	}

	/** The render-thread side of FState: the self fields, the kept triangles and the readback rings. */
	struct FState::FGpu
	{
		/** The self fields of one size: (η, v) per cell, the static head (P bits, kept for a step on the next frame) and the pending impulse,
		 * a slot of N x N cells per field. */
		struct FSelfPool
		{
			int32 Capacity = 0;
			TBitArray<> Used;
			TRefCountPtr<FRDGPooledBuffer> State;
			TRefCountPtr<FRDGPooledBuffer> Head;
			TRefCountPtr<FRDGPooledBuffer> Impulse;
		};
		/** One body's self field: its size and its slot in that size's pool. */
		struct FSelfSlot
		{
			int32 Cells = 0;
			int32 Slot = INDEX_NONE;
		};
		/** 64, 128, 256 and 512 cells across. */
		static constexpr int32 NumSelfSizes = 4;
		struct FReadback
		{
			TUniquePtr<FRHIGPUBufferReadback> Buffer;
			bool bBusy = false;
			double Time = 0.0;
			/** Floats (probes) or float4 partial sums (stats). */
			int32 Count = 0;
			/** Stats: the cell (cm), gravity (cm/s²) and density (kg/cm³), to turn the sums into cm³ and joules. */
			double CellCm = 0.0;
			double Gravity = 0.0;
			double Density = 0.0;
		};
		static constexpr int32 RingSize = 4;
		/** One simulation's summed deep heads (a uint per cell, signed fixed point) and the records and grid they were summed for: reused
		 * while the next frame's are the same, so a sunk body at rest costs no dispatch. */
		struct FDeepCache
		{
			TRefCountPtr<FRDGPooledBuffer> Buffer;
			FIntPoint Cells = FIntPoint::ZeroValue;
			float CellCm = 0.f;
			TArray<FVector4f> Data;
			TArray<FUintVector4> Info;
		};

		FSelfPool Pools[NumSelfSizes];
		TMap<int32, FSelfSlot> Selves;
		/** By FSource::DeepKey. */
		TMap<uint32, FDeepCache> DeepCaches;
		TRefCountPtr<FRDGPooledBuffer> Triangles;
		uint32 TriangleSerial = ~0u;
		int32 TriangleCount = 0;
		FReadback Probes[RingSize];
		FReadback Stats[RingSize];
		bool bWarnedTarget = false;
		bool bWarnedSelfSize = false;
		bool bWarnedAtlas = false;
		bool bWarnedAux = false;
	};

	/** Everything with access to FState's private parts. */
	struct FStateAccess
	{
		static FState::FGpu::FReadback* FreeSlot(FState::FGpu::FReadback (&Ring)[FState::FGpu::RingSize], const TCHAR* Name)
		{
			for (FState::FGpu::FReadback& Slot : Ring)
			{
				if (!Slot.bBusy)
				{
					if (!Slot.Buffer)
					{
						Slot.Buffer = MakeUnique<FRHIGPUBufferReadback>(FName(Name));
					}
					return &Slot;
				}
			}
			return nullptr;   // all in flight: skip this frame's request rather than overwrite one
		}

		/** Copies out the finished readbacks and publishes the newest into the results. */
		static void Poll(FState& State, FState::FGpu& Gpu)
		{
			for (FState::FGpu::FReadback& Slot : Gpu.Probes)
			{
				if (!Slot.bBusy || !Slot.Buffer->IsReady())
				{
					continue;
				}
				TArray<float> Heights;
				const uint32 Bytes = static_cast<uint32>(Slot.Count) * sizeof(float);
				if (const float* Raw = static_cast<const float*>(Slot.Buffer->Lock(Bytes)))
				{
					Heights.Append(Raw, Slot.Count);
					Slot.Buffer->Unlock();
					FScopeLock Guard(&State.ResultsLock);
					if (Slot.Time >= State.Results.ProbeTime)
					{
						State.Results.ProbeTime = Slot.Time;
						State.Results.ProbeHeights = MoveTemp(Heights);
					}
				}
				Slot.bBusy = false;
			}
			for (FState::FGpu::FReadback& Slot : Gpu.Stats)
			{
				if (!Slot.bBusy || !Slot.Buffer->IsReady())
				{
					continue;
				}
				const uint32 Bytes = static_cast<uint32>(Slot.Count) * sizeof(FVector4f);
				if (const FVector4f* Partials = static_cast<const FVector4f*>(Slot.Buffer->Lock(Bytes)))
				{
					// Partial sums per 16 x 16 cells, added in double.
					double Sum = 0.0;
					double SumSquares = 0.0;
					double SumHead = 0.0;
					float MaxAbs = 0.f;
					for (int32 Index = 0; Index < Slot.Count; ++Index)
					{
						Sum += Partials[Index].X;
						SumSquares += Partials[Index].Y;
						SumHead += Partials[Index].Z;
						MaxAbs = FMath::Max(MaxAbs, Partials[Index].W);
					}
					Slot.Buffer->Unlock();
					// cm³; kg·cm²/s² x 1e-4 = J.
					const double CellArea = Slot.CellCm * Slot.CellCm;
					FScopeLock Guard(&State.ResultsLock);
					if (Slot.Time >= State.Results.StatsTime)
					{
						State.Results.StatsTime = Slot.Time;
						State.Results.VolumeCm3 = Sum * CellArea;
						State.Results.PotentialJ = 0.5 * Slot.Density * Slot.Gravity * SumSquares * CellArea * 1.0e-4;
						State.Results.MaxAbsCm = MaxAbs;
						State.Results.SourceVolumeCm3 = SumHead * CellArea;
					}
				}
				Slot.bBusy = false;
			}
		}

		/** The bodies' triangles on the GPU: uploaded once and kept while their serial and count stay the same. */
		static FRDGBufferRef AddTriangles(FRDGBuilder& GraphBuilder, const FFrame& Frame, FState::FGpu& Gpu)
		{
			static const TArray<FVector4f> None = {FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero()};
			if (!Frame.Triangles.IsValid() || Frame.Triangles->IsEmpty())
			{
				return CreateStructuredBuffer(GraphBuilder, TEXT("FluidNinjaBuoyancy.Waves.NoTriangles"), None);
			}
			const TArray<FVector4f>& Shared = *Frame.Triangles;
			if (Gpu.Triangles && Gpu.TriangleSerial == Frame.TriangleSerial && Gpu.TriangleCount == Shared.Num())
			{
				return GraphBuilder.RegisterExternalBuffer(Gpu.Triangles);
			}
			// NoCopy: the upload reads the array while the graph executes, which happens inside Enqueue while the frame still holds it (and the
			// runtime never changes an array it has handed over).
			FRDGBufferRef Buffer = CreateStructuredBuffer(GraphBuilder, TEXT("FluidNinjaBuoyancy.Waves.HullTriangles"), Shared, ERDGInitialDataFlags::NoCopy);
			Gpu.Triangles = GraphBuilder.ConvertToExternalBuffer(Buffer);
			Gpu.TriangleSerial = Frame.TriangleSerial;
			Gpu.TriangleCount = Shared.Num();
			return Buffer;
		}

		/** The bodies' static head: into Head, the deepest body per cell (a simulation; Speed takes the pressing hull's speed through the
		 * water beside it, the fastest body per cell, for the source texture's A), or into each body's own field's slot of HeadPool
		 * (Slots, one per body; no Speed: a self field carries no foam). Origins: the Surface Mirror's grid-relative centre for each body's
		 * grid. Nothing is dispatched when no body has a cell in the grid. */
		static void AddRaster(FRDGBuilder& GraphBuilder, const FFluidNinjaBuoyancyWaveGrid& Grid, TArrayView<const FBody> Bodies, TArrayView<const FVector2f> Origins,
			TArrayView<const int32> Slots, const FMirror& Mirror, const FFrame& Frame, FRDGTextureRef MirrorTexture, FRDGBufferRef Triangles,
			FRDGTextureRef Head, FRDGTextureRef Speed, FRDGBufferRef HeadPool)
		{
			const int32 NumBodies = FMath::Min(Bodies.Num(), WaveMaxBodies);
			const uint32 TriangleCapacity = Frame.Triangles.IsValid() ? static_cast<uint32>(Frame.Triangles->Num() / 3) : 0u;
			TArray<FVector4f> BodyData;
			TArray<FUintVector4> BodyInfo;
			BodyData.Reserve(NumBodies * WaveBodyStride);
			BodyInfo.Reserve(NumBodies * 2);
			FIntPoint Largest = FIntPoint::ZeroValue;
			for (int32 Index = 0; Index < NumBodies; ++Index)
			{
				const FBody& Body = Bodies[Index];
				BodyData.Add(Body.Axis[0]);
				BodyData.Add(Body.Axis[1]);
				BodyData.Add(Body.Axis[2]);
				BodyData.Add(Body.Bounds);
				// P's multiplier at least 0 and finite: P is combined by its float bits, which order like the floats only from 0 up.
				BodyData.Add(FVector4f(Body.Extra.X, Body.Extra.Y, Body.Extra.Z, FMath::IsFinite(Body.Extra.W) ? FMath::Max(Body.Extra.W, 0.f) : 0.f));
				BodyData.Add(FVector4f(Origins[Index].X, Origins[Index].Y, FMath::IsFinite(Body.MaxHeadCm) ? FMath::Max(Body.MaxHeadCm, 0.f) : 0.f,
					FMath::IsFinite(Body.SubmergeFadeCm) ? FMath::Max(Body.SubmergeFadeCm, 0.f) : 0.f));
				// The speed at least 0 and finite: like P it is combined by its float bits, which order like the floats only from 0 up.
				// y: a lying ellipsoid's vertical semi-axis (kind 2).
				BodyData.Add(FVector4f(FMath::IsFinite(Body.SpeedCmS) ? FMath::Max(Body.SpeedCmS, 0.f) : 0.f,
					FMath::IsFinite(Body.HalfThicknessCm) ? FMath::Max(Body.HalfThicknessCm, 0.f) : 0.f, 0.f, 0.f));
				// A triangle range past the uploaded triangles reads nothing rather than another body's hull.
				const uint32 First = Body.FirstTriangle;
				const uint32 Count = (Body.Kind == 0 && First <= TriangleCapacity) ? FMath::Min(Body.NumTriangles, TriangleCapacity - First) : 0u;
				const FIntRect Cells = BodyCells(Body, Grid.GridSize, Grid.CellCm);
				BodyInfo.Add(FUintVector4(First, Count, Body.Kind, Slots.IsEmpty() ? 0u : static_cast<uint32>(Slots[Index])));
				BodyInfo.Add(FUintVector4(static_cast<uint32>(Cells.Min.X), static_cast<uint32>(Cells.Min.Y), static_cast<uint32>(Cells.Width()), static_cast<uint32>(Cells.Height())));
				Largest = Largest.ComponentMax(Cells.Size());
			}
			if (Largest.X <= 0 || Largest.Y <= 0)
			{
				return;
			}
			FFluidNinjaBuoyancyWaveRasterInputs Raster;
			Raster.Grid = Grid;
			Raster.BodyData = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("FluidNinjaBuoyancy.Waves.BodyData"), BodyData));
			Raster.BodyInfo = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("FluidNinjaBuoyancy.Waves.BodyInfo"), BodyInfo));
			Raster.Triangles = GraphBuilder.CreateSRV(Triangles);
			Raster.NumBodies = NumBodies;
			const bool bMirror = Mirror.bValid && Frame.Mirror && Mirror.Size > 0 && Mirror.Texel > 0.f;
			Raster.CaptureTexture = MirrorTexture;
			Raster.HasCapture = bMirror ? 1 : 0;
			Raster.CaptureTexel = bMirror ? Mirror.Texel : 1.f;
			Raster.CaptureSize = bMirror ? Mirror.Size : 1;
			Raster.CaptureTop = Mirror.Top;
			Raster.CaptureFar = Mirror.Far;
			Raster.Controlled = Frame.bControlled ? 1 : 0;
			Raster.ControlledHeight = Frame.ControlledHeight;
			// A thread group per cell of each body's box, its threads sharing the body's triangles.
			const FIntVector Groups(Largest.X, Largest.Y, NumBodies);
			RDG_EVENT_SCOPE_STAT(GraphBuilder, FNB_Raster, "FNB Raster %d bodies", NumBodies);
			if (HeadPool)
			{
				FFluidNinjaBuoyancyWaveSelfRasterCS::FParameters* Parameters = GraphBuilder.AllocParameters<FFluidNinjaBuoyancyWaveSelfRasterCS::FParameters>();
				Parameters->Raster = Raster;
				Parameters->HeadPool = GraphBuilder.CreateUAV(HeadPool);
				TShaderMapRef<FFluidNinjaBuoyancyWaveSelfRasterCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FluidNinjaBuoyancyWaves SelfStaticHead %d bodies", NumBodies), Shader, Parameters, Groups);
			}
			else
			{
				FFluidNinjaBuoyancyWaveRasterCS::FParameters* Parameters = GraphBuilder.AllocParameters<FFluidNinjaBuoyancyWaveRasterCS::FParameters>();
				Parameters->Raster = Raster;
				Parameters->HeadOut = GraphBuilder.CreateUAV(Head);
				Parameters->SpeedOut = GraphBuilder.CreateUAV(Speed);
				TShaderMapRef<FFluidNinjaBuoyancyWaveRasterCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FluidNinjaBuoyancyWaves StaticHead %d bodies", NumBodies), Shader, Parameters, Groups);
			}
		}

		static void AddSplat(FRDGBuilder& GraphBuilder, const FFluidNinjaBuoyancyWaveGrid& Grid, TArrayView<const FVector4f> Impulses, float SmoothCm, FRDGTextureRef Impulse)
		{
			if (Impulses.IsEmpty())
			{
				return;
			}
			RDG_EVENT_SCOPE_STAT(GraphBuilder, FNB_Splat, "FNB Splat %d impulses", Impulses.Num());
			FFluidNinjaBuoyancyWaveSplatCS::FParameters* Parameters = GraphBuilder.AllocParameters<FFluidNinjaBuoyancyWaveSplatCS::FParameters>();
			Parameters->Grid = Grid;
			Parameters->Impulses = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("FluidNinjaBuoyancy.Waves.Impulses"), Impulses));
			Parameters->NumImpulses = Impulses.Num();
			Parameters->ImpulseFixedScale = ImpulseFixedScale;
			Parameters->SmoothCells = Grid.CellCm > 0.f ? FMath::Clamp(SmoothCm / Grid.CellCm, 0.f, WaveSplatMaxSigmaCells) : 0.f;
			Parameters->ImpulseRW = GraphBuilder.CreateUAV(Impulse);
			TShaderMapRef<FFluidNinjaBuoyancyWaveSplatCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FluidNinjaBuoyancyWaves ImpulseSplat %d", Impulses.Num()), Shader, Parameters,
				FIntVector(FMath::DivideAndRoundUp(Impulses.Num(), WaveLineGroup), 1, 1));
		}

		/** One simulation's deep heads (FSource::DeepHeads, the sinkers), summed per cell into a uint in signed fixed point; reused from the
		 * last frame while the records and the grid are the same (FSource::DeepKey names the simulation). Null when none presses a cell of
		 * the grid, and its kept sum is dropped. */
		static FRDGBufferRef AddDeepHeads(FRDGBuilder& GraphBuilder, const FFluidNinjaBuoyancyWaveGrid& Grid, const FSource& Source, FState::FGpu& Gpu)
		{
			TArray<FVector4f> Data;
			TArray<FUintVector4> Info;
			FIntPoint Largest = FIntPoint::ZeroValue;
			const int32 NumHeads = FMath::Min(Source.DeepHeads.Num(), WaveMaxDeep);
			Data.Reserve(NumHeads * WaveDeepStride);
			Info.Reserve(NumHeads);
			for (int32 Index = 0; Index < NumHeads; ++Index)
			{
				const FDeepHead& Head = Source.DeepHeads[Index];
				// A record that is not a number, or not a column of positive volume over a positive rectangle inside its taper, adds nothing;
				// the column ends at the kernel's bed.
				const float Bottom = FMath::Min(Head.BottomCm, Head.DepthCm);
				const bool bFinite = FMath::IsFinite(Head.Centre.X) && FMath::IsFinite(Head.Centre.Y) && FMath::IsFinite(Head.Axis.X) && FMath::IsFinite(Head.Axis.Y)
					&& FMath::IsFinite(Head.Half.X) && FMath::IsFinite(Head.Half.Y) && FMath::IsFinite(Head.TopCm) && FMath::IsFinite(Bottom) && FMath::IsFinite(Head.DepthCm)
					&& FMath::IsFinite(Head.Scale) && FMath::IsFinite(Head.TaperStartCm) && FMath::IsFinite(Head.TaperEndCm);
				if (!bFinite || !(Head.Half.X > 0.f) || !(Head.Half.Y > 0.f) || !(Head.TopCm >= 0.f) || !(Bottom > Head.TopCm) || !(Head.DepthCm > 0.f) || !(Head.Scale > 0.f)
					|| !(Head.TaperStartCm >= 0.f) || !(Head.TaperEndCm > Head.TaperStartCm))
				{
					continue;
				}
				// Its taper's box on the grid, one cell of margin (the cells whose centres may fall within TaperEndCm of its centre).
				FBody Reach;
				Reach.Bounds = FVector4f(Head.Centre.X - Head.TaperEndCm, Head.Centre.Y - Head.TaperEndCm, Head.Centre.X + Head.TaperEndCm, Head.Centre.Y + Head.TaperEndCm);
				const FIntRect Cells = BodyCells(Reach, Grid.GridSize, Grid.CellCm);
				if (Cells.Width() <= 0 || Cells.Height() <= 0)
				{
					continue;
				}
				// A unit axis, so the shader's turn into the rectangle's frame is a rotation whatever the record carries.
				const FVector2f Axis = Head.Axis.SizeSquared() > 1e-12f ? Head.Axis.GetSafeNormal() : FVector2f(1.f, 0.f);
				Data.Add(FVector4f(Head.Centre.X, Head.Centre.Y, Axis.X, Axis.Y));
				Data.Add(FVector4f(Head.Half.X, Head.Half.Y, Head.TopCm, Bottom));
				Data.Add(FVector4f(Head.DepthCm, Head.Scale, Head.TaperStartCm, Head.TaperEndCm));
				Info.Add(FUintVector4(static_cast<uint32>(Cells.Min.X), static_cast<uint32>(Cells.Min.Y), static_cast<uint32>(Cells.Width()), static_cast<uint32>(Cells.Height())));
				Largest = Largest.ComponentMax(Cells.Size());
			}
			if (Info.IsEmpty())
			{
				Gpu.DeepCaches.Remove(Source.DeepKey);
				return nullptr;
			}
			FState::FGpu::FDeepCache* Kept = Source.DeepKey != 0 ? Gpu.DeepCaches.Find(Source.DeepKey) : nullptr;
			if (Kept && Kept->Buffer && Kept->Cells == Grid.GridSize && Kept->CellCm == Grid.CellCm && Kept->Data == Data && Kept->Info == Info)
			{
				return GraphBuilder.RegisterExternalBuffer(Kept->Buffer);
			}
			RDG_EVENT_SCOPE_STAT(GraphBuilder, FNB_DeepHead, "FNB DeepHead %d", Info.Num());
			const uint32 Count = static_cast<uint32>(Grid.GridSize.X) * static_cast<uint32>(Grid.GridSize.Y);
			FRDGBufferRef Deep = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Count), TEXT("FluidNinjaBuoyancy.Waves.DeepHead"));
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Deep), 0u);
			FFluidNinjaBuoyancyWaveDeepHeadCS::FParameters* Parameters = GraphBuilder.AllocParameters<FFluidNinjaBuoyancyWaveDeepHeadCS::FParameters>();
			Parameters->Grid = Grid;
			Parameters->DeepData = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("FluidNinjaBuoyancy.Waves.DeepData"), Data));
			Parameters->DeepInfo = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("FluidNinjaBuoyancy.Waves.DeepInfo"), Info));
			Parameters->NumDeep = Info.Num();
			Parameters->DeepFixedScale = WaveDeepFixedScale;
			Parameters->DeepOut = GraphBuilder.CreateUAV(Deep);
			TShaderMapRef<FFluidNinjaBuoyancyWaveDeepHeadCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			// A thread per cell of each head's box, the head being the group's z.
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FluidNinjaBuoyancyWaves DeepHead %d", Info.Num()), Shader, Parameters, CellGroups(Largest, Info.Num()));
			if (Source.DeepKey != 0)
			{
				FState::FGpu::FDeepCache& Cache = Kept ? *Kept : Gpu.DeepCaches.Add(Source.DeepKey);
				Cache.Buffer = GraphBuilder.ConvertToExternalBuffer(Deep);
				Cache.Cells = Grid.GridSize;
				Cache.CellCm = Grid.CellCm;
				Cache.Data = MoveTemp(Data);
				Cache.Info = MoveTemp(Info);
			}
			return Deep;
		}

		/** One simulation's source texture: P, y (impulses and pokes), the erase flags and the header row. */
		static void AddSource(FRDGBuilder& GraphBuilder, const FSource& Source, const FFrame& Frame, FRDGTextureRef MirrorTexture, FRDGBufferRef Triangles,
			FState::FGpu& Gpu)
		{
			const FIntPoint Cells = Source.Cells;
			if (!Source.Target || Cells.X <= 0 || Cells.Y <= 0 || !(Source.CellCm > 0.f))
			{
				return;
			}
			const FRHITextureDesc& TargetDesc = Source.Target->GetDesc();
			if (TargetDesc.Dimension != ETextureDimension::Texture2D || TargetDesc.Format != PF_A32B32G32R32F || TargetDesc.Extent != FIntPoint(Cells.X, Cells.Y + 1))
			{
				if (!Gpu.bWarnedTarget)
				{
					UE_LOG(LogFluidNinjaBuoyancyWaveSource, Error, TEXT("Waves: the source render target is %dx%d %s; it must be %dx%d RGBA32F. No wave sources are written."),
						TargetDesc.Extent.X, TargetDesc.Extent.Y, GPixelFormats[TargetDesc.Format].Name, Cells.X, Cells.Y + 1);
					Gpu.bWarnedTarget = true;
				}
				return;
			}
			FFluidNinjaBuoyancyWaveGrid Grid;
			Grid.GridSize = Cells;
			Grid.CellCm = Source.CellCm;
			FRDGTextureRef Head = GraphBuilder.CreateTexture(CellDesc(Cells, PF_R32_UINT), TEXT("FluidNinjaBuoyancy.Waves.StaticHead"));
			FRDGTextureRef Speed = GraphBuilder.CreateTexture(CellDesc(Cells, PF_R32_UINT), TEXT("FluidNinjaBuoyancy.Waves.HullSpeed"));
			FRDGTextureRef Impulse = GraphBuilder.CreateTexture(CellDesc(Cells, PF_R32_UINT), TEXT("FluidNinjaBuoyancy.Waves.Impulse"));
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Head), 0u);
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Speed), 0u);
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Impulse), 0u);
			if (!Source.Bodies.IsEmpty())
			{
				TArray<FVector2f> Origins;
				Origins.Init(Source.Mirror.Origin, Source.Bodies.Num());
				AddRaster(GraphBuilder, Grid, Source.Bodies, Origins, {}, Source.Mirror, Frame, MirrorTexture, Triangles, Head, Speed, nullptr);
			}
			AddSplat(GraphBuilder, Grid, Source.Impulses, Frame.SmoothCm, Impulse);
			const double CellArea = static_cast<double>(Source.CellCm) * static_cast<double>(Source.CellCm);
			const double PerHead = static_cast<double>(Frame.Density) * static_cast<double>(Frame.Gravity) * CellArea * ImpulseFixedScale;
			if (!Source.Pokes.IsEmpty())
			{
				RDG_EVENT_SCOPE_STAT(GraphBuilder, FNB_Splat, "FNB Splat %d pokes", Source.Pokes.Num());
				FFluidNinjaBuoyancyWavePokeCS::FParameters* Parameters = GraphBuilder.AllocParameters<FFluidNinjaBuoyancyWavePokeCS::FParameters>();
				Parameters->Grid = Grid;
				Parameters->Pokes = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("FluidNinjaBuoyancy.Waves.Pokes"), Source.Pokes));
				Parameters->NumPokes = Source.Pokes.Num();
				Parameters->PokeToFixed = static_cast<float>(Frame.PokeSeconds * PerHead);
				Parameters->ImpulseRW = GraphBuilder.CreateUAV(Impulse);
				TShaderMapRef<FFluidNinjaBuoyancyWavePokeCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FluidNinjaBuoyancyWaves Pokes %d", Source.Pokes.Num()), Shader, Parameters, CellGroups(Cells));
			}
			// The sinkers' deep heads, added to P in the assembly; without any the assembly reads a one-element stand-in and adds nothing.
			FRDGBufferRef Deep = AddDeepHeads(GraphBuilder, Grid, Source, Gpu);
			static const TArray<uint32> NoDeep = {0u};
			static const TArray<FIntVector4> NoBoxes = {FIntVector4(1, 1, 0, 0)};
			const int32 NumErase = FMath::Min(Source.EraseBoxes.Num(), WaveMaxErase);
			const TArray<FIntVector4> Boxes(Source.EraseBoxes.GetData(), NumErase);

			FRDGTextureRef Target = RegisterExternalTexture(GraphBuilder, Source.Target, TEXT("FluidNinjaBuoyancy.WaveSourceTexture"));
			const bool bUAV = EnumHasAnyFlags(TargetDesc.Flags, ETextureCreateFlags::UAV);
			FRDGTextureRef Written = bUAV ? Target : GraphBuilder.CreateTexture(CellDesc(FIntPoint(Cells.X, Cells.Y + 1), PF_A32B32G32R32F), TEXT("FluidNinjaBuoyancy.Waves.Source"));
			RDG_EVENT_SCOPE_STAT(GraphBuilder, FNB_Assemble, "FNB Assemble %dx%d", Cells.X, Cells.Y);
			FFluidNinjaBuoyancyWaveAssembleCS::FParameters* Parameters = GraphBuilder.AllocParameters<FFluidNinjaBuoyancyWaveAssembleCS::FParameters>();
			Parameters->Grid = Grid;
			Parameters->HeadIn = Head;
			Parameters->SpeedIn = Speed;
			Parameters->ImpulseIn = Impulse;
			Parameters->DeepIn = GraphBuilder.CreateSRV(Deep ? Deep : CreateStructuredBuffer(GraphBuilder, TEXT("FluidNinjaBuoyancy.Waves.NoDeepHead"), NoDeep));
			Parameters->HasDeep = Deep ? 1 : 0;
			Parameters->DeepToCm = 1.f / WaveDeepFixedScale;
			Parameters->EraseBoxes = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("FluidNinjaBuoyancy.Waves.EraseBoxes"), NumErase > 0 ? Boxes : NoBoxes));
			Parameters->NumErase = NumErase;
			Parameters->EraseAll = Source.bEraseAll ? 1 : 0;
			Parameters->ImpulseToHead = PerHead > 0.0 ? static_cast<float>(1.0 / PerHead) : 0.f;
			Parameters->Header0 = Source.Header0;
			Parameters->Header1 = Source.Header1;
			Parameters->SourceOut = GraphBuilder.CreateUAV(Written);
			TShaderMapRef<FFluidNinjaBuoyancyWaveAssembleCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FluidNinjaBuoyancyWaves Source %dx%d", Cells.X, Cells.Y), Shader, Parameters, CellGroups(FIntPoint(Cells.X, Cells.Y + 1)));
			if (!bUAV)
			{
				// The render target was made without UAV access: write a texture of the same format and copy it across.
				AddCopyTexturePass(GraphBuilder, Written, Target);
			}
		}

		/** One self field's part in this frame. */
		struct FSelfWork
		{
			const FSelf* Self = nullptr;
			int32 Slot = INDEX_NONE;
			/** A new slot (new, or a new size): still water with nothing pending, and always reset. */
			bool bNew = false;
			bool bReset = false;
			/** Recentred by whole cells this frame. */
			bool bMoved = false;
		};

		/** One size's pool, registered in this frame's graph. */
		struct FSelfPoolRefs
		{
			FRDGBufferRef State = nullptr;
			FRDGBufferRef Head = nullptr;
			FRDGBufferRef Impulse = nullptr;
		};

		static void ReleaseSlot(FState::FGpu& Gpu, FState::FGpu::FSelfSlot& Kept)
		{
			if (Kept.Slot != INDEX_NONE && IsSelfSize(Kept.Cells))
			{
				FState::FGpu::FSelfPool& Pool = Gpu.Pools[SelfPoolIndex(Kept.Cells)];
				if (Pool.Used.IsValidIndex(Kept.Slot))
				{
					Pool.Used[Kept.Slot] = false;
				}
				if (Pool.Used.Find(true) == INDEX_NONE)
				{
					Pool = FState::FGpu::FSelfPool();   // nothing left in it: its memory goes
				}
			}
			Kept.Slot = INDEX_NONE;
			Kept.Cells = 0;
		}

		static int32 TakeSlot(FState::FGpu::FSelfPool& Pool)
		{
			const int32 Free = Pool.Used.Find(false);
			if (Free == INDEX_NONE)
			{
				return Pool.Used.Add(true);
			}
			Pool.Used[Free] = true;
			return Free;
		}

		/** A pool in this frame's graph, grown (its slots copied across) when its fields outnumber its slots. */
		static FSelfPoolRefs RegisterPool(FRDGBuilder& GraphBuilder, FState::FGpu::FSelfPool& Pool, int32 N)
		{
			const uint64 SlotCells = static_cast<uint64>(N) * static_cast<uint64>(N);
			if (Pool.Used.Num() > Pool.Capacity || !Pool.State || !Pool.Head || !Pool.Impulse)
			{
				const int32 Capacity = FMath::Max(WaveMinSelfPool, static_cast<int32>(FMath::RoundUpToPowerOfTwo(static_cast<uint32>(FMath::Max(Pool.Used.Num(), 1)))));
				const uint32 Cells = static_cast<uint32>(Capacity * SlotCells);
				FRDGBufferRef State = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector2f), Cells), TEXT("FluidNinjaBuoyancy.Waves.SelfStatePool"));
				FRDGBufferRef Head = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Cells), TEXT("FluidNinjaBuoyancy.Waves.SelfHeadPool"));
				FRDGBufferRef Impulse = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Cells), TEXT("FluidNinjaBuoyancy.Waves.SelfImpulsePool"));
				if (Pool.Capacity > 0 && Pool.State && Pool.Head && Pool.Impulse)
				{
					const uint64 OldCells = static_cast<uint64>(Pool.Capacity) * SlotCells;
					AddCopyBufferPass(GraphBuilder, State, 0, GraphBuilder.RegisterExternalBuffer(Pool.State), 0, OldCells * sizeof(FVector2f));
					AddCopyBufferPass(GraphBuilder, Head, 0, GraphBuilder.RegisterExternalBuffer(Pool.Head), 0, OldCells * sizeof(uint32));
					AddCopyBufferPass(GraphBuilder, Impulse, 0, GraphBuilder.RegisterExternalBuffer(Pool.Impulse), 0, OldCells * sizeof(uint32));
				}
				Pool.Capacity = Capacity;
				Pool.State = GraphBuilder.ConvertToExternalBuffer(State);
				Pool.Head = GraphBuilder.ConvertToExternalBuffer(Head);
				Pool.Impulse = GraphBuilder.ConvertToExternalBuffer(Impulse);
				return {State, Head, Impulse};
			}
			return {GraphBuilder.RegisterExternalBuffer(Pool.State), GraphBuilder.RegisterExternalBuffer(Pool.Head), GraphBuilder.RegisterExternalBuffer(Pool.Impulse)};
		}

		/** Whether two self fields can share a batch: the same size and cells, and the same simulation and step. */
		static bool SameBatch(const FSelf& A, const FSelf& B)
		{
			return A.Cells == B.Cells && A.CellCm == B.CellCm && A.StepTicks == B.StepTicks && A.Aux == B.Aux && A.TickSeconds == B.TickSeconds
				&& A.SimCells == B.SimCells && A.bEdgeOpen == B.bEdgeOpen && A.SimSpongeCm == B.SimSpongeCm && A.WallAbsorption == B.WallAbsorption
				&& A.WallBandCm == B.WallBandCm && A.Damping == B.Damping && A.Hyperviscosity == B.Hyperviscosity && A.ShoreDepthCm == B.ShoreDepthCm
				&& A.BreakingSlope == B.BreakingSlope && A.MeanRelax == B.MeanRelax && A.Mirror.bValid == B.Mirror.bValid && A.Mirror.Texel == B.Mirror.Texel
				&& A.Mirror.Size == B.Mirror.Size && A.Mirror.Top == B.Mirror.Top && A.Mirror.Far == B.Mirror.Far;
		}

		/** A batch's records (SelfInfo): slot and flags, atlas tile, Wave Aux cell, the depth outside the simulation's window, the own sponge,
		 * and the place in the shift buffers. */
		static FRDGBufferSRVRef AddSelfInfo(FRDGBuilder& GraphBuilder, TConstArrayView<FSelfWork> Work, TConstArrayView<int32> Members,
			TFunctionRef<int32(const FSelfWork&)> Flags, TConstArrayView<int32> Moved = {})
		{
			TArray<FIntVector4> Info;
			Info.Reserve(Members.Num() * WaveSelfInfoStride);
			for (int32 Place = 0; Place < Members.Num(); ++Place)
			{
				const FSelfWork& Item = Work[Members[Place]];
				const FSelf& Self = *Item.Self;
				Info.Add(FIntVector4(Item.Slot, Flags(Item), Self.Tile.X, Self.Tile.Y));
				Info.Add(FIntVector4(Self.AuxCell.X, Self.AuxCell.Y, FloatBits(FMath::Max(Self.DepthCm, 1.f)), FloatBits(FMath::Max(Self.SpongeCm, 0.f))));
				Info.Add(FIntVector4(Moved.IsEmpty() ? 0 : Moved[Place], Self.Shift.X, Self.Shift.Y, 0));
			}
			return GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("FluidNinjaBuoyancy.Waves.SelfInfo"), MoveTemp(Info)));
		}

		/** One batch of self fields (same size, same simulation): recentre or reset, their static head and impulses (bSources), then their
		 * step (StepTicks), each pass one dispatch for the whole batch. */
		static void AddSelfBatch(FRDGBuilder& GraphBuilder, TConstArrayView<FSelfWork> Work, TConstArrayView<int32> Batch, const FSelfPoolRefs& Pool,
			const FFrame& Frame, FRDGTextureRef MirrorTexture, FRDGBufferRef Triangles, FRDGTextureRef Atlas, FState::FGpu& Gpu)
		{
			const FSelf& First = *Work[Batch[0]].Self;
			const int32 N = First.Cells;
			const FIntPoint Cells(N, N);
			const uint32 SlotCells = static_cast<uint32>(N * N);
			FFluidNinjaBuoyancyWaveGrid Grid;
			Grid.GridSize = Cells;
			Grid.CellCm = First.CellCm;
			const auto Members = [&Work, &Batch](TFunctionRef<bool(const FSelfWork&)> Wanted)
			{
				TArray<int32> Chosen;
				for (const int32 Index : Batch)
				{
					if (Wanted(Work[Index]))
					{
						Chosen.Add(Index);
					}
				}
				return Chosen;
			};
			const auto FieldFlags = [](const FSelfWork& Item)
			{
				const bool bSources = Item.Self->bSources;
				return (Item.bNew ? WaveSelfNew : 0) | (Item.bMoved ? WaveSelfMoved : 0) | (bSources ? WaveSelfSources : 0) | (bSources && Item.bReset ? WaveSelfReset : 0);
			};

			// Recentred by whole cells: each moved field's state and pending impulse, shifted, into the shift buffers (a shift reads its
			// neighbours), and back into its slot in Begin. (The bookkeeping passes, recentre, begin and reset, share the SelfBegin stat.)
			{
				RDG_EVENT_SCOPE_STAT(GraphBuilder, FNB_SelfBegin, "FNB SelfBegin %d fields", Batch.Num());
				const TArray<int32> Moved = Members([](const FSelfWork& Item) { return Item.bMoved; });
				TArray<int32> MovedPlace;
				for (int32 Place = 0; Place < Moved.Num(); ++Place)
				{
					MovedPlace.Add(Place);
				}
				// With nothing moved, Begin reads a one-element stand-in.
				static const FVector2f NoState = FVector2f::ZeroVector;
				static const uint32 NoImpulse = 0u;
				FRDGBufferRef ShiftState = nullptr;
				FRDGBufferRef ShiftImpulse = nullptr;
				if (!Moved.IsEmpty())
				{
					const uint32 ShiftCells = static_cast<uint32>(Moved.Num()) * SlotCells;
					ShiftState = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector2f), ShiftCells), TEXT("FluidNinjaBuoyancy.Waves.SelfShiftState"));
					ShiftImpulse = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), ShiftCells), TEXT("FluidNinjaBuoyancy.Waves.SelfShiftImpulse"));
					FFluidNinjaBuoyancyWaveSelfShiftCS::FParameters* Parameters = GraphBuilder.AllocParameters<FFluidNinjaBuoyancyWaveSelfShiftCS::FParameters>();
					Parameters->Batch.Grid = Grid;
					Parameters->Batch.SelfInfo = AddSelfInfo(GraphBuilder, Work, Moved, FieldFlags, MovedPlace);
					Parameters->StatePoolIn = GraphBuilder.CreateSRV(Pool.State);
					Parameters->ImpulsePoolIn = GraphBuilder.CreateSRV(Pool.Impulse);
					Parameters->ShiftStateOut = GraphBuilder.CreateUAV(ShiftState);
					Parameters->ShiftImpulseOut = GraphBuilder.CreateUAV(ShiftImpulse);
					TShaderMapRef<FFluidNinjaBuoyancyWaveSelfShiftCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FluidNinjaBuoyancyWaves SelfRecentre %d", Moved.Num()), Shader, Parameters, CellGroups(Cells, Moved.Num()));
				}
				else
				{
					ShiftState = CreateStructuredBuffer(GraphBuilder, TEXT("FluidNinjaBuoyancy.Waves.NoShiftState"), sizeof(FVector2f), 1, &NoState, sizeof(NoState), ERDGInitialDataFlags::NoCopy);
					ShiftImpulse = CreateStructuredBuffer(GraphBuilder, TEXT("FluidNinjaBuoyancy.Waves.NoShiftImpulse"), sizeof(uint32), 1, &NoImpulse, sizeof(NoImpulse), ERDGInitialDataFlags::NoCopy);
				}

				// The frame's start: new fields cleared, moved ones back in their slots, P cleared where it is rastered again, and nothing pending
				// where a field resets.
				const TArray<int32> Starting = Members([](const FSelfWork& Item) { return Item.bNew || Item.bMoved || Item.Self->bSources; });
				if (!Starting.IsEmpty())
				{
					TArray<int32> StartingPlace;
					for (const int32 Index : Starting)
					{
						StartingPlace.Add(Moved.Find(Index));
					}
					FFluidNinjaBuoyancyWaveSelfBeginCS::FParameters* Parameters = GraphBuilder.AllocParameters<FFluidNinjaBuoyancyWaveSelfBeginCS::FParameters>();
					Parameters->Batch.Grid = Grid;
					Parameters->Batch.SelfInfo = AddSelfInfo(GraphBuilder, Work, Starting, FieldFlags, StartingPlace);
					Parameters->ShiftStateIn = GraphBuilder.CreateSRV(ShiftState);
					Parameters->ShiftImpulseIn = GraphBuilder.CreateSRV(ShiftImpulse);
					Parameters->StatePool = GraphBuilder.CreateUAV(Pool.State);
					Parameters->HeadPool = GraphBuilder.CreateUAV(Pool.Head);
					Parameters->ImpulsePool = GraphBuilder.CreateUAV(Pool.Impulse);
					TShaderMapRef<FFluidNinjaBuoyancyWaveSelfBeginCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FluidNinjaBuoyancyWaves SelfBegin %d", Starting.Num()), Shader, Parameters, CellGroups(Cells, Starting.Num()));
				}
			}

			// This frame's static head (P = 0 for a body wholly under the surface).
			const TArray<int32> Rastered = Members([](const FSelfWork& Item) { return Item.Self->bSources && Item.Self->bHasBody; });
			if (!Rastered.IsEmpty())
			{
				TArray<FBody> Bodies;
				TArray<FVector2f> Origins;
				TArray<int32> Slots;
				for (const int32 Index : Rastered)
				{
					Bodies.Add(Work[Index].Self->Body);
					Origins.Add(Work[Index].Self->Mirror.Origin);
					Slots.Add(Work[Index].Slot);
				}
				AddRaster(GraphBuilder, Grid, Bodies, Origins, Slots, First.Mirror, Frame, MirrorTexture, Triangles, nullptr, nullptr, Pool.Head);
			}

			// Reset: η = −P, v = 0 and nothing pending, as the core's erase flag does: B_self = 0, and the tile says so at once.
			const TArray<int32> Reset = Members([](const FSelfWork& Item) { return Item.Self->bSources && Item.bReset; });
			if (!Reset.IsEmpty())
			{
				RDG_EVENT_SCOPE_STAT(GraphBuilder, FNB_SelfBegin, "FNB SelfBegin reset %d fields", Reset.Num());
				FFluidNinjaBuoyancyWaveSelfResetCS::FParameters* Parameters = GraphBuilder.AllocParameters<FFluidNinjaBuoyancyWaveSelfResetCS::FParameters>();
				Parameters->Batch.Grid = Grid;
				Parameters->Batch.SelfInfo = AddSelfInfo(GraphBuilder, Work, Reset, FieldFlags);
				Parameters->HeadPoolIn = GraphBuilder.CreateSRV(Pool.Head);
				Parameters->StatePool = GraphBuilder.CreateUAV(Pool.State);
				Parameters->SelfTileOut = GraphBuilder.CreateUAV(Atlas);
				TShaderMapRef<FFluidNinjaBuoyancyWaveSelfResetCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FluidNinjaBuoyancyWaves SelfReset %d", Reset.Num()), Shader, Parameters, CellGroups(Cells, Reset.Num()));
			}

			// The bodies' own impulses, each into its field's slot.
			{
				TArray<FVector4f> Impulses;
				TArray<uint32> ImpulseSlots;
				for (const int32 Index : Batch)
				{
					const FSelf& Self = *Work[Index].Self;
					if (Self.bSources)
					{
						Impulses.Append(Self.Impulses);
						ImpulseSlots.Reserve(Impulses.Num());
						while (ImpulseSlots.Num() < Impulses.Num())
						{
							ImpulseSlots.Add(static_cast<uint32>(Work[Index].Slot));
						}
					}
				}
				if (!Impulses.IsEmpty())
				{
					const int32 Count = Impulses.Num();
					RDG_EVENT_SCOPE_STAT(GraphBuilder, FNB_Splat, "FNB Splat %d self impulses", Count);
					FFluidNinjaBuoyancyWaveSelfSplatCS::FParameters* Parameters = GraphBuilder.AllocParameters<FFluidNinjaBuoyancyWaveSelfSplatCS::FParameters>();
					Parameters->Grid = Grid;
					Parameters->Impulses = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("FluidNinjaBuoyancy.Waves.SelfImpulses"), MoveTemp(Impulses)));
					Parameters->ImpulseSlots = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("FluidNinjaBuoyancy.Waves.SelfImpulseSlots"), MoveTemp(ImpulseSlots)));
					Parameters->NumImpulses = Count;
					Parameters->ImpulseFixedScale = ImpulseFixedScale;
					Parameters->SmoothCells = Grid.CellCm > 0.f ? FMath::Clamp(Frame.SmoothCm / Grid.CellCm, 0.f, WaveSplatMaxSigmaCells) : 0.f;
					Parameters->ImpulsePool = GraphBuilder.CreateUAV(Pool.Impulse);
					TShaderMapRef<FFluidNinjaBuoyancyWaveSelfSplatCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FluidNinjaBuoyancyWaves SelfImpulseSplat %d", Count), Shader, Parameters,
						FIntVector(FMath::DivideAndRoundUp(Count, WaveLineGroup), 1, 1));
				}
			}

			if (First.StepTicks <= 0)
			{
				return;   // no step this frame: the pending impulse is kept
			}
			if (!First.Aux)
			{
				if (!Gpu.bWarnedAux)
				{
					UE_LOG(LogFluidNinjaBuoyancyWaveSource, Error, TEXT("Waves: a self field has no Wave Aux target of its simulation (the core's mask, depth and current); it is not stepped."));
					Gpu.bWarnedAux = true;
				}
				return;
			}
			const int32 Ticks = First.StepTicks;
			const int32 Slices = Batch.Num();
			const double CellArea = static_cast<double>(First.CellCm) * static_cast<double>(First.CellCm);
			const double PerHead = static_cast<double>(Frame.Density) * static_cast<double>(Frame.Gravity) * CellArea * ImpulseFixedScale
				* static_cast<double>(Ticks) * static_cast<double>(First.TickSeconds);
			FRDGTextureRef Aux = RegisterExternalTexture(GraphBuilder, First.Aux, TEXT("FluidNinjaBuoyancy.Waves.Aux"));
			FRDGBufferSRVRef Info = AddSelfInfo(GraphBuilder, Work, Batch, FieldFlags);
			// The core's WaveOp, a slice per field: (s·m, m, v·m, η·m) with the whole mip chain down to one texel, as the core's target has
			// (Niagara generates every level): the step reads levels up to 8 and the window's mean from the last. A 64 or 128 field has fewer
			// levels than 9 (a texture cannot have a mip smaller than one texel); the step reads its last level for the ones it lacks (TopMip).
			const int32 NumMips = static_cast<int32>(FMath::FloorLog2(static_cast<uint32>(N))) + 1;
			FRDGTextureRef WaveOp = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2DArray(Cells, PF_A32B32G32R32F, FClearValueBinding::Black,
				ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV, static_cast<uint16>(Slices), static_cast<uint8>(NumMips)), TEXT("FluidNinjaBuoyancy.Waves.SelfOp"));
			FRHISamplerState* Sampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			const int32 EdgeOpen = First.bEdgeOpen ? 1 : 0;
			for (int32 Tick = 0; Tick < Ticks; ++Tick)
			{
				{
					RDG_EVENT_SCOPE_STAT(GraphBuilder, FNB_SelfPrepare, "FNB SelfPrepare %d fields", Slices);
					FFluidNinjaBuoyancyWaveSelfPrepareCS::FParameters* Parameters = GraphBuilder.AllocParameters<FFluidNinjaBuoyancyWaveSelfPrepareCS::FParameters>();
					Parameters->Batch.Grid = Grid;
					Parameters->Batch.SelfInfo = Info;
					Parameters->StatePoolIn = GraphBuilder.CreateSRV(Pool.State);
					Parameters->HeadPoolIn = GraphBuilder.CreateSRV(Pool.Head);
					Parameters->ImpulsePoolIn = GraphBuilder.CreateSRV(Pool.Impulse);
					Parameters->HeadPerUnit = PerHead > 0.0 ? static_cast<float>(1.0 / PerHead) : 0.f;
					Parameters->AuxTexture = Aux;
					Parameters->SimCells = First.SimCells;
					Parameters->EdgeOpen = EdgeOpen;
					Parameters->WaveOpOut = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(WaveOp, 0));
					TShaderMapRef<FFluidNinjaBuoyancyWaveSelfPrepareCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FluidNinjaBuoyancyWaves SelfPrepare %d", Slices), Shader, Parameters, CellGroups(Cells, Slices));
				}
				{
					RDG_EVENT_SCOPE_STAT(GraphBuilder, FNB_SelfMip, "FNB SelfMip %d levels x %d fields", NumMips - 1, Slices);
					for (int32 Level = 1; Level < NumMips; ++Level)
					{
						const FIntPoint InSize(FMath::Max(N >> (Level - 1), 1), FMath::Max(N >> (Level - 1), 1));
						const FIntPoint OutSize(FMath::Max(N >> Level, 1), FMath::Max(N >> Level, 1));
						FFluidNinjaBuoyancyWaveSelfMipCS::FParameters* Parameters = GraphBuilder.AllocParameters<FFluidNinjaBuoyancyWaveSelfMipCS::FParameters>();
						Parameters->MipIn = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(WaveOp, Level - 1));
						Parameters->MipInSize = InSize;
						Parameters->MipOutSize = OutSize;
						Parameters->MipOut = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(WaveOp, static_cast<uint8>(Level)));
						Parameters->WaveOpSampler = Sampler;
						TShaderMapRef<FFluidNinjaBuoyancyWaveSelfMipCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
						FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FluidNinjaBuoyancyWaves SelfMip %d", Level), Shader, Parameters, CellGroups(OutSize, Slices));
					}
				}
				{
					RDG_EVENT_SCOPE_STAT(GraphBuilder, FNB_SelfStep, "FNB SelfStep %d fields", Slices);
					FFluidNinjaBuoyancyWaveSelfStepCS::FParameters* Parameters = GraphBuilder.AllocParameters<FFluidNinjaBuoyancyWaveSelfStepCS::FParameters>();
					Parameters->Batch.Grid = Grid;
					Parameters->Batch.SelfInfo = Info;
					Parameters->WaveOp = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(WaveOp));
					Parameters->WaveOpSampler = Sampler;
					Parameters->HeadPoolIn = GraphBuilder.CreateSRV(Pool.Head);   // P per cell: the sponge relaxes η + P
					Parameters->AuxTexture = Aux;
					Parameters->SimCells = First.SimCells;
					Parameters->EdgeOpen = EdgeOpen;
					Parameters->Damping = FMath::Max(First.Damping, 0.f);
					Parameters->Hyperviscosity = FMath::Clamp(First.Hyperviscosity, 0.f, 50.f);
					Parameters->SimSpongeCm = FMath::Max(First.SimSpongeCm, 0.f);
					Parameters->WallAbsorption = FMath::Max(First.WallAbsorption, 0.f);
					Parameters->WallBandCm = FMath::Max(First.WallBandCm, 0.f);
					Parameters->ShoreDepthCm = FMath::Max(First.ShoreDepthCm, 0.f);
					Parameters->BreakingSlope = First.BreakingSlope;
					Parameters->MeanRelax = FMath::Max(First.MeanRelax, 0.f);
					Parameters->TopMip = NumMips - 1;
					Parameters->TickSeconds = First.TickSeconds;
					Parameters->SpongeRate = FMath::Max(Frame.SpongeRate, 0.f);
					Parameters->StatePool = GraphBuilder.CreateUAV(Pool.State);
					TShaderMapRef<FFluidNinjaBuoyancyWaveSelfStepCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FluidNinjaBuoyancyWaves SelfStep %d", Slices), Shader, Parameters, CellGroups(Cells, Slices));
				}
			}
			{
				// B_self into the tiles; the impulse the step took is gone (frames without a step keep theirs for the next).
				RDG_EVENT_SCOPE_STAT(GraphBuilder, FNB_SelfExport, "FNB SelfExport %d fields", Slices);
				FFluidNinjaBuoyancyWaveSelfExportCS::FParameters* Parameters = GraphBuilder.AllocParameters<FFluidNinjaBuoyancyWaveSelfExportCS::FParameters>();
				Parameters->Batch.Grid = Grid;
				Parameters->Batch.SelfInfo = Info;
				Parameters->StatePoolIn = GraphBuilder.CreateSRV(Pool.State);
				Parameters->HeadPoolIn = GraphBuilder.CreateSRV(Pool.Head);
				Parameters->ImpulsePool = GraphBuilder.CreateUAV(Pool.Impulse);
				Parameters->SelfTileOut = GraphBuilder.CreateUAV(Atlas);
				TShaderMapRef<FFluidNinjaBuoyancyWaveSelfExportCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FluidNinjaBuoyancyWaves SelfExport %d fields, %d ticks", Slices, Ticks), Shader, Parameters, CellGroups(Cells, Slices));
			}
		}

		/** Every body's self field this frame: each finds or takes its slot, then the fields run in batches of one size and one simulation. */
		static void AddSelves(FRDGBuilder& GraphBuilder, const FFrame& Frame, FRDGTextureRef MirrorTexture, FRDGBufferRef Triangles, FRDGTextureRef Atlas,
			FState::FGpu& Gpu)
		{
			const FIntPoint AtlasSize = Atlas->Desc.Extent;
			TArray<FSelfWork> Work;
			Work.Reserve(Frame.Selves.Num());
			for (const FSelf& Self : Frame.Selves)
			{
				const int32 N = Self.Cells;
				if (!IsSelfSize(N) || !(Self.CellCm > 0.f))
				{
					if (!Gpu.bWarnedSelfSize)
					{
						UE_LOG(LogFluidNinjaBuoyancyWaveSource, Error, TEXT("Waves: a self field of %d cells (cell %.2f cm) is not supported; it must be 64, 128, 256 or 512 cells of a positive size."),
							N, Self.CellCm);
						Gpu.bWarnedSelfSize = true;
					}
					continue;
				}
				if (Self.Tile.X < 0 || Self.Tile.Y < 0 || Self.Tile.X + N > AtlasSize.X || Self.Tile.Y + N > AtlasSize.Y)
				{
					if (!Gpu.bWarnedAtlas)
					{
						UE_LOG(LogFluidNinjaBuoyancyWaveSource, Error, TEXT("Waves: a self field's tile (%d, %d) of %d does not fit the %dx%d atlas; that body's field is not stepped."),
							Self.Tile.X, Self.Tile.Y, N, AtlasSize.X, AtlasSize.Y);
						Gpu.bWarnedAtlas = true;
					}
					continue;
				}
				// The kept state: new (or a new size) starts from still water with nothing pending, and always resets.
				FState::FGpu::FSelfSlot& Kept = Gpu.Selves.FindOrAdd(Self.Id);
				FSelfWork& Item = Work.AddDefaulted_GetRef();
				Item.Self = &Self;
				if (Kept.Cells != N || Kept.Slot == INDEX_NONE)
				{
					ReleaseSlot(Gpu, Kept);
					Kept.Cells = N;
					Kept.Slot = TakeSlot(Gpu.Pools[SelfPoolIndex(N)]);
					Item.bNew = true;
				}
				Item.Slot = Kept.Slot;
				Item.bReset = Self.bReset || Item.bNew;
				// Recentred by whole cells: the state and the pending impulse move with the world. Further than the window: everything starts over.
				if (Self.bSources && Self.Shift != FIntPoint::ZeroValue && !Item.bReset)
				{
					if (FMath::Abs(Self.Shift.X) >= N || FMath::Abs(Self.Shift.Y) >= N)
					{
						Item.bReset = true;
					}
					else
					{
						Item.bMoved = true;
					}
				}
			}
			if (Work.IsEmpty())
			{
				return;
			}
			FSelfPoolRefs Pools[FState::FGpu::NumSelfSizes];
			bool bRegistered[FState::FGpu::NumSelfSizes] = {};
			TArray<TArray<int32>> Batches;
			for (int32 Index = 0; Index < Work.Num(); ++Index)
			{
				const int32 PoolIndex = SelfPoolIndex(Work[Index].Self->Cells);
				if (!bRegistered[PoolIndex])
				{
					Pools[PoolIndex] = RegisterPool(GraphBuilder, Gpu.Pools[PoolIndex], Work[Index].Self->Cells);
					bRegistered[PoolIndex] = true;
				}
				TArray<int32>* Batch = Batches.FindByPredicate([&Work, Index](const TArray<int32>& Existing) { return SameBatch(*Work[Existing[0]].Self, *Work[Index].Self); });
				if (Batch)
				{
					Batch->Add(Index);
				}
				else
				{
					Batches.Add({Index});
				}
			}
			for (const TArray<int32>& Batch : Batches)
			{
				AddSelfBatch(GraphBuilder, Work, Batch, Pools[SelfPoolIndex(Work[Batch[0]].Self->Cells)], Frame, MirrorTexture, Triangles, Atlas, Gpu);
			}
		}

		static void AddProbes(FRDGBuilder& GraphBuilder, const FFrame& Frame, FState::FGpu& Gpu)
		{
			const int32 Count = FMath::Min(Frame.NumProbes, WaveMaxProbes);
			if (Count <= 0)
			{
				return;
			}
			FState::FGpu::FReadback* Slot = FreeSlot(Gpu.Probes, TEXT("FluidNinjaBuoyancyWaveProbes"));
			if (!Slot)
			{
				return;
			}
			RDG_EVENT_SCOPE_STAT(GraphBuilder, FNB_Probes, "FNB Probes %d", Count);
			FRDGBufferRef Values = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(float), Count), TEXT("FluidNinjaBuoyancy.Waves.ProbeValues"));
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Values), 0u);
			for (const FProbeSet& Set : Frame.Probes)
			{
				// Only an RGBA16F pressure buffer holds B (FluidNinja's stock RG16F one has none): its probes read 0.
				if (!Set.Pressure || Set.Pressure->GetFormat() != PF_FloatRGBA || Set.Points.IsEmpty() || Set.Points.Num() != Set.Slots.Num() || Set.Cells.X <= 0 || Set.Cells.Y <= 0)
				{
					continue;
				}
				TArray<FVector2f> Points;
				TArray<int32> Slots;
				for (int32 Index = 0; Index < Set.Points.Num(); ++Index)
				{
					if (Set.Slots[Index] >= 0 && Set.Slots[Index] < Count)
					{
						Points.Add(Set.Points[Index]);
						Slots.Add(Set.Slots[Index]);
					}
				}
				if (Points.IsEmpty())
				{
					continue;
				}
				FFluidNinjaBuoyancyWaveGrid Grid;
				Grid.GridSize = Set.Cells;
				Grid.CellCm = Set.CellCm;
				FFluidNinjaBuoyancyWaveProbeCS::FParameters* Parameters = GraphBuilder.AllocParameters<FFluidNinjaBuoyancyWaveProbeCS::FParameters>();
				Parameters->Grid = Grid;
				Parameters->PressureTexture = RegisterExternalTexture(GraphBuilder, Set.Pressure, TEXT("FluidNinjaBuoyancy.PressureDivergence"));
				Parameters->ProbePoints = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("FluidNinjaBuoyancy.Waves.ProbePoints"), Points));
				Parameters->ProbeSlots = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("FluidNinjaBuoyancy.Waves.ProbeSlots"), Slots));
				Parameters->NumProbes = Points.Num();
				Parameters->ProbeValues = GraphBuilder.CreateUAV(Values);
				TShaderMapRef<FFluidNinjaBuoyancyWaveProbeCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FluidNinjaBuoyancyWaves Probes %d", Points.Num()), Shader, Parameters,
					FIntVector(FMath::DivideAndRoundUp(Points.Num(), WaveLineGroup), 1, 1));
			}
			AddEnqueueCopyPass(GraphBuilder, Slot->Buffer.Get(), Values, static_cast<uint32>(Count) * sizeof(float));
			Slot->bBusy = true;
			Slot->Time = Frame.WorldTime;
			Slot->Count = Count;
		}

		static void AddStats(FRDGBuilder& GraphBuilder, const FFrame& Frame, FState::FGpu& Gpu)
		{
			const FIntPoint Cells = Frame.StatsCells;
			// Only an RGBA16F pressure buffer holds B and A (on FluidNinja's stock RG16F one A reads 1): no stats from any other.
			if (!Frame.StatsPressure || Frame.StatsPressure->GetFormat() != PF_FloatRGBA || Cells.X <= 0 || Cells.Y <= 0)
			{
				return;
			}
			FState::FGpu::FReadback* Slot = FreeSlot(Gpu.Stats, TEXT("FluidNinjaBuoyancyWaveStats"));
			if (!Slot)
			{
				return;
			}
			RDG_EVENT_SCOPE_STAT(GraphBuilder, FNB_Probes, "FNB Probes stats %dx%d", Cells.X, Cells.Y);   // the bench's readbacks, probes and stats alike
			const FIntPoint Groups(FMath::DivideAndRoundUp(Cells.X, WaveStatsGroup), FMath::DivideAndRoundUp(Cells.Y, WaveStatsGroup));
			const int32 Count = Groups.X * Groups.Y;
			FRDGBufferRef Partials = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), Count), TEXT("FluidNinjaBuoyancy.Waves.StatsPartials"));
			FFluidNinjaBuoyancyWaveGrid Grid;
			Grid.GridSize = Cells;
			Grid.CellCm = Frame.StatsCellCm;
			FFluidNinjaBuoyancyWaveStatsCS::FParameters* Parameters = GraphBuilder.AllocParameters<FFluidNinjaBuoyancyWaveStatsCS::FParameters>();
			Parameters->Grid = Grid;
			Parameters->PressureTexture = RegisterExternalTexture(GraphBuilder, Frame.StatsPressure, TEXT("FluidNinjaBuoyancy.PressureDivergence"));
			Parameters->StatsPartials = GraphBuilder.CreateUAV(Partials);
			TShaderMapRef<FFluidNinjaBuoyancyWaveStatsCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FluidNinjaBuoyancyWaves Stats"), Shader, Parameters, FIntVector(Groups.X, Groups.Y, 1));
			AddEnqueueCopyPass(GraphBuilder, Slot->Buffer.Get(), Partials, static_cast<uint32>(Count) * sizeof(FVector4f));
			Slot->bBusy = true;
			Slot->Time = Frame.WorldTime;
			Slot->Count = Count;
			Slot->CellCm = Frame.StatsCellCm;
			Slot->Gravity = Frame.Gravity;
			Slot->Density = Frame.Density;
		}

		static void Run(FRHICommandListImmediate& RHICmdList, FState& State, const FFrame& Frame)
		{
			check(IsInRenderingThread());
			if (!State.Gpu)
			{
				State.Gpu = MakeUnique<FState::FGpu>();
			}
			FState::FGpu& Gpu = *State.Gpu;
			Poll(State, Gpu);
			for (const int32 Released : Frame.Released)
			{
				if (FState::FGpu::FSelfSlot* Kept = Gpu.Selves.Find(Released))
				{
					ReleaseSlot(Gpu, *Kept);
				}
				Gpu.Selves.Remove(Released);
			}
			for (const uint32 Key : Frame.ReleasedDeep)
			{
				Gpu.DeepCaches.Remove(Key);
			}
			const bool bWork = !Frame.Sources.IsEmpty() || !Frame.Selves.IsEmpty() || Frame.NumProbes > 0 || Frame.bStats;
			if (!bWork)
			{
				return;
			}

			FRDGBuilder GraphBuilder(RHICmdList);
			{
				RDG_EVENT_SCOPE_STAT(GraphBuilder, FluidNinjaBuoyancyWaveSource, "FluidNinjaBuoyancyWaves %d sources, %d self fields", Frame.Sources.Num(), Frame.Selves.Num());
				FRDGTextureRef Black = BlackTexture(GraphBuilder);
				FRDGTextureRef MirrorTexture = Frame.Mirror ? RegisterExternalTexture(GraphBuilder, Frame.Mirror, TEXT("FluidNinjaBuoyancy.SurfaceMirror")) : Black;
				const bool bAnyBodies = Frame.Sources.ContainsByPredicate([](const FSource& Source) { return !Source.Bodies.IsEmpty(); })
					|| Frame.Selves.ContainsByPredicate([](const FSelf& Self) { return Self.bSources && Self.bHasBody; });
				FRDGBufferRef Triangles = bAnyBodies ? AddTriangles(GraphBuilder, Frame, Gpu) : nullptr;
				for (const FSource& Source : Frame.Sources)
				{
					AddSource(GraphBuilder, Source, Frame, MirrorTexture, Triangles, Gpu);
				}
				if (!Frame.Selves.IsEmpty())
				{
					if (Frame.Atlas)
					{
						FRDGTextureRef Atlas = RegisterExternalTexture(GraphBuilder, Frame.Atlas, TEXT("FluidNinjaBuoyancy.WaveSelfAtlas"));
						if (EnumHasAnyFlags(Atlas->Desc.Flags, ETextureCreateFlags::UAV))
						{
							AddSelves(GraphBuilder, Frame, MirrorTexture, Triangles, Atlas, Gpu);
						}
						else if (!Gpu.bWarnedAtlas)
						{
							UE_LOG(LogFluidNinjaBuoyancyWaveSource, Error, TEXT("Waves: the self-field atlas has no UAV access; the bodies' own fields are not stepped."));
							Gpu.bWarnedAtlas = true;
						}
					}
					else if (!Gpu.bWarnedAtlas)
					{
						UE_LOG(LogFluidNinjaBuoyancyWaveSource, Error, TEXT("Waves: no self-field atlas; the bodies' own fields are not stepped."));
						Gpu.bWarnedAtlas = true;
					}
				}
				AddProbes(GraphBuilder, Frame, Gpu);
				if (Frame.bStats)
				{
					AddStats(GraphBuilder, Frame, Gpu);
				}
			}
			GraphBuilder.Execute();
		}
	};

	FState::FState() = default;

	FState::~FState()
	{
		if (Gpu && !IsInRenderingThread())
		{
			// The pooled resources and readbacks belong to the render thread.
			FGpu* Released = Gpu.Release();
			ENQUEUE_RENDER_COMMAND(FluidNinjaBuoyancyWavesRelease)([Released](FRHICommandListImmediate&)
			{
				delete Released;
			});
		}
	}

	FResults FState::GetResults() const
	{
		FScopeLock Guard(&ResultsLock);
		return Results;
	}

	void Enqueue(FRHICommandListImmediate& RHICmdList, FState& State, const FFrame& Frame)
	{
		FStateAccess::Run(RHICmdList, State, Frame);
	}
}
