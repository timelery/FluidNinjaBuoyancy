#include "FluidNinjaBuoyancyPresetLibrary.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "FluidNinjaBuoyancyComponent.h"
#include "FluidNinjaBuoyancyPreset.h"
#include "GameFramework/Actor.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"
#if WITH_EDITOR
#include "Kismet/KismetSystemLibrary.h"
#endif

namespace
{
	/**
	 * What a preset reads of the object: its static mesh component, the mesh's own box, the component's scale, and the volume of its
	 * simple collision.
	 *
	 * Frames. The component builds its hull from the collision with every vertex multiplied by the component's scale, and the physics
	 * step moves it with a scale-free transform (rotation and translation only), so a pontoon's Location and Radius are world
	 * centimetres along the body's local axes: a 100 cm sphere mesh at scale 0.139 floats on one pontoon of radius 6.95 at the origin.
	 * HalfExtent below is in that frame. Center Of Mass Offset is not: the component hands it to the body instance's COM nudge, which
	 * the engine multiplies by the component's scale when it works out the mass properties, so it is mesh centimetres before the scale.
	 */
	struct FMeshGeometry
	{
		UStaticMeshComponent* Mesh = nullptr;
		/** The mesh's bounding box, as modelled (before the component's scale). */
		FBox LocalBox = FBox(ForceInit);
		/** The component's world scale, signs kept: positions scale with it as the hull's vertices do. */
		FVector Scale = FVector::OneVector;
		/** Half the box along each local axis, in world centimetres. */
		FVector HalfExtent = FVector::ZeroVector;
		/** The volume of the simple collision at the component's scale, cubic centimetres. */
		double VolumeCm3 = 0.0;
		/** The local axis (0 X, 1 Y, 2 Z) the object is longest along, in world centimetres. */
		int32 LongestAxis = 0;
	};

	const TCHAR* AxisName(int32 Axis)
	{
		return Axis == 0 ? TEXT("X") : Axis == 1 ? TEXT("Y") : TEXT("Z");
	}

	FVector AxisUnit(int32 Axis)
	{
		FVector Unit = FVector::ZeroVector;
		Unit[Axis] = 1.0;
		return Unit;
	}

	/** Reads the geometry from the primitive the component floats, or says why it cannot. */
	bool ReadGeometry(UPrimitiveComponent* Primitive, FMeshGeometry& Out, FString& OutWhy)
	{
		if (!Primitive)
		{
			OutWhy = TEXT("no primitive to float (set Primitive Name, or give the actor a root primitive)");
			return false;
		}
		UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Primitive);
		if (!Mesh)
		{
			OutWhy = FString::Printf(TEXT("%s is a %s, not a static mesh component: a preset builds the hull from a static mesh"), *Primitive->GetName(),
				*Primitive->GetClass()->GetName());
			return false;
		}
		UStaticMesh* Asset = Mesh->GetStaticMesh();
		if (!Asset)
		{
			OutWhy = FString::Printf(TEXT("%s has no static mesh to read"), *Mesh->GetName());
			return false;
		}
		const FVector Scale = Mesh->GetComponentTransform().GetScale3D();
		const UBodySetup* Setup = Mesh->GetBodySetup();
		const double Volume = Setup ? static_cast<double>(Setup->AggGeom.GetScaledVolume(Scale)) : 0.0;
		if (Volume <= 0.0)
		{
			OutWhy = FString::Printf(TEXT("%s needs simple collision (%s has none with a volume: add a box, sphere, capsule or convex hull to the mesh)"),
				*Mesh->GetName(), *Asset->GetName());
			return false;
		}
		Out.Mesh = Mesh;
		Out.LocalBox = Asset->GetBoundingBox();
		Out.Scale = Scale;
		Out.HalfExtent = Out.LocalBox.GetExtent() * Scale.GetAbs();
		Out.VolumeCm3 = Volume;
		Out.LongestAxis = 0;
		for (int32 Axis = 1; Axis < 3; ++Axis)
		{
			if (Out.HalfExtent[Axis] > Out.HalfExtent[Out.LongestAxis])
			{
				Out.LongestAxis = Axis;
			}
		}
		return true;
	}

	FString Litres(double VolumeCm3)
	{
		const double Value = VolumeCm3 / 1000.0;
		return Value >= 10.0 ? FString::Printf(TEXT("%.0f L"), Value) : FString::Printf(TEXT("%.2f L"), Value);
	}

#if WITH_EDITOR
	/**
	 * One undo step around the writes; a nested one joins the step already open. The Engine module opens it through the engine's own
	 * transaction calls, so no editor module is linked for it.
	 */
	struct FPresetUndoStep
	{
		explicit FPresetUndoStep(UObject* Primary)
		{
			UKismetSystemLibrary::BeginTransaction(TEXT("FluidNinjaBuoyancy"), NSLOCTEXT("FluidNinjaBuoyancy", "ApplyPreset", "Apply Buoyancy Preset"), Primary);
		}
		~FPresetUndoStep()
		{
			UKismetSystemLibrary::EndTransaction();
		}
	};
#endif
}

bool UFluidNinjaBuoyancyPresetLibrary::ApplyPreset(AActor* Actor, const UFluidNinjaBuoyancyPreset* Preset, FString& OutReport)
{
	if (!Actor)
	{
		OutReport = TEXT("no actor to apply the preset to");
		return false;
	}
	if (!Preset)
	{
		OutReport = TEXT("no preset to apply");
		return false;
	}
	UFluidNinjaBuoyancyComponent* Component = Actor->FindComponentByClass<UFluidNinjaBuoyancyComponent>();
	if (Component)
	{
		return ApplyPresetToComponent(Component, Preset, OutReport);
	}

	// No component yet: a new one floats the root, so the root is checked before anything is added.
	FMeshGeometry Geometry;
	FString Why;
	if (!ReadGeometry(Cast<UPrimitiveComponent>(Actor->GetRootComponent()), Geometry, Why))
	{
		OutReport = FString::Printf(TEXT("%s: %s"), *Actor->GetActorNameOrLabel(), *Why);
		return false;
	}
#if WITH_EDITOR
	FPresetUndoStep Undo(Actor);
	Actor->Modify();
#endif
	Component = NewObject<UFluidNinjaBuoyancyComponent>(Actor, MakeUniqueObjectName(Actor, UFluidNinjaBuoyancyComponent::StaticClass(), TEXT("FluidNinjaBuoyancy")), RF_Transactional);
	// An instance component, so a placed actor saves it with the level; registered when the actor's other components are.
	Actor->AddInstanceComponent(Component);
	if (Actor->HasActorRegisteredAllComponents())
	{
		Component->RegisterComponent();
	}
	if (!ApplyPresetToComponent(Component, Preset, OutReport))
	{
		Component->DestroyComponent();
		return false;
	}
	OutReport = TEXT("added FluidNinja Buoyancy; ") + OutReport;
	return true;
}

bool UFluidNinjaBuoyancyPresetLibrary::ApplyPresetToComponent(UFluidNinjaBuoyancyComponent* Component, const UFluidNinjaBuoyancyPreset* Preset, FString& OutReport)
{
	if (!Component)
	{
		OutReport = TEXT("no component to apply the preset to");
		return false;
	}
	if (!Preset)
	{
		OutReport = TEXT("no preset to apply");
		return false;
	}
	AActor* Owner = Component->GetOwner();
	if (!Owner)
	{
		OutReport = TEXT("the component has no owner, so there is no mesh to read");
		return false;
	}
	const FString Name = Owner->GetActorNameOrLabel();
	// The same primitive the component floats: the owner's component named Primitive Name, else the root.
	FMeshGeometry Geometry;
	FString Why;
	if (!ReadGeometry(Component->GetSimulatedPrimitive(), Geometry, Why))
	{
		OutReport = FString::Printf(TEXT("%s: %s"), *Name, *Why);
		return false;
	}
	UStaticMeshComponent& Mesh = *Geometry.Mesh;

#if WITH_EDITOR
	FPresetUndoStep Undo(Component);
	Component->Modify();
	Mesh.Modify();
#endif

	// ---- The hull, from the mesh's box (the spheres) or left to the component's own builders (the collision surface, the voxels).
	const int32 Longest = Geometry.LongestAxis;
	const FVector& Half = Geometry.HalfExtent;
	// The box's centre as the hull sees it: scaled like the hull's vertices.
	const FVector Centre = Geometry.LocalBox.GetCenter() * Geometry.Scale;
	FString HullText;
	switch (Preset->Hull)
	{
	case EFluidNinjaPresetHull::FromCollision:
		Component->HullModel = EFluidNinjaHullModel::Triangles;
		Component->HullResolutionCm = FMath::Max(Preset->HullResolutionCm, 2.f);
		HullText = FString::Printf(TEXT("hull from collision at %.0f cm"), Component->HullResolutionCm);
		break;

	case EFluidNinjaPresetHull::Voxels:
		Component->HullModel = EFluidNinjaHullModel::Voxels;
		Component->VoxelCount = FMath::Clamp(Preset->VoxelCount, 1, 512);
		Component->VoxelSizeCm = 0.f;
		HullText = FString::Printf(TEXT("%d voxels"), Component->VoxelCount);
		break;

	case EFluidNinjaPresetHull::OneSphere:
	{
		Component->HullModel = EFluidNinjaHullModel::Spheres;
		const float Radius = FMath::Max(1.f, static_cast<float>((Half.X + Half.Y + Half.Z) / 3.0) * Preset->SphereRadiusScale);
		Component->Pontoons.Reset();
		FFluidNinjaPontoon& Pontoon = Component->Pontoons.AddDefaulted_GetRef();
		Pontoon.Location = Centre;
		Pontoon.Radius = Radius;
		HullText = FString::Printf(TEXT("one sphere of %.1f cm"), Radius);
		break;
	}

	case EFluidNinjaPresetHull::SphereRow:
	{
		// Along the longest axis, each sphere as thick as the object's thinner side, the end spheres just inside the box.
		Component->HullModel = EFluidNinjaHullModel::Spheres;
		const int32 Count = FMath::Max(2, Preset->SphereCount);
		const int32 SideA = (Longest + 1) % 3;
		const int32 SideB = (Longest + 2) % 3;
		const float Radius = FMath::Max(1.f, static_cast<float>(FMath::Min(Half[SideA], Half[SideB])) * Preset->SphereRadiusScale);
		const double Reach = FMath::Max(Half[Longest] - Radius, 0.0);
		const FVector Along = AxisUnit(Longest);
		Component->Pontoons.Reset();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const double Fraction = -1.0 + 2.0 * Index / (Count - 1);
			FFluidNinjaPontoon& Pontoon = Component->Pontoons.AddDefaulted_GetRef();
			Pontoon.Location = Centre + Along * (Fraction * Reach);
			Pontoon.Radius = Radius;
		}
		HullText = FString::Printf(TEXT("%d spheres of %.1f cm in a row along %s"), Count, Radius, AxisName(Longest));
		break;
	}

	case EFluidNinjaPresetHull::SphereRing:
	{
		// The ring lies in the plane of the two longer axes; the tube is as thick as the shortest; the ring's radius reaches the box's
		// edge less the tube.
		Component->HullModel = EFluidNinjaHullModel::Spheres;
		const int32 Count = FMath::Max(2, Preset->SphereCount);
		const int32 OtherA = (Longest + 1) % 3;
		const int32 OtherB = (Longest + 2) % 3;
		const int32 Second = Half[OtherA] >= Half[OtherB] ? OtherA : OtherB;
		const int32 Shortest = Second == OtherA ? OtherB : OtherA;
		const float Radius = FMath::Max(1.f, static_cast<float>(Half[Shortest]) * Preset->SphereRadiusScale);
		const double Ring = FMath::Max(Half[Longest] - Radius, 0.0);
		const FVector Across = AxisUnit(Longest);
		const FVector Along = AxisUnit(Second);
		Component->Pontoons.Reset();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const double Angle = 2.0 * UE_DOUBLE_PI * Index / Count;
			FFluidNinjaPontoon& Pontoon = Component->Pontoons.AddDefaulted_GetRef();
			Pontoon.Location = Centre + Across * (FMath::Cos(Angle) * Ring) + Along * (FMath::Sin(Angle) * Ring);
			Pontoon.Radius = Radius;
		}
		HullText = FString::Printf(TEXT("%d spheres of %.1f cm around a %.0f cm ring in the %s%s plane"), Count, Radius, Ring, AxisName(Longest), AxisName(Second));
		break;
	}
	}

	// ---- The mass: from the density and the collision's volume, from the rest draft, or the body's own.
	FString MassText;
	switch (Preset->Mass)
	{
	case EFluidNinjaPresetMass::Density:
	{
		Component->MassMode = EFluidNinjaBuoyancyMass::BodyMass;
		const float Mass = FMath::Max(0.001f, static_cast<float>(Preset->DensityKgM3 * Geometry.VolumeCm3 * 1e-6));
		// The primitive's own body instance (not a weld parent's), so the override is saved with the mesh component.
		if (FBodyInstance* Body = Mesh.GetBodyInstance(NAME_None, false))
		{
			Body->SetMassOverride(Mass, true);
			if (Body->IsValidBodyInstance())
			{
				// The physics body exists (a placed actor in the editor, or in play): it takes the new mass now. Otherwise the body reads the
				// override when it is created.
				Body->UpdateMassProperties();
			}
		}
		MassText = FString::Printf(TEXT("%.0f kg/m3 -> %.1f kg"), Preset->DensityKgM3, Mass);
		break;
	}

	case EFluidNinjaPresetMass::RestDraft:
		Component->MassMode = EFluidNinjaBuoyancyMass::RestDraft;
		Component->RestDraftCm = FMath::Max(Preset->RestDraftCm, 0.5f);
		MassText = FString::Printf(TEXT("weighs what it displaces %.1f cm deep"), Component->RestDraftCm);
		break;

	case EFluidNinjaPresetMass::KeepBodyMass:
	{
		Component->MassMode = EFluidNinjaBuoyancyMass::BodyMass;
		const FBodyInstance* Body = Mesh.GetBodyInstance(NAME_None, false);
		MassText = Body && Body->bOverrideMass ? FString::Printf(TEXT("keeps its %.1f kg"), Body->GetMassOverride()) : TEXT("keeps its own mass");
		break;
	}
	}

	// ---- The attitude: which way the body's frame turns to rest. The rotation is built from the axes, never from rotator angles.
	FQuat Attitude = FQuat::Identity;
	FString AttitudeText;
	switch (Preset->Attitude)
	{
	case EFluidNinjaPresetAttitude::AsPlaced:
		AttitudeText = TEXT("rests as placed");
		break;

	case EFluidNinjaPresetAttitude::LongestAxisFlat:
		// Only a mesh modelled standing up (longest along Z) is turned: its Z goes onto X. A mesh long along X or Y lies flat already.
		if (Longest == 2)
		{
			Attitude = FQuat::FindBetweenNormals(FVector::ZAxisVector, FVector::XAxisVector);
		}
		AttitudeText = FString::Printf(TEXT("rests with its %s axis flat"), AxisName(Longest));
		break;

	case EFluidNinjaPresetAttitude::LongestAxisUp:
		// The longest axis goes onto Z, its positive end up.
		if (Longest != 2)
		{
			Attitude = FQuat::FindBetweenNormals(AxisUnit(Longest), FVector::ZAxisVector);
		}
		AttitudeText = FString::Printf(TEXT("rests with its %s axis up"), AxisName(Longest));
		break;
	}
	Component->RestAttitude = Attitude.Rotator();
	if (!Attitude.IsIdentity(KINDA_SMALL_NUMBER))
	{
		AttitudeText += FString::Printf(TEXT(" (turned %s)"), *Component->RestAttitude.ToCompactString());
	}

	// ---- The centre of mass, along the longest axis. The offset is the body instance's COM nudge, which the engine multiplies by the
	// component's scale, so it is written in mesh centimetres before the scale: the shift then lands at the share of the scaled half-length
	// the preset asks for.
	const double UnscaledHalf = Geometry.LocalBox.GetExtent()[Longest];
	Component->CenterOfMassOffset = AxisUnit(Longest) * (Preset->CentreOfMassShift * UnscaledHalf);
	FString BalanceText;
	if (!FMath::IsNearlyZero(Preset->CentreOfMassShift))
	{
		BalanceText = FString::Printf(TEXT("; centre of mass %.0f%% toward %s%s"), FMath::Abs(Preset->CentreOfMassShift) * 100.f, Preset->CentreOfMassShift > 0.f ? TEXT("+") : TEXT("-"),
			AxisName(Longest));
	}

	// ---- Everything else is copied as it is.
	Component->bSettleOnStart = Preset->bSettleOnStart;
	Component->bSleepWhenSettled = Preset->bSleepWhenSettled;
	Component->PressureDragLinear = Preset->PressureDragLinear;
	Component->PressureDragQuadratic = Preset->PressureDragQuadratic;
	Component->SuctionDragLinear = Preset->SuctionDragLinear;
	Component->SuctionDragQuadratic = Preset->SuctionDragQuadratic;
	Component->AirDragCoefficient = Preset->AirDragCoefficient;
	if (Preset->bSetInteraction)
	{
		Component->Interaction = Preset->Interaction;
	}
	Component->bWake = Preset->bWake;
	Component->bWetLine = Preset->bWetLine;
	Component->SplashMinWatts = Preset->SplashMinWatts;
	Component->MaxSplashStrength = FMath::Max(Preset->MaxSplashStrength, 0.2f);
	Component->SplashScale = Preset->SplashScale;

#if WITH_EDITOR
	Owner->MarkPackageDirty();
#endif

	OutReport = FString::Printf(TEXT("%s: %s over %s of collision; %s; %s%s"), *Name, *HullText, *Litres(Geometry.VolumeCm3), *MassText, *AttitudeText, *BalanceText);
	return true;
}
