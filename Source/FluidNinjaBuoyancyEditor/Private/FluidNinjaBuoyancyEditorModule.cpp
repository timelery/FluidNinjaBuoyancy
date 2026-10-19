#include "FluidNinjaBuoyancyEditorLog.h"
#include "FluidNinjaBuoyancyPresetCustomization.h"
#include "FluidNinjaBuoyancySetupCheck.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserModule.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "FluidNinjaBuoyancyPreset.h"
#include "FluidNinjaBuoyancyPresetLibrary.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "IContentBrowserSingleton.h"
#include "MessageLogInitializationOptions.h"
#include "MessageLogModule.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Textures/SlateIcon.h"
#include "ToolMenus.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "FluidNinjaBuoyancyEditor"

DEFINE_LOG_CATEGORY(LogFluidNinjaBuoyancyEditor);

namespace
{
	/** The component class the Preset panel is registered for, by name: the class itself need not be around when this module unloads. */
	const TCHAR* ComponentClassName = TEXT("FluidNinjaBuoyancyComponent");
	/** The editor's right-click menu on actors, in the viewport and the Outliner. */
	const TCHAR* ActorContextMenuName = TEXT("LevelEditor.ActorContextMenu");
	/** The level editor's Tools menu, where Check Setup goes. */
	const TCHAR* ToolsMenuName = TEXT("LevelEditor.MainMenu.Tools");
	/** Where the plugin keeps the presets it ships. */
	const TCHAR* PresetsFolder = TEXT("/FluidNinjaBuoyancy/Presets");

	/** fnb.CheckSetup: Check Setup from the console, on the world in Play when there is one (its copies: a Fix there lasts until Play ends),
	 * else on the level. */
	FAutoConsoleCommandWithWorldAndArgs CheckSetupCommand(TEXT("fnb.CheckSetup"),
		TEXT("FluidNinja Buoyancy: checks the modified core, every NinjaLive's switches, the Wave Sims, the surface tags and the physics settings, ")
		TEXT("into the FluidNinja Buoyancy message log, with a Fix on each failing line (Tools → FluidNinja Buoyancy: Check Setup does the same on the level). ")
		TEXT("In Play it checks the Play world (a Fix there lasts until Play ends), else the level. Editor only; no arguments."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			FluidNinjaBuoyancySetupCheck::Run(World && World->IsGameWorld() ? World : nullptr);
		}));

	/** One short notification with nothing to report in detail: no actor selected, a preset that would not load. */
	void ShowNotice(const FText& Text, SNotificationItem::ECompletionState State)
	{
		FNotificationInfo Info(Text);
		Info.bUseLargeFont = false;
		Info.ExpireDuration = 4.f;
		TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
		if (Item.IsValid())
		{
			Item->SetCompletionState(State);
		}
	}
}

void FluidNinjaBuoyancyEditorReport::Show(const FString& PresetName, const FText& What, const TArray<FResult>& Results)
{
	int32 Applied = 0;
	TArray<FString> Failures;
	for (const FResult& Result : Results)
	{
		// The library names the object itself in most of its reports; the target is put in front only where it does not.
		const FString Line = Result.Report.StartsWith(Result.Target + TEXT(":")) ? Result.Report : FString::Printf(TEXT("%s: %s"), *Result.Target, *Result.Report);
		if (Result.bApplied)
		{
			++Applied;
			UE_LOG(LogFluidNinjaBuoyancyEditor, Log, TEXT("%s applied. %s"), *PresetName, *Line);
		}
		else
		{
			Failures.Add(Line);
			UE_LOG(LogFluidNinjaBuoyancyEditor, Warning, TEXT("%s not applied. %s"), *PresetName, *Line);
		}
	}

	FNotificationInfo Info(FText::GetEmpty());
	Info.bUseLargeFont = false;
	if (Failures.IsEmpty())
	{
		Info.Text = FText::Format(LOCTEXT("Applied", "Applied {0} to {1} {2}"), FText::FromString(PresetName), FText::AsNumber(Applied), What);
		Info.ExpireDuration = 4.f;
	}
	else
	{
		// Partly applied or not at all: the count up top, the library's reason for each object below it.
		Info.Text = FText::Format(LOCTEXT("NotApplied", "{0} not applied to {1} of {2} {3}"), FText::FromString(PresetName), FText::AsNumber(Failures.Num()), FText::AsNumber(Results.Num()), What);
		Info.SubText = FText::FromString(FString::Join(Failures, TEXT("\n")));
		Info.Image = FAppStyle::GetBrush("Icons.WarningWithColor");
		Info.bUseSuccessFailIcons = false;
		Info.ExpireDuration = 8.f;
	}
	TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
	if (Item.IsValid())
	{
		Item->SetCompletionState(Failures.IsEmpty() ? SNotificationItem::CS_Success : SNotificationItem::CS_None);
	}
}

/**
 * The editor's side of the presets: the actor context menu's FluidNinja Buoyancy → Apply Preset submenu, listing every preset asset the
 * asset registry knows, and the Preset panel at the top of a FluidNinja Buoyancy component's details. And the setup check: Tools →
 * FluidNinja Buoyancy: Check Setup (fnb.CheckSetup), whose lines go to the FluidNinja Buoyancy message log.
 */
class FFluidNinjaBuoyancyEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropertyEditor.RegisterCustomClassLayout(ComponentClassName, FOnGetDetailCustomizationInstance::CreateStatic(&FFluidNinjaBuoyancyPresetCustomization::MakeInstance));
		PropertyEditor.NotifyCustomizationModuleChanged();
		bCustomizationRegistered = true;

		// The message log Check Setup writes to: a page per run, so the last run's lines and Fix actions stay readable.
		FMessageLogModule& MessageLog = FModuleManager::LoadModuleChecked<FMessageLogModule>("MessageLog");
		FMessageLogInitializationOptions LogOptions;
		LogOptions.bShowPages = true;
		LogOptions.bShowFilters = true;
		LogOptions.bAllowClear = true;
		MessageLog.RegisterLogListing(FluidNinjaBuoyancySetupCheck::LogName, LOCTEXT("MessageLogLabel", "FluidNinja Buoyancy"), LogOptions);
		bLogRegistered = true;

		// At PostEngineInit the menus are usually up and the callback runs at once; when they are not yet, ToolMenus holds it until they are.
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FFluidNinjaBuoyancyEditorModule::RegisterMenus));
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);

		// PropertyEditor and MessageLog may already be gone when the editor shuts down; then there is nothing left to unregister from.
		if (bCustomizationRegistered && FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
		{
			FPropertyEditorModule& PropertyEditor = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
			PropertyEditor.UnregisterCustomClassLayout(ComponentClassName);
			PropertyEditor.NotifyCustomizationModuleChanged();
		}
		bCustomizationRegistered = false;
		if (bLogRegistered && FModuleManager::Get().IsModuleLoaded("MessageLog"))
		{
			FModuleManager::GetModuleChecked<FMessageLogModule>("MessageLog").UnregisterLogListing(FluidNinjaBuoyancySetupCheck::LogName);
		}
		bLogRegistered = false;
	}

private:
	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);
		// Tools → FluidNinja Buoyancy: Check Setup, on the level in the editor.
		UToolMenu* Tools = UToolMenus::Get()->ExtendMenu(ToolsMenuName);
		FToolMenuSection& ToolsSection = Tools->AddSection("FluidNinjaBuoyancy", LOCTEXT("ToolsSectionHeading", "FluidNinja Buoyancy"));
		ToolsSection.AddMenuEntry(
			"CheckSetup",
			LOCTEXT("CheckSetup", "FluidNinja Buoyancy: Check Setup"),
			LOCTEXT("CheckSetupTooltip", "Checks what the plugin needs of this project and level: the modified NinjaLiveCore, every NinjaLive's switches (relative tracking, tracked density, the data channel reader, its tagged surface), the Wave Sims, the surface tags and the physics settings. Each line is PASS, FAIL or WARN in the FluidNinja Buoyancy message log, with a Fix where a property or a tag puts it right."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Validate"),
			FUIAction(FExecuteAction::CreateLambda([] { FluidNinjaBuoyancySetupCheck::Run(nullptr); })));

		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(ActorContextMenuName);
		FToolMenuSection& Section = Menu->AddSection("FluidNinjaBuoyancy", LOCTEXT("SectionHeading", "FluidNinja Buoyancy"));
		Section.AddSubMenu(
			"ApplyPreset",
			LOCTEXT("ApplyPreset", "Apply Preset"),
			LOCTEXT("ApplyPresetTooltip", "Makes the selected actors float as a FluidNinja Buoyancy preset says: each gets a buoyancy component when it has none, and the hull, mass and attitude are built from its own mesh."),
			FNewToolMenuChoice(FNewToolMenuDelegate::CreateStatic(&FFluidNinjaBuoyancyEditorModule::BuildApplyPresetMenu)),
			/*bInOpenSubMenuOnClick*/ false,
			TAttribute<FSlateIcon>(FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.DataAsset")));
		Section.AddMenuEntry(
			"OpenPresetsFolder",
			LOCTEXT("OpenPresetsFolder", "Open Presets Folder"),
			LOCTEXT("OpenPresetsFolderTooltip", "Shows the plugin's presets in the Content Browser."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.FolderOpen"),
			FUIAction(FExecuteAction::CreateStatic(&FFluidNinjaBuoyancyEditorModule::OpenPresetsFolder)));
	}

	/** Fills the Apply Preset submenu each time it opens: every preset asset, by name, with its Description as the tooltip. */
	static void BuildApplyPresetMenu(UToolMenu* Menu)
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(UFluidNinjaBuoyancyPreset::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		TArray<FAssetData> Presets;
		FAssetRegistryModule::GetRegistry().GetAssets(Filter, Presets);
		Presets.Sort([](const FAssetData& A, const FAssetData& B) { return A.AssetName.LexicalLess(B.AssetName); });

		FToolMenuSection& Section = Menu->AddSection("Presets");
		if (Presets.IsEmpty())
		{
			Section.AddMenuEntry(
				"NoPresets",
				LOCTEXT("NoPresets", "No presets found"),
				LOCTEXT("NoPresetsTooltip", "Make one with Content Browser → Miscellaneous → Data Asset → FluidNinja Buoyancy Preset."),
				FSlateIcon(),
				FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([] { return false; })));
			return;
		}

		for (const FAssetData& PresetAsset : Presets)
		{
			// A preset already in memory lends its Description; one still on disk is not loaded just to open a menu, its path stands in.
			FText ToolTip = FText::FromString(PresetAsset.GetObjectPathString());
			if (const UFluidNinjaBuoyancyPreset* Loaded = Cast<UFluidNinjaBuoyancyPreset>(PresetAsset.FastGetAsset(false)))
			{
				if (!Loaded->Description.IsEmpty())
				{
					ToolTip = Loaded->Description;
				}
			}
			Section.AddMenuEntry(
				PresetAsset.PackageName,
				FText::FromName(PresetAsset.AssetName),
				ToolTip,
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([PresetAsset] { ApplyPresetToSelectedActors(PresetAsset); })));
		}
	}

	/** Applies one preset to every selected actor, as one undo step, and reports. */
	static void ApplyPresetToSelectedActors(const FAssetData& PresetAsset)
	{
		const UFluidNinjaBuoyancyPreset* Preset = Cast<UFluidNinjaBuoyancyPreset>(PresetAsset.GetAsset());
		if (!Preset)
		{
			UE_LOG(LogFluidNinjaBuoyancyEditor, Warning, TEXT("Preset %s would not load"), *PresetAsset.GetObjectPathString());
			ShowNotice(FText::Format(LOCTEXT("PresetNotLoaded", "{0} would not load"), FText::FromName(PresetAsset.AssetName)), SNotificationItem::CS_Fail);
			return;
		}

		TArray<AActor*> Actors;
		if (GEditor)
		{
			GEditor->GetSelectedActors()->GetSelectedObjects<AActor>(Actors);
		}
		if (Actors.IsEmpty())
		{
			ShowNotice(LOCTEXT("NoActorSelected", "No actor selected"), SNotificationItem::CS_Fail);
			return;
		}

		TArray<FluidNinjaBuoyancyEditorReport::FResult> Results;
		{
			FScopedTransaction Transaction(FText::Format(LOCTEXT("ApplyPresetTransaction", "Apply Buoyancy Preset {0}"), FText::FromName(PresetAsset.AssetName)));
			for (AActor* Actor : Actors)
			{
				FluidNinjaBuoyancyEditorReport::FResult& Result = Results.AddDefaulted_GetRef();
				Result.Target = Actor->GetActorNameOrLabel();
				Result.bApplied = UFluidNinjaBuoyancyPresetLibrary::ApplyPreset(Actor, Preset, Result.Report);
			}
		}
		FluidNinjaBuoyancyEditorReport::Show(PresetAsset.AssetName.ToString(), LOCTEXT("Actors", "actor(s)"), Results);
	}

	static void OpenPresetsFolder()
	{
		TArray<FString> Folders;
		Folders.Add(PresetsFolder);
		FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().SyncBrowserToFolders(Folders);
	}

	bool bCustomizationRegistered = false;
	bool bLogRegistered = false;
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFluidNinjaBuoyancyEditorModule, FluidNinjaBuoyancyEditor)
