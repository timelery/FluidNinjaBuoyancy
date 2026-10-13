#include "FluidNinjaBuoyancyTestBench.h"

#include "Camera/PlayerCameraManager.h"
#include "CollisionQueryParams.h"
#include "Components/PrimitiveComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "FluidNinjaBuoyancyComponent.h"
#include "FluidNinjaBuoyancyLog.h"
#include "FluidNinjaBuoyancySettings.h"
#include "FluidNinjaBuoyancySubsystem.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SViewport.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	TAutoConsoleVariable<int32> CVarTestHUD(
		TEXT("fnb.TestHUD"), 0,
		TEXT("FluidNinja Buoyancy test bench panel (top right) and body labels: 0 hidden (default), 1 in PIE, 2 in any non-shipping game world. Commands: fnb.Test help."));

	constexpr int32 MaxPhases = 6;
	constexpr float WindPresets[] = {-1.f, 10.f, 20.f, 0.f};   // the project's Wind, 10 m/s, 20 m/s, calm (Ctrl+Shift+W)

	const TCHAR* ModelName(EFluidNinjaHullModel Model)
	{
		switch (Model)
		{
		case EFluidNinjaHullModel::Voxels: return TEXT("vox");
		case EFluidNinjaHullModel::Spheres: return TEXT("sph");
		default: return TEXT("tri");
		}
	}

	/** Ctrl+Shift hotkeys, consumed only while the game viewport has focus. */
	class FFluidNinjaBuoyancyBenchInput : public IInputProcessor
	{
	public:
		explicit FFluidNinjaBuoyancyBenchInput(const TSharedPtr<FFluidNinjaBuoyancyTestBench>& InBench) : Bench(InBench) {}

		virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}

		virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& Event) override
		{
			const TSharedPtr<FFluidNinjaBuoyancyTestBench> Pinned = Bench.Pin();
			if (!Pinned || !Event.IsControlDown() || !Event.IsShiftDown() || Event.IsAltDown() || Event.IsRepeat() || !Pinned->IsGameFocused())
			{
				return false;
			}
			const FKey Key = Event.GetKey();
			if (Key == EKeys::B) { Pinned->ToggleVisible(); return true; }
			if (Key == EKeys::R) { Pinned->ResetBodies(true); return true; }
			if (Key == EKeys::PageDown) { Pinned->Select(1); return true; }
			if (Key == EKeys::PageUp) { Pinned->Select(-1); return true; }
			if (Key == EKeys::T) { Pinned->Run(TEXT("all")); return true; }
			if (Key == EKeys::W) { Pinned->CycleWind(); return true; }
			if (Key == EKeys::C) { Pinned->Command({TEXT("water"), TEXT("toggle")}); return true; }
			if (Key == EKeys::D) { Pinned->ToggleDebugView(); return true; }
			if (Key == EKeys::K) { Pinned->Capsize(30.f); return true; }
			if (Key == EKeys::J) { Pinned->Drop(3.f); return true; }
			return false;
		}

	private:
		TWeakPtr<FFluidNinjaBuoyancyTestBench> Bench;
	};
}

/** One body followed through a test: its peaks, and per phase its mean velocity, yaw rate, sinking and water over the window. */
struct FFluidNinjaBuoyancyTestBench::FTrack
{
	TWeakObjectPtr<UFluidNinjaBuoyancyComponent> Body;
	FString Name;
	bool bEligible = true;
	float MaxSpeed = 0.f;
	float MaxSlam = 0.f;
	bool bBroke = false;
	bool bSawHeld = false;
	bool bSawDormant = false;
	bool bWasInWater = false;
	/** World time the body last touched the water (a light float on live waves leaves it for a moment now and then). */
	double LastInWater = -1.0;
	float Yaw = 0.f;            // heading, unwrapped
	float LastRawYaw = 0.f;
	FVector WindowLocation = FVector::ZeroVector;
	float WindowYaw = 0.f;
	double WindowTime = -1.0;
	FVector Velocity[MaxPhases];
	float YawRate[MaxPhases] = {};
	float MaxTilt[MaxPhases] = {};
	float MaxSink[MaxPhases] = {};
	float WaterSum[MaxPhases] = {};
	int32 WaterCount[MaxPhases] = {};
	// Height: where each phase began, the deepest it went below that, and its mean over the window.
	float PhaseStartZ[MaxPhases] = {};
	float MaxDrop[MaxPhases] = {};
	double ZSum[MaxPhases] = {};
	int32 ZCount[MaxPhases] = {};
};

struct FFluidNinjaBuoyancyTestBench::FPhase
{
	FString Label;
	float Seconds = 0.f;
	/** Seconds into the phase the measuring window opens (velocities, yaw rate, water heights are over the window). */
	float MeasureFrom = 0.f;
	TFunction<void(FFluidNinjaBuoyancyTestBench&, FRun&)> Enter;
	TFunction<void(FFluidNinjaBuoyancyTestBench&, FRun&)> Exit;
};

struct FFluidNinjaBuoyancyTestBench::FRun
{
	FString Name;
	TArray<FPhase> Phases;
	TArray<FTrack> Tracks;
	int32 Phase = -1;
	double PhaseStarted = 0.0;
	bool bWindowOpen = false;
	int32 FirstResult = 0;
	float WindDegrees = 0.f;
	float Cost[MaxPhases] = {};
	TFunction<void(FFluidNinjaBuoyancyTestBench&, FRun&)> Evaluate;
	// What the test changes, put back after it.
	bool bWasControlled = false;
	float WasHeight = 0.f;
	bool bWasWindOverride = false;
	float WasWindMs = 0.f;
	float WasWindDegrees = 0.f;
	int32 WasGather = 1;
	bool bPawnAway = false;
	bool bPawnMoved = false;
	bool bWasSceneryCollision = true;
	/** Put every body back when done (the controlled tests move them, a boat at full throttle a long way). */
	bool bResetAfter = false;
	FTransform PawnReturn;
	FRotator ControlReturn = FRotator::ZeroRotator;
};

TSharedPtr<FFluidNinjaBuoyancyTestBench> FFluidNinjaBuoyancyTestBench::Create(UFluidNinjaBuoyancySubsystem& InSubsystem)
{
#if UE_BUILD_SHIPPING
	return nullptr;
#else
	UWorld* World = InSubsystem.GetWorld();
	if (!World || !World->IsGameWorld() || World->GetNetMode() == NM_DedicatedServer)
	{
		return nullptr;
	}
	TSharedPtr<FFluidNinjaBuoyancyTestBench> Bench = MakeShared<FFluidNinjaBuoyancyTestBench>();
	Bench->Subsystem = &InSubsystem;
	return Bench;
#endif
}

void FFluidNinjaBuoyancyTestBench::AttachPanel()
{
	// Once the world has floating bodies (a level without any gets no panel), and only where fnb.TestHUD allows it.
	UWorld* World = GetWorld();
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	const int32 Mode = CVarTestHUD.GetValueOnGameThread();
	if (Widget.IsValid() || !World || !Hydro || Mode <= 0 || (Mode == 1 && World->WorldType != EWorldType::PIE) || !FSlateApplication::IsInitialized()
		|| Hydro->GetBodies().IsEmpty())
	{
		return;
	}
	UGameViewportClient* GameViewport = World->GetGameViewport();
	if (!GameViewport)
	{
		return;
	}
	Viewport = GameViewport;
	Widget = Build();
	GameViewport->AddViewportWidgetContent(Widget.ToSharedRef(), 1001);
	Input = MakeShared<FFluidNinjaBuoyancyBenchInput>(AsShared());
	FSlateApplication::Get().RegisterInputPreProcessor(Input);
}

FFluidNinjaBuoyancyTestBench::~FFluidNinjaBuoyancyTestBench()
{
	Remove();
}

void FFluidNinjaBuoyancyTestBench::Remove()
{
	if (Current.IsValid())
	{
		FinishRun(true);
	}
	Queue.Reset();
	SetSceneryCollision(true);
	if (Input.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(Input);
	}
	Input.Reset();
	if (Widget.IsValid())
	{
		if (UGameViewportClient* GameViewport = Viewport.Get())
		{
			GameViewport->RemoveViewportWidgetContent(Widget.ToSharedRef());
		}
	}
	Widget.Reset();
}

UWorld* FFluidNinjaBuoyancyTestBench::GetWorld() const
{
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	return Hydro ? Hydro->GetWorld() : nullptr;
}

bool FFluidNinjaBuoyancyTestBench::IsGameFocused() const
{
	const UGameViewportClient* GameViewport = Viewport.Get();
	const TSharedPtr<SViewport> ViewportWidget = GameViewport ? GameViewport->GetGameViewportWidget() : nullptr;
	return ViewportWidget.IsValid() && (ViewportWidget->HasAnyUserFocusOrFocusedDescendants() || ViewportWidget->HasMouseCapture());
}

FVector FFluidNinjaBuoyancyTestBench::ViewLocation() const
{
	const UWorld* World = GetWorld();
	const APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
	return Controller && Controller->PlayerCameraManager ? Controller->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector;
}

float FFluidNinjaBuoyancyTestBench::ViewYaw() const
{
	const UWorld* World = GetWorld();
	const APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
	return Controller && Controller->PlayerCameraManager ? static_cast<float>(Controller->PlayerCameraManager->GetCameraRotation().Yaw) : 0.f;
}

FString FFluidNinjaBuoyancyTestBench::BodyName(const UFluidNinjaBuoyancyComponent* Body) const
{
	const AActor* Owner = Body ? Body->GetOwner() : nullptr;
	if (!Owner)
	{
		return TEXT("?");
	}
#if WITH_EDITOR
	FString Name = Owner->GetActorLabel();
#else
	FString Name = Owner->GetName();
#endif
	Name.RemoveFromStart(TEXT("Buoyancy Test "));
	return Name;
}

float FFluidNinjaBuoyancyTestBench::Tilt(const UFluidNinjaBuoyancyComponent* Body) const
{
	const UPrimitiveComponent* Primitive = Body ? Body->GetSimulatedPrimitive() : nullptr;
	if (!Primitive)
	{
		return 0.f;
	}
	const FQuat Rested = Primitive->GetComponentQuat() * Body->RestAttitude.Quaternion().Inverse();
	return static_cast<float>(FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Rested.GetUpVector().Z, -1.0, 1.0))));
}

bool FFluidNinjaBuoyancyTestBench::Draft(const UFluidNinjaBuoyancyComponent* Body, float& OutDraft) const
{
	FFluidNinjaBuoyancyBodyDebug Debug;
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (!Hydro || !Body || !Hydro->GetBodyDebug(Body, Debug) || !Debug.bHasWaterZ)
	{
		return false;
	}
	OutDraft = Debug.WaterZ - Body->GetLowestPointZ();
	return true;
}

bool FFluidNinjaBuoyancyTestBench::BodiesWaterHeight(float& OutHeight) const
{
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (!Hydro)
	{
		return false;
	}
	double Sum = 0.0;
	int32 Count = 0;
	for (const UFluidNinjaBuoyancyComponent* Body : Hydro->GetBodies())
	{
		FFluidNinjaBuoyancyBodyDebug Debug;
		if (Hydro->GetBodyDebug(Body, Debug) && Debug.bHasWaterZ && (Debug.bInCapture || Debug.bFlat || Hydro->IsWaterControlled()))
		{
			// Only bodies afloat now (a body stranded on a bank reads the water beside it).
			if (Body->GetReadout().bInWater || Hydro->IsWaterControlled())
			{
				Sum += Debug.WaterZ;
				++Count;
			}
		}
	}
	if (Count == 0)
	{
		return false;
	}
	OutHeight = static_cast<float>(Sum / Count);
	return true;
}

/* ---- Controls ---- */

UFluidNinjaBuoyancyComponent* FFluidNinjaBuoyancyTestBench::GetSelected() const
{
	return Selected.Get();
}

void FFluidNinjaBuoyancyTestBench::Select(int32 Step)
{
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (!Hydro)
	{
		return;
	}
	TArray<UFluidNinjaBuoyancyComponent*> Bodies = Hydro->GetBodies();
	if (Bodies.IsEmpty())
	{
		Selected.Reset();
		return;
	}
	const int32 Index = Bodies.IndexOfByKey(Selected.Get());
	Selected = Bodies[Index == INDEX_NONE ? 0 : (Index + Step % Bodies.Num() + Bodies.Num()) % Bodies.Num()];
}

bool FFluidNinjaBuoyancyTestBench::SelectByName(const FString& Part)
{
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	for (UFluidNinjaBuoyancyComponent* Body : Hydro ? Hydro->GetBodies() : TArray<UFluidNinjaBuoyancyComponent*>())
	{
		if (BodyName(Body).Contains(Part))
		{
			Selected = Body;
			return true;
		}
	}
	return false;
}

void FFluidNinjaBuoyancyTestBench::ResetBodies(bool bAll)
{
	UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (!Hydro)
	{
		return;
	}
	for (UFluidNinjaBuoyancyComponent* Body : Hydro->GetBodies())
	{
		if (bAll || Body == Selected.Get())
		{
			Body->RestartAt(Body->GetStartTransform());
		}
	}
}

void FFluidNinjaBuoyancyTestBench::SetControlledWater(bool bOn, const float* Height)
{
	UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (!Hydro)
	{
		return;
	}
	float Level = Hydro->GetControlledWaterHeight();
	if (bOn && !Height && !BodiesWaterHeight(Level))
	{
		Level = static_cast<float>(ViewLocation().Z) - 170.f;   // no body knows its water: about where the player stands
	}
	Hydro->SetControlledWater(bOn, Height ? *Height : Level);
}

void FFluidNinjaBuoyancyTestBench::SetTestWind(bool bOn, float SpeedMs, const float* Degrees)
{
	if (UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get())
	{
		Hydro->SetWindOverride(bOn, SpeedMs, Degrees ? *Degrees : ViewYaw());
	}
}

void FFluidNinjaBuoyancyTestBench::CycleWind()
{
	WindPreset = (WindPreset + 1) % UE_ARRAY_COUNT(WindPresets);
	const float Speed = WindPresets[WindPreset];
	SetTestWind(Speed >= 0.f, FMath::Max(Speed, 0.f));
}

void FFluidNinjaBuoyancyTestBench::Push(float NewtonSeconds)
{
	UFluidNinjaBuoyancyComponent* Body = Selected.Get();
	UPrimitiveComponent* Primitive = Body ? Body->GetSimulatedPrimitive() : nullptr;
	if (Primitive && Primitive->IsSimulatingPhysics())
	{
		const FVector Along = FRotator(0.f, ViewYaw(), 0.f).Vector();
		Primitive->AddImpulse(Along * (NewtonSeconds * 100.f));   // N·s = 100 kg·cm/s
	}
}

void FFluidNinjaBuoyancyTestBench::Capsize(float Degrees)
{
	UFluidNinjaBuoyancyComponent* Body = Selected.Get();
	UPrimitiveComponent* Primitive = Body ? Body->GetSimulatedPrimitive() : nullptr;
	if (!Primitive)
	{
		return;
	}
	if (Body->IsAwaitingWater())
	{
		Body->ReleaseHold();
	}
	// Heeled about its own fore-and-aft axis (in its rest attitude), through its centre of mass.
	const FQuat Rested = Primitive->GetComponentQuat() * Body->RestAttitude.Quaternion().Inverse();
	FVector Axis = Rested.GetForwardVector();
	Axis.Z = 0.0;
	Axis = Axis.GetSafeNormal(UE_SMALL_NUMBER, FVector::XAxisVector);
	const FQuat Heel(Axis, FMath::DegreesToRadians(Degrees));
	const FVector Centre = Primitive->IsSimulatingPhysics() ? Primitive->GetCenterOfMass() : Primitive->Bounds.Origin;
	const FVector Location = Centre + Heel.RotateVector(Primitive->GetComponentLocation() - Centre);
	Primitive->SetWorldLocationAndRotation(Location, Heel * Primitive->GetComponentQuat(), false, nullptr, ETeleportType::ResetPhysics);
	Primitive->SetPhysicsLinearVelocity(FVector::ZeroVector);
	Primitive->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector);
	Primitive->WakeRigidBody();
}

void FFluidNinjaBuoyancyTestBench::Drop(float Metres)
{
	UFluidNinjaBuoyancyComponent* Body = Selected.Get();
	UPrimitiveComponent* Primitive = Body ? Body->GetSimulatedPrimitive() : nullptr;
	if (!Primitive)
	{
		return;
	}
	if (Body->IsAwaitingWater())
	{
		Body->ReleaseHold();
	}
	Primitive->SetWorldLocation(Primitive->GetComponentLocation() + FVector(0.0, 0.0, Metres * 100.0), false, nullptr, ETeleportType::ResetPhysics);
	Primitive->SetPhysicsLinearVelocity(FVector::ZeroVector);
	Primitive->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector);
	Primitive->WakeRigidBody();
}

void FFluidNinjaBuoyancyTestBench::BringHere()
{
	UFluidNinjaBuoyancyComponent* Body = Selected.Get();
	if (!Body)
	{
		return;
	}
	// Four metres ahead of the view, turned to it, then settled on the water there as at the start.
	const float Yaw = ViewYaw();
	const FVector Location = ViewLocation() + FRotator(0.f, Yaw, 0.f).Vector() * 400.0;
	Body->RestartAt(FTransform(FQuat(FVector::ZAxisVector, FMath::DegreesToRadians(Yaw)) * Body->RestAttitude.Quaternion(), Location));
}

void FFluidNinjaBuoyancyTestBench::SetBoat(float Throttle, float Rudder)
{
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	UFluidNinjaBuoyancyComponent* Chosen = Selected.Get();
	for (UFluidNinjaBuoyancyComponent* Body : Hydro ? Hydro->GetBodies() : TArray<UFluidNinjaBuoyancyComponent*>())
	{
		// The selected body when it has propulsion, else every body that has.
		if (Body->bPropulsion && (Body == Chosen || !Chosen || !Chosen->bPropulsion))
		{
			Body->Throttle = FMath::Clamp(Throttle, -1.f, 1.f);
			Body->Rudder = FMath::Clamp(Rudder, -1.f, 1.f);
		}
	}
}

void FFluidNinjaBuoyancyTestBench::ToggleDebugView()
{
	FluidNinjaBuoyancyCVars::Enable = FluidNinjaBuoyancyCVars::Enable >= 2 ? 1 : 2;
}

void FFluidNinjaBuoyancyTestBench::SetSceneryCollision(bool bCollide)
{
	if (bCollide)
	{
		for (const TPair<TWeakObjectPtr<UPrimitiveComponent>, FSavedResponse>& Saved : SceneryOff)
		{
			if (UPrimitiveComponent* Primitive = Saved.Key.Get())
			{
				Primitive->SetCollisionResponseToChannel(ECC_WorldStatic, Saved.Value.WorldStatic);
				Primitive->SetCollisionResponseToChannel(ECC_WorldDynamic, Saved.Value.WorldDynamic);
			}
		}
		SceneryOff.Reset();
		return;
	}
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	for (const UFluidNinjaBuoyancyComponent* Body : Hydro ? Hydro->GetBodies() : TArray<UFluidNinjaBuoyancyComponent*>())
	{
		UPrimitiveComponent* Primitive = Body->GetSimulatedPrimitive();
		if (!Primitive || SceneryOff.Contains(Primitive))
		{
			continue;
		}
		SceneryOff.Add(Primitive, {Primitive->GetCollisionResponseToChannel(ECC_WorldStatic), Primitive->GetCollisionResponseToChannel(ECC_WorldDynamic)});
		Primitive->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Ignore);
		Primitive->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Ignore);
	}
}

/* ---- Tests ---- */

void FFluidNinjaBuoyancyTestBench::Report(const FString& Test, const FString& Body, int32 Verdict, const FString& Text)
{
	Results.Add({Test, Body, Verdict, Text});
	UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy test: %s %s %s: %s"), Verdict > 0 ? TEXT("PASS") : Verdict < 0 ? TEXT("FAIL") : TEXT("INFO"), *Test, *Body, *Text);
}

void FFluidNinjaBuoyancyTestBench::SaveResults(const FString& Test, int32 FirstResult) const
{
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("test"), Test);
	Root->SetStringField(TEXT("time"), FDateTime::Now().ToIso8601());
	int32 Passed = 0;
	int32 Failed = 0;
	TArray<TSharedPtr<FJsonValue>> Items;
	for (int32 Index = FMath::Max(FirstResult, 0); Index < Results.Num(); ++Index)
	{
		const FResult& Result = Results[Index];
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("test"), Result.Test);
		Item->SetStringField(TEXT("body"), Result.Body);
		Item->SetStringField(TEXT("verdict"), Result.Verdict > 0 ? TEXT("pass") : Result.Verdict < 0 ? TEXT("fail") : TEXT("info"));
		Item->SetStringField(TEXT("text"), Result.Text);
		Items.Add(MakeShared<FJsonValueObject>(Item));
		Passed += Result.Verdict > 0;
		Failed += Result.Verdict < 0;
	}
	Root->SetNumberField(TEXT("passed"), Passed);
	Root->SetNumberField(TEXT("failed"), Failed);
	Root->SetArrayField(TEXT("results"), Items);
	if (Hydro)
	{
		Root->SetStringField(TEXT("status"), Hydro->GetStatus());
	}
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Root, Writer);
	const FString Folder = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FluidNinjaBuoyancy"), TEXT("TestResults"));
	FFileHelper::SaveStringToFile(Json, *FPaths::Combine(Folder, FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S_")) + Test + TEXT(".json")));
	FFileHelper::SaveStringToFile(Json, *FPaths::Combine(Folder, TEXT("latest.json")));
	UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy test %s done: %d passed, %d failed (Saved/FluidNinjaBuoyancy/TestResults/latest.json)"), *Test, Passed, Failed);
}

void FFluidNinjaBuoyancyTestBench::Run(const FString& Name)
{
	if (Current.IsValid())
	{
		Stop();
	}
	if (Name.Equals(TEXT("all"), ESearchCase::IgnoreCase))
	{
		Queue = {TEXT("settle"), TEXT("capsize"), TEXT("drop"), TEXT("wind"), TEXT("throttle"), TEXT("gather"), TEXT("walkaway")};
		QueueName = TEXT("all");
		QueueFirstResult = Results.Num();
		const FString First = Queue[0];
		Queue.RemoveAt(0);
		StartRun(MakeRun(First));
		return;
	}
	QueueName.Reset();
	StartRun(MakeRun(Name.ToLower()));
}

void FFluidNinjaBuoyancyTestBench::Stop()
{
	Queue.Reset();
	QueueName.Reset();
	if (Current.IsValid())
	{
		Report(Current->Name, TEXT("-"), 0, TEXT("stopped"));
		FinishRun(true);
	}
}

void FFluidNinjaBuoyancyTestBench::StartRun(TUniquePtr<FRun> NewRun)
{
	UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (!NewRun.IsValid() || !Hydro)
	{
		return;
	}
	const FFluidNinjaBuoyancyWind& WindNow = Hydro->GetWind();
	NewRun->bWasControlled = Hydro->IsWaterControlled();
	NewRun->WasHeight = Hydro->GetControlledWaterHeight();
	NewRun->bWasWindOverride = WindNow.bOverride;
	NewRun->WasWindMs = WindNow.SpeedMs;
	NewRun->WasWindDegrees = WindNow.DirectionDegrees;
	NewRun->WasGather = FluidNinjaBuoyancyCVars::Gather;
	NewRun->bWasSceneryCollision = IsSceneryCollision();
	NewRun->WindDegrees = ViewYaw();
	NewRun->FirstResult = Results.Num();
	for (UFluidNinjaBuoyancyComponent* Body : Hydro->GetBodies())
	{
		FTrack& Track = NewRun->Tracks.AddDefaulted_GetRef();
		Track.Body = Body;
		Track.Name = BodyName(Body);
		if (const UPrimitiveComponent* Primitive = Body->GetSimulatedPrimitive())
		{
			Track.LastRawYaw = static_cast<float>((Primitive->GetComponentQuat() * Body->RestAttitude.Quaternion().Inverse()).Rotator().Yaw);
			Track.Yaw = Track.LastRawYaw;
		}
	}
	UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy test %s: started with %d bodies"), *NewRun->Name, NewRun->Tracks.Num());
	Current = MoveTemp(NewRun);
}

void FFluidNinjaBuoyancyTestBench::FinishRun(bool bStopped)
{
	if (!Current.IsValid())
	{
		return;
	}
	TUniquePtr<FRun> Done = MoveTemp(Current);
	if (!bStopped && Done->Evaluate)
	{
		Done->Evaluate(*this, *Done);
	}
	// Put back what the test changed.
	if (UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get())
	{
		Hydro->SetControlledWater(Done->bWasControlled, Done->WasHeight);
		Hydro->SetWindOverride(Done->bWasWindOverride, Done->WasWindMs, Done->WasWindDegrees);
	}
	FluidNinjaBuoyancyCVars::Gather = Done->WasGather;
	if (Done->bWasSceneryCollision)
	{
		SetSceneryCollision(true);
	}
	if (Done->bResetAfter)
	{
		ResetBodies(true);
	}
	if (UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get())
	{
		Hydro->SetVerifyGather(false);
	}
	for (FTrack& Track : Done->Tracks)
	{
		if (UFluidNinjaBuoyancyComponent* Body = Track.Body.Get(); Body && Body->bPropulsion && Done->Name == TEXT("throttle"))
		{
			Body->Throttle = 0.f;
			Body->Rudder = 0.f;
		}
	}
	if (Done->bPawnAway)
	{
		if (UWorld* World = GetWorld())
		{
			if (APlayerController* Controller = World->GetFirstPlayerController())
			{
				if (APawn* Pawn = Controller->GetPawn())
				{
					Pawn->TeleportTo(Done->PawnReturn.GetLocation(), Done->PawnReturn.Rotator());
				}
				Controller->SetControlRotation(Done->ControlReturn);
			}
		}
	}
	if (!bStopped)
	{
		SaveResults(Done->Name, Done->FirstResult);
	}
	if (!bStopped && !Queue.IsEmpty())
	{
		const FString Next = Queue[0];
		Queue.RemoveAt(0);
		StartRun(MakeRun(Next));
	}
	else if (!bStopped && !QueueName.IsEmpty())
	{
		SaveResults(QueueName, QueueFirstResult);
		QueueName.Reset();
	}
}

void FFluidNinjaBuoyancyTestBench::TickRun()
{
	FRun& Active = *Current;
	UWorld* World = GetWorld();
	UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (!World || !Hydro || Active.Phases.IsEmpty())
	{
		FinishRun(true);
		return;
	}
	const double Now = World->GetTimeSeconds();
	auto EnterPhase = [this, &Active, Now](int32 Index)
	{
		Active.Phase = Index;
		Active.PhaseStarted = Now;
		Active.bWindowOpen = false;
		for (FTrack& Track : Active.Tracks)
		{
			const UPrimitiveComponent* Primitive = Track.Body.IsValid() ? Track.Body->GetSimulatedPrimitive() : nullptr;
			Track.PhaseStartZ[FMath::Min(Index, MaxPhases - 1)] = Primitive ? static_cast<float>(Primitive->GetComponentLocation().Z) : 0.f;
		}
		if (Active.Phases[Index].Enter)
		{
			Active.Phases[Index].Enter(*this, Active);
		}
	};
	if (Active.Phase < 0)
	{
		EnterPhase(0);
	}
	const int32 P = FMath::Min(Active.Phase, MaxPhases - 1);
	const FPhase& Phase = Active.Phases[Active.Phase];
	const double Elapsed = Now - Active.PhaseStarted;

	// Follow every body.
	for (FTrack& Track : Active.Tracks)
	{
		const UFluidNinjaBuoyancyComponent* Body = Track.Body.Get();
		const UPrimitiveComponent* Primitive = Body ? Body->GetSimulatedPrimitive() : nullptr;
		if (!Primitive)
		{
			continue;
		}
		const FVector Location = Primitive->GetComponentLocation();
		const FVector Velocity = Primitive->IsSimulatingPhysics() ? Primitive->GetPhysicsLinearVelocity() : FVector::ZeroVector;
		if (Location.ContainsNaN() || Velocity.ContainsNaN() || Velocity.Size() > 3000.0)
		{
			Track.bBroke = true;
		}
		Track.MaxSpeed = FMath::Max(Track.MaxSpeed, static_cast<float>(Velocity.Size()));
		Track.MaxSlam = FMath::Max(Track.MaxSlam, Body->GetReadout().SlamWatts);
		const float RawYaw = static_cast<float>((Primitive->GetComponentQuat() * Body->RestAttitude.Quaternion().Inverse()).Rotator().Yaw);
		Track.Yaw += FMath::FindDeltaAngleDegrees(Track.LastRawYaw, RawYaw);
		Track.LastRawYaw = RawYaw;
		FFluidNinjaBuoyancyBodyDebug Debug;
		if (Hydro->GetBodyDebug(Body, Debug))
		{
			Track.bSawHeld |= Debug.bHeld;
			Track.bSawDormant |= Debug.bDormant;
			if (Active.bWindowOpen && Debug.bHasWaterZ && Debug.bInCapture)
			{
				Track.WaterSum[P] += Debug.WaterZ;
				++Track.WaterCount[P];
			}
		}
		Track.MaxDrop[P] = FMath::Max(Track.MaxDrop[P], Track.PhaseStartZ[P] - static_cast<float>(Location.Z));
		if (Active.bWindowOpen)
		{
			Track.MaxTilt[P] = FMath::Max(Track.MaxTilt[P], Tilt(Body));
			Track.MaxSink[P] = FMath::Max(Track.MaxSink[P], static_cast<float>(Track.WindowLocation.Z - Location.Z));
			Track.ZSum[P] += Location.Z;
			++Track.ZCount[P];
		}
	}
	if (!Active.bWindowOpen && Elapsed >= Phase.MeasureFrom)
	{
		Active.bWindowOpen = true;
		for (FTrack& Track : Active.Tracks)
		{
			const UPrimitiveComponent* Primitive = Track.Body.IsValid() ? Track.Body->GetSimulatedPrimitive() : nullptr;
			Track.WindowLocation = Primitive ? Primitive->GetComponentLocation() : FVector::ZeroVector;
			Track.WindowYaw = Track.Yaw;
			Track.WindowTime = Now;
		}
	}
	if (Elapsed < Phase.Seconds)
	{
		return;
	}
	// The phase is over: what each body did over its window.
	for (FTrack& Track : Active.Tracks)
	{
		const UPrimitiveComponent* Primitive = Track.Body.IsValid() ? Track.Body->GetSimulatedPrimitive() : nullptr;
		const double Span = Now - Track.WindowTime;
		if (Primitive && Track.WindowTime >= 0.0 && Span > 0.05)
		{
			Track.Velocity[P] = (Primitive->GetComponentLocation() - Track.WindowLocation) / Span;
			Track.YawRate[P] = static_cast<float>((Track.Yaw - Track.WindowYaw) / Span);
		}
	}
	float GameMs = 0.f;
	float PatchesMs = 0.f;
	float PreTickMs = 0.f;
	float PhysicsMs = 0.f;
	Hydro->GetCosts(GameMs, PatchesMs, PreTickMs, PhysicsMs);
	Active.Cost[P] = GameMs + PreTickMs;
	if (Phase.Exit)
	{
		Phase.Exit(*this, Active);
	}
	if (Active.Phase + 1 >= Active.Phases.Num())
	{
		FinishRun(false);
		return;
	}
	EnterPhase(Active.Phase + 1);
}

TUniquePtr<FFluidNinjaBuoyancyTestBench::FRun> FFluidNinjaBuoyancyTestBench::MakeRun(const FString& Name)
{
	TUniquePtr<FRun> Made = MakeUnique<FRun>();
	Made->Name = Name;
	// Controlled still water at the bodies' water, calm air, every body put back.
	auto Prepare = [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
	{
		R.bResetAfter = true;
		Bench.SetSceneryCollision(false);
		Bench.SetControlledWater(true);
		const float Zero = 0.f;
		Bench.SetTestWind(true, 0.f, &Zero);
		Bench.ResetBodies(true);
	};
	auto Settling = [Prepare](float Seconds) { return FPhase{TEXT("settling"), Seconds, Seconds, Prepare, nullptr}; };
	auto ForEach = [](FRun& R, TFunction<void(FTrack&, UFluidNinjaBuoyancyComponent&)> Action)
	{
		for (FTrack& Track : R.Tracks)
		{
			if (UFluidNinjaBuoyancyComponent* Body = Track.Body.Get())
			{
				Action(Track, *Body);
			}
		}
	};
	auto Speed2D = [](const FVector& Velocity) { return static_cast<float>(Velocity.Size2D()); };

	if (Name == TEXT("settle"))
	{
		Made->Phases.Add({TEXT("settling"), 12.f, 9.f, Prepare, nullptr});
		Made->Evaluate = [ForEach](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			ForEach(R, [&Bench, &R](FTrack& Track, UFluidNinjaBuoyancyComponent& Body)
			{
				float Measured = 0.f;
				const float Expected = Body.GetRestingDraft();
				const bool bDraft = Bench.Draft(&Body, Measured);
				const float Tolerance = FMath::Max(2.f, 0.1f * Expected);
				const float Drift = static_cast<float>(Track.Velocity[0].Size());
				const FString Text = FString::Printf(TEXT("draft %.1f cm (rest %.1f, within %.1f), drifting %.1f cm/s, tilt %.1f deg, peak speed %.0f cm/s"),
					Measured, Expected, Tolerance, Drift, Bench.Tilt(&Body), Track.MaxSpeed);
				const bool bPass = !Track.bBroke && bDraft && FMath::Abs(Measured - Expected) <= Tolerance && Drift < 3.f;
				Bench.Report(R.Name, Track.Name, bPass ? 1 : -1, Track.bBroke ? TEXT("flung or NaN; ") + Text : Text);
			});
		};
	}
	else if (Name == TEXT("capsize"))
	{
		Made->Phases.Add(Settling(6.f));
		Made->Phases.Add({TEXT("heeled 30 deg"), 12.f, 8.f, [ForEach](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			ForEach(R, [&Bench](FTrack& Track, UFluidNinjaBuoyancyComponent& Body)
			{
				// Only bodies stable both ways in their rest attitude (a round log rolls freely about its axis).
				Track.bEligible = Body.GetRollStability() > 1.f && Body.GetPitchStability() > 1.f;
				if (Track.bEligible)
				{
					const TWeakObjectPtr<UFluidNinjaBuoyancyComponent> Was = Bench.Selected;
					Bench.Selected = &Body;
					Bench.Capsize(30.f);
					Bench.Selected = Was;
				}
			});
		}, nullptr});
		Made->Evaluate = [ForEach](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			ForEach(R, [&Bench, &R](FTrack& Track, UFluidNinjaBuoyancyComponent& Body)
			{
				if (!Track.bEligible)
				{
					Bench.Report(R.Name, Track.Name, 0, FString::Printf(TEXT("not heeled: metacentric height roll %.0f cm, pitch %.0f cm (neutral or unstable)"),
						Body.GetRollStability(), Body.GetPitchStability()));
					return;
				}
				// Righted: in the last 4 s it swings less than half the heel (a ball buoy keeps rocking a while; a stuck body does not come back).
				const float TiltNow = Bench.Tilt(&Body);
				const bool bPass = !Track.bBroke && Track.MaxTilt[1] < 15.f;
				Bench.Report(R.Name, Track.Name, bPass ? 1 : -1, FString::Printf(TEXT("%s: tilt %.1f deg 12 s after a 30 deg heel, at most %.0f deg in the last 4 s"),
					bPass ? TEXT("righted") : TEXT("did not right"), TiltNow, Track.MaxTilt[1]));
			});
		};
	}
	else if (Name == TEXT("drop"))
	{
		Made->Phases.Add(Settling(6.f));
		Made->Phases.Add({TEXT("dropped from 3 m"), 10.f, 7.f, [ForEach](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			ForEach(R, [&Bench](FTrack& Track, UFluidNinjaBuoyancyComponent& Body)
			{
				Track.MaxSpeed = 0.f;
				Track.MaxSlam = 0.f;
				const TWeakObjectPtr<UFluidNinjaBuoyancyComponent> Was = Bench.Selected;
				Bench.Selected = &Body;
				Bench.Drop(3.f);
				Bench.Selected = Was;
			});
		}, nullptr});
		Made->Evaluate = [ForEach](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			const float Impact = FMath::Sqrt(2.f * 980.f * 300.f);
			ForEach(R, [&Bench, &R, Impact](FTrack& Track, UFluidNinjaBuoyancyComponent& Body)
			{
				float Measured = 0.f;
				const float Expected = Body.GetRestingDraft();
				const bool bDraft = Bench.Draft(&Body, Measured);
				const float Drift = static_cast<float>(Track.Velocity[1].Size());
				const bool bPass = !Track.bBroke && Track.MaxSpeed < Impact * 1.3f + 100.f && bDraft && FMath::Abs(Measured - Expected) <= FMath::Max(3.f, 0.25f * Expected) && Drift < 5.f;
				Bench.Report(R.Name, Track.Name, bPass ? 1 : -1, FString::Printf(TEXT("peak speed %.0f cm/s (free fall %.0f), slam %.0f W, then draft %.1f cm (rest %.1f), drifting %.1f cm/s"),
					Track.MaxSpeed, Impact, Track.MaxSlam, Measured, Expected, Drift));
			});
		};
	}
	else if (Name == TEXT("wind"))
	{
		auto Blow = [](float Speed)
		{
			return [Speed](FFluidNinjaBuoyancyTestBench& Bench, FRun& R) { Bench.SetTestWind(true, Speed, &R.WindDegrees); };
		};
		Made->Phases.Add(Settling(5.f));
		Made->Phases.Add({TEXT("wind 8 m/s"), 9.f, 3.f, Blow(8.f), nullptr});
		Made->Phases.Add(Settling(5.f));   // back to the start, so the stronger wind is not measured against a bank
		Made->Phases.Add({TEXT("wind 16 m/s"), 9.f, 3.f, Blow(16.f), [ForEach](FFluidNinjaBuoyancyTestBench&, FRun& R)
		{
			ForEach(R, [](FTrack& Track, UFluidNinjaBuoyancyComponent& Body) { Track.Velocity[5] = Body.GetReadout().WindForceN; });   // the wind's force at 16 m/s
		}});
		Made->Evaluate = [ForEach](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			const FVector Along = FRotator(0.f, R.WindDegrees, 0.f).Vector();
			ForEach(R, [&Bench, &R, &Along](FTrack& Track, UFluidNinjaBuoyancyComponent&)
			{
				const FVector Slow(Track.Velocity[1].X, Track.Velocity[1].Y, 0.0);
				const FVector Fast(Track.Velocity[3].X, Track.Velocity[3].Y, 0.0);
				const float AlongSlow = static_cast<float>(Slow | Along);
				const float AlongFast = static_cast<float>(Fast | Along);
				const float Cosine = Fast.Size() > 0.01 ? static_cast<float>((Fast.GetSafeNormal() | Along)) : 0.f;
				// A long hull slips more easily along its length than across it, so it drifts between the wind and its own axis (leeway):
				// up to about 40 degrees off for a lying barrel turned 55 degrees to the wind. Up to 45 passes; a wind force with the
				// wrong sign or frame goes further off than that.
				const bool bPass = !Track.bBroke && AlongSlow > 0.5f && AlongFast > AlongSlow * 1.1f && Cosine > 0.7f;
				Bench.Report(R.Name, Track.Name, bPass ? 1 : -1, FString::Printf(TEXT("drift at 8 m/s %.1f cm/s, at 16 m/s %.1f cm/s (toward %.0f deg, %.0f deg off the wind, which blows toward %.0f deg; turning %.1f deg/s); wind force %.0f N at 16 m/s"),
					AlongSlow, AlongFast, FMath::RadiansToDegrees(FMath::Atan2(Fast.Y, Fast.X)), FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Cosine, -1.f, 1.f))),
					R.WindDegrees, Track.YawRate[3], Track.Velocity[5].Size2D()));
			});
		};
	}
	else if (Name == TEXT("throttle"))
	{
		auto Drive = [ForEach](float Throttle, float Rudder)
		{
			return [ForEach, Throttle, Rudder](FFluidNinjaBuoyancyTestBench&, FRun& R)
			{
				ForEach(R, [Throttle, Rudder](FTrack& Track, UFluidNinjaBuoyancyComponent& Body)
				{
					Track.bEligible = Body.bPropulsion;
					if (Body.bPropulsion)
					{
						Body.Throttle = Throttle;
						Body.Rudder = Rudder;
					}
				});
			};
		};
		Made->Phases.Add(Settling(5.f));
		Made->Phases.Add({TEXT("full ahead"), 14.f, 8.f, Drive(1.f, 0.f), nullptr});
		Made->Phases.Add({TEXT("full ahead, full rudder"), 10.f, 5.f, Drive(1.f, 1.f), nullptr});
		Made->Evaluate = [ForEach, Speed2D](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			bool bAny = false;
			ForEach(R, [&Bench, &R, &bAny, Speed2D](FTrack& Track, UFluidNinjaBuoyancyComponent&)
			{
				if (!Track.bEligible)
				{
					return;
				}
				bAny = true;
				const float Speed = Speed2D(Track.Velocity[1]);
				const bool bPass = !Track.bBroke && Speed > 50.f && FMath::Abs(Track.YawRate[1]) < 2.f && FMath::Abs(Track.YawRate[2]) > 5.f;
				Bench.Report(R.Name, Track.Name, bPass ? 1 : -1, FString::Printf(TEXT("full ahead %.2f m/s, heading drifting %.1f deg/s; with full rudder turning %.1f deg/s at %.2f m/s"),
					Speed / 100.f, Track.YawRate[1], Track.YawRate[2], Speed2D(Track.Velocity[2]) / 100.f));
			});
			if (!bAny)
			{
				Bench.Report(R.Name, TEXT("-"), 0, TEXT("no body with propulsion"));
			}
		};
	}
	else if (Name == TEXT("gather"))
	{
		auto Mode = [](int32 Gather, bool bVerify)
		{
			return [Gather, bVerify](FFluidNinjaBuoyancyTestBench& Bench, FRun&)
			{
				Bench.SetControlledWater(false);
				FluidNinjaBuoyancyCVars::Gather = Gather;
				if (UFluidNinjaBuoyancySubsystem* H = Bench.GetHydro())
				{
					H->SetVerifyGather(bVerify);
				}
			};
		};
		Made->Phases.Add({TEXT("gather and copies, same frames"), 6.f, 6.f, Mode(1, true), [](FFluidNinjaBuoyancyTestBench& Bench, FRun&)
		{
			UFluidNinjaBuoyancySubsystem* H = Bench.GetHydro();
			if (!H)
			{
				return;
			}
			for (UFluidNinjaBuoyancyComponent* Body : H->GetBodies())
			{
				FFluidNinjaBuoyancyBodyDebug Debug;
				if (!H->GetBodyDebug(Body, Debug))
				{
					continue;
				}
				const FString Who = Bench.BodyName(Body);
				if (Debug.VerifyCount == 0)
				{
					Bench.Report(TEXT("gather"), Who, 0, TEXT("nothing compared (out of the capture or asleep)"));
					continue;
				}
				const float FarShare = 100.f * Debug.VerifyFar / Debug.VerifyCount;
				const bool bPass = Debug.VerifyMeanCm < 0.5f && FarShare < 1.f && Debug.VerifyFlowMeanCmS < 5.f;
				Bench.Report(TEXT("gather"), Who, bPass ? 1 : -1, FString::Printf(TEXT("gather vs copies over %d points: height %.2f cm mean, %.1f cm at most, %.2f%% over 2 cm; current %.1f cm/s mean (%d points only the gather has)"),
					Debug.VerifyCount, Debug.VerifyMeanCm, Debug.VerifyMaxCm, FarShare, Debug.VerifyFlowMeanCmS, Debug.VerifyFlowOnlyGathered));
			}
			H->SetVerifyGather(false);
		}});
		Made->Phases.Add({TEXT("GPU gather only"), 4.f, 4.f, Mode(1, false), nullptr});
		Made->Phases.Add({TEXT("per-body copies only"), 4.f, 4.f, Mode(0, false), nullptr});
		Made->Evaluate = [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			Bench.Report(R.Name, TEXT("-"), 0, FString::Printf(TEXT("game thread %.2f ms with the gather, %.2f ms with copies (GPU: ProfileGPU, pass FluidNinjaBuoyancyGather)"), R.Cost[1], R.Cost[2]));
		};
	}
	else if (Name == TEXT("walkaway"))
	{
		Made->Phases.Add({TEXT("player here"), 6.f, 1.f, [](FFluidNinjaBuoyancyTestBench& Bench, FRun&) { Bench.SetControlledWater(false); }, nullptr});
		Made->Phases.Add({TEXT("player 70 m away"), 14.f, 9.f, [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			Bench.SetControlledWater(false);
			UWorld* World = Bench.GetWorld();
			APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
			APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
			if (!Pawn)
			{
				return;
			}
			// Away from the bodies, looking away, on the ground there.
			FVector Centre = FVector::ZeroVector;
			int32 Count = 0;
			for (const FTrack& Track : R.Tracks)
			{
				if (const UPrimitiveComponent* Primitive = Track.Body.IsValid() ? Track.Body->GetSimulatedPrimitive() : nullptr)
				{
					Centre += Primitive->GetComponentLocation();
					++Count;
				}
			}
			const FVector From = Pawn->GetActorLocation();
			FVector Away = Count > 0 ? From - Centre / Count : FVector::XAxisVector;
			Away.Z = 0.0;
			Away = Away.GetSafeNormal(UE_SMALL_NUMBER, FVector::XAxisVector);
			FVector Target = From + Away * 7000.0;
			FHitResult Hit;
			FCollisionQueryParams Params(TEXT("FluidNinjaBuoyancyWalkAway"), false, Pawn);
			if (!World->LineTraceSingleByChannel(Hit, Target + FVector(0.0, 0.0, 6000.0), Target - FVector(0.0, 0.0, 20000.0), ECC_Visibility, Params))
			{
				// No ground out there (a small test level): stay, rather than drop the player into nothing.
				Bench.Report(R.Name, TEXT("-"), 0, TEXT("no ground 70 m away: the player stays (nothing measured)"));
				return;
			}
			Target.Z = Hit.ImpactPoint.Z + 150.0;
			R.bPawnAway = true;
			R.bPawnMoved = true;
			R.PawnReturn = Pawn->GetActorTransform();
			R.ControlReturn = Controller->GetControlRotation();
			Pawn->TeleportTo(Target, Away.Rotation());
			Controller->SetControlRotation(Away.Rotation());
		}, nullptr});
		Made->Phases.Add({TEXT("player back"), 8.f, 5.f, [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			UWorld* World = Bench.GetWorld();
			APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
			if (APawn* Pawn = Controller ? Controller->GetPawn() : nullptr; Pawn && R.bPawnAway)
			{
				Pawn->TeleportTo(R.PawnReturn.GetLocation(), R.PawnReturn.Rotator());
				Controller->SetControlRotation(R.ControlReturn);
				R.bPawnAway = false;
			}
		}, nullptr});
		Made->Evaluate = [ForEach](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			if (!R.bPawnMoved)
			{
				return;   // nowhere to walk to
			}
			ForEach(R, [&Bench, &R](FTrack& Track, UFluidNinjaBuoyancyComponent& Body)
			{
				// Afloat: touching the water now or within the last second (a light float bobbing on live waves leaves it for a moment).
				const UWorld* World = Bench.GetWorld();
				const bool bAfloat = Body.GetReadout().bInWater || (World && Track.LastInWater >= 0.0 && World->GetTimeSeconds() - Track.LastInWater < 1.0);
				// Where it floats once settled out there (the far field is flat: no waves), against where it floated on average before.
				const float Settled = Track.ZCount[0] > 0 && Track.ZCount[1] > 0 ? static_cast<float>(Track.ZSum[0] / Track.ZCount[0] - Track.ZSum[1] / Track.ZCount[1]) : 0.f;
				const bool bPass = !Track.bBroke && FMath::Abs(Settled) < 15.f && (bAfloat || !Track.bWasInWater);
				FString Away = Track.bSawHeld ? FString(TEXT("held")) : FString();
				if (Track.bSawDormant)
				{
					Away += Away.IsEmpty() ? TEXT("asleep") : TEXT(", asleep");
				}
				Bench.Report(R.Name, Track.Name, bPass ? 1 : -1, FString::Printf(TEXT("floated %.0f cm lower (mean) once settled while the player was away (%.0f cm below the start at the deepest; %s), %s after"),
					Settled, Track.MaxDrop[1], Away.IsEmpty() ? TEXT("awake") : *Away, bAfloat ? TEXT("afloat") : TEXT("not in the water")));
			});
		};
	}
	else
	{
		Report(Name, TEXT("-"), 0, TEXT("no such test (settle, capsize, drop, wind, throttle, gather, walkaway, all)"));
		return nullptr;
	}
	return Made;
}

/* ---- Every frame ---- */

void FFluidNinjaBuoyancyTestBench::Tick(float DeltaSeconds)
{
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (!Hydro)
	{
		return;
	}
	AttachPanel();
	if (!Selected.IsValid())
	{
		Select(0);
	}
	if (Current.IsValid())
	{
		// The tracks are made when the run starts; bodies known before it (walkaway reads who was afloat).
		if (Current->Phase < 0 && Current->Name == TEXT("walkaway"))
		{
			for (FTrack& Track : Current->Tracks)
			{
				Track.bWasInWater = Track.Body.IsValid() && Track.Body->GetReadout().bInWater;
			}
		}
		if (const UWorld* World = GetWorld())
		{
			for (FTrack& Track : Current->Tracks)
			{
				if (Track.Body.IsValid() && Track.Body->GetReadout().bInWater)
				{
					Track.LastInWater = World->GetTimeSeconds();
				}
			}
		}
		TickRun();
	}
	if (Widget.IsValid() && bVisible && bLabels && CVarTestHUD.GetValueOnGameThread() > 0)
	{
		DrawLabels();
	}
}

void FFluidNinjaBuoyancyTestBench::DrawLabels() const
{
	UWorld* World = GetWorld();
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (!World || !Hydro)
	{
		return;
	}
	for (const UFluidNinjaBuoyancyComponent* Body : Hydro->GetBodies())
	{
		const UPrimitiveComponent* Primitive = Body->GetSimulatedPrimitive();
		if (!Primitive)
		{
			continue;
		}
		const FBoxSphereBounds Bounds = Primitive->Bounds;
		const bool bSelected = Body == Selected.Get();
		FFluidNinjaBuoyancyBodyDebug Debug;
		Hydro->GetBodyDebug(Body, Debug);
		float DraftNow = 0.f;
		const FString State = Debug.bHeld ? TEXT("held") : Debug.bDormant ? TEXT("asleep") : Body->IsAwaitingWater() ? TEXT("settling") : Body->GetReadout().bInWater ? TEXT("afloat") : TEXT("dry");
		const FString Label = FString::Printf(TEXT("%s%s\n%s  %s  tilt %.0f"), bSelected ? TEXT("> ") : TEXT(""), *BodyName(Body), *State,
			Draft(Body, DraftNow) ? *FString::Printf(TEXT("draft %.0f/%.0f cm"), DraftNow, Body->GetRestingDraft()) : TEXT("no water"), Tilt(Body));
		DrawDebugString(World, Bounds.Origin + FVector(0.0, 0.0, Bounds.BoxExtent.Z + 60.0), Label, nullptr,
			bSelected ? FColor(255, 220, 90) : FColor(170, 220, 255), 0.f, true, bSelected ? 1.1f : 0.9f);
		if (bSelected)
		{
			DrawDebugBox(World, Bounds.Origin, Bounds.BoxExtent + FVector(5.0), FColor(255, 220, 90), false, -1.f, 0, 2.f);
		}
	}
}

/* ---- Panel ---- */

FString FFluidNinjaBuoyancyTestBench::BodyLine(const UFluidNinjaBuoyancyComponent* Body, bool bIsSelected) const
{
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	const UPrimitiveComponent* Primitive = Body ? Body->GetSimulatedPrimitive() : nullptr;
	if (!Hydro || !Primitive)
	{
		return FString();
	}
	FFluidNinjaBuoyancyBodyDebug Debug;
	Hydro->GetBodyDebug(Body, Debug);
	const FFluidNinjaBuoyancyReadout Read = Body->GetReadout();
	const FString State = Debug.bHeld ? TEXT("held") : Debug.bDormant ? TEXT("asleep") : Body->IsAwaitingWater() ? TEXT("settling") : Read.bInWater ? TEXT("afloat") : TEXT("dry");
	float DraftNow = 0.f;
	const FString DraftText = Draft(Body, DraftNow) ? FString::Printf(TEXT("%5.1f/%-5.1f"), DraftNow, Body->GetRestingDraft()) : FString(TEXT("  -  /  -  "));
	const float Speed = Primitive->IsSimulatingPhysics() ? static_cast<float>(Primitive->GetPhysicsLinearVelocity().Size2D()) : 0.f;
	return FString::Printf(TEXT("%s %-18s %s L%d %-8s %s %5.1f %5.2f %5.2f %5.0f"), bIsSelected ? TEXT(">") : TEXT(" "), *BodyName(Body).Left(18), ModelName(Body->HullModel),
		Debug.Level, *State, *DraftText, Tilt(Body), Speed / 100.f, Read.FlowCmS.Size() / 100.f, Read.WindForceN.Size2D());
}

FText FFluidNinjaBuoyancyTestBench::Readout() const
{
	const double Now = FPlatformTime::Seconds();
	if (Now - CachedAt < 0.1)
	{
		return CachedText;
	}
	CachedAt = Now;
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (!Hydro)
	{
		CachedText = FText::GetEmpty();
		return CachedText;
	}
	FString Text = TEXT("FLUIDNINJA BUOYANCY TEST BENCH\n");
	if (Hydro->IsWaterControlled())
	{
		Text += FString::Printf(TEXT("Water    CONTROLLED: flat at %.0f cm, still (no current, waves or wakes)%s\n"), Hydro->GetControlledWaterHeight(),
			IsSceneryCollision() ? TEXT("") : TEXT(", free water (scenery ignored)"));
	}
	else
	{
		Text += FString::Printf(TEXT("Water    live: %d of %d surface pieces drawn, %s\n"), Hydro->GetCapturedPrimitives(), Hydro->GetSurfacePrimitives(),
			Hydro->IsGathering() ? *FString::Printf(TEXT("GPU gather %d bodies, %d points"), Hydro->GetGatherBodies(), Hydro->GetGatherPoints()) : TEXT("per-body copies"));
	}
	FString Calibration = Hydro->GetCalibrationReport();
	if (Calibration.Len() > 150)
	{
		Calibration = Calibration.Left(147) + TEXT("...");
	}
	Text += FString::Printf(TEXT("Current  %.0f cm/s per buffer unit (multiplier %.2f)%s%s\n"), Hydro->GetFlowScale(), UFluidNinjaBuoyancySettings::Get().FlowScaleMultiplier,
		Calibration.IsEmpty() ? TEXT("") : TEXT("\n         "), *Calibration);
	const FFluidNinjaBuoyancyWind& Wind = Hydro->GetWind();
	Text += FString::Printf(TEXT("Wind     %.1f m/s toward %.0f deg, %s\n"), Wind.SpeedMs, Wind.DirectionDegrees,
		Wind.bOverride ? TEXT("TEST WIND") : TEXT("project setting"));
	float GameMs = 0.f;
	float PatchesMs = 0.f;
	float PreTickMs = 0.f;
	float PhysicsMs = 0.f;
	Hydro->GetCosts(GameMs, PatchesMs, PreTickMs, PhysicsMs);
	Text += FString::Printf(TEXT("Cost     game %.2f ms (patches %.2f), pre-tick %.2f, physics %.2f ms/frame, wakes %d, debug view %s\n"), GameMs, PatchesMs, PreTickMs, PhysicsMs,
		Hydro->GetWakePoints(), FluidNinjaBuoyancyCVars::Enable >= 2 ? TEXT("on") : TEXT("off"));
	Text += TEXT("\n  body               mdl lod state    draft/rest  tilt  m/s  water windN\n");
	for (const UFluidNinjaBuoyancyComponent* Body : Hydro->GetBodies())
	{
		Text += BodyLine(Body, Body == Selected.Get()) + TEXT("\n");
	}
	if (const UFluidNinjaBuoyancyComponent* Body = Selected.Get())
	{
		FFluidNinjaBuoyancyBodyDebug Debug;
		Hydro->GetBodyDebug(Body, Debug);
		const FFluidNinjaBuoyancyReadout& R = Body->GetReadout();
		Text += FString::Printf(TEXT("\n%s: %.0f kg, %.0f L under water, %.2f m2 wet, force (%.0f, %.0f, %.0f) N, wind (%.0f, %.0f) N, slam %.0f W\n"), *BodyName(Body), R.MassKg,
			R.DisplacedLitres, R.WettedAreaM2, R.ForceN.X, R.ForceN.Y, R.ForceN.Z, R.WindForceN.X, R.WindForceN.Y, R.SlamWatts);
		Text += FString::Printf(TEXT("  water %s, %d points %.1f cm apart, %.0f ms old, far field %s; stability roll %.0f cm, pitch %.0f cm%s\n"),
			Debug.bInCapture ? TEXT("read") : Debug.bFlat ? TEXT("water volume (flat)") : TEXT("unknown"), Debug.PatchPoints, Debug.PatchSpacing, Debug.PatchAge * 1000.f,
			Debug.bFarFieldLearned ? *FString::Printf(TEXT("%+.0f cm"), Debug.FarFieldCorrection) : TEXT("not learned"),
			Body->GetRollStability(), Body->GetPitchStability(),
			Body->bPropulsion ? *FString::Printf(TEXT("; throttle %.1f rudder %.1f"), Body->Throttle, Body->Rudder) : TEXT(""));
	}
	if (const FRun* Running = Current.Get(); Running && Running->Phase >= 0)
	{
		const UWorld* World = GetWorld();
		const double Left = World ? Running->Phases[Running->Phase].Seconds - (World->GetTimeSeconds() - Running->PhaseStarted) : 0.0;
		Text += FString::Printf(TEXT("\nRUNNING  %s: %s (%d/%d), %.0f s left%s\n"), *Running->Name, *Running->Phases[Running->Phase].Label, Running->Phase + 1, Running->Phases.Num(),
			FMath::Max(0.0, Left), Queue.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", then %s"), *FString::Join(Queue, TEXT(", "))));
	}
	if (!Results.IsEmpty())
	{
		Text += TEXT("\nResults (newest first)\n");
		for (int32 Index = Results.Num() - 1, Shown = 0; Index >= 0 && Shown < 12; --Index, ++Shown)
		{
			const FResult& Result = Results[Index];
			FString Line = FString::Printf(TEXT("%s %-8s %-16s %s"), Result.Verdict > 0 ? TEXT("PASS") : Result.Verdict < 0 ? TEXT("FAIL") : TEXT("info"), *Result.Test,
				*Result.Body.Left(16), *Result.Text);
			Text += (Line.Len() > 150 ? Line.Left(147) + TEXT("...") : Line) + TEXT("\n");
		}
	}
	Text.RemoveFromEnd(TEXT("\n"));
	CachedText = FText::FromString(Text);
	return CachedText;
}

TSharedRef<SWidget> FFluidNinjaBuoyancyTestBench::Build()
{
	const TWeakPtr<FFluidNinjaBuoyancyTestBench> Weak = AsShared();
	const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Regular", 9);
	const FSlateFontInfo Mono = FCoreStyle::GetDefaultFontStyle("Mono", 9);

	auto Button = [Weak, Font](const FString& Label, TFunction<void(FFluidNinjaBuoyancyTestBench&)> Action, TFunction<bool(const FFluidNinjaBuoyancyTestBench&)> IsActive = nullptr,
		bool bKeepMenuOpen = false) -> TSharedRef<SWidget>
	{
		return SNew(SBox).Padding(FMargin(0.f, 0.f, 3.f, 3.f))
		[
			SNew(SButton)
			.ContentPadding(FMargin(6.f, 1.f))
			.ButtonColorAndOpacity_Lambda([Weak, IsActive]()
			{
				const TSharedPtr<FFluidNinjaBuoyancyTestBench> Pinned = Weak.Pin();
				return FSlateColor(Pinned && IsActive && IsActive(*Pinned) ? FLinearColor(0.15f, 0.5f, 0.95f) : FLinearColor(0.22f, 0.22f, 0.22f));
			})
			.OnClicked_Lambda([Weak, Action, bKeepMenuOpen]()
			{
				if (const TSharedPtr<FFluidNinjaBuoyancyTestBench> Pinned = Weak.Pin())
				{
					Action(*Pinned);
				}
				if (!bKeepMenuOpen && FSlateApplication::IsInitialized())
				{
					FSlateApplication::Get().DismissAllMenus();
				}
				return FReply::Handled();
			})
			[
				SNew(STextBlock).Text(FText::FromString(Label)).Font(Font).ColorAndOpacity(FLinearColor::White)
			]
		];
	};
	auto Dropdown = [Weak, Font](const FString& Label, TFunction<FString(const FFluidNinjaBuoyancyTestBench&)> Value, const TArray<TSharedRef<SWidget>>& Items) -> TSharedRef<SWidget>
	{
		TSharedRef<SWrapBox> Menu = SNew(SWrapBox).PreferredSize(420.f);
		for (const TSharedRef<SWidget>& Item : Items)
		{
			Menu->AddSlot()[Item];
		}
		return SNew(SBox).Padding(FMargin(0.f, 0.f, 4.f, 4.f))
		[
			SNew(SComboButton)
			.ButtonColorAndOpacity(FLinearColor(0.22f, 0.22f, 0.22f))
			.ForegroundColor(FLinearColor::White)
			.ContentPadding(FMargin(6.f, 1.f))
			.ButtonContent()
			[
				SNew(STextBlock).Font(Font).ColorAndOpacity(FLinearColor::White)
				.Text_Lambda([Weak, Label, Value]()
				{
					const TSharedPtr<FFluidNinjaBuoyancyTestBench> Pinned = Weak.Pin();
					return FText::FromString(Pinned && Value ? Label + TEXT(": ") + Value(*Pinned) : Label);
				})
			]
			.MenuContent()
			[
				SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
				.BorderBackgroundColor(FLinearColor(0.02f, 0.02f, 0.02f, 0.92f))
				.Padding(FMargin(6.f, 6.f, 3.f, 3.f))
				[
					Menu
				]
			]
		];
	};
	auto Hydro = [](const FFluidNinjaBuoyancyTestBench& Bench) { return Bench.Subsystem.Get(); };
	auto TestWind = [](float Speed)
	{
		return [Speed](FFluidNinjaBuoyancyTestBench& Bench) { Bench.SetTestWind(true, Speed); };
	};
	auto TestWindIs = [Hydro](float Speed)
	{
		return [Hydro, Speed](const FFluidNinjaBuoyancyTestBench& Bench) { const UFluidNinjaBuoyancySubsystem* H = Hydro(Bench); return H && H->GetWind().bOverride && FMath::IsNearlyEqual(H->GetWind().SpeedMs, Speed, 0.05f); };
	};
	auto Test = [](const TCHAR* Name) { return [Name = FString(Name)](FFluidNinjaBuoyancyTestBench& Bench) { Bench.Run(Name); }; };

	TArray<TSharedRef<SWidget>> WaterButtons = {
		Button(TEXT("Live water"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.SetControlledWater(false); }, [Hydro](const FFluidNinjaBuoyancyTestBench& Bench) { const UFluidNinjaBuoyancySubsystem* H = Hydro(Bench); return H && !H->IsWaterControlled(); }),
		Button(TEXT("Controlled (flat, still)"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.SetControlledWater(true); }, [Hydro](const FFluidNinjaBuoyancyTestBench& Bench) { const UFluidNinjaBuoyancySubsystem* H = Hydro(Bench); return H && H->IsWaterControlled(); }),
		Button(TEXT("Calibrate current (at the body)"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.Command({TEXT("calibrate")}); }),
		Button(TEXT("Gather on/off"), [](FFluidNinjaBuoyancyTestBench&) { FluidNinjaBuoyancyCVars::Gather = FluidNinjaBuoyancyCVars::Gather ? 0 : 1; }, [](const FFluidNinjaBuoyancyTestBench&) { return FluidNinjaBuoyancyCVars::Gather != 0; }, true),
		Button(TEXT("Wakes on/off"), [](FFluidNinjaBuoyancyTestBench&) { FluidNinjaBuoyancyCVars::Wake = FluidNinjaBuoyancyCVars::Wake ? 0 : 1; }, [](const FFluidNinjaBuoyancyTestBench&) { return FluidNinjaBuoyancyCVars::Wake != 0; }, true),
		Button(TEXT("Debug view"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.ToggleDebugView(); }, [](const FFluidNinjaBuoyancyTestBench&) { return FluidNinjaBuoyancyCVars::Enable >= 2; }, true),
		Button(TEXT("Free water (ignore scenery)"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.SetSceneryCollision(!Bench.IsSceneryCollision()); },
			[](const FFluidNinjaBuoyancyTestBench& Bench) { return !Bench.IsSceneryCollision(); }, true) };
	TArray<TSharedRef<SWidget>> WindButtons = {
		Button(TEXT("Project wind"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.SetTestWind(false); }, [Hydro](const FFluidNinjaBuoyancyTestBench& Bench) { const UFluidNinjaBuoyancySubsystem* H = Hydro(Bench); return H && !H->GetWind().bOverride; }),
		Button(TEXT("Calm"), TestWind(0.f), TestWindIs(0.f)),
		Button(TEXT("5 m/s"), TestWind(5.f), TestWindIs(5.f)),
		Button(TEXT("10 m/s"), TestWind(10.f), TestWindIs(10.f)),
		Button(TEXT("20 m/s"), TestWind(20.f), TestWindIs(20.f)),
		Button(TEXT("30 m/s"), TestWind(30.f), TestWindIs(30.f)) };
	TArray<TSharedRef<SWidget>> BodyButtons = {
		Button(TEXT("< Previous"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.Select(-1); }, nullptr, true),
		Button(TEXT("Next >"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.Select(1); }, nullptr, true),
		Button(TEXT("Reset all"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.ResetBodies(true); }),
		Button(TEXT("Reset this"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.ResetBodies(false); }),
		Button(TEXT("Bring here"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.BringHere(); }),
		Button(TEXT("Push 300 N.s"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.Push(300.f); }, nullptr, true),
		Button(TEXT("Heel 30"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.Capsize(30.f); }, nullptr, true),
		Button(TEXT("Capsize 120"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.Capsize(120.f); }, nullptr, true),
		Button(TEXT("Drop 3 m"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.Drop(3.f); }, nullptr, true),
		Button(TEXT("Labels"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.bLabels = !Bench.bLabels; }, [](const FFluidNinjaBuoyancyTestBench& Bench) { return Bench.bLabels; }, true) };
	TArray<TSharedRef<SWidget>> BoatButtons = {
		Button(TEXT("Astern"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.SetBoat(-0.5f, 0.f); }),
		Button(TEXT("Stop"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.SetBoat(0.f, 0.f); }),
		Button(TEXT("Half ahead"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.SetBoat(0.5f, 0.f); }),
		Button(TEXT("Full ahead"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.SetBoat(1.f, 0.f); }),
		Button(TEXT("Port"), [](FFluidNinjaBuoyancyTestBench& Bench) { if (const UFluidNinjaBuoyancyComponent* B = Bench.GetSelected()) { Bench.SetBoat(B->Throttle, -1.f); } }, nullptr, true),
		Button(TEXT("Rudder centre"), [](FFluidNinjaBuoyancyTestBench& Bench) { if (const UFluidNinjaBuoyancyComponent* B = Bench.GetSelected()) { Bench.SetBoat(B->Throttle, 0.f); } }, nullptr, true),
		Button(TEXT("Starboard"), [](FFluidNinjaBuoyancyTestBench& Bench) { if (const UFluidNinjaBuoyancyComponent* B = Bench.GetSelected()) { Bench.SetBoat(B->Throttle, 1.f); } }, nullptr, true) };
	TArray<TSharedRef<SWidget>> TestButtons = {
		Button(TEXT("Settle"), Test(TEXT("settle"))), Button(TEXT("Capsize"), Test(TEXT("capsize"))), Button(TEXT("Drop"), Test(TEXT("drop"))),
		Button(TEXT("Wind"), Test(TEXT("wind"))), Button(TEXT("Throttle"), Test(TEXT("throttle"))), Button(TEXT("Gather A/B"), Test(TEXT("gather"))),
		Button(TEXT("Walk away"), Test(TEXT("walkaway"))), Button(TEXT("Run all"), Test(TEXT("all"))),
		Button(TEXT("Stop"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.Stop(); }),
		Button(TEXT("Clear results"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.Results.Reset(); Bench.CachedAt = -1.0; }) };

	TSharedRef<SWrapBox> Controls = SNew(SWrapBox).UseAllottedSize(true);
	auto AddControl = [&Controls](const TSharedRef<SWidget>& Item) { Controls->AddSlot()[Item]; };
	AddControl(Dropdown(TEXT("Water"), [Hydro](const FFluidNinjaBuoyancyTestBench& Bench) { const UFluidNinjaBuoyancySubsystem* H = Hydro(Bench); return FString(H && H->IsWaterControlled() ? TEXT("controlled") : TEXT("live")); }, WaterButtons));
	AddControl(Dropdown(TEXT("Wind"), [Hydro](const FFluidNinjaBuoyancyTestBench& Bench)
	{
		const UFluidNinjaBuoyancySubsystem* H = Hydro(Bench);
		return H ? FString::Printf(TEXT("%s %.0f m/s"), H->GetWind().bOverride ? TEXT("test") : TEXT("project"), H->GetWind().SpeedMs) : FString();
	}, WindButtons));
	AddControl(Dropdown(TEXT("Body"), [](const FFluidNinjaBuoyancyTestBench& Bench) { return Bench.BodyName(Bench.GetSelected()); }, BodyButtons));
	AddControl(Dropdown(TEXT("Boat"), [](const FFluidNinjaBuoyancyTestBench& Bench)
	{
		const UFluidNinjaBuoyancyComponent* B = Bench.GetSelected();
		return B && B->bPropulsion ? FString::Printf(TEXT("%.1f / %.1f"), B->Throttle, B->Rudder) : FString(TEXT("-"));
	}, BoatButtons));
	AddControl(Dropdown(TEXT("Tests"), [](const FFluidNinjaBuoyancyTestBench& Bench) { return Bench.Current.IsValid() ? Bench.Current->Name : FString(TEXT("idle")); }, TestButtons));

	return SNew(SBox)
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Top)
		.Padding(FMargin(14.f, 14.f, 14.f, 14.f))
		.Visibility(EVisibility::SelfHitTestInvisible)
		[
			SNew(SBorder)
			.Visibility_Lambda([Weak]()
			{
				const TSharedPtr<FFluidNinjaBuoyancyTestBench> Pinned = Weak.Pin();
				return Pinned && Pinned->bVisible && CVarTestHUD.GetValueOnGameThread() > 0 ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FLinearColor(0.f, 0.02f, 0.05f, 0.66f))
			.Padding(FMargin(10.f, 8.f))
			[
				SNew(SBox).MaxDesiredWidth(780.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Text_Lambda([Weak]() { const TSharedPtr<FFluidNinjaBuoyancyTestBench> Pinned = Weak.Pin(); return Pinned ? Pinned->Readout() : FText::GetEmpty(); })
						.Font(Mono)
						.ColorAndOpacity(FLinearColor::White)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 6.f, 0.f, 0.f))
					[
						Controls
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 2.f, 0.f, 0.f))
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Shift+F1 frees the mouse. Ctrl+Shift: B hide, R reset, PgUp/PgDn select, T run all, W wind, C water, D debug, K heel, J drop. Console: fnb.Test help")))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
						.ColorAndOpacity(FLinearColor(0.65f, 0.65f, 0.65f))
						.AutoWrapText(true)
					]
				]
			]
		];
}

/* ---- Console ---- */

FString FFluidNinjaBuoyancyTestBench::Command(const TArray<FString>& Args)
{
	const FString Verb = Args.IsEmpty() ? FString(TEXT("help")) : Args[0].ToLower();
	auto Arg = [&Args](int32 Index) { return Args.IsValidIndex(Index) ? Args[Index] : FString(); };
	auto Number = [&Args](int32 Index, float Default) { return Args.IsValidIndex(Index) && Args[Index].IsNumeric() ? FCString::Atof(*Args[Index]) : Default; };
	UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (Verb == TEXT("run"))
	{
		Run(Arg(1).IsEmpty() ? FString(TEXT("all")) : Arg(1));
		return Current.IsValid() ? FString::Printf(TEXT("running %s"), *Current->Name) : FString(TEXT("nothing to run"));
	}
	if (Verb == TEXT("stop"))
	{
		Stop();
		return TEXT("stopped");
	}
	if (Verb == TEXT("reset"))
	{
		const bool bAll = !Arg(1).Equals(TEXT("selected"), ESearchCase::IgnoreCase);
		ResetBodies(bAll);
		return bAll ? TEXT("every body put back") : TEXT("selected body put back");
	}
	if (Verb == TEXT("select"))
	{
		const FString Which = Arg(1);
		if (Which.IsEmpty() || Which.Equals(TEXT("next"), ESearchCase::IgnoreCase)) { Select(1); }
		else if (Which.Equals(TEXT("prev"), ESearchCase::IgnoreCase)) { Select(-1); }
		else if (!SelectByName(Which)) { return FString::Printf(TEXT("no body named like %s"), *Which); }
		return FString::Printf(TEXT("selected %s"), *BodyName(GetSelected()));
	}
	if (Verb == TEXT("water"))
	{
		const FString Mode = Arg(1).ToLower();
		const bool bOn = Mode == TEXT("toggle") ? !(Hydro && Hydro->IsWaterControlled()) : Mode == TEXT("controlled") || Mode == TEXT("flat");
		if (bOn && Args.IsValidIndex(2) && Args[2].IsNumeric())
		{
			const float Height = FCString::Atof(*Args[2]);
			SetControlledWater(true, &Height);
		}
		else
		{
			SetControlledWater(bOn);
		}
		return Hydro && Hydro->IsWaterControlled() ? FString::Printf(TEXT("controlled water at %.0f"), Hydro->GetControlledWaterHeight()) : FString(TEXT("live water"));
	}
	if (Verb == TEXT("wind"))
	{
		const FString Mode = Arg(1).ToLower();
		if (Mode.IsEmpty() || Mode == TEXT("project"))
		{
			SetTestWind(false);
		}
		else
		{
			const float Speed = Mode == TEXT("calm") ? 0.f : Number(1, 0.f);
			if (Args.IsValidIndex(2) && Args[2].IsNumeric())
			{
				const float Degrees = FCString::Atof(*Args[2]);
				SetTestWind(true, Speed, &Degrees);
			}
			else
			{
				SetTestWind(true, Speed);
			}
		}
		return Hydro ? FString::Printf(TEXT("wind %.1f m/s toward %.0f deg (%s)"), Hydro->GetWind().SpeedMs, Hydro->GetWind().DirectionDegrees, Hydro->GetWind().bOverride ? TEXT("test") : TEXT("project")) : FString();
	}
	if (Verb == TEXT("push")) { Push(Number(1, 300.f)); return TEXT("pushed"); }
	if (Verb == TEXT("capsize") || Verb == TEXT("heel")) { Capsize(Number(1, 30.f)); return TEXT("heeled"); }
	if (Verb == TEXT("drop")) { Drop(Number(1, 3.f)); return TEXT("dropped"); }
	if (Verb == TEXT("here")) { BringHere(); return TEXT("brought in front of the view"); }
	if (Verb == TEXT("boat")) { SetBoat(Number(1, 0.f), Number(2, 0.f)); return TEXT("throttle and rudder set"); }
	if (Verb == TEXT("debug")) { ToggleDebugView(); return FluidNinjaBuoyancyCVars::Enable >= 2 ? TEXT("debug view on") : TEXT("debug view off"); }
	if (Verb == TEXT("scenery")) { SetSceneryCollision(!Arg(1).Equals(TEXT("off"), ESearchCase::IgnoreCase)); return IsSceneryCollision() ? TEXT("bodies meet the scenery") : TEXT("free water: bodies ignore the scenery"); }
	if (Verb == TEXT("calibrate"))
	{
		// fnb.Test calibrate [apply] [view]: at the selected body (in its water), or around the view.
		const bool bApply = Args.ContainsByPredicate([](const FString& A) { return A.Equals(TEXT("apply"), ESearchCase::IgnoreCase); });
		const bool bView = Args.ContainsByPredicate([](const FString& A) { return A.Equals(TEXT("view"), ESearchCase::IgnoreCase); });
		const UPrimitiveComponent* Primitive = GetSelected() ? GetSelected()->GetSimulatedPrimitive() : nullptr;
		if (Hydro && Primitive && !bView)
		{
			Hydro->CalibrateFlowAt(Primitive->Bounds.Origin, bApply);
			return FString::Printf(TEXT("calibrating at %s"), *BodyName(GetSelected()));
		}
		if (Hydro)
		{
			Hydro->CalibrateFlow(bApply);
		}
		return TEXT("calibrating around the view");
	}
	if (Verb == TEXT("hud")) { bVisible = !bVisible; return bVisible ? TEXT("panel shown") : TEXT("panel hidden"); }
	if (Verb == TEXT("results"))
	{
		FString Out;
		for (const FResult& Result : Results)
		{
			Out += FString::Printf(TEXT("%s %s %s: %s\n"), Result.Verdict > 0 ? TEXT("PASS") : Result.Verdict < 0 ? TEXT("FAIL") : TEXT("INFO"), *Result.Test, *Result.Body, *Result.Text);
		}
		return Out.IsEmpty() ? FString(TEXT("no results yet")) : Out;
	}
	return TEXT("fnb.Test run <settle|capsize|drop|wind|throttle|gather|walkaway|all> | stop | reset [selected] | select <next|prev|name> | ")
		TEXT("water <live|controlled [height]|toggle> | wind <project|calm|m/s [toward degrees]> | push [N.s] | heel [deg] | drop [m] | here | ")
		TEXT("boat <throttle> [rudder] | scenery <on|off> | debug | calibrate [apply] [view] | hud | results");
}

#if !UE_BUILD_SHIPPING
namespace
{
	FFluidNinjaBuoyancyTestBench* FindBench(UWorld* World)
	{
		auto From = [](UWorld* Candidate) -> FFluidNinjaBuoyancyTestBench*
		{
			const UFluidNinjaBuoyancySubsystem* Hydro = Candidate ? Candidate->GetSubsystem<UFluidNinjaBuoyancySubsystem>() : nullptr;
			return Hydro ? Hydro->GetTestBench() : nullptr;
		};
		if (FFluidNinjaBuoyancyTestBench* Bench = From(World))
		{
			return Bench;
		}
		// Typed into the editor's console: the world in Play.
		if (!GEngine)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE || Context.WorldType == EWorldType::Game)
			{
				if (FFluidNinjaBuoyancyTestBench* Bench = From(Context.World()))
				{
					return Bench;
				}
			}
		}
		return nullptr;
	}

	FAutoConsoleCommandWithWorldAndArgs GFluidNinjaBuoyancyTestCommand(
		TEXT("fnb.Test"),
		TEXT("FluidNinja Buoyancy test bench (in Play): fnb.Test help lists the verbs (run <test>, reset, select, water, wind, push, heel, drop, here, boat, debug, calibrate)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			FFluidNinjaBuoyancyTestBench* Bench = FindBench(World);
			const FString Out = Bench ? Bench->Command(Args) : FString(TEXT("fnb.Test: run it in Play (no FluidNinja Buoyancy test bench in this world)"));
			UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("fnb.Test: %s"), *Out);
		}));
}
#endif
