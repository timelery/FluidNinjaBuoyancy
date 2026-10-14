#include "FluidNinjaBuoyancyLog.h"
#include "FluidNinjaBuoyancySettings.h"

#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogFluidNinjaBuoyancy);

namespace FluidNinjaBuoyancyCVars
{
	int32 Enable = 1;
	int32 Wake = 1;
	int32 Gather = 1;
	int32 Implicit = 1;
	int32 Tracking = 1;
	float BoatThrottle = 0.f;
	float BoatRudder = 0.f;

	static FAutoConsoleVariableRef CVarWake(TEXT("fnb.Buoyancy.Wake"), Wake,
		TEXT("Floating bodies write their wakes into FluidNinja (Project Settings → Game → FluidNinja Buoyancy → Wake). 0 off (A/B), 1 on."));
	static FAutoConsoleVariableRef CVarGather(TEXT("fnb.Buoyancy.Gather"), Gather,
		TEXT("1: every body's water in one GPU pass and one readback (when Project Settings → FluidNinja Buoyancy → GPU Gather is on). 0: two copies and readbacks per body (A/B)."));
	static FAutoConsoleVariableRef CVarImplicit(TEXT("fnb.Buoyancy.Implicit"), Implicit,
		TEXT("1: the water's stiffness and drag that a physics step is too long for are applied implicitly, so light bodies stay stable at any frame rate ")
		TEXT("(Project Settings → FluidNinja Buoyancy → Stable Steps). 0: plain explicit forces (A/B)."));

	static FAutoConsoleVariableRef CVarTracking(TEXT("fnb.Buoyancy.Tracking"), Tracking,
		TEXT("Bodies whose Interaction asks for FluidNinja's own tracking: 0 never (wakes only), 1 only while every NinjaLive paints tracked velocity ")
		TEXT("relative to the water (Project Settings → FluidNinja Buoyancy → FluidNinja Tracking), 2 always, for testing: today's FluidNinja then ")
		TEXT("paints their world velocity, which speeds a current up."));
	static FAutoConsoleVariableRef CVarEnable(TEXT("fnb.Buoyancy"), Enable,
		TEXT("Buoyancy on FluidNinja water (Project Settings → Game → FluidNinja Buoyancy). 0 off, 1 on, 2 on with the debug view (water line, forces, the water read back)."));
	static FAutoConsoleVariableRef CVarThrottle(TEXT("fnb.Boat.Throttle"), BoatThrottle,
		TEXT("Test throttle (-1 astern … 1 ahead) for every FluidNinja Buoyancy component with propulsion and Console Control on."));
	static FAutoConsoleVariableRef CVarRudder(TEXT("fnb.Boat.Rudder"), BoatRudder,
		TEXT("Test rudder (-1 left … 1 right) for every FluidNinja Buoyancy component with propulsion and Console Control on."));
}

UFluidNinjaBuoyancySettings::UFluidNinjaBuoyancySettings()
{
	// FluidNinja's own tags for its river spline pieces and sea surfaces, and this plugin's tag for any other surface to float on.
	SurfaceTags = {TEXT("SplineSource"), TEXT("SeaSurface"), TEXT("FluidNinjaBuoyancySurface")};
	// FluidNinja LIVE installed at its default location (Content/FluidNinjaLive).
	NinjaParameterCollection = FSoftObjectPath(TEXT("/Game/FluidNinjaLive/OutputMaterials/MP_NinjaMaterialParamCollection01.MP_NinjaMaterialParamCollection01"));
	WakeDataChannel = FSoftObjectPath(TEXT("/Game/FluidNinjaLive/Core/DataChannels/NinjaDataChannel1_Global.NinjaDataChannel1_Global"));
	// The plugin's own spray and hull attenuation. No hull sounds ship with the plugin: set Slap, Wash and Creak Sound to your own.
	SplashSystem = FSoftObjectPath(TEXT("/FluidNinjaBuoyancy/Effects/NS_FNB_HullSplash.NS_FNB_HullSplash"));
	HullAttenuation = FSoftObjectPath(TEXT("/FluidNinjaBuoyancy/Audio/SA_FNB_Hull.SA_FNB_Hull"));
}

IMPLEMENT_MODULE(FDefaultModuleImpl, FluidNinjaBuoyancy)
