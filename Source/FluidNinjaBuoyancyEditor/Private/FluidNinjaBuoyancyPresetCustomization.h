#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "IDetailCustomization.h"
#include "Input/Reply.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtrTemplates.h"

class IDetailLayoutBuilder;
class UFluidNinjaBuoyancyComponent;
class UFluidNinjaBuoyancyPreset;
struct FAssetData;

/**
 * The Preset panel at the top of a FluidNinja Buoyancy component's details: pick a preset, press Apply, and the component gets the
 * hull, mass, attitude and the rest the preset builds from its owner's mesh. The pick lives in this panel, not on the component: a
 * preset is applied, not referenced, and everything it wrote stays editable in the categories below.
 */
class FFluidNinjaBuoyancyPresetCustomization : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	virtual void CustomizeDetails(IDetailLayoutBuilder& InDetailBuilder) override;
	virtual void CustomizeDetails(const TSharedPtr<IDetailLayoutBuilder>& InDetailBuilder) override;

private:
	FString GetPresetPath() const;
	void OnPresetChanged(const FAssetData& AssetData);
	bool CanApply() const;
	FReply OnApplyClicked();

	/** The preset picked in the panel. The panel is rebuilt after Apply, to show what was written, and the pick goes with it. */
	TWeakObjectPtr<UFluidNinjaBuoyancyPreset> Preset;
	/** The components whose details this panel is in: one, or several selected together. */
	TArray<TWeakObjectPtr<UFluidNinjaBuoyancyComponent>> Components;
	/** The layout the panel is in, to rebuild it after Apply. */
	TWeakPtr<IDetailLayoutBuilder> DetailBuilder;
};
