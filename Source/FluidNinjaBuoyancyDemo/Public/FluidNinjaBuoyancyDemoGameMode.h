#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "FluidNinjaBuoyancyDemoGameMode.generated.h"

/** The demo level's game mode: the player is a FluidNinja Buoyancy Demo Character. */
UCLASS(meta = (DisplayName = "FluidNinja Buoyancy Demo Game Mode"))
class FLUIDNINJABUOYANCYDEMO_API AFluidNinjaBuoyancyDemoGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AFluidNinjaBuoyancyDemoGameMode();
};
