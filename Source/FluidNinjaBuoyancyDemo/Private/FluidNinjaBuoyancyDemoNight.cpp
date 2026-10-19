#include "FluidNinjaBuoyancyDemoNight.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/LightComponentBase.h"
#include "Components/PostProcessComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FluidNinjaBuoyancyDemoBall.h"
#include "FluidNinjaBuoyancyDemoLantern.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogFluidNinjaBuoyancyNight, Log, All);

namespace
{
	/** A lantern's own light (on a lantern, or on the lantern Blueprint it carries): never switched off. */
	bool IsLanternLight(const UActorComponent& Component)
	{
		const AActor* Owner = Component.GetOwner();
		return Owner && (Owner->IsA<AFluidNinjaBuoyancyDemoLantern>() || Cast<AFluidNinjaBuoyancyDemoLantern>(Owner->GetAttachParentActor())
			|| Cast<AFluidNinjaBuoyancyDemoLantern>(Owner->GetOwner()));
	}

	FAutoConsoleCommandWithWorldAndArgs NightCommand(TEXT("fnb.Demo.Night"),
		TEXT("Tunes a running lantern night: fnb.Demo.Night [moon lux] [sky light scale] [exposure bias]. No arguments: the values now."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			int32 Count = 0;
			if (GEngine)
			{
				for (const FWorldContext& Context : GEngine->GetWorldContexts())
				{
					UWorld* World = Context.World();
					if (!World || !World->IsGameWorld())
					{
						continue;
					}
					for (TActorIterator<AFluidNinjaBuoyancyDemoNight> It(World); It; ++It)
					{
						FFluidNinjaNightLook& Look = It->Look;
						if (Args.IsValidIndex(0)) { Look.MoonLux = FMath::Max(0.f, FCString::Atof(*Args[0])); }
						if (Args.IsValidIndex(1)) { Look.SkyLightScale = FMath::Clamp(FCString::Atof(*Args[1]), 0.f, 1.f); }
						if (Args.IsValidIndex(2)) { Look.ExposureBias = FMath::Clamp(FCString::Atof(*Args[2]), -15.f, 15.f); }
						It->ApplyLook();
						UE_LOG(LogFluidNinjaBuoyancyNight, Display, TEXT("Lantern night: moon %.3g lux, sky light scale %.3g, exposure bias %.2f"), Look.MoonLux, Look.SkyLightScale, Look.ExposureBias);
						++Count;
					}
				}
			}
			if (Count == 0)
			{
				UE_LOG(LogFluidNinjaBuoyancyNight, Warning, TEXT("fnb.Demo.Night: no lantern night is running (start one from the river's panel)."));
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs DropBallCommand(TEXT("fnb.Demo.DropBall"),
		TEXT("Drops a large, heavy ball into the middle of the lanterns during a lantern night (as its key, B, does)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld*)
		{
			if (GEngine)
			{
				for (const FWorldContext& Context : GEngine->GetWorldContexts())
				{
					UWorld* World = Context.World();
					if (!World || !World->IsGameWorld())
					{
						continue;
					}
					for (TActorIterator<AFluidNinjaBuoyancyDemoNight> It(World); It; ++It)
					{
						It->DropBall();
						return;
					}
				}
			}
			UE_LOG(LogFluidNinjaBuoyancyNight, Warning, TEXT("fnb.Demo.DropBall: no lantern night is running (start one from the river's panel)."));
		}));
}

AFluidNinjaBuoyancyDemoNight::AFluidNinjaBuoyancyDemoNight()
{
	PrimaryActorTick.bCanEverTick = true;
	DropBallKey = EKeys::B;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	Moon = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("Moon"));
	Moon->SetupAttachment(Root);
	Moon->SetMobility(EComponentMobility::Movable);
	Moon->SetRelativeRotation(FRotator(-38.0, 35.0, 0.0));
	Moon->LightColor = FColor(140, 168, 255);
	Moon->CastShadows = false;
	Moon->bAtmosphereSunLight = false;   // the sky is hidden for the night: the moon lights the scene only

	Exposure = CreateDefaultSubobject<UPostProcessComponent>(TEXT("Exposure"));
	Exposure->SetupAttachment(Root);
	Exposure->bUnbound = true;
	Exposure->Priority = 10000.f;
}

AFluidNinjaBuoyancyDemoNight* AFluidNinjaBuoyancyDemoNight::Begin(UWorld& World, const FFluidNinjaNightLook& Look)
{
	for (TActorIterator<AFluidNinjaBuoyancyDemoNight> It(&World); It; ++It)
	{
		It->Look = Look;
		It->ApplyLook();
		return *It;
	}
	AFluidNinjaBuoyancyDemoNight* Night = World.SpawnActorDeferred<AFluidNinjaBuoyancyDemoNight>(StaticClass(), FTransform::Identity);
	if (Night)
	{
		Night->Look = Look;
		Night->FinishSpawning(FTransform::Identity);
	}
	return Night;
}

void AFluidNinjaBuoyancyDemoNight::BeginPlay()
{
	Super::BeginPlay();
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	// Everything that lights or brightens the level, as it is now; the lanterns and this night's moon stay.
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (*It == this)
		{
			continue;
		}
		for (UActorComponent* Component : It->GetComponents())
		{
			USceneComponent* Scene = Cast<USceneComponent>(Component);
			if (!Scene || !Scene->IsVisible() || IsLanternLight(*Scene))
			{
				continue;
			}
			if (USkyLightComponent* Sky = Cast<USkyLightComponent>(Scene))
			{
				SkyLights.Add(Sky, Sky->Intensity);
			}
			else if (Scene->IsA<ULightComponentBase>() || Scene->IsA<USkyAtmosphereComponent>() || Scene->IsA<UExponentialHeightFogComponent>()
				|| Scene->IsA<UVolumetricCloudComponent>())
			{
				Scene->SetVisibility(false);
				Hidden.Add(Scene);
			}
		}
	}
	ApplyLook();
	UE_LOG(LogFluidNinjaBuoyancyNight, Log, TEXT("Lantern night: %d lights, skies and fogs off, %d sky lights dimmed; moon %.3g lux, exposure bias %.2f (fnb.Demo.Night tunes it)"),
		Hidden.Num(), SkyLights.Num(), Look.MoonLux, Look.ExposureBias);
}

void AFluidNinjaBuoyancyDemoNight::ApplyLook()
{
	Moon->SetIntensity(Look.MoonLux);
	Moon->SetVisibility(Look.MoonLux > 0.f);
	for (const TPair<TWeakObjectPtr<USceneComponent>, float>& Entry : SkyLights)
	{
		if (USkyLightComponent* Sky = Cast<USkyLightComponent>(Entry.Key.Get()))
		{
			Sky->SetIntensity(Entry.Value * Look.SkyLightScale);
			Sky->SetVisibility(Look.SkyLightScale > 0.f);
		}
	}
	FPostProcessSettings& Settings = Exposure->Settings;
	Settings.bOverride_AutoExposureMethod = true;
	Settings.AutoExposureMethod = AEM_Manual;
	Settings.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	Settings.AutoExposureApplyPhysicalCameraExposure = 0;
	Settings.bOverride_AutoExposureBias = true;
	Settings.AutoExposureBias = Look.ExposureBias;
}

void AFluidNinjaBuoyancyDemoNight::DropBall()
{
	UWorld* World = GetWorld();
	AFluidNinjaBuoyancyDemoBall* Ball = World ? AFluidNinjaBuoyancyDemoBall::DropOnLanterns(*World, DropHeightCm) : nullptr;
	if (!Ball)
	{
		return;
	}
	Balls.RemoveAll([](const TWeakObjectPtr<AActor>& Old) { return !Old.IsValid(); });
	while (Balls.Num() >= MaxBalls)
	{
		Balls[0]->Destroy();
		Balls.RemoveAt(0);
	}
	Balls.Add(Ball);
}

void AFluidNinjaBuoyancyDemoNight::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const APlayerController* Player = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (Player && DropBallKey.IsValid() && Player->WasInputKeyJustPressed(DropBallKey)
		&& !Player->IsInputKeyDown(EKeys::LeftControl) && !Player->IsInputKeyDown(EKeys::RightControl))   // Ctrl+Shift keys belong to the test bench
	{
		DropBall();
	}
}

void AFluidNinjaBuoyancyDemoNight::EndPlay(const EEndPlayReason::Type Reason)
{
	for (const TWeakObjectPtr<AActor>& Ball : Balls)
	{
		if (Ball.IsValid())
		{
			Ball->Destroy();
		}
	}
	Balls.Reset();
	for (const TWeakObjectPtr<USceneComponent>& Scene : Hidden)
	{
		if (Scene.IsValid())
		{
			Scene->SetVisibility(true);
		}
	}
	for (const TPair<TWeakObjectPtr<USceneComponent>, float>& Entry : SkyLights)
	{
		if (USkyLightComponent* Sky = Cast<USkyLightComponent>(Entry.Key.Get()))
		{
			Sky->SetIntensity(Entry.Value);
			Sky->SetVisibility(true);
		}
	}
	Hidden.Reset();
	SkyLights.Reset();
	Super::EndPlay(Reason);
}
