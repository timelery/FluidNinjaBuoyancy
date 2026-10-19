#include "FluidNinjaBuoyancyStats.h"

// The stats declared in FluidNinjaBuoyancyStats.h, defined once for the plugin.
DEFINE_STAT(STAT_FNB_Tick);
DEFINE_STAT(STAT_FNB_Patches);
DEFINE_STAT(STAT_FNB_Capture);
DEFINE_STAT(STAT_FNB_GatherBuild);
DEFINE_STAT(STAT_FNB_WavesBuild);
DEFINE_STAT(STAT_FNB_PreTick);
DEFINE_STAT(STAT_FNB_PhysicsStep);
DEFINE_STAT(STAT_FNB_Integrate);

DEFINE_STAT(STAT_FNB_BodiesAwake);
DEFINE_STAT(STAT_FNB_BodiesAsleep);
DEFINE_STAT(STAT_FNB_BodiesInCapture);
DEFINE_STAT(STAT_FNB_GatherPoints);
DEFINE_STAT(STAT_FNB_WakePoints);
DEFINE_STAT(STAT_FNB_ImpulsePieces);
DEFINE_STAT(STAT_FNB_SelfFields);
DEFINE_STAT(STAT_FNB_SelfCells);
DEFINE_STAT(STAT_FNB_ReadbackAgeFrames);
DEFINE_STAT(STAT_FNB_SplashSpawns);
DEFINE_STAT(STAT_FNB_WaveSimsActive);

// On by default: a -csvCaptureFrames run records the FNB stats without asking for the category.
CSV_DEFINE_CATEGORY_MODULE(FLUIDNINJABUOYANCY_API, FNB, true);
