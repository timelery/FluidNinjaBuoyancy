#pragma once

#include "CoreMinimal.h"

class UWorld;

/**
 * Tools → FluidNinja Buoyancy: Check Setup, and the console command fnb.CheckSetup: everything the plugin needs of a project and a level,
 * each a PASS / FAIL / WARN line in the FluidNinja Buoyancy message log, with a Fix action where a property or a tag puts it right.
 * - the modified NinjaLiveCore (/Game/FluidNinjaLive/NinjaLiveCore): FluidNinja Waves' interface version and core build, and the
 *   relative-tracking user parameter;
 * - every NinjaLive actor in the level: its tracking relative to the water, tracked bodies' density off, the data-channel reader for the
 *   wake points, and the surfaces it draws on tagged for the plugin, with the pressure buffer their material needs;
 * - the Wave Sims: every ini entry names a NinjaLive in the level (a typo silently runs a pool at the project's depth with open edges),
 *   every FluidNinja Wave Sim component sits on a NinjaLive, no actor is set up twice;
 * - the water surface tags the Surface Mirror looks for;
 * - the project's physics settings and Stable Steps.
 * In Play the checks run on the Play world and a Fix changes its copies, which Play ends; run it on the level to save a fix.
 */
namespace FluidNinjaBuoyancySetupCheck
{
	/** The message log the checks write to (registered by the editor module). */
	extern const FName LogName;

	/** Runs every check on World (the editor's level when null) into a new page of the log, opens the log, and returns the failures. */
	int32 Run(UWorld* World);
}
