using UnrealBuildTool;

public class FluidNinjaBuoyancyEditor : ModuleRules
{
	public FluidNinjaBuoyancyEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
		// The editor's side of the presets: the actor context menu's Apply Preset submenu (ToolMenus, LevelEditor, the asset registry for
		// the list) and the component's Preset panel (PropertyEditor). ContentBrowser only for the Open Presets Folder entry.
		PrivateDependencyModuleNames.AddRange(new[] { "Slate", "SlateCore", "UnrealEd", "PropertyEditor", "ToolMenus", "LevelEditor", "AssetRegistry", "EditorFramework", "InputCore", "ContentBrowser", "FluidNinjaBuoyancy" });
		// Check Setup (Tools → FluidNinja Buoyancy: Check Setup, fnb.CheckSetup): its PASS / FAIL lines and Fix actions go to a message log page.
		PrivateDependencyModuleNames.AddRange(new[] { "MessageLog" });
		// FNBDevLibrary, the Niagara and material editing helpers Tools/CorePatch/apply_fluidninja_waves.py calls from Python (the
		// external-edit API and the stack utilities of NiagaraEditor, data interfaces of Niagara / NiagaraCore, the GPU script's
		// shader map of NiagaraShader, a material resource compiled by hand through RenderCore / RHI). Editor-only; nothing of it
		// reaches a cooked game.
		PrivateDependencyModuleNames.AddRange(new[] { "Niagara", "NiagaraCore", "NiagaraShader", "NiagaraEditor", "RenderCore", "RHI" });
		// Live-editor material compilation must use the editor's resource ownership/update context.
		PrivateDependencyModuleNames.AddRange(new[] { "MaterialEditor" });
		// FNBDevLibrary's Blueprint variable helpers (tooltips and ranges of member variables): the K2 schema's type names and metadata keys.
		PrivateDependencyModuleNames.AddRange(new[] { "BlueprintGraph" });
		// FNBDevLibrary's user parameter hierarchy helpers (the sections and categories a NinjaLive's User Parameters are grouped by).
		PrivateDependencyModuleNames.AddRange(new[] { "DataHierarchyEditor" });
	}
}
