#include "FluidNinjaBuoyancyDemoGameMode.h"

#include "FluidNinjaBuoyancyDemoCharacter.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, FluidNinjaBuoyancyDemo)

AFluidNinjaBuoyancyDemoGameMode::AFluidNinjaBuoyancyDemoGameMode()
{
	DefaultPawnClass = AFluidNinjaBuoyancyDemoCharacter::StaticClass();
}
