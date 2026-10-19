using UnrealBuildTool;

public class FluidNinjaBuoyancy : ModuleRules
{
    public FluidNinjaBuoyancy(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "DeveloperSettings", "PhysicsCore", "Chaos" });
        // The water as drawn: a depth capture of FluidNinja's surface and the simulation's velocity buffer, read back asynchronously.
        PrivateDependencyModuleNames.AddRange(new[] { "RenderCore", "RHI", "Niagara" });
        // Many bodies: one compute pass gathers every body's water onto a fixed grid (FluidNinjaBuoyancyGather.usf).
        PrivateDependencyModuleNames.AddRange(new[] { "FluidNinjaBuoyancyShaders" });
        // No UI, input or JSON here: the test bench (its Slate panel, hotkeys and JSON results) is the FluidNinjaBuoyancyDeveloper module,
        // which reaches the water through FluidNinjaBuoyancyBench.h and is left out of shipping builds.
    }
}
