using UnrealBuildTool;

public class FluidNinjaBuoyancyDemo : ModuleRules
{
    public FluidNinjaBuoyancyDemo(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        // The demo level's player (a third-person character and its game mode) and the river's switch. Nothing in the buoyancy module depends on it.
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "InputCore", "PhysicsCore" });
        // The river switch: its on-screen panel, and the floaters it puts back.
        PrivateDependencyModuleNames.AddRange(new[] { "Slate", "SlateCore", "FluidNinjaBuoyancy" });
    }
}
