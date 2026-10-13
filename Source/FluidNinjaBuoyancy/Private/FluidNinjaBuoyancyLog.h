#pragma once

#include "Logging/LogMacros.h"

DECLARE_LOG_CATEGORY_EXTERN(LogFluidNinjaBuoyancy, Log, All);

namespace FluidNinjaBuoyancyCVars
{
	/** fnb.Buoyancy: 0 off, 1 on, 2 on with the debug view. */
	extern int32 Enable;
	/** fnb.Buoyancy.Wake: 0 off, 1 on. */
	extern int32 Wake;
	/** fnb.Buoyancy.Gather: 1 the GPU gather (when the settings allow it), 0 per-body copies. */
	extern int32 Gather;
	/** fnb.Buoyancy.Implicit: 1 stable steps (when the settings allow them), 0 plain explicit forces. */
	extern int32 Implicit;
	extern float BoatThrottle;
	extern float BoatRudder;
}
