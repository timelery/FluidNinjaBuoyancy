using UnrealBuildTool;

public class FluidNinjaBuoyancyDeveloper : ModuleRules
{
    public FluidNinjaBuoyancyDeveloper(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        // The test bench (fnb.Test, fnb.TestHUD): a DeveloperTool module, so it is compiled and loaded wherever the engine's developer tools
        // are (the editor, Play In Editor and -game, Debug and Development game builds) and never in Test or Shipping ones; a Test build
        // that wants it sets bBuildDeveloperTools in its Target.cs. It reaches the water through the runtime module's public
        // FluidNinjaBuoyancyBench.h (the host interface) and the component's public API; nothing of it is known to the runtime.
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
        PrivateDependencyModuleNames.AddRange(new[] { "FluidNinjaBuoyancy", "PhysicsCore" });
        // The panel on the game viewport, its hotkeys (an input preprocessor), and the results written as JSON.
        PrivateDependencyModuleNames.AddRange(new[] { "Slate", "SlateCore", "InputCore", "Json" });
    }
}
