#pragma once

#include "CoreMinimal.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "Stats/Stats.h"

/**
 * What FluidNinja Buoyancy costs and carries, frame by frame: the stat group (stat FluidNinjaBuoyancy) and the CSV category (FNB,
 * for -csvCaptureFrames runs and the CSV profiler's percentiles). The cycle stats time the parts of a frame on the game thread
 * (the subsystem's tick, its patches, the Surface Mirror capture, the gather's dispatch, the waves' input, the physics pre-tick)
 * and on the physics thread (the callback's step and the per-body integration inside it). The counters say how much work the
 * frame carried; each is set once a frame where the work happens and reads zero when nothing sets it. Declared here for every
 * file of the plugin, defined once in FluidNinjaBuoyancyStats.cpp.
 */
DECLARE_STATS_GROUP(TEXT("FluidNinjaBuoyancy"), STATGROUP_FluidNinjaBuoyancy, STATCAT_Advanced);

// Game thread.
DECLARE_CYCLE_STAT_EXTERN(TEXT("Tick"), STAT_FNB_Tick, STATGROUP_FluidNinjaBuoyancy, FLUIDNINJABUOYANCY_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Patches"), STAT_FNB_Patches, STATGROUP_FluidNinjaBuoyancy, FLUIDNINJABUOYANCY_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Capture"), STAT_FNB_Capture, STATGROUP_FluidNinjaBuoyancy, FLUIDNINJABUOYANCY_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Gather build"), STAT_FNB_GatherBuild, STATGROUP_FluidNinjaBuoyancy, FLUIDNINJABUOYANCY_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Waves build"), STAT_FNB_WavesBuild, STATGROUP_FluidNinjaBuoyancy, FLUIDNINJABUOYANCY_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Physics pre-tick"), STAT_FNB_PreTick, STATGROUP_FluidNinjaBuoyancy, FLUIDNINJABUOYANCY_API);
// Physics thread.
DECLARE_CYCLE_STAT_EXTERN(TEXT("Physics step"), STAT_FNB_PhysicsStep, STATGROUP_FluidNinjaBuoyancy, FLUIDNINJABUOYANCY_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Integrate"), STAT_FNB_Integrate, STATGROUP_FluidNinjaBuoyancy, FLUIDNINJABUOYANCY_API);

// Per frame.
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Bodies awake"), STAT_FNB_BodiesAwake, STATGROUP_FluidNinjaBuoyancy, FLUIDNINJABUOYANCY_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Bodies asleep"), STAT_FNB_BodiesAsleep, STATGROUP_FluidNinjaBuoyancy, FLUIDNINJABUOYANCY_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Bodies in capture"), STAT_FNB_BodiesInCapture, STATGROUP_FluidNinjaBuoyancy, FLUIDNINJABUOYANCY_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Gather points"), STAT_FNB_GatherPoints, STATGROUP_FluidNinjaBuoyancy, FLUIDNINJABUOYANCY_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Wake points"), STAT_FNB_WakePoints, STATGROUP_FluidNinjaBuoyancy, FLUIDNINJABUOYANCY_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Impulse pieces"), STAT_FNB_ImpulsePieces, STATGROUP_FluidNinjaBuoyancy, FLUIDNINJABUOYANCY_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Self fields"), STAT_FNB_SelfFields, STATGROUP_FluidNinjaBuoyancy, FLUIDNINJABUOYANCY_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Self cells"), STAT_FNB_SelfCells, STATGROUP_FluidNinjaBuoyancy, FLUIDNINJABUOYANCY_API);
/** How old the water in use is: frames since the newest readback the bodies' patches came from was queued. */
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Readback age (frames)"), STAT_FNB_ReadbackAgeFrames, STATGROUP_FluidNinjaBuoyancy, FLUIDNINJABUOYANCY_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Splash spawns"), STAT_FNB_SplashSpawns, STATGROUP_FluidNinjaBuoyancy, FLUIDNINJABUOYANCY_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Wave sims active"), STAT_FNB_WaveSimsActive, STATGROUP_FluidNinjaBuoyancy, FLUIDNINJABUOYANCY_API);

/** The CSV profiler's category: CSV_CUSTOM_STAT(FNB, TickMs, ...) and the like, one value a frame. */
CSV_DECLARE_CATEGORY_MODULE_EXTERN(FLUIDNINJABUOYANCY_API, FNB);
