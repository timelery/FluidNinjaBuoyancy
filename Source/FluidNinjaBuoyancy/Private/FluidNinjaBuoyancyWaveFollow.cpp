#include "FluidNinjaBuoyancySubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "FluidNinjaBuoyancyLog.h"
#include "FluidNinjaBuoyancySettings.h"
#include "FluidNinjaBuoyancyWaves.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "TextureResource.h"

// Wave Follow: the drawn wave height under the view for materials that move with the waves (MF_FNB_WaveFollow). The pressure buffers are
// made by FluidNinja at run time, so a material cannot name one: the one under the view is copied into a render target the material does
// name, and its square goes into a parameter collection beside it.

namespace
{
	const FName WaveFollowWindowName(TEXT("Window"));
	const FName WaveFollowShapeName(TEXT("Shape"));
	// FluidNinja's pressure buffer is a 1 x 1 placeholder until NinjaLiveCore's render target DI sizes it (a tick's PostSimulate).
	constexpr int32 WaveFollowMinCells = 64;
}

void UFluidNinjaBuoyancySubsystem::UpdateWaveFollow()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	if (!bWaveFollowLoaded)
	{
		bWaveFollowLoaded = true;
		WaveFollowTarget = Cast<UTextureRenderTarget2D>(Settings.WaveFollowTarget.TryLoad());
		WaveFollowCollection = Cast<UMaterialParameterCollection>(Settings.WaveFollowCollection.TryLoad());
		if (Settings.bWaveFollow && (!WaveFollowTarget || !WaveFollowCollection))
		{
			UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("FluidNinja Buoyancy Wave Follow: %s or %s did not load, so materials that follow the waves stand still. Point Wave Follow Target and Wave Follow Collection (Project Settings > FluidNinja Buoyancy > Waves) at a render target and a parameter collection, or turn Wave Follow off."),
				*Settings.WaveFollowTarget.ToString(), *Settings.WaveFollowCollection.ToString());
		}
	}
	UMaterialParameterCollectionInstance* Collection = WaveFollowCollection ? World->GetParameterCollectionInstance(WaveFollowCollection) : nullptr;
	if (!Collection)
	{
		return;
	}

	// The simulation to follow: the one under the view, else the nearest square. Its pressure buffer must carry the wave height (RGBA16F:
	// the modified core's; a stock RG16F one has no B) and be past the placeholder.
	const FFlowSource* Followed = nullptr;
	UTextureRenderTarget2D* Pressure = nullptr;
	const APlayerController* Controller = World->GetFirstPlayerController();
	if (Settings.bWaveFollow && Settings.bWaves && Waves && Waves->IsActive() && WaveFollowTarget && Controller && Controller->PlayerCameraManager)
	{
		const FVector2D View(Controller->PlayerCameraManager->GetCameraLocation());
		const int32 Under = FindFlowSource(View);
		double Nearest = TNumericLimits<double>::Max();
		for (int32 Index = 0; Index < FlowSources.Num(); ++Index)
		{
			const FFlowSource& Source = FlowSources[Index];
			UTextureRenderTarget2D* Buffer = Source.Pressure.Get();
			if (!Source.bValid || !Buffer || Buffer->SizeX < WaveFollowMinCells || Buffer->SizeY < WaveFollowMinCells || Buffer->GetFormat() != PF_FloatRGBA)
			{
				continue;
			}
			const double Away = Index == Under ? -1.0
				: FMath::Max(FMath::Abs(View.X - Source.Centre.X), FMath::Abs(View.Y - Source.Centre.Y)) - Source.SizeCm * 0.5;
			if (Away < Nearest)
			{
				Nearest = Away;
				Followed = &Source;
				Pressure = Buffer;
			}
		}
	}

	if (Followed)
	{
		if (WaveFollowTarget->SizeX != Pressure->SizeX || WaveFollowTarget->SizeY != Pressure->SizeY || WaveFollowTarget->GetFormat() != PF_FloatRGBA)
		{
			// Not saved: the asset keeps its own size on disk (nothing marks it dirty).
			WaveFollowTarget->bAutoGenerateMips = false;
			WaveFollowTarget->InitCustomFormat(Pressure->SizeX, Pressure->SizeY, PF_FloatRGBA, true);
		}
		if (WaveFollowSource.Get() != Pressure)
		{
			// The NinjaLive actor by its label where it has one (the buffer's outer is its component).
			const UObject* Ninja = Followed->Ninja.Get();
			const AActor* NinjaActor = Ninja ? (Ninja->IsA<AActor>() ? Cast<AActor>(Ninja) : Ninja->GetTypedOuter<AActor>()) : nullptr;
			UE_LOG(LogFluidNinjaBuoyancy, Log, TEXT("FluidNinja Buoyancy Wave Follow: following %s (%d x %d cells, %.0f m square)."),
				NinjaActor ? *NinjaActor->GetActorNameOrLabel() : *GetNameSafe(Ninja), Pressure->SizeX, Pressure->SizeY, Followed->SizeCm / 100.0);
		}
		FTextureRenderTargetResource* From = Pressure->GameThread_GetRenderTargetResource();
		FTextureRenderTargetResource* To = WaveFollowTarget->GameThread_GetRenderTargetResource();
		if (From && To)
		{
			// Before this frame's Niagara GPU step: the copy trails the drawn surface by a frame.
			ENQUEUE_RENDER_COMMAND(FluidNinjaBuoyancyWaveFollowCopy)([From, To](FRHICommandListImmediate& RHICmdList)
			{
				FRHITexture* Source = From->GetRenderTargetTexture();
				FRHITexture* Target = To->GetRenderTargetTexture();
				if (!Source || !Target || Source->GetDesc().Extent != Target->GetDesc().Extent || Source->GetFormat() != Target->GetFormat())
				{
					return;
				}
				FRDGBuilder GraphBuilder(RHICmdList);
				AddCopyTexturePass(GraphBuilder, RegisterExternalTexture(GraphBuilder, Source, TEXT("FluidNinjaBuoyancy.WaveFollow.Pressure")),
					RegisterExternalTexture(GraphBuilder, Target, TEXT("FluidNinjaBuoyancy.WaveFollow")));
				GraphBuilder.Execute();
			});
		}
		// The drawn height is B × the surface's WaveHeightScale, read here with the Surface Mirror's 0 put back.
		float HeightScale = 1.f;
		if (const UMaterialInterface* Material = Followed->Material.Get())
		{
			Material->GetScalarParameterValue(FHashedMaterialParameterInfo(Settings.WaveHeightScaleParameter), HeightScale);
		}
		Collection->SetVectorParameterValue(WaveFollowWindowName,
			FLinearColor(static_cast<float>(Followed->Centre.X), static_cast<float>(Followed->Centre.Y), static_cast<float>(Followed->SizeCm), 1.f));
		Collection->SetVectorParameterValue(WaveFollowShapeName,
			FLinearColor(HeightScale, Settings.WaveFollowMaxHeightCm, Settings.WaveFollowEdgeFadeCm, 0.f));
		WaveFollowSource = Pressure;
		bWaveFollowPublished = true;
	}
	else if (bWaveFollowPublished)
	{
		Collection->SetVectorParameterValue(WaveFollowWindowName, FLinearColor::Transparent);
		WaveFollowSource.Reset();
		bWaveFollowPublished = false;
	}
}
