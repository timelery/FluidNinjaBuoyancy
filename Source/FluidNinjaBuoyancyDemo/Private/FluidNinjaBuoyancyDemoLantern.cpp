#include "FluidNinjaBuoyancyDemoLantern.h"

#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "FluidNinjaBuoyancyComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogFluidNinjaBuoyancyLantern, Log, All);

const TCHAR* AFluidNinjaBuoyancyDemoLantern::DefaultLanternPath = TEXT("/FluidNinjaBuoyancy/Demo/Lantern/BP_FNB_Lantern.BP_FNB_Lantern_C");

namespace
{
	// The lantern's wooden base: 36 x 36 cm planks 2.4 cm thick. The hull is 6 cm deep (the planks and the frame's foot) so the wave
	// shape, built at Hull Resolution Cm, has room; at 0.9 cm draft only its bottom is ever wet.
	constexpr float BaseHalfWidthCm = 18.f;
	constexpr float BaseHalfHeightCm = 3.f;
}

AFluidNinjaBuoyancyDemoLantern::AFluidNinjaBuoyancyDemoLantern()
{
	PrimaryActorTick.bCanEverTick = false;

	Base = CreateDefaultSubobject<UBoxComponent>(TEXT("Base"));
	SetRootComponent(Base);
	Base->SetBoxExtent(FVector(BaseHalfWidthCm, BaseHalfWidthCm, BaseHalfHeightCm));
	Base->SetCollisionProfileName(TEXT("PhysicsActor"));
	Base->SetSimulatePhysics(true);
	Base->SetGenerateOverlapEvents(false);   // FluidNinja's own overlap tracking would paint the lantern's world velocity into the river
	Base->SetHiddenInGame(true);

	Buoyancy = CreateDefaultSubobject<UFluidNinjaBuoyancyComponent>(TEXT("Buoyancy"));
	// A light, flat wooden base: the exact waterline cut (Triangles) at a resolution well under its depth. Paulownia planks and a candle,
	// about 1.2 kg, float 0.9 cm deep.
	Buoyancy->HullModel = EFluidNinjaHullModel::Triangles;
	Buoyancy->HullResolutionCm = 3.f;
	Buoyancy->MaxHullTriangles = 1000;
	Buoyancy->MassMode = EFluidNinjaBuoyancyMass::RestDraft;
	Buoyancy->RestDraftCm = 0.9f;
	Buoyancy->bSettleOnStart = true;   // set down on the water where it is spawned, not dropped into it
	// A light floater rides the water others push: no wakes of its own (Interaction Wakes with Wake off writes nothing).
	Buoyancy->Interaction = EFluidNinjaBuoyancyInteraction::Wakes;
	Buoyancy->bWake = false;
	Buoyancy->AngularDamping = FVector(0.5, 0.5, 0.2);

	LanternClass = TSoftClassPtr<AActor>(FSoftObjectPath(DefaultLanternPath));
}

void AFluidNinjaBuoyancyDemoLantern::BeginPlay()
{
	Super::BeginPlay();
	UClass* Class = LanternClass.LoadSynchronous();
	UWorld* World = GetWorld();
	if (!Class || !World)
	{
		UE_LOG(LogFluidNinjaBuoyancyLantern, Warning, TEXT("%s: no lantern to draw (Lantern Class %s did not load)."), *GetName(), *LanternClass.ToString());
		return;
	}
	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.bDeferConstruction = true;
	const FTransform At(GetActorQuat(), GetActorLocation() - GetActorUpVector() * BaseHalfHeightCm);
	Lantern = World->SpawnActor<AActor>(Class, At, Params);
	if (!Lantern)
	{
		return;
	}
	// Its own flicker phase, before its construction script runs (the Blueprint's PhaseOffset is a double; set it whatever it is).
	if (const FNumericProperty* Phase = FindFProperty<FNumericProperty>(Class, TEXT("PhaseOffset")))
	{
		void* Address = Phase->ContainerPtrToValuePtr<void>(Lantern.Get());
		Phase->IsFloatingPoint() ? Phase->SetFloatingPointPropertyValue(Address, PhaseOffset) : Phase->SetIntPropertyValue(Address, static_cast<int64>(PhaseOffset));
	}
	Lantern->FinishSpawning(At);
	Lantern->AttachToComponent(Base, FAttachmentTransformRules::KeepWorldTransform);
}

void AFluidNinjaBuoyancyDemoLantern::EndPlay(const EEndPlayReason::Type Reason)
{
	if (Lantern)
	{
		Lantern->Destroy();
		Lantern = nullptr;
	}
	Super::EndPlay(Reason);
}

FVector AFluidNinjaBuoyancyDemoLantern::GroupCenterBeyond(const TArray<AActor*>& Bodies, const FVector& Downstream, const FFluidNinjaLanternGroup& Group, const FVector& Fallback)
{
	const FVector Along = Downstream.GetSafeNormal2D().IsNearlyZero() ? FVector::ForwardVector : Downstream.GetSafeNormal2D();
	FVector Sum = FVector::ZeroVector;
	int32 Count = 0;
	for (const AActor* Body : Bodies)
	{
		if (Body)
		{
			Sum += Body->GetActorLocation();
			++Count;
		}
	}
	if (Count == 0)
	{
		return Fallback;
	}
	const FVector Middle = Sum / Count;
	double Furthest = 0.0;
	for (const AActor* Body : Bodies)
	{
		if (Body)
		{
			FVector Origin, Extent;
			Body->GetActorBounds(true, Origin, Extent);
			const double Reach = FVector::DotProduct(Origin - Middle, Along) + FMath::Abs(Extent.X * Along.X) + FMath::Abs(Extent.Y * Along.Y);
			Furthest = FMath::Max(Furthest, Reach);
		}
	}
	return Middle + Along * (Furthest + Group.ClearanceCm + Group.HalfLengthCm);
}

int32 AFluidNinjaBuoyancyDemoLantern::SpawnGroup(UWorld& World, const FFluidNinjaLanternGroup& Group, const FVector& Center, const FVector& Downstream, const TArray<AActor*>& Avoid,
	TArray<AFluidNinjaBuoyancyDemoLantern*>& OutLanterns)
{
	const FVector Along = Downstream.GetSafeNormal2D().IsNearlyZero() ? FVector::ForwardVector : Downstream.GetSafeNormal2D();
	const FVector Across(-Along.Y, Along.X, 0.0);
	FRandomStream Random(FPlatformTime::Cycles());

	// The other bodies' footprints, grown by the clearance: no lantern is set down there.
	TArray<FBox2D> Keep;
	for (const AActor* Body : Avoid)
	{
		if (Body)
		{
			FVector Origin, Extent;
			Body->GetActorBounds(true, Origin, Extent);
			const FVector2D Grow(Extent.X + Group.ClearanceCm, Extent.Y + Group.ClearanceCm);
			Keep.Add(FBox2D(FVector2D(Origin) - Grow, FVector2D(Origin) + Grow));
		}
	}

	// Random points in the ellipse, the nearer to its middle the likelier (a loose cluster, not an even sheet), kept apart by Spacing.
	TArray<FVector> Points;
	for (int32 Try = 0; Try < 6000 && Points.Num() < Group.Count; ++Try)
	{
		const double Angle = Random.FRandRange(0.f, 2.f * PI);
		const double Radius = FMath::Pow(Random.FRand(), 0.65f);
		const FVector Point = Center + Along * (FMath::Cos(Angle) * Radius * Group.HalfLengthCm) + Across * (FMath::Sin(Angle) * Radius * Group.HalfWidthCm);
		const bool bClear = !Points.ContainsByPredicate([&](const FVector& Other) { return FVector::Dist2D(Other, Point) < Group.SpacingCm; })
			&& !Keep.ContainsByPredicate([&](const FBox2D& Box) { return Box.IsInside(FVector2D(Point)); });
		if (bClear)
		{
			Points.Add(Point);
		}
	}
	if (Points.Num() < Group.Count)
	{
		UE_LOG(LogFluidNinjaBuoyancyLantern, Warning, TEXT("Lanterns: only %d of %d fit in the %.0f x %.0f cm group at %.0f cm spacing."),
			Points.Num(), Group.Count, Group.HalfLengthCm * 2.f, Group.HalfWidthCm * 2.f, Group.SpacingCm);
	}

	for (const FVector& Point : Points)
	{
		const FTransform At(FRotator(0.0, Random.FRandRange(0.f, 360.f), 0.0), Point + FVector(0.0, 0.0, 30.0));
		AFluidNinjaBuoyancyDemoLantern* Lantern = World.SpawnActorDeferred<AFluidNinjaBuoyancyDemoLantern>(AFluidNinjaBuoyancyDemoLantern::StaticClass(), At, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Lantern)
		{
			continue;
		}
		if (!Group.LanternClass.IsNull())
		{
			Lantern->LanternClass = Group.LanternClass;
		}
		Lantern->PhaseOffset = static_cast<float>(Random.FRandRange(0.0, 100.0));
		Lantern->FinishSpawning(At);
		OutLanterns.Add(Lantern);
	}
	UE_LOG(LogFluidNinjaBuoyancyLantern, Log, TEXT("Lanterns: %d set on the water round (%.0f, %.0f)"), Points.Num(), Center.X, Center.Y);
	return Points.Num();
}
