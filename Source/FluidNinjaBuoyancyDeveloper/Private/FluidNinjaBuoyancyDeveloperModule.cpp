#include "FluidNinjaBuoyancyTestBench.h"

#include "FluidNinjaBuoyancySubsystem.h"
#include "Modules/ModuleManager.h"

/**
 * The developer module: the FluidNinja Buoyancy test bench, out of the runtime module so that players never ship it. It binds the
 * bench factory the runtime's subsystem asks for a bench through (one per game world, from the subsystem's first tick), and its
 * translation unit registers fnb.Test and fnb.TestHUD (FluidNinjaBuoyancyTestBench.cpp). Loaded wherever the engine's developer tools are
 * (DeveloperTool in the .uplugin): the editor, Play In Editor, -game, Debug and Development game builds; not Test or Shipping.
 */
class FFluidNinjaBuoyancyDeveloperModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UFluidNinjaBuoyancySubsystem::BenchFactory().BindStatic(&FFluidNinjaBuoyancyTestBench::Create);
	}

	virtual void ShutdownModule() override
	{
		UFluidNinjaBuoyancySubsystem::BenchFactory().Unbind();
	}
};

IMPLEMENT_MODULE(FFluidNinjaBuoyancyDeveloperModule, FluidNinjaBuoyancyDeveloper)
