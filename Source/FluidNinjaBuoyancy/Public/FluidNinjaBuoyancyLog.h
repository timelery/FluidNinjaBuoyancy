#pragma once

#include "Logging/LogMacros.h"

FLUIDNINJABUOYANCY_API DECLARE_LOG_CATEGORY_EXTERN(LogFluidNinjaBuoyancy, Log, All);

/**
 * The runtime module's console variables (FluidNinjaBuoyancyModule.cpp), for the module's own code: they are not exported. Another
 * module reads a switch's effect through UFluidNinjaBuoyancySettings (IsEnabled, IsDebugView, WakesEnabled, SwimmerWakesEnabled,
 * GatherEnabled, StableStepsEnabled), which fold the project setting and its console override into one answer, and sets a variable
 * through IConsoleManager by name.
 *
 * Each switch has one authority, the project setting; its console variable is a debugging override that reads -1 by default (the
 * setting decides), and 0 or 1 overrides it for the session: an A/B without leaving Play.
 */
namespace FluidNinjaBuoyancyCVars
{
	/** fnb.Buoyancy: -1 the Enabled setting, 0 off, 1 on, 2 on with the debug view. */
	extern int32 Enable;
	/** fnb.Buoyancy.Wake: -1 the Wakes setting, 0 no wakes at all (the swimmers' too), 1 the floating bodies' wakes on. */
	extern int32 Wake;
	/** fnb.Buoyancy.Gather: -1 the GPU Gather setting, 1 the gather, 0 per-body copies. */
	extern int32 Gather;
	/** fnb.Buoyancy.Implicit: -1 the Stable Steps setting, 1 stable steps, 0 plain explicit forces. */
	extern int32 Implicit;
	/** fnb.Buoyancy.Tracking: 0 never hand bodies to FluidNinja's tracking (they do not stir the water), 1 when FluidNinja tracks relative
	 * to the water, 2 always (testing). No project setting duplicates it: 1 is the designed behaviour. */
	extern int32 Tracking;
	extern float BoatThrottle;
	extern float BoatRudder;
	/** fnb.Waves.RadiationScale: scale on every body's radiation damping (A/B; 1 as calibrated). */
	extern float WaveRadiationScale;
}
