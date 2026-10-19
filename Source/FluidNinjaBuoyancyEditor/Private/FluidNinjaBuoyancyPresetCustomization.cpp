#include "FluidNinjaBuoyancyPresetCustomization.h"

#include "AssetRegistry/AssetData.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "FluidNinjaBuoyancyComponent.h"
#include "FluidNinjaBuoyancyEditorLog.h"
#include "FluidNinjaBuoyancyPreset.h"
#include "FluidNinjaBuoyancyPresetLibrary.h"
#include "GameFramework/Actor.h"
#include "PropertyCustomizationHelpers.h"
#include "ScopedTransaction.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "FluidNinjaBuoyancyEditor"

TSharedRef<IDetailCustomization> FFluidNinjaBuoyancyPresetCustomization::MakeInstance()
{
	return MakeShared<FFluidNinjaBuoyancyPresetCustomization>();
}

void FFluidNinjaBuoyancyPresetCustomization::CustomizeDetails(const TSharedPtr<IDetailLayoutBuilder>& InDetailBuilder)
{
	DetailBuilder = InDetailBuilder;
	CustomizeDetails(*InDetailBuilder);
}

void FFluidNinjaBuoyancyPresetCustomization::CustomizeDetails(IDetailLayoutBuilder& InDetailBuilder)
{
	Components.Reset();
	TArray<TWeakObjectPtr<UObject>> Objects;
	InDetailBuilder.GetObjectsBeingCustomized(Objects);
	for (const TWeakObjectPtr<UObject>& Object : Objects)
	{
		if (UFluidNinjaBuoyancyComponent* Component = Cast<UFluidNinjaBuoyancyComponent>(Object.Get()))
		{
			Components.Add(Component);
		}
	}

	// First in the panel: the preset is where a new component starts.
	IDetailCategoryBuilder& Category = InDetailBuilder.EditCategory("Preset", LOCTEXT("PresetCategory", "Preset"), ECategoryPriority::Important);
	Category.SetSortOrder(0);
	Category.AddCustomRow(LOCTEXT("PresetRowFilter", "Preset Apply"))
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("PresetLabel", "Preset"))
			.ToolTipText(LOCTEXT("PresetTooltip", "A FluidNinja Buoyancy preset to apply to this component: the hull, mass and attitude are built from the owner's mesh, and the drag, wake and splash values are written. Everything stays editable below afterwards."))
			.Font(IDetailLayoutBuilder::GetDetailFont())
		]
		.ValueContent()
		.MinDesiredWidth(300.f)
		.MaxDesiredWidth(600.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			.VAlign(VAlign_Center)
			[
				SNew(SObjectPropertyEntryBox)
				.AllowedClass(UFluidNinjaBuoyancyPreset::StaticClass())
				.ObjectPath(this, &FFluidNinjaBuoyancyPresetCustomization::GetPresetPath)
				.OnObjectChanged(this, &FFluidNinjaBuoyancyPresetCustomization::OnPresetChanged)
				.ThumbnailPool(InDetailBuilder.GetThumbnailPool())
				.DisplayThumbnail(false)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(4.f, 0.f, 0.f, 0.f)
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.Text(LOCTEXT("Apply", "Apply"))
				.ToolTipText(LOCTEXT("ApplyTooltip", "Applies the picked preset to this component (to each of them, when several are selected), as one undo step."))
				.IsEnabled(this, &FFluidNinjaBuoyancyPresetCustomization::CanApply)
				.OnClicked(this, &FFluidNinjaBuoyancyPresetCustomization::OnApplyClicked)
			]
		];
}

FString FFluidNinjaBuoyancyPresetCustomization::GetPresetPath() const
{
	const UFluidNinjaBuoyancyPreset* Picked = Preset.Get();
	return Picked ? Picked->GetPathName() : FString();
}

void FFluidNinjaBuoyancyPresetCustomization::OnPresetChanged(const FAssetData& AssetData)
{
	// Clearing the picker hands over an empty asset: the pick goes back to none.
	Preset = Cast<UFluidNinjaBuoyancyPreset>(AssetData.GetAsset());
}

bool FFluidNinjaBuoyancyPresetCustomization::CanApply() const
{
	if (!Preset.IsValid())
	{
		return false;
	}
	for (const TWeakObjectPtr<UFluidNinjaBuoyancyComponent>& Component : Components)
	{
		if (Component.IsValid())
		{
			return true;
		}
	}
	return false;
}

FReply FFluidNinjaBuoyancyPresetCustomization::OnApplyClicked()
{
	const UFluidNinjaBuoyancyPreset* Picked = Preset.Get();
	if (!Picked)
	{
		return FReply::Handled();
	}

	TArray<FluidNinjaBuoyancyEditorReport::FResult> Results;
	{
		FScopedTransaction Transaction(FText::Format(LOCTEXT("ApplyPresetTransaction", "Apply Buoyancy Preset {0}"), FText::FromString(Picked->GetName())));
		for (const TWeakObjectPtr<UFluidNinjaBuoyancyComponent>& WeakComponent : Components)
		{
			UFluidNinjaBuoyancyComponent* Component = WeakComponent.Get();
			if (!Component)
			{
				continue;
			}
			// Named as the library names it in its reports: by the owner, whose mesh the preset reads.
			const AActor* Owner = Component->GetOwner();
			FluidNinjaBuoyancyEditorReport::FResult& Result = Results.AddDefaulted_GetRef();
			Result.Target = Owner ? Owner->GetActorNameOrLabel() : Component->GetName();
			Result.bApplied = UFluidNinjaBuoyancyPresetLibrary::ApplyPresetToComponent(Component, Picked, Result.Report);
		}
	}
	FluidNinjaBuoyancyEditorReport::Show(Picked->GetName(), LOCTEXT("Components", "component(s)"), Results);

	// The values the preset wrote show once the panel is rebuilt. That rebuild replaces this instance, so nothing of it is used after the call.
	if (TSharedPtr<IDetailLayoutBuilder> Builder = DetailBuilder.Pin())
	{
		Builder->ForceRefreshDetails();
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
