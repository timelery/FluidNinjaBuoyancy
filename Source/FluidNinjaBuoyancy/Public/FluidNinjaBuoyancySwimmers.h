#pragma once

#include "CoreMinimal.h"
#include "FluidNinjaBuoyancySubsystem.h"
#include "FluidNinjaBuoyancySwimmerProvider.h"
#include "FluidNinjaBuoyancyWaterQuery.h"
#include "NiagaraSystem.h"   // TStrongObjectPtr<UNiagaraSystem> below needs the full type (a unit compiled outside unity has no other way to it)
#include "UObject/StrongObjectPtr.h"

class UNiagaraDataChannelAsset;
class USkeletalMeshComponent;

/** One sphere of a registered swimmer's body pushing the water aside this frame (FluidNinja Waves' static head, an upright capsule as
 * short as it is wide). */
struct FFluidNinjaSwimmerSphere
{
	TWeakObjectPtr<USkeletalMeshComponent> Mesh;
	int32 Part = 0;
	FVector Centre = FVector::ZeroVector;
	float Radius = 0.f;
	FVector Velocity = FVector::ZeroVector;
	/** The still water the sphere's static head is measured against (never the waves: the head would chase its own hollow). */
	float WaterZ = 0.f;
};

/** A stroke's wake point this frame (FFluidNinjaSwimmerPose::StrokeImpulse): FluidNinja's point input, as the swimmers' wakes write it. */
struct FFluidNinjaSwimmerStrokePoint
{
	FVector Position = FVector::ZeroVector;
	FVector Previous = FVector::ZeroVector;
	/** The water's velocity to write (cm/s, world) and the brush size (cm). */
	FVector4 VelocityAndSize = FVector4(0.0, 0.0, 0.0, 0.0);
};

/**
 * The swimmers' body-part contacts (owned by UFluidNinjaBuoyancySubsystem). Every frame, for the player's pawn (its pose from its
 * swimmer provider or its skeletal mesh, while it is in the water) and each registered swimmer (UFluidNinjaBuoyancySubsystem::RegisterSwimmer),
 * each body part of its pose (FFluidNinjaSwimmerPose: hands, elbows, feet, head; a diver's chest, hips and knees too; the bones and sizes
 * from its UFluidNinjaSwimmerProfile) is followed against the water:
 *  - going in faster than Swimmer Entry Min: a splash on the project's scale (its slam power, the part's added mass m going in at v, is
 *    m·v³ / 2r: the energy ½·m·v² put in over the time r / v it takes to go under) and an impulse m·v into FluidNinja Waves (a ring);
 *    a part going into the air pocket another just opened makes a third of the splash and half the impulse;
 *  - coming out fast: a small splash, and the water it lifts (a negative impulse, a third as big);
 *  - feet moving up and down just under the surface: the water above pushed up or drawn down (a kick's churn);
 *  - the chest going under at the profile's Dive Min: a dive, OnEntry once (the entry package: cavity, crown, bubbles, sound);
 *  - a stroke (the pose's StrokeImpulse): a wake point behind the swimmer and a small push on the waves (Swimmer Stroke Scale).
 * Registered swimmers with displacement also give FluidNinja Waves their head, chest and hips as spheres pushing the water aside. A
 * swimmer registered with the water query takes its water (still surface, waves, current) from UFluidNinjaBuoyancySubsystem::SampleWater
 * at its hips every frame, the one source of truth its contacts, its displacement, its wake and its locomotion share (GetWater).
 */
class FLUIDNINJABUOYANCY_API FFluidNinjaBuoyancySwimmers
{
public:
	/** A swimmer dived: its mesh, where its chest went under (on the surface), how fast (cm/s, straight down) and the body's mass under
	 * the surface at that moment (kg). Per world (OnEntry) and for every world (OnAnyEntry). */
	DECLARE_MULTICAST_DELEGATE_FourParams(FEntryEvent, const USkeletalMeshComponent*, const FVector&, float, float);

	explicit FFluidNinjaBuoyancySwimmers(UFluidNinjaBuoyancySubsystem& InOwner);
	~FFluidNinjaBuoyancySwimmers();

	/** The swimmers of World (its subsystem's), or null before Begin Play. */
	static FFluidNinjaBuoyancySwimmers* Find(const UWorld* World);

	/** As UFluidNinjaBuoyancySubsystem::RegisterSwimmer: Mesh with Source's still water, its parts and spheres from the preset fitting it. */
	void Register(USkeletalMeshComponent* Mesh, const FFluidNinjaSwimmerSource& Source);
	/** Mesh with its own provider (an object implementing IFluidNinjaSwimmerProvider: a UFluidNinjaSwimmerComponent, a pawn; null: the
	 * default pose from the mesh and Profile), its profile (null: the preset fitting the mesh), and bWaterFromQuery: its water from
	 * SampleWater at its hips every frame instead of Source.WaterZ. Registering a mesh again changes these. */
	void Register(USkeletalMeshComponent* Mesh, const FFluidNinjaSwimmerSource& Source, UObject* Provider, const UFluidNinjaSwimmerProfile* Profile, bool bWaterFromQuery);
	void Unregister(USkeletalMeshComponent* Mesh);
	bool IsRegistered(const USkeletalMeshComponent* Mesh) const;
	bool GetStats(const USkeletalMeshComponent* Mesh, FFluidNinjaSwimmerStats& Out) const;
	/** The live water of a registered swimmer (or the player's mesh) as last sampled, and its filtered drift current; false before any. */
	bool GetWater(const USkeletalMeshComponent* Mesh, FFluidNinjaWaterSample& Out, FVector& OutDriftCurrent) const;
	/** The pose a registered swimmer (or the player's mesh) was ticked with this frame; false when it was not. */
	bool GetPose(const USkeletalMeshComponent* Mesh, FFluidNinjaSwimmerPose& Out) const;
	/** The wake tuning a swimmer's wake and its strokes' wake points run on: its provider's Wake Tuning when Override is on (each value
	 * held to its range: Strength and Gain 0-1, Size at least 1 cm, Min Speed at least 0, the ring's inner radius at least 0 and its outer
	 * at least 10 cm past it), else the project's Swimmers settings. The player's unregistered pawn and unregistered meshes get the
	 * project's. */
	FFluidNinjaSwimmerWakeTuning GetWakeTuning(const USkeletalMeshComponent* Mesh) const;
	/** The project's Swimmers settings as a wake tuning (bOverride false). */
	static FFluidNinjaSwimmerWakeTuning ProjectWakeTuning();

	/** Every frame. PlayerMesh: the player's pawn's mesh (null: none, or not in the water), on still water at PlayerWaterZ: its pose is the
	 * default one (the pawn's Character Movement, the preset fitting the mesh). */
	void Tick(float DeltaSeconds, USkeletalMeshComponent* PlayerMesh, float PlayerWaterZ);
	/** Every frame. PlayerPose: the player's pawn's pose this frame (null: none, or not in the water), on water at PlayerWaterZ (the surface
	 * its parts meet: with the waves when the caller has them). */
	void Tick(float DeltaSeconds, const FFluidNinjaSwimmerPose* PlayerPose, float PlayerWaterZ);

	/** The registered swimmers' spheres pushing the water aside this frame (after Tick). */
	const TArray<FFluidNinjaSwimmerSphere>& GetSpheres() const { return Spheres; }

	/** The registered swimmers whose mesh is shown, each with its source (WaterZ: the live still water for a swimmer on the water query). */
	void GetRegistered(TArray<TPair<USkeletalMeshComponent*, FFluidNinjaSwimmerSource>>& Out) const;

	/** The strokes' wake points this frame (after Tick; already written to the Wake Data Channel by Tick). */
	const TArray<FFluidNinjaSwimmerStrokePoint>& GetStrokePoints() const { return StrokePoints; }
	int32 GetStrokeWakePoints() const { return LastStrokePoints; }

	/** Dives in this world. */
	FEntryEvent OnEntry;
	/** Dives in every world (a component binds this and filters by its mesh). */
	static FEntryEvent& OnAnyEntry();

private:
	struct FPart
	{
		FVector Previous = FVector::ZeroVector;
		bool bHasPrevious = false;
	};

	struct FEntry
	{
		FVector2D Where = FVector2D::ZeroVector;
		double Time = 0.0;
	};

	struct FSwimmer
	{
		TWeakObjectPtr<USkeletalMeshComponent> Mesh;
		FFluidNinjaSwimmerSource Source;
		bool bPlayer = false;
		/** Its provider and profile (Register), and whether its water is sampled (bWaterFromQuery). */
		TWeakObjectPtr<UObject> Provider;
		TWeakObjectPtr<const UFluidNinjaSwimmerProfile> Profile;
		bool bWaterFromQuery = false;
		/** The parts' state, by index into the pose's parts; PartBones says which pose they were for (a change starts them over). */
		TArray<FPart> Parts;
		TArray<FName> PartBones;
		/** The spheres' last centres (head, chest, hips), for their velocity. */
		TArray<FVector> SphereWas;
		bool bHasSpheres = false;
		FFluidNinjaSwimmerStats Stats;
		TArray<double> EntryTimes;
		TArray<FEntry> Recent;
		/** Since when no part has been under the water (-1: some part is); a new burst starts after half a second out. */
		double OutSince = 0.0;
		bool bNewBurst = true;
		/** This frame's pose (bValid while it was ticked), and the water it was ticked on. */
		FFluidNinjaSwimmerPose Pose;
		FFluidNinjaWaterSample Water;
		FVector Drift = FVector::ZeroVector;
		bool bHasWater = false;
		/** The chest against the surface last frame, for the dive. */
		bool bChestKnown = false;
		bool bChestWasUnder = false;
		float ChestWasZ = 0.f;
	};

	/** The swimmer's pose this frame: its provider's, else the default from its mesh and profile; false: not a swimmer this frame. */
	bool PoseFor(FSwimmer& Swimmer, USkeletalMeshComponent& Mesh, FFluidNinjaSwimmerPose& Out) const;
	/** The water the swimmer's parts meet and its spheres stand in this frame: the sample at its hips (bWaterFromQuery), else the pose's
	 * Water Z (bHasWaterZ), else Source.WaterZ. */
	void WaterFor(FSwimmer& Swimmer, const FFluidNinjaSwimmerPose& Pose, float DeltaSeconds, float& OutSurfaceZ, float& OutStillZ);
	/** The still water of a swimmer not on the water query: its pose's Water Z when its provider gave one (bHasWaterZ), else its
	 * registration's. */
	static float FixedWaterZ(const FSwimmer& Swimmer);
	void TickSwimmer(FSwimmer& Swimmer, const FFluidNinjaSwimmerPose& Pose, float WaterZ, float DeltaSeconds, double Now);
	/** The chest going under: the dive event. */
	void TickDive(FSwimmer& Swimmer, const FFluidNinjaSwimmerPose& Pose, float WaterZ, float DeltaSeconds);
	/** The stroke's push on the water: a wake point and a small impulse. */
	void TickStroke(FSwimmer& Swimmer, const FFluidNinjaSwimmerPose& Pose, float StillZ, float DeltaSeconds);
	void WriteStrokePoints();
	/** A splash of Strength (the splash system's size) at Where; nothing below the smallest worth drawing. */
	void Splash(const FVector& Where, float Strength);
	const FSwimmer* FindSwimmer(const USkeletalMeshComponent* Mesh) const;
	const UFluidNinjaSwimmerProfile* ProfileOf(const FSwimmer& Swimmer, const USkeletalMeshComponent* Mesh) const;

	TWeakObjectPtr<UFluidNinjaBuoyancySubsystem> Owner;
	TWeakObjectPtr<const UWorld> World;
	TArray<FSwimmer> Registered;
	FSwimmer Player;
	TArray<FFluidNinjaSwimmerSphere> Spheres;
	TArray<FFluidNinjaSwimmerStrokePoint> StrokePoints;
	int32 LastStrokePoints = 0;
	TStrongObjectPtr<UNiagaraSystem> SplashSystem;
	bool bSplashLoaded = false;
	/** The Wake Data Channel the strokes write to (the subsystem holds the asset; looked up again when it is gone). */
	TWeakObjectPtr<UNiagaraDataChannelAsset> WakeChannel;
	bool bWakeChannelLoaded = false;
};
