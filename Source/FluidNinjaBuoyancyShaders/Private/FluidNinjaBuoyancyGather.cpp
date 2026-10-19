#include "FluidNinjaBuoyancyGather.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalRenderResources.h"
#include "GlobalShader.h"
#include "Interfaces/IPluginManager.h"
#include "ModuleDescriptor.h"
#include "PluginDescriptor.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderResource.h"
#include "RHIGPUReadback.h"
#include "ShaderCore.h"
#include "ShaderParameterStruct.h"

DECLARE_GPU_STAT_NAMED(FluidNinjaBuoyancyGather, TEXT("FluidNinja Buoyancy gather"));
// The pass family, named like the wave sources' (FluidNinjaBuoyancyWaveSource.cpp), so stat gpu lists FNB Raster ... FNB Gather together.
DECLARE_GPU_STAT_NAMED(FNB_Gather, TEXT("FNB Gather"));

class FFluidNinjaBuoyancyGatherCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFluidNinjaBuoyancyGatherCS);
	SHADER_USE_PARAMETER_STRUCT(FFluidNinjaBuoyancyGatherCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, HeightTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, FlowTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, WaveTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SelfTexture)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Slots)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, Samples)
		SHADER_PARAMETER(FIntPoint, HeightSize)
		SHADER_PARAMETER(FIntPoint, FlowSize)
		SHADER_PARAMETER(FIntPoint, WaveSize)
		SHADER_PARAMETER(int32, NumSlots)
		SHADER_PARAMETER(float, CaptureTop)
		SHADER_PARAMETER(float, CaptureFar)
		SHADER_PARAMETER(int32, HasFlow)
		SHADER_PARAMETER(int32, HasWave)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FFluidNinjaBuoyancyGatherCS, "/Plugin/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancyGather.usf", "GatherCS", SF_Compute);

void FluidNinjaBuoyancyGather::Enqueue(FRHICommandListImmediate& RHICmdList, const FRequest& Request, FRHIGPUBufferReadback& Readback)
{
	if (!Request.Height || Request.NumSlots <= 0 || Request.MaxGrid <= 0 || Request.TotalSamples <= 0 || Request.Slots.Num() < Request.NumSlots * SlotStride)
	{
		return;
	}
	FRDGBuilder GraphBuilder(RHICmdList);
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, FluidNinjaBuoyancyGather, "FluidNinjaBuoyancyGather %d bodies", Request.NumSlots);
		FRDGTextureRef Height = RegisterExternalTexture(GraphBuilder, Request.Height, TEXT("FluidNinjaBuoyancy.SurfaceMirror"));
		FRDGTextureRef Flow = RegisterExternalTexture(GraphBuilder, Request.Flow ? Request.Flow : GBlackTexture->TextureRHI.GetReference(),
			Request.Flow ? TEXT("FluidNinjaBuoyancy.NinjaVelocity") : TEXT("FluidNinjaBuoyancy.NoVelocity"));
		// FluidNinja Waves: the simulation's B and the bodies' own fields, both or neither (a body feels B only less its own field).
		// Only an RGBA16F pressure buffer holds B (FluidNinja's stock RG16F one has none, and its A reads 1).
		const bool bWave = Request.Wave && Request.Self && Request.WaveSize.X > 0 && Request.WaveSize.Y > 0 && Request.Wave->GetFormat() == PF_FloatRGBA;
		FRDGTextureRef Wave = RegisterExternalTexture(GraphBuilder, bWave ? Request.Wave : GBlackTexture->TextureRHI.GetReference(),
			bWave ? TEXT("FluidNinjaBuoyancy.PressureDivergence") : TEXT("FluidNinjaBuoyancy.NoWaves"));
		FRDGTextureRef Self = RegisterExternalTexture(GraphBuilder, bWave ? Request.Self : GBlackTexture->TextureRHI.GetReference(),
			bWave ? TEXT("FluidNinjaBuoyancy.WaveSelfAtlas") : TEXT("FluidNinjaBuoyancy.NoSelfFields"));
		FRDGBufferRef Slots = CreateStructuredBuffer(GraphBuilder, TEXT("FluidNinjaBuoyancy.GatherSlots"), Request.Slots);
		FRDGBufferRef Samples = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), Request.TotalSamples), TEXT("FluidNinjaBuoyancy.GatherSamples"));

		FFluidNinjaBuoyancyGatherCS::FParameters* Parameters = GraphBuilder.AllocParameters<FFluidNinjaBuoyancyGatherCS::FParameters>();
		Parameters->HeightTexture = Height;
		Parameters->FlowTexture = Flow;
		Parameters->WaveTexture = Wave;
		Parameters->SelfTexture = Self;
		Parameters->Slots = GraphBuilder.CreateSRV(Slots);
		Parameters->Samples = GraphBuilder.CreateUAV(Samples);
		Parameters->HeightSize = Request.HeightSize;
		Parameters->FlowSize = Request.Flow ? Request.FlowSize : FIntPoint(1, 1);
		Parameters->WaveSize = bWave ? Request.WaveSize : FIntPoint(1, 1);
		Parameters->NumSlots = Request.NumSlots;
		Parameters->CaptureTop = Request.CaptureTop;
		Parameters->CaptureFar = Request.CaptureFar;
		Parameters->HasFlow = Request.Flow ? 1 : 0;
		Parameters->HasWave = bWave ? 1 : 0;

		TShaderMapRef<FFluidNinjaBuoyancyGatherCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		const int32 Groups = FMath::DivideAndRoundUp(Request.MaxGrid, 8);
		{
			RDG_EVENT_SCOPE_STAT(GraphBuilder, FNB_Gather, "FNB Gather %d bodies", Request.NumSlots);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FluidNinjaBuoyancyGather"), Shader, Parameters, FIntVector(Groups, Groups, Request.NumSlots));
		}
		AddEnqueueCopyPass(GraphBuilder, &Readback, Samples, static_cast<uint32>(Request.TotalSamples) * sizeof(FVector4f));
	}
	GraphBuilder.Execute();
}

class FFluidNinjaBuoyancyShadersModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// The plugin holding this module: its Shaders folder is /Plugin/FluidNinjaBuoyancy.
		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
		{
			if (Plugin->GetDescriptor().Modules.ContainsByPredicate([](const FModuleDescriptor& Module) { return Module.Name == TEXT("FluidNinjaBuoyancyShaders"); }))
			{
				AddShaderSourceDirectoryMapping(TEXT("/Plugin/FluidNinjaBuoyancy"), FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
				return;
			}
		}
	}
};

IMPLEMENT_MODULE(FFluidNinjaBuoyancyShadersModule, FluidNinjaBuoyancyShaders)
