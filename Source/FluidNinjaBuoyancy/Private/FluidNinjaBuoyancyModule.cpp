#include "FluidNinjaBuoyancyLog.h"
#include "FluidNinjaBuoyancySettings.h"
#include "FluidNinjaBuoyancySubsystem.h"

#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ConfigUtilities.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Scalability.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY(LogFluidNinjaBuoyancy);

namespace FluidNinjaBuoyancyCVars
{
	// The switches: -1 leaves the decision to the project setting (the authority); 0 or 1 overrides it for the session.
	int32 Enable = -1;
	int32 Wake = -1;
	int32 Gather = -1;
	int32 Implicit = -1;
	int32 Tracking = 1;
	float BoatThrottle = 0.f;
	float BoatRudder = 0.f;

	static FAutoConsoleVariableRef CVarWake(TEXT("fnb.Buoyancy.Wake"), Wake,
		TEXT("Floating bodies write their wakes into FluidNinja: -1 (the default) as Project Settings → Game → FluidNinja Buoyancy → Wake → Wakes says, ")
		TEXT("0 no wakes at all (the swimmers' too; A/B), 1 (or more) the bodies' wakes on whatever the setting; below -1 counts as -1. Needs fnb.Buoyancy on. ")
		TEXT("Each body's own Wake switch and Interaction still apply, and the swimmers' wakes still need the Swimmer Wakes setting."));
	static FAutoConsoleVariableRef CVarGather(TEXT("fnb.Buoyancy.Gather"), Gather,
		TEXT("Every body's water in one GPU pass and one readback: -1 (the default) as Project Settings → FluidNinja Buoyancy → GPU Gather says, ")
		TEXT("1 (or more) the gather, 0 two copies and readbacks per body (A/B; more GPU work and readbacks with many bodies). Below -1 counts as -1."));
	static FAutoConsoleVariableRef CVarImplicit(TEXT("fnb.Buoyancy.Implicit"), Implicit,
		TEXT("The water's stiffness and drag that a physics step is too long for applied implicitly, so light bodies stay stable at any frame rate: ")
		TEXT("-1 (the default) as Project Settings → FluidNinja Buoyancy → Stable Steps says, 1 (or more) on, 0 plain explicit forces (A/B). Below -1 counts as -1."));

	static FAutoConsoleVariableRef CVarTracking(TEXT("fnb.Buoyancy.Tracking"), Tracking,
		TEXT("Bodies whose Interaction is FluidNinja's tracking: 0 never handed to it (they do not stir the water), 1 (the default) handed to it while every NinjaLive ")
		TEXT("paints tracked velocity relative to the water and no density for them (Project Settings → FluidNinja Buoyancy → FluidNinja Tracking), 2 always, ")
		TEXT("for testing: today's FluidNinja then paints their world velocity and density, which speeds a current up. Any other value hands none over. ")
		TEXT("Checked about once a second."));
	static FAutoConsoleVariableRef CVarEnable(TEXT("fnb.Buoyancy"), Enable,
		TEXT("Buoyancy on FluidNinja water: -1 (the default) as Project Settings → Game → FluidNinja Buoyancy → Enabled says, 0 off, 1 on, ")
		TEXT("2 (or more) on with the debug view (water line, forces, the water read back). Below -1 counts as -1."));
	static FAutoConsoleVariableRef CVarThrottle(TEXT("fnb.Boat.Throttle"), BoatThrottle,
		TEXT("Test throttle (-1 astern … 1 ahead; clamped to that range) for every FluidNinja Buoyancy component with propulsion and Console Control on. ")
		TEXT("Sent to them when the value changes; 0 (the default) again whenever Play begins."));
	static FAutoConsoleVariableRef CVarRudder(TEXT("fnb.Boat.Rudder"), BoatRudder,
		TEXT("Test rudder (-1 left … 1 right; clamped to that range) for every FluidNinja Buoyancy component with propulsion and Console Control on. ")
		TEXT("Sent to them when the value changes; 0 (the default) again whenever Play begins."));

	float WaveRadiationScale = 1.f;
	static FAutoConsoleVariableRef CVarWaveRadiationScale(TEXT("fnb.Waves.RadiationScale"), WaveRadiationScale,
		TEXT("FluidNinja Waves: scale on every body's radiation damping, the drag of the waves it makes itself (A/B). 1 (the default) as calibrated, 0 none, ")
		TEXT("above 1 more (bodies settle faster); negative counts as 0."));

	/**
	 * sg.WaterQuality: the water's scalability group. Its levels are the cvar sections [WaterQuality@0] … [WaterQuality@3] and
	 * [WaterQuality@Cine] of the plugin's Config/DefaultFluidNinjaBuoyancy.ini, which set the fnb.* cvars flagged ECVF_Scalability (the
	 * capture, the gather's cadence and budget, the hull and effect distances, the wave grid, the self fields, the splash budget); a
	 * project overrides a key in its own Config/DefaultFluidNinjaBuoyancy.ini, or in Config/DefaultScalability.ini under the same section,
	 * which is applied last. -1 (the default) follows sg.EffectsQuality, so the engine's quality presets carry the water with them.
	 */
	static TAutoConsoleVariable<int32> CVarWaterQuality(TEXT("sg.WaterQuality"), -1,
		TEXT("FluidNinja Buoyancy's quality tier: -1 (the default; any negative value) follows sg.EffectsQuality (3 if it is missing), 0 Low, 1 Medium, 2 High, ")
		TEXT("3 Epic (the calibrated defaults: the project settings as they are), 4 Cinematic (above the defaults); values above 4 count as 4. ")
		TEXT("Each tier applies the fnb.* cvars of its [WaterQuality@N] section (the plugin's Config/DefaultFluidNinjaBuoyancy.ini; a project overrides them in its own ")
		TEXT("Config/DefaultFluidNinjaBuoyancy.ini or Config/DefaultScalability.ini)."),
		ECVF_ScalabilityGroup);
}

namespace FluidNinjaBuoyancyQuality
{
	/** The plugin's own ini branch (Config/DefaultFluidNinjaBuoyancy.ini, with the project's Config/DefaultFluidNinjaBuoyancy.ini layered on it). */
	static const TCHAR* PluginIniBaseName = TEXT("FluidNinjaBuoyancy");
	static const TCHAR* GroupName = TEXT("WaterQuality");
	constexpr int32 NumLevels = 5;
	static const TCHAR* TierNames[NumLevels] = {TEXT("Low"), TEXT("Medium"), TEXT("High"), TEXT("Epic"), TEXT("Cinematic")};

	/** The level last applied (-1 before any), so the sink and the delegates below apply a tier only when it changes. */
	static int32 AppliedLevel = -1;
	static FDelegateHandle PostEngineInitHandle;
	static FDelegateHandle ScalabilityChangedHandle;
	static FConsoleVariableSinkHandle SinkHandle;

	int32 GetLevel()
	{
		const int32 Wanted = FluidNinjaBuoyancyCVars::CVarWaterQuality.GetValueOnGameThread();
		if (Wanted >= 0)
		{
			return FMath::Clamp(Wanted, 0, NumLevels - 1);
		}
		// Following sg.EffectsQuality: read directly, so a value set from the console or a device profile counts too, not only one set
		// through Scalability::SetQualityLevels.
		static IConsoleVariable* EffectsQuality = IConsoleManager::Get().FindConsoleVariable(TEXT("sg.EffectsQuality"));
		return FMath::Clamp(EffectsQuality ? EffectsQuality->GetInt() : 3, 0, NumLevels - 1);
	}

	bool IsFollowing()
	{
		return FluidNinjaBuoyancyCVars::CVarWaterQuality.GetValueOnGameThread() < 0;
	}

	const TCHAR* GetTierName(int32 Level)
	{
		return TierNames[FMath::Clamp(Level, 0, NumLevels - 1)];
	}

	FString Describe()
	{
		const int32 Level = GetLevel();
		return IsFollowing() ? FString::Printf(TEXT("%s (sg.WaterQuality -1 follows sg.EffectsQuality %d)"), GetTierName(Level), Level)
			: FString::Printf(TEXT("%s (sg.WaterQuality %d)"), GetTierName(Level), Level);
	}

	FString GetSectionName(int32 Level)
	{
		// As the engine names its own groups' sections: the last level is "Cine".
		return Level >= NumLevels - 1 ? FString::Printf(TEXT("%s@Cine"), GroupName) : FString::Printf(TEXT("%s@%d"), GroupName, Level);
	}

	/**
	 * Sets the tier's cvars: the section of the plugin's own ini (Config/DefaultFluidNinjaBuoyancy.ini with the project's layered on it;
	 * read through the branch, since a plugin's ini is keyed by its own name rather than a path), then the same section of the scalability
	 * ini, where a project may override single keys in its DefaultScalability.ini. Each key is set as the engine sets scalability
	 * (ECVF_SetByScalability: it yields to a project setting or the console, and only cvars flagged ECVF_Scalability take it).
	 */
	void Apply(bool bForce)
	{
		const int32 Level = GetLevel();
		if (!bForce && Level == AppliedLevel)
		{
			return;
		}
		AppliedLevel = Level;
		const FString Section = GetSectionName(Level);
		int32 Applied = 0;
		if (const FConfigFile* PluginIni = GConfig ? GConfig->FindConfigFileWithBaseName(FName(PluginIniBaseName)) : nullptr)
		{
			if (const FConfigSection* Values = PluginIni->FindSection(Section))
			{
				for (FConfigSectionMap::TConstIterator It(*Values); It; ++It)
				{
					UE::ConfigUtilities::OnSetCVarFromIniEntry(PluginIniBaseName, *It.Key().ToString(), *It.Value().GetValue(), ECVF_SetByScalability);
					++Applied;
				}
			}
		}
		if (Applied == 0)
		{
			UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("sg.WaterQuality: no [%s] section in the plugin's Config/DefaultFluidNinjaBuoyancy.ini (or the project's): the %s tier sets nothing."),
				*Section, GetTierName(Level));
		}
		if (!GScalabilityIni.IsEmpty())
		{
			UE::ConfigUtilities::ApplyCVarSettingsFromIni(*Section, *GScalabilityIni, ECVF_SetByScalability);
		}
		UE_LOG(LogFluidNinjaBuoyancy, Log, TEXT("FluidNinja Buoyancy quality: %s, %d cvars from [%s]."), *Describe(), Applied, *Section);
	}

	static void OnWaterQualityChanged(IConsoleVariable*)
	{
		Apply(true);
	}

	static void OnScalabilityChanged(const Scalability::FQualityLevels&)
	{
		Apply(false);
	}

	/** The console variable sink runs once a frame: sg.EffectsQuality set by any route (the console, a device profile) reaches a following
	 * sg.WaterQuality here. */
	static void OnSink()
	{
		Apply(false);
	}

	static void OnPostEngineInit()
	{
		// After the engine's own scalability (Scalability::LoadState, the game user settings, the device profile), so the tier is applied
		// over them as the engine's groups are.
		Apply(true);
	}
}

TSubclassOf<UFluidNinjaBuoyancySettings>& UFluidNinjaBuoyancySettings::OverrideClass()
{
	static TSubclassOf<UFluidNinjaBuoyancySettings> Class;
	return Class;
}

// ---- The switches, each read in one place: the setting unless its console variable overrides it.

bool UFluidNinjaBuoyancySettings::IsEnabled() const
{
	return FluidNinjaBuoyancyCVars::Enable < 0 ? bEnabled : FluidNinjaBuoyancyCVars::Enable != 0;
}

bool UFluidNinjaBuoyancySettings::IsDebugView()
{
	return FluidNinjaBuoyancyCVars::Enable >= 2;
}

bool UFluidNinjaBuoyancySettings::WakesEnabled() const
{
	return IsEnabled() && (FluidNinjaBuoyancyCVars::Wake < 0 ? bWakes : FluidNinjaBuoyancyCVars::Wake != 0);
}

bool UFluidNinjaBuoyancySettings::SwimmerWakesEnabled() const
{
	return IsEnabled() && bSwimmerWakes && FluidNinjaBuoyancyCVars::Wake != 0;
}

bool UFluidNinjaBuoyancySettings::GatherEnabled() const
{
	return FluidNinjaBuoyancyCVars::Gather < 0 ? bGPUGather : FluidNinjaBuoyancyCVars::Gather != 0;
}

bool UFluidNinjaBuoyancySettings::StableStepsEnabled() const
{
	return FluidNinjaBuoyancyCVars::Implicit < 0 ? bStableSteps : FluidNinjaBuoyancyCVars::Implicit != 0;
}

// ---- Config migration: a settings key renamed or removed in a later version is still in a project's DefaultGame.ini.

namespace FluidNinjaBuoyancyConfigRedirects
{
	/** Keys of earlier versions and the setting that replaced each (null: the key was removed). UObject::LoadConfig matches keys by the
	 * property's name alone (core redirects do not reach ini files), so a renamed setting reads its old key here, once, when the new key
	 * is absent from the section, and a removed one is reported. Add a row with every rename; the terminating row stays last. */
	struct FRedirect
	{
		const TCHAR* OldKey;
		const TCHAR* NewKey;
	};
	static const FRedirect Redirects[] = {
		{nullptr, nullptr},
	};

	/** Every key of the class's section in its ini (the derived class's section and its bases', which LoadConfig reads too) against the
	 * class's config properties. Warnings name the file and the key, so a project can fix its ini by hand. */
	void Migrate(UFluidNinjaBuoyancySettings& Settings)
	{
		if (!GConfig)
		{
			return;
		}
		const UClass* Class = Settings.GetClass();
		const FString Filename = Class->GetConfigName();
		const FString FileLabel = FPaths::GetCleanFilename(Filename);
		for (const UClass* Owner = Class; Owner && Owner->IsChildOf(UFluidNinjaBuoyancySettings::StaticClass()); Owner = Owner->GetSuperClass())
		{
			const FString SectionName = Owner->GetPathName();
			const FConfigSection* Keys = GConfig->GetSection(*SectionName, false, Filename);
			if (!Keys)
			{
				continue;
			}
			TSet<FName> Seen;
			for (FConfigSectionMap::TConstIterator It(*Keys); It; ++It)
			{
				const FName Key = It.Key();
				if (Seen.Contains(Key))
				{
					continue;   // an array's lines share one key
				}
				Seen.Add(Key);
				const FProperty* Property = FindFProperty<FProperty>(Class, Key);
				if (Property && Property->HasAnyPropertyFlags(CPF_Config))
				{
					continue;
				}
				// Not a setting of this version: renamed, or gone.
				const FRedirect* Redirect = nullptr;
				for (const FRedirect& Candidate : Redirects)
				{
					if (Candidate.OldKey && Key == Candidate.OldKey)
					{
						Redirect = &Candidate;
						break;
					}
				}
				const FProperty* NewProperty = Redirect && Redirect->NewKey ? FindFProperty<FProperty>(Class, Redirect->NewKey) : nullptr;
				if (NewProperty && !Keys->Contains(NewProperty->GetFName()))
				{
					const FString Value = It.Value().GetValue();
					NewProperty->ImportText_Direct(*Value, NewProperty->ContainerPtrToValuePtr<void>(&Settings), &Settings, PPF_None);
					UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("FluidNinja Buoyancy settings: [%s] in %s sets %s, which is now %s (read from the old key this time; rename it)."),
						*SectionName, *FileLabel, *Key.ToString(), Redirect->NewKey);
				}
				else if (NewProperty)
				{
					UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("FluidNinja Buoyancy settings: [%s] in %s still sets %s, which is now %s (set too, and it wins; delete the old key)."),
						*SectionName, *FileLabel, *Key.ToString(), Redirect->NewKey);
				}
				else
				{
					UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("FluidNinja Buoyancy settings: [%s] in %s sets %s, which is not a setting of this version (removed, or misspelt); it does nothing. Delete it."),
						*SectionName, *FileLabel, *Key.ToString());
				}
			}
		}
	}
}

void UFluidNinjaBuoyancySettings::PostInitProperties()
{
	Super::PostInitProperties();
	// On the class defaults, after LoadConfig: the plugin's own, and a project's derived class's (whose section is read over the plugin's).
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		FluidNinjaBuoyancyConfigRedirects::Migrate(*this);
	}
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
	// The spray sheets (Spray Sheets on a body): the plugin's own sheet system.
	SpraySheetSystem = FSoftObjectPath(TEXT("/FluidNinjaBuoyancy/Effects/NS_FNB_HullSpray.NS_FNB_HullSpray"));
	// FluidNinja Waves on the demo level's simulations: its pool is a closed basin 4 m deep, its river lane 3 m deep.
	FFluidNinjaWaveSimSettings Pool;
	Pool.NinjaLive = TEXT("Pool_FluidNinja");
	Pool.bOverrideDepth = true;
	Pool.DepthCm = 400.f;
	Pool.bOverrideEdgeOpen = true;
	Pool.bEdgeOpen = false;
	WaveSims.Add(Pool);
	FFluidNinjaWaveSimSettings River;
	River.NinjaLive = TEXT("River_FluidNinja");
	River.bOverrideDepth = true;
	River.DepthCm = 300.f;
	WaveSims.Add(River);
}

/**
 * The runtime module: the quality tier (sg.WaterQuality) is applied once the engine's own scalability is in, again whenever the group
 * changes, and, while it follows sg.EffectsQuality, whenever the overall quality changes.
 */
class FFluidNinjaBuoyancyModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		using namespace FluidNinjaBuoyancyQuality;
		FluidNinjaBuoyancyCVars::CVarWaterQuality.AsVariable()->SetOnChangedCallback(FConsoleVariableDelegate::CreateStatic(&OnWaterQualityChanged));
		ScalabilityChangedHandle = Scalability::OnScalabilitySettingsChanged.AddStatic(&OnScalabilityChanged);
		SinkHandle = IConsoleManager::Get().RegisterConsoleVariableSink_Handle(FConsoleCommandDelegate::CreateStatic(&OnSink));
		if (GEngine)
		{
			OnPostEngineInit();   // loaded after the engine came up (the plugin enabled at runtime): now is after its scalability
		}
		else
		{
			PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddStatic(&OnPostEngineInit);
		}
	}

	virtual void ShutdownModule() override
	{
		using namespace FluidNinjaBuoyancyQuality;
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		Scalability::OnScalabilitySettingsChanged.Remove(ScalabilityChangedHandle);
		IConsoleManager::Get().UnregisterConsoleVariableSink_Handle(SinkHandle);
	}
};

IMPLEMENT_MODULE(FFluidNinjaBuoyancyModule, FluidNinjaBuoyancy)
