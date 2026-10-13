#include "FluidNinjaBuoyancyDemoRiver.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FluidNinjaBuoyancyComponent.h"
#include "FluidNinjaBuoyancySubsystem.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Components/PrimitiveComponent.h"
#include "InputCoreTypes.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY_STATIC(LogFluidNinjaBuoyancyDemo, Log, All);

namespace
{
	/** The demo river of the world in play (a command typed in the editor's console reaches the Play world). */
	AFluidNinjaBuoyancyDemoRiver* FindRiver(UWorld* World)
	{
		auto In = [](UWorld* Candidate) -> AFluidNinjaBuoyancyDemoRiver*
		{
			if (Candidate && Candidate->IsGameWorld())
			{
				for (TActorIterator<AFluidNinjaBuoyancyDemoRiver> It(Candidate); It; ++It)
				{
					return *It;
				}
			}
			return nullptr;
		};
		if (AFluidNinjaBuoyancyDemoRiver* River = In(World))
		{
			return River;
		}
		if (GEngine)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (AFluidNinjaBuoyancyDemoRiver* River = In(Context.World()))
				{
					return River;
				}
			}
		}
		return nullptr;
	}

	FAutoConsoleCommandWithWorldAndArgs RiverCommand(TEXT("fnb.Demo.River"), TEXT("The demo level's river: start | stop | reset."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			AFluidNinjaBuoyancyDemoRiver* River = FindRiver(World);
			const FString Verb = Args.IsEmpty() ? FString() : Args[0].ToLower();
			if (!River)
			{
				UE_LOG(LogFluidNinjaBuoyancyDemo, Warning, TEXT("fnb.Demo.River: no demo river in play."));
			}
			else if (Verb == TEXT("start"))
			{
				River->StartRiver();
			}
			else if (Verb == TEXT("stop"))
			{
				River->StopRiver();
			}
			else if (Verb == TEXT("reset"))
			{
				River->ResetRiver();
			}
			else
			{
				UE_LOG(LogFluidNinjaBuoyancyDemo, Display, TEXT("fnb.Demo.River start | stop | reset (now %s)"), River->IsRunning() ? TEXT("running") : TEXT("still"));
			}
		}));
}

AFluidNinjaBuoyancyDemoRiver::AFluidNinjaBuoyancyDemoRiver()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bTickEvenWhenPaused = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

UActorComponent* AFluidNinjaBuoyancyDemoRiver::FindSimulation() const
{
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (It->ActorHasTag(SimulationTag))
		{
			for (UActorComponent* Component : It->GetComponents())
			{
				if (Component && Component->GetClass()->GetName().Contains(TEXT("NinjaLiveComponent")))
				{
					return Component;
				}
			}
		}
	}
	return nullptr;
}

void AFluidNinjaBuoyancyDemoRiver::ApplyFlow(bool bRun)
{
	UActorComponent* Simulation = FindSimulation();
	if (!Simulation)
	{
		UE_LOG(LogFluidNinjaBuoyancyDemo, Warning, TEXT("Demo river: no NinjaLive simulation tagged %s."), *SimulationTag.ToString());
		return;
	}
	// FluidNinja is driven by name: its component's variables, which it hands to the running simulation at its next parameter update.
	auto Number = [Simulation](FName Name) -> const FNumericProperty* { return FindFProperty<FNumericProperty>(Simulation->GetClass(), Name); };
	auto Get = [Simulation, &Number](FName Name, double& Out)
	{
		if (const FNumericProperty* Property = Number(Name))
		{
			const void* Address = Property->ContainerPtrToValuePtr<void>(Simulation);
			Out = Property->IsFloatingPoint() ? Property->GetFloatingPointPropertyValue(Address) : static_cast<double>(Property->GetSignedIntPropertyValue(Address));
			return true;
		}
		return false;
	};
	auto Set = [Simulation, &Number](FName Name, double Value)
	{
		if (const FNumericProperty* Property = Number(Name))
		{
			void* Address = Property->ContainerPtrToValuePtr<void>(Simulation);
			Property->IsFloatingPoint() ? Property->SetFloatingPointPropertyValue(Address, Value) : Property->SetIntPropertyValue(Address, static_cast<int64>(Value));
			return true;
		}
		return false;
	};
	static const FName NoiseNames[] = {TEXT("VeloDirNoise"), TEXT("VeloFromBitmaps")};
	if (Turbulence.IsEmpty())
	{
		for (const FName& Name : NoiseNames)
		{
			double Value = 0.0;
			if (Get(Name, Value))
			{
				Turbulence.Add(Name, Value);
			}
		}
	}
	const bool bSet = Set(TEXT("DirectVelocityX"), bRun ? FlowVelocity.X : 0.0) && Set(TEXT("DirectVelocityY"), bRun ? FlowVelocity.Y : 0.0);
	for (const TPair<FName, double>& Noise : Turbulence)
	{
		Set(Noise.Key, bRun ? Noise.Value : 0.0);
	}
	UE_LOG(LogFluidNinjaBuoyancyDemo, Log, TEXT("Demo river: %s%s"), bRun ? *FString::Printf(TEXT("running, direct velocity (%.2f, %.2f)"), FlowVelocity.X, FlowVelocity.Y)
		: TEXT("still"), bSet ? TEXT("") : TEXT(" (NOT SET: the simulation has no DirectVelocityX/Y)"));
}

void AFluidNinjaBuoyancyDemoRiver::Moor(float DeltaSeconds)
{
	const UFluidNinjaBuoyancySubsystem* Buoyancy = GetWorld() ? GetWorld()->GetSubsystem<UFluidNinjaBuoyancySubsystem>() : nullptr;
	if (bRunning || MooringPeriod <= 0.f || (Buoyancy && Buoyancy->IsWaterControlled()))
	{
		return;   // running, not moored, or the test bench is measuring on its controlled water
	}
	// A critically damped horizontal spring to where each floater started: it rides the water there instead of wandering off.
	const float Omega = 2.f * PI / MooringPeriod;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (!It->ActorHasTag(FloaterTag))
		{
			continue;
		}
		const UFluidNinjaBuoyancyComponent* Body = It->FindComponentByClass<UFluidNinjaBuoyancyComponent>();
		UPrimitiveComponent* Primitive = Body ? Body->GetSimulatedPrimitive() : nullptr;
		if (!Primitive || !Primitive->IsSimulatingPhysics() || !Primitive->RigidBodyIsAwake())
		{
			continue;   // asleep, it stays where it is: a force would only wake it
		}
		FVector Offset = Body->GetStartTransform().GetLocation() - Primitive->GetComponentLocation();
		FVector Velocity = Primitive->GetPhysicsLinearVelocity();
		Offset.Z = Velocity.Z = 0.0;
		const float Mass = Primitive->GetMass();
		Primitive->AddForce((Offset * (Omega * Omega) - Velocity * (2.f * Omega)) * Mass);
	}
}

void AFluidNinjaBuoyancyDemoRiver::BeginPlay()
{
	Super::BeginPlay();
	ApplyFlow(false);
	if (bShowPanel)
	{
		AddPanel();   // the mouse looks around; Tab brings the cursor out to click the panel
	}
}

void AFluidNinjaBuoyancyDemoRiver::EndPlay(const EEndPlayReason::Type Reason)
{
	RemovePanel();
	Super::EndPlay(Reason);
}

void AFluidNinjaBuoyancyDemoRiver::StartRiver()
{
	ApplyFlow(true);
	bRunning = true;
}

void AFluidNinjaBuoyancyDemoRiver::StopRiver()
{
	ApplyFlow(false);
	bRunning = false;
}

void AFluidNinjaBuoyancyDemoRiver::ResetRiver()
{
	StopRiver();
	int32 Count = 0;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (It->ActorHasTag(FloaterTag))
		{
			if (UFluidNinjaBuoyancyComponent* Body = It->FindComponentByClass<UFluidNinjaBuoyancyComponent>())
			{
				Body->RestartAt(Body->GetStartTransform());
				++Count;
			}
		}
	}
	UE_LOG(LogFluidNinjaBuoyancyDemo, Log, TEXT("Demo river: %d floaters back at the head"), Count);
}

void AFluidNinjaBuoyancyDemoRiver::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	// FluidNinja may apply its preset after this actor's Begin Play: say "still" again once it is surely up.
	if (!bRunning && !bStillAgain && GetWorld()->GetTimeSeconds() > 2.0)
	{
		bStillAgain = true;
		ApplyFlow(false);
	}
	Moor(DeltaSeconds);
	APlayerController* Player = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!Player || Player->IsInputKeyDown(EKeys::LeftControl) || Player->IsInputKeyDown(EKeys::RightControl))
	{
		return;   // Ctrl+Shift keys belong to the test bench
	}
	if (Player->WasInputKeyJustPressed(EKeys::Enter))
	{
		bRunning ? StopRiver() : StartRiver();
	}
	if (Player->WasInputKeyJustPressed(EKeys::R))
	{
		ResetRiver();
	}
	if (Player->WasInputKeyJustPressed(EKeys::Tab))
	{
		ShowCursor(!bCursor);
	}
}

void AFluidNinjaBuoyancyDemoRiver::ShowCursor(bool bShow)
{
	APlayerController* Player = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!Player)
	{
		return;
	}
	bCursor = bShow;
	Player->SetShowMouseCursor(bShow);
	if (bShow)
	{
		FInputModeGameAndUI Mode;
		Mode.SetHideCursorDuringCapture(false);
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Player->SetInputMode(Mode);
	}
	else
	{
		Player->SetInputMode(FInputModeGameOnly());
	}
}

void AFluidNinjaBuoyancyDemoRiver::AddPanel()
{
	UWorld* World = GetWorld();
	UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;
	if (!Viewport || Panel.IsValid() || !FSlateApplication::IsInitialized())
	{
		return;
	}
	TWeakObjectPtr<AFluidNinjaBuoyancyDemoRiver> Self(this);
	auto Text = [](const FString& Value, int32 Size)
	{
		return SNew(STextBlock).Text(FText::FromString(Value)).Font(FCoreStyle::GetDefaultFontStyle("Regular", Size)).ColorAndOpacity(FLinearColor(0.92f, 0.96f, 1.f));
	};
	Panel = SNew(SBox)
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Top)
		.Padding(FMargin(24.f, 24.f))
		[
			SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FLinearColor(0.01f, 0.04f, 0.07f, 0.72f))
			.Padding(FMargin(16.f, 12.f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
				[
					SNew(STextBlock).Text(FText::FromString(TEXT("FluidNinja Buoyancy"))).Font(FCoreStyle::GetDefaultFontStyle("Bold", 15)).ColorAndOpacity(FLinearColor::White)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 10.f)
				[
					SNew(STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 11))
					.ColorAndOpacity(FLinearColor(0.92f, 0.96f, 1.f))
					.Text_Lambda([Self]()
					{
						return FText::FromString(Self.IsValid() && Self->IsRunning() ? TEXT("The river is flowing: everything drifts downstream.")
							: TEXT("The boat, crates and balls wait at the head of the river."));
					})
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 10.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)
					[
						SNew(SButton)
						.ContentPadding(FMargin(14.f, 6.f))
						.OnClicked_Lambda([Self]()
						{
							if (Self.IsValid())
							{
								Self->IsRunning() ? Self->StopRiver() : Self->StartRiver();
								Self->ShowCursor(false);   // back to play
							}
							return FReply::Handled();
						})
						[
							SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
							.Text_Lambda([Self]() { return FText::FromString(Self.IsValid() && Self->IsRunning() ? TEXT("Stop river") : TEXT("Start river")); })
						]
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.ContentPadding(FMargin(14.f, 6.f))
						.OnClicked_Lambda([Self]()
						{
							if (Self.IsValid())
							{
								Self->ResetRiver();
								Self->ShowCursor(false);   // back to play
							}
							return FReply::Handled();
						})
						[
							SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 12)).Text(FText::FromString(TEXT("Reset")))
						]
					]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					Text(TEXT("Enter start / stop    R reset    Tab mouse cursor (to click these)\nWASD move    mouse look    Space jump (swim up)"), 9)
				]
			]
		];
	Viewport->AddViewportWidgetContent(Panel.ToSharedRef(), 20);
}

void AFluidNinjaBuoyancyDemoRiver::RemovePanel()
{
	if (Panel.IsValid())
	{
		if (UWorld* World = GetWorld())
		{
			if (UGameViewportClient* Viewport = World->GetGameViewport())
			{
				Viewport->RemoveViewportWidgetContent(Panel.ToSharedRef());
			}
		}
		Panel.Reset();
	}
}
