#include "FluidNinjaWaterBirdComponent.h"

#include "AnimationRuntime.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "NiagaraSystem.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "FluidNinjaBuoyancyLog.h"
#include "FluidNinjaBuoyancySettings.h"
#include "FluidNinjaBuoyancySubsystem.h"
#include "FluidNinjaBuoyancyWaterQuery.h"

namespace FluidNinjaWaterBird
{
	/** Water in the plugin's units: kg/cm3 and cm/s2. */
	constexpr double WaterDensityKgCm3 = 0.001;
	constexpr double GravityCmS2 = 981.0;
	/** A depth not measured (no water under the part, or not sampled yet). */
	constexpr float NoDepthCm = -10000.f;
	/** The least a moving part sends between its entries and exits (kg·cm/s): smaller pushes are summed until they reach it. */
	constexpr float MinImpulseKgCmS = 0.5f;
	/** A part moving further than this in one frame was teleported, and a frame longer than MaxStepSeconds is a hitch: no speed then. */
	constexpr double TeleportCm = 150.0;
	constexpr float MaxStepSeconds = 0.2f;
	/** A wet part is out once its sphere is this far clear of the surface, so a part resting at the water line on small waves does not
	 * flicker in and out (each entry may splash). It goes in as soon as it touches. */
	constexpr float ExitClearanceCm = 1.f;
	/** A part coming out throws this share of the splash it would throw going in at the same speed. */
	constexpr float ExitSplashShare = 0.35f;
	/** A splash weaker than this is not drawn (the floating bodies' and the swimmers' rule; Add Water Contact spawns any strength above 0). */
	constexpr float SmallestSplash = 0.2f;
	/** Air in the plugin's units (kg/cm3), at sea level and 20 C. */
	constexpr double AirDensityKgCm3 = 1.2e-6;
	/** A downwash push is spread over this many points of its patch, each a fine ripple this wide (cm, at scale 1): a gust's patch of short
	 * ripples rather than one ring. */
	constexpr int32 AirPatchPoints = 3;
	constexpr float AirRippleRadiusCm = 4.f;

	/** The volume of a sphere's cap Height deep: pi h2 (3 R - h) / 3. */
	double CapVolume(double Radius, double Height)
	{
		return PI * Height * Height * (3.0 * Radius - Height) / 3.0;
	}

	/** How deep a sphere of Radius sits in the water to displace Volume (cm3): from 0 to its diameter, the diameter when it cannot. */
	double SphereDraft(double Radius, double Volume)
	{
		if (Radius <= 0.0 || Volume <= 0.0)
		{
			return 0.0;
		}
		if (Volume >= CapVolume(Radius, 2.0 * Radius))
		{
			return 2.0 * Radius;
		}
		// The cap's volume grows with its depth: halve the interval (48 halvings leave far under a micron).
		double Low = 0.0;
		double High = 2.0 * Radius;
		for (int32 Step = 0; Step < 48; ++Step)
		{
			const double Mid = 0.5 * (Low + High);
			if (CapVolume(Radius, Mid) < Volume)
			{
				Low = Mid;
			}
			else
			{
				High = Mid;
			}
		}
		return 0.5 * (Low + High);
	}

	/** A part's bone in Mesh and its offset from that bone: Reference Point expressed in the bone's component-space reference transform, so
	 * the point keeps its place on the animated bone. Bone None: the component itself, the point as it is. False when the mesh has no asset
	 * or its skeleton lacks the bone. */
	bool PartOffset(const USkeletalMeshComponent& Mesh, const FFluidNinjaWaterBirdPart& Part, int32& OutBone, FVector& OutOffset)
	{
		const USkeletalMesh* Asset = Mesh.GetSkeletalMeshAsset();
		if (!Asset)
		{
			return false;
		}
		if (Part.Bone.IsNone())
		{
			OutBone = INDEX_NONE;
			OutOffset = Part.ReferencePoint;
			return true;
		}
		const FReferenceSkeleton& Reference = Asset->GetRefSkeleton();
		const int32 Bone = Reference.FindBoneIndex(Part.Bone);
		if (Bone == INDEX_NONE)
		{
			return false;
		}
		OutBone = Bone;
		OutOffset = FAnimationRuntime::GetComponentSpaceTransformRefPose(Reference, Bone).InverseTransformPosition(Part.ReferencePoint);
		return true;
	}

	/** The part's point in the world now, from the animated bone (or the component). */
	FVector PartWorld(const USkeletalMeshComponent& Mesh, int32 Bone, const FVector& Offset)
	{
		return Bone == INDEX_NONE ? Mesh.GetComponentTransform().TransformPosition(Offset) : Mesh.GetBoneTransform(Bone).TransformPosition(Offset);
	}

	FString JsonNumber(double Value)
	{
		return FMath::IsFinite(Value) ? FString::Printf(TEXT("%.3f"), Value) : FString(TEXT("null"));
	}

	FString JsonString(const FString& Text)
	{
		return TEXT("\"") + Text.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\"")) + TEXT("\"");
	}
}

UFluidNinjaWaterBirdComponent::UFluidNinjaWaterBirdComponent()
{
	// After physics, and after the mesh's own tick (a prerequisite added when the mesh is bound): the bones are this frame's animated pose.
	// The tick runs only while the contacts do.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

void UFluidNinjaWaterBirdComponent::BeginPlay()
{
	Super::BeginPlay();
	if (!IsValid(Mesh))
	{
		Mesh = FindMesh();
	}
	LoadSplash();
	LoadPlunge();
	Bind(Mesh);
	SetComponentTickEnabled(bContactsEnabled);
}

#if WITH_EDITOR
void UFluidNinjaWaterBirdComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	if (HasBegunPlay() && PropertyChangedEvent.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(UFluidNinjaWaterBirdComponent, bContactsEnabled))
	{
		// The Details panel wrote the flag itself: the setter must see the switch to start the parts over and set the tick.
		const bool bEnabled = bContactsEnabled;
		bContactsEnabled = !bEnabled;
		SetContactsEnabled(bEnabled);
	}
}
#endif

void UFluidNinjaWaterBirdComponent::SetContactsEnabled(bool bEnabled)
{
	const bool bSwitchedOn = bEnabled && !bContactsEnabled;
	bContactsEnabled = bEnabled;
	// Before Begin Play the flag is all there is to keep: Begin Play binds the mesh and starts the tick.
	if (!HasBegunPlay())
	{
		return;
	}
	if (bSwitchedOn)
	{
		Bind(FindMesh());
	}
	SetComponentTickEnabled(bEnabled);
}

void UFluidNinjaWaterBirdComponent::ResetPeakDepths()
{
	for (float& Peak : PeakDepths)
	{
		Peak = FluidNinjaWaterBird::NoDepthCm;
	}
}

USkeletalMeshComponent* UFluidNinjaWaterBirdComponent::FindMesh() const
{
	if (IsValid(Mesh))
	{
		return Mesh.Get();
	}
	const AActor* Owner = GetOwner();
	return Owner ? Owner->FindComponentByClass<USkeletalMeshComponent>() : nullptr;
}

USkeletalMeshComponent* UFluidNinjaWaterBirdComponent::ResolveMesh()
{
	USkeletalMeshComponent* Wanted = FindMesh();
	const USkeletalMesh* Asset = Wanted ? Wanted->GetSkeletalMeshAsset() : nullptr;
	// Parts is Blueprint-writable: a list that grew or shrank is bound again before it is indexed.
	if (Wanted != BoundMesh.Get() || Asset != BoundAsset.Get() || PartOffsets.Num() != Parts.Num())
	{
		Bind(Wanted);
	}
	return Asset ? Wanted : nullptr;
}

void UFluidNinjaWaterBirdComponent::Bind(USkeletalMeshComponent* InMesh)
{
	USkeletalMeshComponent* Previous = BoundMesh.Get();
	const USkeletalMesh* Asset = InMesh ? InMesh->GetSkeletalMeshAsset() : nullptr;
	const bool bNewMesh = InMesh != Previous || Asset != BoundAsset.Get() || PartOffsets.Num() != Parts.Num();
	if (Previous && Previous != InMesh)
	{
		RemoveTickPrerequisiteComponent(Previous);
	}
	if (InMesh && InMesh != Previous)
	{
		// The mesh's tick waits for its parallel animation evaluation, so after it the bones are final for the frame.
		AddTickPrerequisiteComponent(InMesh);
	}
	BoundMesh = InMesh;
	BoundAsset = Asset;
	const int32 Count = Parts.Num();
	PartBones.Init(INDEX_NONE, Count);
	PartFound.Init(false, Count);
	PartOffsets.Init(FVector::ZeroVector, Count);
	States.Init(FPartState(), Count);
	Depths.Init(FluidNinjaWaterBird::NoDepthCm, Count);
	// The deepest depths are QA's, reset by Reset Peak Depths, and the entry speeds too: kept while the list keeps its length.
	if (PeakDepths.Num() != Count)
	{
		PeakDepths.Init(FluidNinjaWaterBird::NoDepthCm, Count);
	}
	if (EntrySpeeds.Num() != Count)
	{
		EntrySpeeds.Init(0.f, Count);
	}
	if (!InMesh || !Asset)
	{
		return;
	}
	for (int32 Index = 0; Index < Count; ++Index)
	{
		PartFound[Index] = FluidNinjaWaterBird::PartOffset(*InMesh, Parts[Index], PartBones[Index], PartOffsets[Index]);
		if (!PartFound[Index] && bNewMesh)
		{
			UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("FluidNinja Water Bird on %s: part %s's bone %s is not in %s; the part is left out."),
				*GetNameSafe(GetOwner()), *Parts[Index].Name.ToString(), *Parts[Index].Bone.ToString(), *GetNameSafe(Asset));
		}
	}
}

void UFluidNinjaWaterBirdComponent::LoadSplash()
{
	LoadedSplashPath = SplashSystem.ToSoftObjectPath();
	LoadedSplash = SplashSystem.LoadSynchronous();
	// A system that was asked for and is missing spawns nothing: the project's splash does not stand in for it.
	bSplashMissing = !LoadedSplashPath.IsNull() && !LoadedSplash;
	if (bSplashMissing)
	{
		UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("FluidNinja Water Bird on %s: Splash System %s did not load; this bird makes no splashes."),
			*GetNameSafe(GetOwner()), *LoadedSplashPath.ToString());
	}
}

void UFluidNinjaWaterBirdComponent::LoadPlunge()
{
	LoadedPlungePath = PlungeSplashSystem.ToSoftObjectPath();
	LoadedPlunge = PlungeSplashSystem.LoadSynchronous();
	if (!LoadedPlungePath.IsNull() && !LoadedPlunge)
	{
		UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("FluidNinja Water Bird on %s: Plunge Splash System %s did not load; this bird makes no plunge splash."),
			*GetNameSafe(GetOwner()), *LoadedPlungePath.ToString());
	}
}

void UFluidNinjaWaterBirdComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FluidNinjaBuoyancy_WaterBird);
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (bContactsEnabled && DeltaTime > 0.f)
	{
		UpdateContacts(DeltaTime);
	}
}

bool UFluidNinjaWaterBirdComponent::SampleWater(const FVector& Location, FFluidNinjaWaterSample& OutWater)
{
	if (!UFluidNinjaBuoyancyWaterLibrary::GetWaterAtLocation(this, Location, OutWater))
	{
		return false;
	}
	// The age is the waves' and the current's readback's, not the still water's: past half a second (a hitch, a simulation switched off)
	// the still surface, whose coverage and height stay current, stands for the water.
	if (OutWater.AgeSeconds > 0.5f)
	{
		OutWater.SurfaceZ = OutWater.StillZ;
		OutWater.WaveHeight = 0.f;
		OutWater.Depth = OutWater.SurfaceZ - static_cast<float>(Location.Z);
		OutWater.Normal = FVector::UpVector;
		OutWater.Current = FVector::ZeroVector;
		OutWater.bHasWaves = false;
		OutWater.bHasCurrent = false;
		++ReadbackFallbacks;
	}
	return true;
}

void UFluidNinjaWaterBirdComponent::UpdateContacts(float DeltaSeconds)
{
	USkeletalMeshComponent* Skeletal = ResolveMesh();
	UFluidNinjaBuoyancySubsystem* Subsystem = UFluidNinjaBuoyancyWaterLibrary::FindSubsystem(this);
	if (!Skeletal || !Subsystem)
	{
		return;
	}
	if (SplashSystem.ToSoftObjectPath() != LoadedSplashPath)
	{
		LoadSplash();
	}
	if (PlungeSplashSystem.ToSoftObjectPath() != LoadedPlungePath)
	{
		LoadPlunge();
	}
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	const float Scale = OwnerScale();
	const float MassScale = Scale * Scale * Scale;
	SincePlunge += DeltaSeconds;
	// The downwash only within Air View Distance Cm of the player's camera (no camera, as in a server: everywhere).
	bool bAirInView = true;
	if (AirViewDistanceCm > 0.f)
	{
		const APlayerCameraManager* Camera = UGameplayStatics::GetPlayerCameraManager(this, 0);
		const AActor* Owner = GetOwner();
		if (Camera && Owner)
		{
			bAirInView = FVector::DistSquared(Camera->GetCameraLocation(), Owner->GetActorLocation()) <= FMath::Square(AirViewDistanceCm);
		}
	}
	for (int32 Index = 0; Index < Parts.Num(); ++Index)
	{
		if (!PartFound[Index])
		{
			continue;
		}
		const FFluidNinjaWaterBirdPart& Part = Parts[Index];
		FPartState& State = States[Index];
		const FVector Position = FluidNinjaWaterBird::PartWorld(*Skeletal, PartBones[Index], PartOffsets[Index]);
		FFluidNinjaWaterSample Water;
		if (!SampleWater(Position, Water))
		{
			// No water known here: the part starts over where water is found again (no speed for that frame, no entry from a stale state).
			State = FPartState();
			Depths[Index] = FluidNinjaWaterBird::NoDepthCm;
			if (!BodyPart.IsNone() && Part.Name == BodyPart)
			{
				bBodyDiving = false;
			}
			continue;
		}
		const float Depth = Water.SurfaceZ - static_cast<float>(Position.Z);
		Depths[Index] = Depth;
		PeakDepths[Index] = FMath::Max(PeakDepths[Index], Depth);
		const float Radius = Part.RadiusCm * Scale;
		const float AddedMass = Part.AddedMassKg * MassScale;
		const FVector Step = Position - State.Previous;
		const bool bMoving = State.bInitialized && DeltaSeconds < FluidNinjaWaterBird::MaxStepSeconds
			&& Step.SizeSquared() < FMath::Square(FluidNinjaWaterBird::TeleportCm);
		const FVector Velocity = bMoving ? Step / DeltaSeconds : FVector::ZeroVector;
		State.Previous = Position;
		State.bInitialized = true;
		State.SinceSent += DeltaSeconds;
		State.SinceWakeSent += DeltaSeconds;
		State.SinceAirSent += DeltaSeconds;

		// The water's vertical momentum the part carries with it: its added mass, as much of it as is wet, at its speed down. Its change is
		// what the part hands the water: going in presses it down, slowing under it or coming up pulls it back. A frame without a speed (the
		// first, a teleport) only takes the new value, so a bird switched on or moved while in the water hands over nothing for it.
		const float Wet = FMath::SmoothStep(-Radius, Radius, Depth);
		const float Momentum = AddedMass * Wet * -static_cast<float>(Velocity.Z);
		// A push deeper down reaches the surface weaker (its top Depth - Radius under it), as a submerged source's short waves do.
		const float Reach = DepthFadeRadii * FMath::Max(Radius, 1.f);
		const float Surfacing = Reach > 0.f ? FMath::Exp(-FMath::Max(Depth - Radius, 0.f) / Reach) : 1.f;
		if (bMoving && State.bHasMomentum)
		{
			State.Pending += (Momentum - State.Momentum) * Surfacing;
		}
		State.Momentum = Momentum;
		State.bHasMomentum = bMoving;
		State.Wet = Wet;
		if (!BodyPart.IsNone() && Part.Name == BodyPart)
		{
			// The body diving in: still dry, coming down at plunge speed. Parts going in ahead of it leave their spray to the plunge.
			bBodyDiving = bMoving && Wet <= 0.f && Velocity.Z < 0.0 && Velocity.Size() >= PlungeMinSpeedCmS;
		}

		// Moving along the water, a wet part with Wake pays drag, 0.5 rho Cd A v2 over the frame (kg·cm/s), pressed in ahead of it.
		const FVector Along(Velocity.X, Velocity.Y, 0.0);
		if (Part.bWake && bMoving && Wet > 0.f && WakeDragCoefficient > 0.f)
		{
			const double WetArea = PI * Radius * Radius * Wet;
			const double Drag = 0.5 * FluidNinjaWaterBird::WaterDensityKgCm3 * WakeDragCoefficient * WetArea * Along.SizeSquared();
			State.PendingWake += static_cast<float>(Drag * DeltaSeconds) * Surfacing;
			if (Along.SizeSquared() > 1.0)
			{
				State.WakeDirection = Along.GetSafeNormal();
			}
		}

		// Clear of the water and coming down within Air Reach Cm of it (a wing beating towards it), the part drives air onto the water under
		// it: the momentum flux of the air it pushes, air density x Air Area x v_z2 over the frame, fading to nothing at the reach.
		if (!bAirInView)
		{
			State.PendingAir = 0.f;   // nothing gathered out of view comes back as a burst when the camera nears
		}
		else if (Part.AirReachCm > 0.f && Part.AirAreaCm2 > 0.f && bMoving && Wet <= 0.f && Velocity.Z < 0.0)
		{
			const float AirReach = Part.AirReachCm * Scale;
			const float Height = FMath::Max(-Depth - Radius, 0.f);   // the sphere's bottom above the surface
			if (Height < AirReach)
			{
				const float Fade = FMath::Square(1.f - Height / AirReach);
				const double Area = static_cast<double>(Part.AirAreaCm2) * Scale * Scale;
				State.PendingAir += static_cast<float>(FluidNinjaWaterBird::AirDensityKgCm3 * Area * FMath::Square(Velocity.Z) * DeltaSeconds) * Fade
					* FMath::Max(Part.AirStrength, 0.f);
			}
		}

		// Going in (touching the water) and coming out (clear of it) are sent at once, with what the part handed over so far, the splash and
		// the body's foam.
		const bool bEntry = !State.bWet && Wet > 0.f;
		const bool bExit = State.bWet && Depth < -Radius - FluidNinjaWaterBird::ExitClearanceCm;
		if (bEntry || bExit)
		{
			State.bWet = bEntry;
			if (bMoving)
			{
				const float Speed = static_cast<float>(Velocity.Size());
				const bool bBody = !BodyPart.IsNone() && Part.Name == BodyPart;
				const bool bPlungeOn = LoadedPlunge && PlungeSplashScale > 0.f;
				if (bEntry)
				{
					EntrySpeeds[Index] = Speed;
				}
				// A dive: the plunge splash goes first, so the frame's splash budget draws it before the parts' small ones.
				if (bPlungeOn && bEntry && bBody && Speed >= PlungeMinSpeedCmS)
				{
					SendPlunge(*Subsystem, Position, Water.SurfaceZ, Water.Normal, Radius, Velocity);
				}
				// A dive's spray is the plunge splash: a part going in while the body dives in behind it (the beak, the head), and every part
				// for Plunge Quiet Seconds after the plunge (the feet following the body, the surfacing), throws none of its own.
				const bool bHeldBack = bPlungeOn && (SincePlunge < PlungeQuietSeconds || (bEntry && !bBody && bBodyDiving));
				float Splash = 0.f;
				if (!bSplashMissing && !bHeldBack && Speed > 0.f && Speed >= MinSplashSpeedCmS)
				{
					// The slam's power on the project's splash scale, in watts: the part's momentum m v, stopped over its own size (2 r at
					// speed v), times v.
					const double SpeedMS = Speed * 0.01;
					const double Watts = AddedMass * SpeedMS * SpeedMS * SpeedMS / (2.0 * FMath::Max(Radius * 0.01, 0.001));
					const float Share = bEntry ? 1.f : FluidNinjaWaterBird::ExitSplashShare;
					Splash = SplashScale * Part.SplashScale * Settings.SplashStrength(static_cast<float>(Watts)) * Share;
					if (Splash < FluidNinjaWaterBird::SmallestSplash)
					{
						Splash = 0.f;
					}
				}
				else if (bHeldBack && Speed >= MinSplashSpeedCmS)
				{
					++HeldBack;
				}
				const float Foam = bEntry && bBody ? EntryFoamRate : 0.f;
				if (FMath::Abs(State.Pending) >= FluidNinjaWaterBird::MinImpulseKgCmS || Splash > 0.f || Foam > 0.f)
				{
					Send(*Subsystem, Position, Water.SurfaceZ, Water.Normal, Radius, State.Pending, Splash, Foam);
					State.Pending = 0.f;
					State.SinceSent = 0.f;
				}
				if (bEntry)
				{
					++Entries;
				}
				else
				{
					++Exits;
				}
			}
		}

		// In between, what gathered is sent at most every Ring Interval Seconds, once it is worth a ring.
		if (State.SinceSent >= RingIntervalSeconds && FMath::Abs(State.Pending) >= FluidNinjaWaterBird::MinImpulseKgCmS)
		{
			Send(*Subsystem, Position, Water.SurfaceZ, Water.Normal, Radius, State.Pending, 0.f, 0.f);
			State.Pending = 0.f;
			State.SinceSent = 0.f;
		}
		if (State.SinceWakeSent >= WakeIntervalSeconds && State.PendingWake >= FluidNinjaWaterBird::MinImpulseKgCmS)
		{
			// At the sphere's bow, as wide as the edge that pushes it (Wake Edge Radius Cm; the sphere when unset).
			const float WakeWidth = Part.WakeEdgeRadiusCm > 0.f ? Part.WakeEdgeRadiusCm * Scale : Radius;
			Send(*Subsystem, Position + State.WakeDirection * Radius, Water.SurfaceZ, Water.Normal, WakeWidth, State.PendingWake, 0.f, 0.f);
			State.PendingWake = 0.f;
			State.SinceWakeSent = 0.f;
		}
		// The downwash at most every Ring Interval Seconds, spread over a patch of the wing's area under the part, as fine ripples.
		if (State.SinceAirSent >= RingIntervalSeconds && State.PendingAir >= FluidNinjaWaterBird::MinImpulseKgCmS * FluidNinjaWaterBird::AirPatchPoints)
		{
			const float PatchRadius = FMath::Sqrt(Part.AirAreaCm2 / PI) * Scale;
			const float Share = State.PendingAir / FluidNinjaWaterBird::AirPatchPoints;
			for (int32 Point = 0; Point < FluidNinjaWaterBird::AirPatchPoints; ++Point)
			{
				const float Angle = FMath::FRand() * UE_TWO_PI;
				const float Distance = PatchRadius * FMath::Sqrt(FMath::FRand());
				const FVector Where = Position + FVector(Distance * FMath::Cos(Angle), Distance * FMath::Sin(Angle), 0.0);
				Send(*Subsystem, Where, Water.SurfaceZ, Water.Normal, FluidNinjaWaterBird::AirRippleRadiusCm * Scale, Share, 0.f, 0.f);
			}
			++AirPushes;
			AirImpulseSum += State.PendingAir;
			State.PendingAir = 0.f;
			State.SinceAirSent = 0.f;
		}
	}
}

void UFluidNinjaWaterBirdComponent::Send(UFluidNinjaBuoyancySubsystem& Subsystem, const FVector& Where, float SurfaceZ, const FVector& Normal,
	float RadiusCm, float ImpulseDown, float SplashStrength, float FoamRate)
{
	FFluidNinjaWaterContact Contact;
	Contact.Location = FVector(Where.X, Where.Y, SurfaceZ);
	Contact.ImpulseDown = ImpulseDown;
	Contact.RadiusCm = FMath::Clamp(RadiusCm, 0.5f, 200.f);
	Contact.SplashStrength = SplashStrength;
	Contact.SplashNormal = Normal;
	Contact.SplashSystem = LoadedSplash;
	Contact.FoamRate = FoamRate;
	Subsystem.AddWaterContact(Contact);
	++Contacts;
	if (SplashStrength > 0.f)
	{
		++Splashes;
	}
	ImpulseAbsSum += FMath::Abs(ImpulseDown);
}

void UFluidNinjaWaterBirdComponent::SendPlunge(UFluidNinjaBuoyancySubsystem& Subsystem, const FVector& Where, float SurfaceZ, const FVector& Normal,
	float RadiusCm, const FVector& Velocity)
{
	// The heading: the owner's forward laid on the water; an owner pointing along the normal (straight down) has none, so the dive's own
	// motion along the water stands in. Neither: the subsystem keeps the world's heading.
	const FVector Up = Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
	const AActor* Owner = GetOwner();
	FVector Heading = Owner ? FVector::VectorPlaneProject(Owner->GetActorForwardVector(), Up) : FVector::ZeroVector;
	if (Heading.SizeSquared() < 0.01)
	{
		Heading = FVector::VectorPlaneProject(Velocity, Up);
	}
	FFluidNinjaWaterContact Contact;
	Contact.Location = FVector(Where.X, Where.Y, SurfaceZ);
	Contact.RadiusCm = FMath::Clamp(RadiusCm, 0.5f, 200.f);
	// The system's own size is for a bird at actor scale 1: a bigger bird throws a bigger splash, as its parts do.
	Contact.SplashStrength = PlungeSplashScale * OwnerScale();
	Contact.SplashNormal = Up;
	Contact.SplashDirection = Heading.GetSafeNormal();
	Contact.SplashSystem = LoadedPlunge;
	Contact.SplashParameters = PlungeSplashParameters;
	Subsystem.AddWaterContact(Contact);
	++Plunges;
	SincePlunge = 0.f;
}

float UFluidNinjaWaterBirdComponent::OwnerScale() const
{
	// A bird is scaled uniformly: X stands for the scale.
	const AActor* Owner = GetOwner();
	return Owner ? FMath::Max(FMath::Abs(static_cast<float>(Owner->GetActorScale3D().X)), 0.01f) : 1.f;
}

const FFluidNinjaWaterBirdPart* UFluidNinjaWaterBirdComponent::FindBodyPart() const
{
	return BodyPart.IsNone() ? nullptr : Parts.FindByPredicate([this](const FFluidNinjaWaterBirdPart& Part) { return Part.Name == BodyPart; });
}

FVector UFluidNinjaWaterBirdComponent::GetPartLocation(FName Part) const
{
	const AActor* Owner = GetOwner();
	const FVector Fallback = Owner ? Owner->GetActorLocation() : FVector::ZeroVector;
	const FFluidNinjaWaterBirdPart* Found = Parts.FindByPredicate([Part](const FFluidNinjaWaterBirdPart& Candidate) { return Candidate.Name == Part; });
	const USkeletalMeshComponent* Skeletal = FindMesh();
	// Straight from the reference pose (a few bones' walk), so it answers before Begin Play and after a part was edited as well.
	int32 Bone = INDEX_NONE;
	FVector Offset = FVector::ZeroVector;
	if (!Found || !Skeletal || !FluidNinjaWaterBird::PartOffset(*Skeletal, *Found, Bone, Offset))
	{
		return Fallback;
	}
	return FluidNinjaWaterBird::PartWorld(*Skeletal, Bone, Offset);
}

float UFluidNinjaWaterBirdComponent::GetFloatBodyAboveWaterCm() const
{
	const FFluidNinjaWaterBirdPart* Body = FindBodyPart();
	if (!Body)
	{
		return 0.f;
	}
	// Archimedes: the body sphere sinks until the cap under the water holds the whole bird's mass.
	const double Scale = OwnerScale();
	const double Radius = Body->RadiusCm * Scale;
	const double Volume = MassKg * Scale * Scale * Scale / FluidNinjaWaterBird::WaterDensityKgCm3;
	return static_cast<float>(Radius - FluidNinjaWaterBird::SphereDraft(Radius, Volume));
}

float UFluidNinjaWaterBirdComponent::GetHeaveFrequencyHz() const
{
	const FFluidNinjaWaterBirdPart* Body = FindBodyPart();
	if (!Body)
	{
		return 0.f;
	}
	const double Scale = OwnerScale();
	const double Radius = Body->RadiusCm * Scale;
	const double Volume = MassKg * Scale * Scale * Scale / FluidNinjaWaterBird::WaterDensityKgCm3;
	const double Draft = FluidNinjaWaterBird::SphereDraft(Radius, Volume);
	// The waterplane is the sphere's circle at the water line, radius2 = R2 - (R - h)2 = h (2 R - h); none once the sphere is under. Its
	// stiffness rho g A pushes the bird and the water moving with its body.
	const double Area = Draft < 2.0 * Radius ? PI * Draft * (2.0 * Radius - Draft) : 0.0;
	const double Mass = (MassKg + Body->AddedMassKg) * Scale * Scale * Scale;
	if (Area <= 0.0 || Mass <= 0.0)
	{
		return 0.f;
	}
	return static_cast<float>(FMath::Sqrt(FluidNinjaWaterBird::WaterDensityKgCm3 * FluidNinjaWaterBird::GravityCmS2 * Area / Mass) / (2.0 * PI));
}

FString UFluidNinjaWaterBirdComponent::GetDiagnostics() const
{
	// Written by hand: the runtime module carries no JSON dependency. The arrays follow Parts' order.
	FString Names;
	FString DepthList;
	FString PeakList;
	FString WetList;
	FString EntrySpeedList;
	for (int32 Index = 0; Index < Parts.Num(); ++Index)
	{
		const TCHAR* Separator = Index > 0 ? TEXT(",") : TEXT("");
		Names += Separator + FluidNinjaWaterBird::JsonString(Parts[Index].Name.ToString());
		DepthList += Separator + FluidNinjaWaterBird::JsonNumber(Depths.IsValidIndex(Index) ? Depths[Index] : FluidNinjaWaterBird::NoDepthCm);
		PeakList += Separator + FluidNinjaWaterBird::JsonNumber(PeakDepths.IsValidIndex(Index) ? PeakDepths[Index] : FluidNinjaWaterBird::NoDepthCm);
		WetList += Separator + FluidNinjaWaterBird::JsonNumber(States.IsValidIndex(Index) ? States[Index].Wet : 0.f);
		EntrySpeedList += Separator + FluidNinjaWaterBird::JsonNumber(EntrySpeeds.IsValidIndex(Index) ? EntrySpeeds[Index] : 0.f);
	}
	return FString::Printf(TEXT("{\"water_contacts\":%s,\"water_entries\":%d,\"water_exits\":%d,\"contacts\":%d,\"splashes\":%d,\"plunges\":%d,")
		TEXT("\"splashes_held_back\":%d,\"air_pushes\":%d,\"air_impulse_sum_kg_cm_s\":%s,\"water_readback_fallbacks\":%d,\"impulse_abs_sum_kg_cm_s\":%s,\"part_names\":[%s],\"depth_cm\":[%s],")
		TEXT("\"peak_depth_cm\":[%s],\"wet_fraction\":[%s],\"entry_speed_cm_s\":[%s],\"float_body_above_water_cm\":%s,\"heave_frequency_hz\":%s}"),
		bContactsEnabled ? TEXT("true") : TEXT("false"), Entries, Exits, Contacts, Splashes, Plunges, HeldBack, AirPushes, *FluidNinjaWaterBird::JsonNumber(AirImpulseSum), ReadbackFallbacks,
		*FluidNinjaWaterBird::JsonNumber(ImpulseAbsSum), *Names, *DepthList, *PeakList, *WetList, *EntrySpeedList,
		*FluidNinjaWaterBird::JsonNumber(GetFloatBodyAboveWaterCm()), *FluidNinjaWaterBird::JsonNumber(GetHeaveFrequencyHz()));
}
