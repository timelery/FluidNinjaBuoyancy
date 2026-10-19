#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "FluidNinjaBuoyancyPresetLibrary.generated.h"

class AActor;
class UFluidNinjaBuoyancyComponent;
class UFluidNinjaBuoyancyPreset;

/** Applies FluidNinja Buoyancy presets to objects: from the editor's menus, from Blueprint, from Python. */
UCLASS()
class FLUIDNINJABUOYANCY_API UFluidNinjaBuoyancyPresetLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Makes Actor float as Preset says: its FluidNinja Buoyancy component (added when it has none) gets the hull the preset builds
	 * from the actor's mesh, the mass, the attitude and the rest. The actor's primitive is the component's Primitive Name, else its
	 * root; it needs simple collision. In the editor the change is one undo step and the actor is marked dirty. False, with the reason
	 * in OutReport, when the actor has no primitive, no collision, or no mesh the preset's hull can be built from; OutReport otherwise
	 * says what was set (the volume found, the mass, the pontoons).
	 */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Buoyancy", meta = (DisplayName = "Apply Buoyancy Preset"))
	static bool ApplyPreset(AActor* Actor, const UFluidNinjaBuoyancyPreset* Preset, FString& OutReport);

	/** The same for a component that already exists (its owner's mesh is read). */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Buoyancy", meta = (DisplayName = "Apply Buoyancy Preset To Component"))
	static bool ApplyPresetToComponent(UFluidNinjaBuoyancyComponent* Component, const UFluidNinjaBuoyancyPreset* Preset, FString& OutReport);
};
