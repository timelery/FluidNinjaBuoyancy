#include "FluidNinjaBuoyancyDemoBall.h"

#include "Components/BoxComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FluidNinjaBuoyancyComponent.h"
#include "FluidNinjaBuoyancyDemoLantern.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogFluidNinjaBuoyancyBall, Log, All);

AFluidNinjaBuoyancyDemoBall::AFluidNinjaBuoyancyDemoBall()
{
	PrimaryActorTick.bCanEverTick = false;

	Ball = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Ball"));
	SetRootComponent(Ball);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (Sphere.Succeeded())
	{
		Ball->SetStaticMesh(Sphere.Object);   // 100 cm across, its simple collision a sphere
	}
	// Pale paint that glows (GlowColor x GlowStrength, set at Begin Play), so the ball can be followed in the dark.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Paint(TEXT("/FluidNinjaBuoyancy/Demo/Materials/M_FNB_BallGlow.M_FNB_BallGlow"));
	if (Paint.Succeeded())
	{
		Ball->SetMaterial(0, Paint.Object);
	}
	Ball->SetCollisionProfileName(TEXT("PhysicsActor"));
	Ball->SetSimulatePhysics(true);
	Ball->SetGenerateOverlapEvents(false);   // FluidNinja's own overlap tracking would paint the ball's world velocity into the river
	Ball->BodyInstance.bUseCCD = true;
	Ball->SetMobility(EComponentMobility::Movable);

	GlowLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("GlowLight"));
	GlowLight->SetupAttachment(Ball);
	GlowLight->SetMobility(EComponentMobility::Movable);
	GlowLight->IntensityUnits = ELightUnits::Lumens;
	GlowLight->CastShadows = false;
	GlowLight->bUseInverseSquaredFalloff = true;
	GlowLight->AttenuationRadius = 600.f;
	GlowLight->SourceRadius = 45.f;   // the ball's own size: soft-edged light on the water

	Buoyancy = CreateDefaultSubobject<UFluidNinjaBuoyancyComponent>(TEXT("Buoyancy"));
	Buoyancy->HullModel = EFluidNinjaHullModel::Spheres;
	Buoyancy->MassMode = EFluidNinjaBuoyancyMass::BodyMass;
	Buoyancy->bSettleOnStart = false;   // dropped, not set on the water
	Buoyancy->Interaction = EFluidNinjaBuoyancyInteraction::Wakes;
	Buoyancy->bWake = true;
	Buoyancy->AngularDamping = FVector(0.5, 0.5, 0.5);
}

void AFluidNinjaBuoyancyDemoBall::BeginPlay()
{
	// Size and weight before the buoyancy component reads the hull at its own Begin Play.
	const float Scale = DiameterCm / 100.f;
	Ball->SetWorldScale3D(FVector(Scale));
	Ball->SetMassOverrideInKg(NAME_None, MassKg, true);
	FFluidNinjaPontoon Whole;
	Whole.Location = FVector::ZeroVector;
	Whole.Radius = DiameterCm * 0.5f;
	Buoyancy->Pontoons = {Whole};
	if (UMaterialInstanceDynamic* Glow = Ball->CreateDynamicMaterialInstance(0))
	{
		Glow->SetVectorParameterValue(TEXT("GlowColor"), GlowColor);
		Glow->SetScalarParameterValue(TEXT("GlowStrength"), GlowStrength);
	}
	GlowLight->SetLightColor(GlowColor);
	GlowLight->SetIntensity(GlowLumens);
	GlowLight->SetVisibility(GlowLumens > 0.f);
	GlowLight->SetSourceRadius(DiameterCm * 0.45f);
	Super::BeginPlay();
}

AFluidNinjaBuoyancyDemoBall* AFluidNinjaBuoyancyDemoBall::DropOnLanterns(UWorld& World, float HeightCm)
{
	FVector Sum = FVector::ZeroVector;
	FVector Drift = FVector::ZeroVector;
	int32 Count = 0;
	for (TActorIterator<AFluidNinjaBuoyancyDemoLantern> It(&World); It; ++It)
	{
		if (!It->IsActorBeingDestroyed())
		{
			Sum += It->GetActorLocation();
			Drift += It->Base && It->Base->IsSimulatingPhysics() ? It->Base->GetPhysicsLinearVelocity() : FVector::ZeroVector;
			++Count;
		}
	}
	if (Count == 0)
	{
		UE_LOG(LogFluidNinjaBuoyancyBall, Display, TEXT("Drop ball: no lanterns afloat (start a lantern night first)."));
		return nullptr;
	}
	// Aimed where the group will be when the ball reaches the water: the lanterns drift on with the current while it falls (a 4 m drop
	// takes 0.9 s, 1.4 m on a 1.5 m/s river).
	const double FallSeconds = FMath::Sqrt(2.0 * FMath::Max(HeightCm, 0.f) / 980.0);
	FVector Lead = Drift / Count * FallSeconds;
	Lead.Z = 0.0;
	const FVector Middle = Sum / Count + Lead;
	AFluidNinjaBuoyancyDemoBall* Dropped = World.SpawnActorDeferred<AFluidNinjaBuoyancyDemoBall>(StaticClass(), FTransform::Identity, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Dropped)
	{
		return nullptr;
	}
	// Its bottom HeightCm above the lanterns' bases.
	const FTransform At(FRotator::ZeroRotator, Middle + FVector(0.0, 0.0, HeightCm + Dropped->DiameterCm * 0.5));
	Dropped->FinishSpawning(At);
	UE_LOG(LogFluidNinjaBuoyancyBall, Log, TEXT("Drop ball: %.0f cm, %.0f kg, from %.0f cm into %d lanterns at (%.0f, %.0f)"), Dropped->DiameterCm, Dropped->MassKg, HeightCm,
		Count, Middle.X, Middle.Y);
	return Dropped;
}
