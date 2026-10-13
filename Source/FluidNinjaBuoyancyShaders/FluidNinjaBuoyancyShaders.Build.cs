using UnrealBuildTool;

public class FluidNinjaBuoyancyShaders : ModuleRules
{
    public FluidNinjaBuoyancyShaders(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        // FluidNinja Buoyancy's global compute shader (the water gather). Loaded at PostConfigInit, as global shaders must be, with its plugin's
        // Shaders folder mapped to /Plugin/FluidNinjaBuoyancy.
        PublicDependencyModuleNames.AddRange(new[] { "Core", "RenderCore", "RHI" });
        PrivateDependencyModuleNames.AddRange(new[] { "Projects" });
    }
}
