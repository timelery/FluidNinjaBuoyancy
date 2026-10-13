#pragma once

#include "CoreMinimal.h"

class FRHICommandListImmediate;
class FRHIGPUBufferReadback;
class FRHITexture;

/** FluidNinja Buoyancy's GPU gather: every floating body's water resampled in one compute pass onto its own grid, read back once a frame. */
namespace FluidNinjaBuoyancyGather
{
	struct FRequest
	{
		FRHITexture* Height = nullptr;   // the Surface Mirror (R32F scene depth)
		FRHITexture* Flow = nullptr;     // FluidNinja's velocity/density buffer, or null
		FIntPoint HeightSize = FIntPoint::ZeroValue;
		FIntPoint FlowSize = FIntPoint::ZeroValue;
		float CaptureTop = 0.f;
		float CaptureFar = 0.f;
		int32 NumSlots = 0;
		/** The largest grid of any body (the dispatch covers it). */
		int32 MaxGrid = 0;
		/** Points of all bodies together. */
		int32 TotalSamples = 0;
		/** Three per body, in texels of each texture: (height column, row, step, grid points across), (flow column, row, step,
		 * 1 for flow), (index of its first sample, 0, 0, 0). */
		TArray<FVector4f> Slots;
	};

	/** Render thread: dispatches the gather and queues its copy into Readback. Each sample is (height, or below -1e29 for no water,
	 * flow u, flow v, density); a body's point (i, j) is at first + j * grid + i. */
	FLUIDNINJABUOYANCYSHADERS_API void Enqueue(FRHICommandListImmediate& RHICmdList, const FRequest& Request, FRHIGPUBufferReadback& Readback);
}
