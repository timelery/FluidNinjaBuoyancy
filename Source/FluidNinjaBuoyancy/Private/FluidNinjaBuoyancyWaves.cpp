#include "FluidNinjaBuoyancyWaves.h"

// The render-thread side (FluidNinjaBuoyancyShaders).
#include "FluidNinjaBuoyancyWaveSource.h"

#include "CollisionQueryParams.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/Engine.h"
#include "Engine/HitResult.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "FluidNinjaBuoyancyComponent.h"
#include "FluidNinjaBuoyancyCore.h"
#include "FluidNinjaBuoyancyLog.h"
#include "FluidNinjaBuoyancySettings.h"
#include "FluidNinjaBuoyancyStats.h"
#include "FluidNinjaBuoyancySubsystem.h"
#include "FluidNinjaBuoyancySwimmerBody.h"
#include "FluidNinjaWaveSimComponent.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/App.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraSystemInstanceController.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PixelFormat.h"
#include "RenderingThread.h"
#include "TextureResource.h"

namespace FluidNinjaBuoyancyWaveRuntime
{
	/** FluidNinja's fixed tick (NinjaLiveCore's), used where a core has none (the waves' interface asks for it). */
	constexpr float WaveDefaultTickSeconds = 1.f / 60.f;
	/** The water a body is measured against where nothing is known (cm): far below anything, so it has no static head there. */
	constexpr float WaveNoWaterZ = -1.0e7f;
	/** A body that moved further than this many times its speed allows in a frame (plus the margins) was put there. */
	constexpr double WaveTeleportSlack = 1.5;
	constexpr double WaveTeleportMarginCm = 5.0;
	constexpr double WaveTeleportMarginRad = 0.1;
	/** Each body's own field holds its bounding sphere with this much more on every side (besides two cells), clear of its sponge. */
	constexpr double WaveSelfSlackCm = 50.0;
	/** Below this many cells of sponge a body's own field would reflect its waves back at it: it gets none (and feels no waves). */
	constexpr double WaveSelfMinSpongeCells = 4.0;
	/** The self fields' sponge strength at the window's very edge, 1/s. */
	constexpr float WaveSelfSpongeRate = 10.f;
	/** The self-field atlas: R16F, square. It starts at the first size (2 MB: four tiles of the largest field, 512 cells) and doubles when
	 * its tiles run out, up to the largest (32 MB: 64 tiles of 512; a level of many simulations, eleven tanks say, has a few dozen bodies
	 * with fields at once and filled a 2048 one). */
	constexpr int32 WaveAtlasFirstSize = 1024;
	constexpr int32 WaveAtlasMaxSize = 4096;
	/** The core's interface this plugin was written against (User.Wave.InterfaceVersion), and the first core build (User.Wave.CoreBuild,
	 * YYYYMMDD) that has it, from the zip named here. An older core is refused like a stock one. */
	constexpr int32 WaveRequiredInterfaceVersion = 2;
	constexpr int32 WaveRequiredCoreBuild = 20261017;
	const TCHAR* const WaveCoreZipName = TEXT("FluidNinjaLive_2.0.0.56_Waves_RelativeTracking_UE5.8.zip");
	/** The first interface with User.Wave.ScaleDown (the wave grid decoupled from the pressure buffer). An older core still runs, at 1. */
	constexpr int32 WaveScaleDownInterfaceVersion = 3;
	/** fnb.Waves.MaxActiveSims: the simulations are ranked this often (s); a running one keeps its place against a newcomer whose rank is
	 * within this share of its own (so two at the line do not swap every ranking); and a simulation stays in each state at least this long. */
	constexpr double WaveActiveSimRankSeconds = 1.0;
	constexpr double WaveActiveSimHysteresis = 0.1;
	constexpr double WaveActiveSimDwellSeconds = 1.0;
	/** At most this many probes. */
	constexpr int32 WaveMaxProbes = 256;
	/** The depth under a body is traced again after it moves this far, or this long after the last trace. */
	constexpr double WaveDepthRetraceCm = 100.0;
	constexpr double WaveDepthRetraceSeconds = 1.0;
	/** The trace looks this far down from the water for the ground. */
	constexpr double WaveDepthTraceCm = 5000.0;
	/** Surface tension over density, cm³/s² (water: 72.8), as the core's dispersion. */
	constexpr double WaveCapillarity = 72.8;
	/** Radiation damping: the ring of wave directions its far field is summed over. */
	constexpr int32 WaveRadiationAngles = 256;
	/**
	 * Radiation damping: the calibration on the Haskind value. A body's own waves (taken out of what it feels) are radiated by the core at
	 * every frequency it moves at. While this damping was one constant, taken at each motion's natural frequency (the dry one, without
	 * added mass), bodies that make and feel waves shared a collective mode near 1 Hz (well below a crate's or a ball's own heave), where
	 * the core radiates 2 to 8 times that constant: with the readback's few frames of lag, the mode gained energy and the bodies rocked
	 * each other up to a few cm (2026-10-17, the FluidNinja Buoyancy demo river's eight floaters at rest, fixed 60 fps). Three times the
	 * Haskind value held it everywhere measured, but on every motion it also stopped a crate's bob dead and took the rock out of a boat a
	 * person jumps in; the physics step then added the other two times only to slow motion (FluidNinjaBuoyancyCallback.cpp,
	 * RestRadiationBoost). The damping is now taken at the frequency each body actually moves at (UpdateMotion's running estimate, at the
	 * natural frequency with the added mass in while it is still), so the mode meets the damping of its own frequency; this stays the
	 * physical value, and the boost is to retire to 1.
	 */
	constexpr double WaveRadiationCalibration = 1.0;
	/** The running estimate of a body's motion frequency averages v² and a² over about this long (s): about a period of the slowest motion
	 * it is for (the 1 Hz collective mode), short enough to follow a body set rocking. */
	constexpr double WaveMotionAverageSeconds = 1.0;
	/** Below these mean squares of the vertical speed (cm²/s²: 0.5 cm/s) and of the roll or pitch rate (rad²/s²: 0.005 rad/s) a body is
	 * still, and its estimate is its natural frequency: the ratio of two noises would be anything. */
	constexpr double WaveMotionHeaveFloor = 0.25;
	constexpr double WaveMotionSpinFloor = 2.5e-5;
	/** The radiation damping follows the estimate from one frequency bin to another at most this often (s), so an estimate wandering
	 * about a bin's edge does not flap the damping. */
	constexpr double WaveRadiationSwitchSeconds = 0.25;
	/** The player's capsule lets go of its static head over this much (cm) as its top goes under (FluidNinjaBuoyancyCurrent's SurfaceReachCm). */
	constexpr float WaveSwimmerSubmergeFadeCm = 50.f;

	const FName WaveSimEnabledName(TEXT("User.WaveSimEnabled"));
	const FName WaveSourceTextureName(TEXT("User.WaveSourceTexture"));
	const FName WaveAuxTargetName(TEXT("User.WaveAuxTarget"));
	const FName WaveDepthFromBedName(TEXT("User.Wave.DepthFromBed"));
	const FName WaveWallAbsorptionName(TEXT("User.Wave.WallAbsorption"));
	const FName WaveWallBandName(TEXT("User.Wave.WallBandCm"));
	const FName WaveDepthName(TEXT("User.Wave.DepthCm"));
	const FName WaveDampingName(TEXT("User.Wave.Damping"));
	const FName WaveSpongeName(TEXT("User.Wave.SpongeCm"));
	const FName WaveEdgeOpenName(TEXT("User.Wave.EdgeOpen"));
	const FName WaveShoreName(TEXT("User.Wave.ShoreDepthCm"));
	const FName WaveBreakingName(TEXT("User.Wave.BreakingSlope"));
	const FName WaveHyperviscosityName(TEXT("User.Wave.Hyperviscosity"));
	const FName WaveCurrentCouplingName(TEXT("User.Wave.CurrentCoupling"));
	const FName WaveMeanRelaxName(TEXT("User.Wave.MeanRelax"));
	const FName WaveScaleDownName(TEXT("User.Wave.ScaleDown"));
	const FName WaveInterfaceVersionName(TEXT("User.Wave.InterfaceVersion"));
	const FName WaveCoreBuildName(TEXT("User.Wave.CoreBuild"));
	// The foam channel (interface version 4): the core accumulates F per wave cell from what breaks, what a moving hull's waterline sheds
	// and what hits, and its Wave Aux writes F into the target the plugin binds at User.WaveFoamTarget; the surface material samples it.
	const FName WaveFoamTargetName(TEXT("User.WaveFoamTarget"));
	const FName WaveFoamName(TEXT("User.Wave.Foam"));
	const FName WaveFoamDecayName(TEXT("User.Wave.FoamDecay"));
	const FName WaveFoamBreakingName(TEXT("User.Wave.FoamBreaking"));
	const FName WaveFoamWakeName(TEXT("User.Wave.FoamWake"));
	const FName WaveFoamImpactName(TEXT("User.Wave.FoamImpact"));
	/** The surface materials' foam-buffer texture parameter (the material step's WaveFoamBuffer), bound per simulation on the surfaces'
	 * dynamic materials beside WaveNormalScale. */
	const FName WaveFoamBufferParameterName(TEXT("WaveFoamBuffer"));
	/** The first interface with the foam channel. An older core still runs, without foam. */
	constexpr int32 WaveFoamInterfaceVersion = 4;
	// The closed-window <v> pin (interface version 5): Wave Step pulls a walled window's mean vertical velocity to 0 at User.Wave.VMeanRelax,
	// and clamps both mean-relax rates against the Dt it steps with.
	const FName WaveVMeanRelaxName(TEXT("User.Wave.VMeanRelax"));
	/** The first interface with the closed-window <v> pin and the in-step clamp of both mean-relax rates. An older core still runs, with
	 * the level pinned through the mean of s alone. */
	constexpr int32 WaveVPinInterfaceVersion = 5;

	/** fnb.Waves.SelfFieldDistanceCm: a body farther than this from the camera gets no own field (it makes waves and feels none); 0 gives
	 * every body one. The fields' cost scales with the sum of their cells squared, so the far ones are most of it. A scalability variable,
	 * so the quality tiers (Config/DefaultFluidNinjaBuoyancy.ini) set it. */
	float WaveSelfFieldDistanceCm = 6000.f;
	static FAutoConsoleVariableRef CVarWaveSelfFieldDistanceCm(TEXT("fnb.Waves.SelfFieldDistanceCm"), WaveSelfFieldDistanceCm,
		TEXT("FluidNinja Waves: bodies farther than this (cm) from the camera get no own field: they make waves and feel none. A body without one gets it back inside ")
		TEXT("0.9 times this distance. Lower saves GPU time (the far fields are most of the cost); raise it if distant floaters should still ride the waves. ")
		TEXT("0 or less: every body gets one. Default 6000. Set by sg.WaterQuality (1500 / 3000 / 4500 / 6000 / 0, Low to Cinematic)."),
		ECVF_Scalability | ECVF_Default);
	/**
	 * fnb.Waves.MeanRelax: the core's k = 0 kill, 1/s (User.Wave.MeanRelax). Each tick Wave Step takes Dt * r * <s> off eta in every wet
	 * cell, <s> being the window's mean of s = eta + P over its wet cells (WaveOp's 1 x 1 top mip, .x / .y). No other mode changes: the
	 * operator of a constant is 0.
	 * Why not the 0.5 /s it was: in a walled window under a current, breaking at the floating hulls' hollow edges damps the refilling v on
	 * one side only, and so pumps the uniform vertical velocity <v>, which the operator does not see and only Damping and the wall band
	 * decay (about 0.06 /s on an 800² pool). A pin on the mean of s then parks the level at <v> / r. Measured on 2026-10-18 on the demo
	 * pool: at 0.5 /s the level sank from -40 to -67 cm over minutes of the bench suite, and with Wave Breaking Slope 100 it did not sink;
	 * in a standalone walkaway at 5, 30 and 60 /s it held at -0.7 cm, about 0 and about 0. This is a mitigation: <v> itself stays free
	 * until interface 5, whose Wave Step pulls a walled window's <v> to 0 (fnb.Waves.VMeanRelax), and the s pin can then be gentle: 5 /s
	 * keeps a slam's P_dyn, which is inside the pinned mean, to a uniform kick under about 0.1 cm for one tick (port).
	 * Below 0 (the default, -1) is automatic, by the window: walled, 5 /s where that <v> pin runs (WaveVPinRuns), else 30 /s; open, 0.5 /s
	 * as it always was. An open window never sank (its sponge band relaxes eta toward 0 at its edge, and the bench's river floaters held to a
	 * millimetre), and there a strong pin only took the level a sinking body raises out from under its own waves: the ripple-evers block in
	 * the river read its troughs 0.25-0.3 cm deeper at 30 /s (its 11 m³ over the 60 m window is a 0.3 cm rise). A value of 0 or more is used
	 * as given (0 off). Either way it is clamped to 0.5 / the longest Dt Wave Step integrates with (WaveWorstStepSeconds): as an explicit term
	 * it stays monotone there, and it still damps if the top mip were a tick late (at Dt r = 1 that rings undamped).
	 */
	float WaveMeanRelax = -1.f;
	static FAutoConsoleVariableRef CVarWaveMeanRelax(TEXT("fnb.Waves.MeanRelax"), WaveMeanRelax,
		TEXT("FluidNinja Waves: rate (1/s) at which the core relaxes its window's mean level (the mean of eta + P over its wet cells) toward 0 (User.Wave.MeanRelax). Below 0 (the default, -1): automatic, by the window: walled, 5 where the core pins its mean vertical velocity (interface version 5 and fnb.Waves.VMeanRelax above 0), else 30; open (a sponge edge), 0.5. 0 or more: used as given, 0 off. Clamped to 0.5 / the core's longest step: 30 at a fixed 60 ticks/s, 4 when the core has no fixed tick. Lower lets the level drift; higher holds it harder against a sinking body's rise. The bodies' own fields take fnb.Waves.SelfMeanRelax."));
	/** fnb.Waves.SelfMeanRelax: the same k = 0 kill in the bodies' own fields (SelfStepCS), 1/s. Their absorbing band (10 /s, ramped as the
	 * square over the sponge) already dominates their k = 0 decay, so the rate barely matters (port: B - B_self within +-0.3 cm at 0, 0.5
	 * and 5 /s). It stays low because P_dyn is inside the pinned mean and a self field is small: on a 168 m² tile each impulse becomes a
	 * uniform one-tick kick about 2.4 times the core's. */
	float WaveSelfMeanRelax = 0.5f;
	static FAutoConsoleVariableRef CVarWaveSelfMeanRelax(TEXT("fnb.Waves.SelfMeanRelax"), WaveSelfMeanRelax,
		TEXT("FluidNinja Waves: rate (1/s) at which each body's own field relaxes its mean level toward 0 (the core takes fnb.Waves.MeanRelax). Range 0 to 5, and at most 0.5 / tick; negative counts as 0. 0 off. Default 0.5; the fields' absorbing band dominates, so it barely matters."));
	/** fnb.Waves.VMeanRelax: rate (1/s) at which an interface-5 core pulls a walled window's mean vertical velocity <v> to 0
	 * (User.Wave.VMeanRelax). In a walled window the sum of v over the wet cells is d/dt of the sum of eta, which volume conservation holds
	 * at 0, so the pin restores a true constraint: <v> then sits at -B / (Gamma + r_v), about -0.03 cm/s on the demo pool, instead of
	 * -B / Gamma, about -17 cm/s (port). Wave Step skips it in an open window, whose sum of v has real fluxes through its edge, and clamps it
	 * to 1 / Dt. Sent only to a core that has the parameter; the bodies' own fields get no such pin (their tiles are open and their band
	 * dominates). */
	float WaveVMeanRelax = 30.f;
	static FAutoConsoleVariableRef CVarWaveVMeanRelax(TEXT("fnb.Waves.VMeanRelax"), WaveVMeanRelax,
		TEXT("FluidNinja Waves: rate (1/s) at which the core pulls a walled window's mean vertical velocity toward 0 (User.Wave.VMeanRelax). Needs a core at interface version 5; open windows ignore it, and the core clamps it to 1 / its step. While it runs, fnb.Waves.MeanRelax's automatic rate is 5 instead of 30. 0 off (the automatic fnb.Waves.MeanRelax is then 30); negative counts as 0. Default 30."));

	/** Niagara's longest tick (s): a core without a fixed tick integrates up to this in one step. */
	constexpr float WaveMaxVariableStepSeconds = 0.125f;
	/** fnb.Waves.MeanRelax's automatic rates (1/s): a walled window whose <v> the core pins, a walled window nothing pins, and an open one. */
	constexpr float WaveAutoMeanRelaxPinned = 5.f;
	constexpr float WaveAutoMeanRelaxUnpinned = 30.f;
	constexpr float WaveAutoMeanRelaxOpen = 0.5f;
	/** The bodies' own fields' largest mean relax (1/s). */
	constexpr float WaveSelfMeanRelaxMax = 5.f;

	/** The longest Dt Wave Step integrates with (s): the core's fixed tick, or Niagara's longest tick when it has none. */
	float WaveWorstStepSeconds(bool bFixedTick, float TickSeconds)
	{
		return bFixedTick ? FMath::Max(TickSeconds, 1.e-4f) : WaveMaxVariableStepSeconds;
	}

	/** The core pins the window's <v>: a core at interface 5 or later, a walled window (Wave Step skips the pin in an open one) and
	 * fnb.Waves.VMeanRelax above 0. */
	bool WaveVPinRuns(int32 InterfaceVersion, bool bEdgeOpen)
	{
		return InterfaceVersion >= WaveVPinInterfaceVersion && !bEdgeOpen && WaveVMeanRelax > 0.f;
	}

	/** User.Wave.MeanRelax as the core is sent it (1/s): fnb.Waves.MeanRelax, or its automatic rate below 0 (by the window), clamped to 0.5 /
	 * the longest step. */
	float WaveCoreMeanRelax(bool bFixedTick, float TickSeconds, int32 InterfaceVersion, bool bEdgeOpen)
	{
		const float Auto = bEdgeOpen ? WaveAutoMeanRelaxOpen : (WaveVPinRuns(InterfaceVersion, bEdgeOpen) ? WaveAutoMeanRelaxPinned : WaveAutoMeanRelaxUnpinned);
		const float Wanted = WaveMeanRelax < 0.f ? Auto : WaveMeanRelax;
		return FMath::Clamp(Wanted, 0.f, 0.5f / WaveWorstStepSeconds(bFixedTick, TickSeconds));
	}

	/** The bodies' own fields' mean relax (1/s): fnb.Waves.SelfMeanRelax, clamped to WaveSelfMeanRelaxMax and to 0.5 / tick (the self step
	 * always steps TickSeconds, whatever the core's frame does). */
	float WaveSelfMeanRelaxRate(float TickSeconds)
	{
		return FMath::Clamp(WaveSelfMeanRelax, 0.f, FMath::Min(WaveSelfMeanRelaxMax, 0.5f / FMath::Max(TickSeconds, 1.e-4f)));
	}
	/** fnb.Waves.ScaleDown: the wave grid's scale-down S for every simulation without an override of its own (User.Wave.ScaleDown): the core
	 * runs the waves on its pressure buffer's cells over S, so a tick costs 1 / S² of what it does at 1 and each body's own field spans S times
	 * the water for the same cost. A scalability variable, so the quality tiers set it. */
	int32 WaveScaleDown = 1;
	static FAutoConsoleVariableRef CVarWaveScaleDown(TEXT("fnb.Waves.ScaleDown"), WaveScaleDown,
		TEXT("FluidNinja Waves: the wave grid's scale-down, 1, 2 or 4: the waves run on the pressure buffer's cells over it (User.Wave.ScaleDown), a tick costing 1 / S² of what it does at 1. Higher is cheaper and coarser: short ripples are lost. Clamped to 1-4; 3 counts as 2. A simulation's own override (its Wave Sim component or Wave Sims entry, above 0) wins. Needs a core at interface version 3; an older one runs at 1. Changing it starts the simulations' waves afresh. Default 1. Set by sg.WaterQuality (4 / 2 / 1 / 1 / 1, Low to Cinematic)."),
		ECVF_Scalability | ECVF_Default);
	/** fnb.Waves.Foam: the foam channel's kill switch (with the Foam setting): 0 sets User.Wave.Foam 0, so the core accumulates no foam and
	 * its Wave Aux writes 0 into the foam target (the surfaces go clean within a tick; the small R16F target stays allocated and listed).
	 * The physics is identical either way. A scalability variable, so the quality tiers set it. */
	int32 WaveFoamOn = 1;
	static FAutoConsoleVariableRef CVarWaveFoam(TEXT("fnb.Waves.Foam"), WaveFoamOn,
		TEXT("FluidNinja Waves: the foam channel (whitecaps, wake trails, impact rings) on the simulations' surfaces (User.Wave.Foam; needs a core at interface version 4). 0 turns the accumulation off and the surfaces go clean within a tick; any other value leaves it to the Foam setting. The waves and the physics are unchanged either way. Default 1. Set by sg.WaterQuality (off at Low)."),
		ECVF_Scalability | ECVF_Default);
	/** fnb.Waves.MaxActiveSims: only the N simulations whose waves are nearest the camera (the distance to their nearest wave source divided
	 * by their importance) run waves; the others are switched off on their cores, raster no sources and their bodies feel no waves, until
	 * they come near. 0: all of them. A wave source is a floating body on the simulation's surface (awake, its Waves on) or a swimmer in its
	 * square; a simulation with none ranks behind those with one. Ranking by the square alone let a simulation that follows the player (a
	 * river template: the camera is always inside its square) win every time, so at fnb.Waves.MaxActiveSims 1 a pool full of floating bodies
	 * 15 m away never ran waves (the demo's Low tier, 2026-10-18). */
	int32 WaveMaxActiveSims = 0;
	static FAutoConsoleVariableRef CVarWaveMaxActiveSims(TEXT("fnb.Waves.MaxActiveSims"), WaveMaxActiveSims,
		TEXT("FluidNinja Waves: at most this many simulations run waves, those whose nearest wave source (a floating body on their surface, or a swimmer in their square) is nearest the camera, over the simulation's Importance; a simulation with no wave source ranks behind those with one. Ranked each second with 10 % of hysteresis and a second's dwell; the others are off (no waves, their bodies feel none) until they come near. Lower saves GPU time on levels with many simulations. 0 or less: all. Default 0 here, but the quality tiers set it: 1 / 2 / 3 / 4 / 0 (Low to Cinematic), so 4 at Epic, the desktop default."),
		ECVF_Scalability | ECVF_Default);
	/** fnb.Waves.SubmergeRelease: a rigid hull whose top goes under keeps pressing the static head it went under with, fading as
	 * e^(−t/τ) with τ = √(w / g) (w its waterline width as it went under), and takes it back up the same way when it comes through the
	 * surface again; 0 is the old behaviour, the head dropped in the one tick the top goes under (an 87 cm hollow left to spring back
	 * under a plunged 15 t block: the documented spike). For A/B. */
	int32 WaveSubmergeRelease = 1;
	static FAutoConsoleVariableRef CVarWaveSubmergeRelease(TEXT("fnb.Waves.SubmergeRelease"), WaveSubmergeRelease,
		TEXT("FluidNinja Waves: 1 releases a rigid hull's static head gradually once its top goes under (it fades as e^(-t/tau), tau = sqrt(waterline width / g), and comes back the same way when the hull surfaces); 0 drops it in one tick, the old behaviour (its hollow can spring back as a spike), for A/B. Any value but 0 is on. Swimmers are not affected; with fnb.Waves.PlungeSource on it applies to floating hulls only. Default 1."));
	/** The release ends (the record is dropped) once P's multiplier falls below this: 1 % of a block's 87 cm cap is under a centimetre. */
	constexpr float WaveSubmergeReleaseFloor = 0.01f;

	/**
	 * fnb.Waves.PlungeSource: a body that sinks (its resting draft below 0: its whole hull cannot carry its mass) presses a deep volume head
	 * instead of the capped static head. Its submerged volume is carried up to the surface through the exact linear transfer of a volume
	 * source at depth over a flat bed (FluidNinjaBuoyancyWaveSource.usf, DeepHeadCS), from its first contact. A plunge's first wave is then a
	 * long one (2.1-2.8 s at the bench's probes, where the capped head sent a 1.4 s packet first), though still shorter than Evers' 3.4 s,
	 * which no linear volume source reaches. The head is held while the body moves and through its wave event, with no graded release; it
	 * sends no ripple impulses, which stood in for the displacement a lagging surface head missed and which the deep head now carries (its
	 * physics still feels the slam and the drag); and it has no own field (Feel 0). Floating bodies take exactly today's path. 0: the capped
	 * static head with the graded release and the impulses for every body, for A/B. Measured on the numpy port of Wave Prepare and Wave Step:
	 * the bench's 15 t block, 54 of 54 Evers measures inside the scatter, both drops; the GPU's own run is still to come.
	 */
	int32 WavePlungeSource = 1;
	static FAutoConsoleVariableRef CVarWavePlungeSource(TEXT("fnb.Waves.PlungeSource"), WavePlungeSource,
		TEXT("FluidNinja Waves: 1 gives a body that sinks (resting draft below 0) a deep volume head: its submerged volume carried up to the surface by the finite-depth kernel, from first contact, held while it moves, fading slowly after a long rest or once the body is gone; it sends no ripple impulses and has no own field. Floating bodies are not affected. 0: the capped static head with the graded release and the impulses, the old behaviour, for A/B. Any value but 0 is on. Switching it drops the deep heads at once (a step, for A/B only). Default 1."));
	/** fnb.Waves.PlungeTaperDepths: a deep head reaches this many water depths from its centre, the last fifth of it a cos² taper, and is
	 * renormalised for what the taper takes off. At 4 the finite-depth kernel (it falls as e^(−π r / 2h)) has 99.4 % of the volume inside,
	 * and the bench's results moved under 1 % between a 12 m and a 24 m reach; 2 failed (46 of 54). The reach is also where the core's
	 * breaking is not judged while the head is there (its free-cell mask sees P), and the size of the GPU's work. */
	float WavePlungeTaperDepths = 4.f;
	static FAutoConsoleVariableRef CVarWavePlungeTaperDepths(TEXT("fnb.Waves.PlungeTaperDepths"), WavePlungeTaperDepths,
		TEXT("FluidNinja Waves: a sinker's deep head reaches this many water depths from its centre (the last fifth a cos^2 taper; the volume renormalised). Larger costs more GPU work and widens the disc where the core does not judge breaking; 2 failed the bench. At least 0.5 (smaller counts as 0.5). Needs fnb.Waves.PlungeSource on. Default 4."));
	/** fnb.Waves.PlungeLag: a first-order lag on the deep column's top and bottom, τ = c √(h / g) (c this value; 0, the default, off). The
	 * fallback if a fast entry ever draws glass: in the port c = 1 took the bench block's peak P from 93 to 60 cm and the steepest drawn
	 * slope outside it from 1.01 to 0.75. */
	float WavePlungeLag = 0.f;
	static FAutoConsoleVariableRef CVarWavePlungeLag(TEXT("fnb.Waves.PlungeLag"), WavePlungeLag,
		TEXT("FluidNinja Waves: the deep head's column follows the hull through a lag of tau = c * sqrt(depth / g), c this value. 0 (the default) off; negative counts as 0. 1 softens a fast entry if it ever draws glass (in the port it took a 15 t block's peak head from 93 to 60 cm). Needs fnb.Waves.PlungeSource on."));
	/** A deep head's body is at rest below these speeds (cm/s, rad/s); after WaveDeepRestSeconds at rest its head fades. */
	constexpr double WaveDeepRestSpeedCmS = 2.0;
	constexpr double WaveDeepRestSpinRad = 0.02;
	constexpr double WaveDeepRestSeconds = 15.0;
	/** The slow fade's time constant (s). A static P changing this slowly makes waves of about P / (ω τ)², under a percent of it even at the
	 * longest waves a 12 m head carries, so it goes without a ring; a release over 0.42 s fell straight into the long-wave band (the bench's
	 * first crests a third low in the port), and one over 4 s still left its troughs 4-8 % high. While a head fades, the core's breaking is
	 * judged over its disc again only once it is gone. */
	constexpr double WaveDeepFadeSeconds = 10.0;
	/** A fading head is dropped once it presses less than this (cm: its column's height times its fade), a step far below anything drawn. */
	constexpr double WaveDeepDropCm = 0.05;
	/** Below this submerged volume (cm³) a deep head sends nothing: the hull only grazes the water. */
	constexpr double WaveDeepMinVolumeCm3 = 1.0;
	/** The deep head's taper starts at this share of its reach. */
	constexpr double WaveDeepTaperStartShare = 0.8;
	/** The equivalent rectangle's half extents and the column's height are at least this (cm): a hull's submerged sliver as thin as a sheet
	 * would make the kernel's rectangle degenerate; its volume is kept by P's multiplier. */
	constexpr double WaveDeepMinExtentCm = 1.0;

	/** The lowest point of the hull in the world now (cm): above the water, nothing of it is in. */
	double WaveHullBottom(const FluidNinjaBuoyancy::FHull& Hull, const FTransform& Frame)
	{
		double Bottom = TNumericLimits<double>::Max();
		for (const FVector3f& Vertex : Hull.Vertices)
		{
			Bottom = FMath::Min(Bottom, Frame.TransformPosition(FVector(Vertex)).Z);
		}
		return Bottom;
	}

	/** A hull's submerged part as the deep head takes it: its volume, and the rectangle and column with the same plan second moments and the
	 * same vertical spread (a uniform column over a rectangle 2a x 2b from z1 to z2 has variance a²/3, b²/3 across and (z2 − z1)²/12 down). */
	struct FWaveDeepShape
	{
		double VolumeCm3 = 0.0;
		/** World XY of the submerged volume's centroid, and the angle of the rectangle's first axis from world X (rad). */
		FVector2D Centre = FVector2D::ZeroVector;
		double AxisAngle = 0.0;
		/** Half extents along that axis and across it, cm. */
		FVector2D Half = FVector2D::ZeroVector;
		/** The column's top and bottom below the surface, cm, positive down. */
		double TopCm = 0.0;
		double BottomCm = 0.0;
	};

	/**
	 * The hull's part below SurfaceZ (the wave-free water) and its moments: every triangle clipped at the surface, and the volume integrals of
	 * the submerged part summed over signed tetrahedra from an origin in the surface plane, so the cap the clip leaves at the surface adds
	 * nothing (its tetrahedra are flat) and need not be built: V = Σ det / 6, ∫x = Σ det (A + B + C) / 24, ∫x_i x_j = Σ det (A_i A_j + B_i B_j
	 * + C_i C_j + S_i S_j) / 120 with S = A + B + C. Exact for any closed hull wound outwards, and continuous as the hull moves (no cell
	 * snapping). The plan covariance is decomposed in the frame of the body's own X axis laid flat (its Y axis while X stands upright), so a
	 * box keeps its own axes even when its two plan moments are equal (a square), where principal axes alone are not defined. For an upright
	 * box it gives the box exactly: 174 x 300 cm across at any yaw, z1 = 0 and z2 = its depth while it pierces, its top and bottom once under
	 * (checked offline at several yaws). A tilted or rounded hull gets the rectangle and column of the same second moments. False when nothing
	 * of it is under.
	 */
	bool WaveSubmergedShape(const FluidNinjaBuoyancy::FHull& Hull, const FTransform& Frame, double SurfaceZ, FWaveDeepShape& Out)
	{
		Out = FWaveDeepShape();
		if (Hull.Triangles.IsEmpty() || Hull.Vertices.IsEmpty())
		{
			return false;
		}
		// Relative to a point of the surface under the body, so the numbers stay small.
		const FVector Origin(Frame.GetLocation().X, Frame.GetLocation().Y, SurfaceZ);
		TArray<FVector, TInlineAllocator<256>> World;
		World.Reserve(Hull.Vertices.Num());
		for (const FVector3f& Vertex : Hull.Vertices)
		{
			World.Add(Frame.TransformPosition(FVector(Vertex)) - Origin);
		}
		double Volume = 0.0;
		FVector First = FVector::ZeroVector;
		double Second[3][3] = {};
		const auto AddTetrahedron = [&Volume, &First, &Second](const FVector& A, const FVector& B, const FVector& C)
		{
			const double Det = FVector::DotProduct(A, FVector::CrossProduct(B, C));
			const FVector Sum = A + B + C;
			Volume += Det / 6.0;
			First += Sum * (Det / 24.0);
			for (int32 I = 0; I < 3; ++I)
			{
				for (int32 J = 0; J < 3; ++J)
				{
					Second[I][J] += Det / 120.0 * (A[I] * A[J] + B[I] * B[J] + C[I] * C[J] + Sum[I] * Sum[J]);
				}
			}
		};
		for (const FIntVector3& Triangle : Hull.Triangles)
		{
			if (!World.IsValidIndex(Triangle.X) || !World.IsValidIndex(Triangle.Y) || !World.IsValidIndex(Triangle.Z))
			{
				continue;
			}
			// The triangle's part under the surface (z <= 0): none, a triangle or a quadrilateral, in the triangle's own winding.
			const FVector Corners[3] = {World[Triangle.X], World[Triangle.Y], World[Triangle.Z]};
			FVector Kept[4];
			int32 Count = 0;
			for (int32 Edge = 0; Edge < 3; ++Edge)
			{
				const FVector& P = Corners[Edge];
				const FVector& Q = Corners[(Edge + 1) % 3];
				const bool bPUnder = P.Z <= 0.0;
				const bool bQUnder = Q.Z <= 0.0;
				if (bPUnder)
				{
					Kept[Count++] = P;
				}
				if (bPUnder != bQUnder)
				{
					Kept[Count++] = P + (Q - P) * (P.Z / (P.Z - Q.Z));
				}
			}
			for (int32 Fan = 1; Fan + 1 < Count; ++Fan)
			{
				AddTetrahedron(Kept[0], Kept[Fan], Kept[Fan + 1]);
			}
		}
		// A hull wound inwards gives every integral the opposite sign; the moments' ratios below are the same either way.
		if (Volume < 0.0)
		{
			Volume = -Volume;
			First = -First;
			for (int32 I = 0; I < 3; ++I)
			{
				for (int32 J = 0; J < 3; ++J)
				{
					Second[I][J] = -Second[I][J];
				}
			}
		}
		if (!(Volume > 0.0) || !FMath::IsFinite(Volume))
		{
			return false;
		}
		const FVector Mean = First / Volume;
		const auto Covariance = [&Second, &Mean, Volume](int32 I, int32 J) { return Second[I][J] / Volume - Mean[I] * Mean[J]; };
		// Down: the centroid's depth below the surface and the column of the same variance about it.
		const double Depth = -Mean.Z;
		const double Spread = FMath::Sqrt(3.0 * FMath::Max(Covariance(2, 2), 0.0));
		// Across: the plan covariance in the frame of the body's flat X axis, then its principal axes there.
		FVector Reference = Frame.GetRotation().GetAxisX();
		if (FVector2D(Reference.X, Reference.Y).SizeSquared() < 0.25)
		{
			Reference = Frame.GetRotation().GetAxisY();
		}
		const double Yaw = FMath::Atan2(Reference.Y, Reference.X);
		const double Cr = FMath::Cos(Yaw);
		const double Sr = FMath::Sin(Yaw);
		const double Cxx = Covariance(0, 0);
		const double Cyy = Covariance(1, 1);
		const double Cxy = Covariance(0, 1);
		const double Cuu = Cr * Cr * Cxx + 2.0 * Cr * Sr * Cxy + Sr * Sr * Cyy;
		const double Cvv = Sr * Sr * Cxx - 2.0 * Cr * Sr * Cxy + Cr * Cr * Cyy;
		const double Cuv = (Cr * Cr - Sr * Sr) * Cxy + Cr * Sr * (Cyy - Cxx);
		// Equal moments and no cross term (a square, a disc): the body's own axes, not the noise of a near-zero angle.
		const double Theta = FMath::Abs(Cuv) + FMath::Abs(Cuu - Cvv) > 1e-6 * FMath::Max(Cuu + Cvv, 1e-12) ? 0.5 * FMath::Atan2(2.0 * Cuv, Cuu - Cvv) : 0.0;
		const double Ct = FMath::Cos(Theta);
		const double St = FMath::Sin(Theta);
		const double Along = Ct * Ct * Cuu + 2.0 * Ct * St * Cuv + St * St * Cvv;
		const double Across = St * St * Cuu - 2.0 * Ct * St * Cuv + Ct * Ct * Cvv;
		Out.VolumeCm3 = Volume;
		Out.Centre = FVector2D(Origin.X + Mean.X, Origin.Y + Mean.Y);
		Out.AxisAngle = Yaw + Theta;
		Out.Half = FVector2D(FMath::Sqrt(3.0 * FMath::Max(Along, 0.0)), FMath::Sqrt(3.0 * FMath::Max(Across, 0.0)));
		Out.TopCm = FMath::Max(Depth - Spread, 0.0);
		Out.BottomCm = FMath::Max(Depth + Spread, Out.TopCm);
		return FMath::IsFinite(Out.Centre.X) && FMath::IsFinite(Out.Centre.Y) && FMath::IsFinite(Out.Half.X) && FMath::IsFinite(Out.Half.Y)
			&& FMath::IsFinite(Out.TopCm) && FMath::IsFinite(Out.BottomCm);
	}

	/** K0(x), the modified Bessel function of the second kind of order 0, x > 0 (Abramowitz & Stegun 9.8.5 with 9.8.1, and 9.8.6; relative
	 * error under 2e-7). */
	double WaveBesselK0(double X)
	{
		if (X <= 2.0)
		{
			const double T = X / 3.75;
			const double T2 = T * T;
			const double I0 = 1.0 + T2 * (3.5156229 + T2 * (3.0899424 + T2 * (1.2067492 + T2 * (0.2659732 + T2 * (0.0360768 + T2 * 0.0045813)))));
			const double U = X * X / 4.0;
			return -FMath::Loge(X / 2.0) * I0 - 0.57721566 + U * (0.42278420 + U * (0.23069756 + U * (0.03488590 + U * (0.00262698 + U * (0.00010750 + U * 0.0000074)))));
		}
		const double T = 2.0 / X;
		const double Poly = 1.25331414 + T * (-0.07832358 + T * (0.02189568 + T * (-0.01062446 + T * (0.00587872 + T * (-0.00251540 + T * 0.00053208)))));
		return FMath::Exp(-X) / FMath::Sqrt(X) * Poly;
	}

	/**
	 * The share of a deep head's volume (0..1) its taper takes off. The column's far field in water Depth deep is its eigenfunction series
	 * Σ_n [cos(κ_n z1) − cos(κ_n z2)] K0(κ_n r) / (π h) per unit of footprint, κ_n = (n + 1/2) π / h (three modes: the next is e^(−2π r / h)
	 * smaller), each times the rectangle's far-field factor sinh(κ a cos φ) / (κ a cos φ) · sinh(κ b sin φ) / (κ b sin φ) averaged over the
	 * directions; integrated against (1 − taper) from the taper's start to well past its end (Simpson), over the column's whole volume per
	 * unit of footprint, z2 − z1. Checked against the k-space head on the GPU's grid: 0.399 / 0.795 / 1.090 / 1.245 % for the bench block at
	 * four depths, the k-space truth 0.399 / 0.795 / 1.090 / 1.245 %.
	 */
	double WaveDeepTaperLoss(double HalfA, double HalfB, double Top, double Bottom, double Depth, double Start, double End)
	{
		if (!(Bottom > Top) || !(End > Start) || !(Depth > 0.0))
		{
			return 0.0;
		}
		constexpr int32 Modes = 3;
		constexpr int32 Angles = 8;
		constexpr int32 Steps = 64;   // even, for Simpson
		double Loss = 0.0;
		for (int32 Mode = 0; Mode < Modes; ++Mode)
		{
			const double Kappa = (Mode + 0.5) * UE_DOUBLE_PI / Depth;
			const double Weight = (FMath::Cos(Kappa * Top) - FMath::Cos(Kappa * Bottom)) / (UE_DOUBLE_PI * Depth);
			double Factor = 0.0;
			for (int32 Angle = 0; Angle < Angles; ++Angle)
			{
				const double Phi = (Angle + 0.5) * 0.5 * UE_DOUBLE_PI / Angles;
				const double EA = Kappa * HalfA * FMath::Cos(Phi);
				const double EB = Kappa * HalfB * FMath::Sin(Phi);
				Factor += (EA > 1e-6 ? FMath::Sinh(EA) / EA : 1.0) * (EB > 1e-6 ? FMath::Sinh(EB) / EB : 1.0);
			}
			Factor /= Angles;
			const double Far = End + 8.0 * Depth / (2.0 * Mode + 1.0);
			const double Step = (Far - Start) / Steps;
			double Sum = 0.0;
			for (int32 Index = 0; Index <= Steps; ++Index)
			{
				const double Radius = Start + Index * Step;
				const double Ramp = FMath::Sin(0.5 * UE_DOUBLE_PI * FMath::Clamp((End - Radius) / (End - Start), 0.0, 1.0));
				const double Value = (1.0 - Ramp * Ramp) * WaveBesselK0(Kappa * Radius) * 2.0 * UE_DOUBLE_PI * Radius;
				Sum += Value * ((Index == 0 || Index == Steps) ? 1.0 : ((Index & 1) != 0 ? 4.0 : 2.0));
			}
			Loss += Weight * Factor * Sum * Step / 3.0;
		}
		return FMath::Clamp(Loss / (Bottom - Top), 0.0, 0.5);
	}

	/** fnb.Waves.Memory: the GPU memory the waves hold in this world, one line per target and the total (UFluidNinjaBuoyancyWaves::ReportMemory). */
	static FAutoConsoleCommandWithWorldAndArgs CmdWaveMemory(TEXT("fnb.Waves.Memory"),
		TEXT("FluidNinja Waves: lists the GPU memory the waves hold (each simulation's source texture and Wave Aux target, the self-field atlas and pools, the bench's readbacks while they run) and the total, MB. No arguments."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			const UFluidNinjaBuoyancySubsystem* Hydro = World ? World->GetSubsystem<UFluidNinjaBuoyancySubsystem>() : nullptr;
			const UFluidNinjaBuoyancyWaves* Waves = Hydro ? Hydro->GetWaves() : nullptr;
			if (!Waves)
			{
				UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy waves: no waves in this world (no FluidNinja Buoyancy subsystem)."));
				return;
			}
			TArray<FFluidNinjaWaveMemoryItem> Items;
			Waves->ReportMemory(Items);
			double Total = 0.0;
			for (const FFluidNinjaWaveMemoryItem& Item : Items)
			{
				UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy waves memory: %-48s %5d x %-5d %-10s %8.2f MB"), *Item.Name, Item.Size.X, Item.Size.Y, *Item.Format, Item.MegaBytes);
				Total += Item.MegaBytes;
			}
			UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy waves memory: %d targets, %.2f MB in all (the per-frame transients, a simulation's static head and impulse textures and a batch's WaveOp slices, come from the render graph's pool and are not counted)."),
				Items.Num(), Total);
		}));

	/** An int32 user parameter of the core: its instance's value (the override store lists every user parameter), else the system's
	 * default, else Fallback (a core without it). */
	int32 WaveReadCoreInt(UNiagaraComponent& Core, const FName& Name, int32 Fallback)
	{
		const FNiagaraVariable Variable(FNiagaraTypeDefinition::GetIntDef(), Name);
		const FNiagaraParameterStore& Overrides = Core.GetOverrideParameters();
		if (Overrides.IndexOf(Variable) != INDEX_NONE)
		{
			return Overrides.GetParameterValue<int32>(Variable);
		}
		if (const UNiagaraSystem* System = Core.GetAsset())
		{
			const FNiagaraParameterStore& Exposed = System->GetExposedParameters();
			if (Exposed.IndexOf(Variable) != INDEX_NONE)
			{
				return Exposed.GetParameterValue<int32>(Variable);
			}
		}
		return Fallback;
	}

	/**
	 * How wide the hull is where it comes up through the water at WaterZ (cm): the spread of its waterline section across its narrower
	 * principal axis (0 when nothing of it crosses that height). A log speared in at an angle is 40 cm wide there, however its box lies.
	 */
	double WaveSectionWidth(const FluidNinjaBuoyancy::FHull& Hull, const FTransform& Frame, double WaterZ)
	{
		TArray<FVector, TInlineAllocator<256>> World;
		World.Reserve(Hull.Vertices.Num());
		for (const FVector3f& Vertex : Hull.Vertices)
		{
			World.Add(Frame.TransformPosition(FVector(Vertex)));
		}
		TArray<FVector2D, TInlineAllocator<256>> Crossings;
		const auto Cross = [&Crossings, WaterZ](const FVector& P, const FVector& Q)
		{
			if ((P.Z - WaterZ) * (Q.Z - WaterZ) < 0.0)
			{
				Crossings.Add(FVector2D(FMath::Lerp(P, Q, (WaterZ - P.Z) / (Q.Z - P.Z))));
			}
		};
		for (const FIntVector3& Triangle : Hull.Triangles)
		{
			if (World.IsValidIndex(Triangle.X) && World.IsValidIndex(Triangle.Y) && World.IsValidIndex(Triangle.Z))
			{
				Cross(World[Triangle.X], World[Triangle.Y]);
				Cross(World[Triangle.Y], World[Triangle.Z]);
				Cross(World[Triangle.Z], World[Triangle.X]);
			}
		}
		if (Crossings.Num() < 3)
		{
			return 0.0;
		}
		FVector2D Mean = FVector2D::ZeroVector;
		for (const FVector2D& Point : Crossings)
		{
			Mean += Point;
		}
		Mean /= Crossings.Num();
		double XX = 0.0;
		double XY = 0.0;
		double YY = 0.0;
		for (const FVector2D& Point : Crossings)
		{
			const FVector2D D = Point - Mean;
			XX += D.X * D.X;
			XY += D.X * D.Y;
			YY += D.Y * D.Y;
		}
		const double Angle = 0.5 * FMath::Atan2(2.0 * XY, XX - YY);
		const FVector2D Across(-FMath::Sin(Angle), FMath::Cos(Angle));
		double Low = TNumericLimits<double>::Max();
		double High = -TNumericLimits<double>::Max();
		for (const FVector2D& Point : Crossings)
		{
			const double Along = FVector2D::DotProduct(Point - Mean, Across);
			Low = FMath::Min(Low, Along);
			High = FMath::Max(High, Along);
		}
		return High - Low;
	}

	/** The world XY box of the hull's local bounds turned and moved to Frame (a little generous for a turned hull). */
	FBox2D WaveHullBox(const FluidNinjaBuoyancy::FHull& Hull, const FTransform& Frame)
	{
		FBox2D Box(ForceInit);
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector Point((Corner & 1) ? Hull.BoundsMax.X : Hull.BoundsMin.X, (Corner & 2) ? Hull.BoundsMax.Y : Hull.BoundsMin.Y,
				(Corner & 4) ? Hull.BoundsMax.Z : Hull.BoundsMin.Z);
			Box += FVector2D(Frame.TransformPosition(Point));
		}
		return Box;
	}

	/** The highest point of the hull in the world now (cm): below the water, the body is wholly under and has no static head. */
	double WaveHullTop(const FluidNinjaBuoyancy::FHull& Hull, const FTransform& Frame)
	{
		double Top = -TNumericLimits<double>::Max();
		for (const FVector3f& Vertex : Hull.Vertices)
		{
			Top = FMath::Max(Top, Frame.TransformPosition(FVector(Vertex)).Z);
		}
		return Top;
	}

	/** fnb.Waves.PlungeSource's test for a body that sinks: its whole hull cannot carry its mass with the lift its physics really has.
	 * GetRestingDraft is below 0 when the water of the finest hull's whole volume (ρ V, kg) weighs less than the body, but
	 * FluidNinjaBuoyancy::DraftForMass works it out at a Buoyancy Multiplier of 1 (StaticParams), while every physics step lifts by the
	 * multiplier (an art control, 0 and up). A body with a multiplier above 1 and a mass between ρ V and the multiplier times ρ V floats,
	 * yet reads -1; taken for a sinker it would lose its static head, its ripple impulses and its own field (so it would feel no waves at
	 * all), and its head would fade after its long rest. It is no sinker here: it takes the floating path, its draft clamped to 0, as
	 * before the plunge source. At a multiplier of 1 or less the resting draft decides alone, exactly as before (a draft of 0 or more is
	 * never a sinker, even where a multiplier below 1 sinks it: this test only takes bodies off the deep path, it puts none on it). The
	 * mass is the body instance's now and the volume the finest physics hull's (cm³), the ones RefreshRestingDraft measures with; the far
	 * levels' buoyancy scales match their lift to that hull's. Without a physics body or a hull it is no sinker (the floating path). */
	bool WaveBodySinks(const UFluidNinjaBuoyancyComponent& Component)
	{
		if (!(Component.GetRestingDraft() < 0.f))
		{
			return false;
		}
		// A small body keeps the floater path: the deep head of a few hundred cm³ carries no wave anyone sees, while dropping its ripple
		// impulses (as a sinker's are) took away the whole of its impact ring (the showcase's 30 g arrow went in without a ring).
		if (const TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe> Hull = Component.GetHull(0); !Hull.IsValid() || Hull->Volume < 500.0)
		{
			return false;
		}
		const double Multiplier = FMath::Max(static_cast<double>(Component.BuoyancyMultiplier), 0.0);
		if (Multiplier <= 1.0)
		{
			return true;
		}
		const UPrimitiveComponent* Primitive = Component.GetSimulatedPrimitive();
		const FBodyInstance* Instance = Primitive ? Primitive->GetBodyInstance() : nullptr;
		const TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe> Finest = Component.GetHull(0);
		if (!Instance || !Finest.IsValid())
		{
			return false;
		}
		const double Rho = UFluidNinjaBuoyancySettings::Get().WaterDensity * 1e-6;   // kg/cm³
		return static_cast<double>(Instance->GetBodyMass()) > Multiplier * Rho * static_cast<double>(Finest->Volume);
	}

	/** A world XY box relative to a grid's lower-left corner: (min x, min y, max x, max y). */
	FVector4f WaveRelativeBox(const FBox2D& Box, const FVector2D& GridMin)
	{
		if (!Box.bIsValid)
		{
			return FVector4f::Zero();
		}
		return FVector4f(static_cast<float>(Box.Min.X - GridMin.X), static_cast<float>(Box.Min.Y - GridMin.Y),
			static_cast<float>(Box.Max.X - GridMin.X), static_cast<float>(Box.Max.Y - GridMin.Y));
	}

	/** The body's frame for the raster: the rows of its body-to-world rotation (column vectors: world = R·local + T), each with one
	 * component of its translation (X and Y relative to the grid, Z absolute). R's columns are the body's axes in the world. */
	void WaveSetFrame(FluidNinjaBuoyancyWaveSource::FBody& Record, const FQuat& Rotation, const FVector& Location, const FVector2D& GridMin)
	{
		const FVector AxisX = Rotation.GetAxisX();
		const FVector AxisY = Rotation.GetAxisY();
		const FVector AxisZ = Rotation.GetAxisZ();
		Record.Axis[0] = FVector4f(static_cast<float>(AxisX.X), static_cast<float>(AxisY.X), static_cast<float>(AxisZ.X), static_cast<float>(Location.X - GridMin.X));
		Record.Axis[1] = FVector4f(static_cast<float>(AxisX.Y), static_cast<float>(AxisY.Y), static_cast<float>(AxisZ.Y), static_cast<float>(Location.Y - GridMin.Y));
		Record.Axis[2] = FVector4f(static_cast<float>(AxisX.Z), static_cast<float>(AxisY.Z), static_cast<float>(AxisZ.Z), static_cast<float>(Location.Z));
	}

	/** The volume of an upright capsule below the water at WaterZ (cm³), the shape the raster takes for the swimmer: a straight part of
	 * half-length max(HalfHeight - Radius, 0) between two hemispheres. Each piece is cut at the water: the lower cap as a spherical cap,
	 * the straight part as a cylinder, the upper cap from its equator up. */
	double WaveCapsuleVolumeBelow(double CentreZ, double Radius, double HalfHeight, double WaterZ)
	{
		const double R = FMath::Max(Radius, 0.0);
		const double Straight = FMath::Max(HalfHeight - R, 0.0);
		const double Level = WaterZ - CentreZ;
		const double LowerCap = FMath::Clamp(Level + Straight + R, 0.0, R);
		const double Middle = FMath::Clamp(Level + Straight, 0.0, 2.0 * Straight);
		const double UpperCap = FMath::Clamp(Level - Straight, 0.0, R);
		return UE_DOUBLE_PI * (LowerCap * LowerCap * (3.0 * R - LowerCap) / 3.0 + R * R * Middle + R * R * UpperCap - UpperCap * UpperCap * UpperCap / 3.0);
	}

	/** The cells of a world XY box on a grid (lower-left GridMin, CellCm), one cell more all round: (min x, min y, max x, max y),
	 * inclusive, clamped to the grid; false when it misses the grid. */
	bool WaveCellBox(const FBox2D& Box, const FVector2D& GridMin, double CellCm, const FIntPoint& Cells, FIntVector4& Out)
	{
		if (!Box.bIsValid || !(CellCm > 0.0))
		{
			return false;
		}
		const int64 X0 = FMath::FloorToInt64((Box.Min.X - GridMin.X) / CellCm) - 1;
		const int64 Y0 = FMath::FloorToInt64((Box.Min.Y - GridMin.Y) / CellCm) - 1;
		const int64 X1 = FMath::FloorToInt64((Box.Max.X - GridMin.X) / CellCm) + 1;
		const int64 Y1 = FMath::FloorToInt64((Box.Max.Y - GridMin.Y) / CellCm) + 1;
		if (X1 < 0 || Y1 < 0 || X0 >= Cells.X || Y0 >= Cells.Y)
		{
			return false;
		}
		Out = FIntVector4(static_cast<int32>(FMath::Max<int64>(X0, 0)), static_cast<int32>(FMath::Max<int64>(Y0, 0)),
			static_cast<int32>(FMath::Min<int64>(X1, Cells.X - 1)), static_cast<int32>(FMath::Min<int64>(Y1, Cells.Y - 1)));
		return true;
	}

	/** ω (rad/s) of a wave of wavenumber K (rad/cm) in water Depth deep: ω² = (g·k + σ/ρ·k³)·tanh(k·h), as the core's dispersion. */
	double WaveOmega(double K, double Depth, double Gravity)
	{
		return FMath::Sqrt(FMath::Max((Gravity * K + WaveCapillarity * K * K * K) * FMath::Tanh(FMath::Min(K * Depth, 20.0)), 0.0));
	}

	/**
	 * A body's rest footprint on the simulation's cells, with the stiffness of its heave, roll and pitch, its added mass and the natural
	 * frequencies the two give: what its radiation damping (WaveRadiationAt) is summed over. The footprint is the hull resting in its rest
	 * attitude at its rest draft on the cells: a cell is in where its vertical line meets the hull below the water. Its modes are the
	 * footprint itself (heave), times the arm across (roll, about the rest attitude's world X) and along (pitch, about Y).
	 * The added mass is by strip theory on the footprint's rows of cells. A strip of the footprint B wide carries ρ π B² / 8 of water per
	 * unit of its length (the same for a semicircular section and for a flat one at the free surface), so heave is ρ π / 8 × Σ B_row² × cell
	 * over the rows across the long axis (each row spans the short axis), pitch the same rows weighted by their arm squared along the
	 * long axis from the centroid, and roll the rows the other way (each spanning the long axis) by their arm squared across it; all
	 * times L / (L + B) for the water that goes round the ends instead, L and B the footprint's long and short extents (a disc comes out
	 * at half the water a hemisphere of its size displaces, a long raft at the strip value). Each natural frequency is √(C / (I + s A))
	 * with the waterplane's stiffness C, the body's mass or inertia I and its added mass A at Wave Added Mass Scale s.
	 */
	bool WaveFootprint(const FluidNinjaBuoyancy::FHull& Hull, const FQuat& Attitude, double Draft, double Mass, const FVector& InertiaLocal, double Cell, double Depth,
		double Gravity, double Rho, double AddedMassScale, FFluidNinjaWaveFootprint& Out, FVector& OutRollAxis, FVector& OutPitchAxis)
	{
		Out = FFluidNinjaWaveFootprint();
		OutRollAxis = Attitude.Inverse().RotateVector(FVector::XAxisVector);
		OutPitchAxis = Attitude.Inverse().RotateVector(FVector::YAxisVector);
		if (Hull.Triangles.IsEmpty() || Hull.Vertices.IsEmpty() || !(Draft > 0.0) || !(Cell > 0.0) || !(Mass > 0.0) || !(Depth > 0.0))
		{
			return false;
		}
		// The hull in its rest attitude about the body's origin, and the water at the rest draft above its lowest point.
		TArray<FVector> Points;
		Points.Reserve(Hull.Vertices.Num());
		double Bottom = TNumericLimits<double>::Max();
		FBox2D Extent(ForceInit);
		for (const FVector3f& Vertex : Hull.Vertices)
		{
			const FVector Rested = Attitude.RotateVector(FVector(Vertex));
			Points.Add(Rested);
			Bottom = FMath::Min(Bottom, Rested.Z);
			Extent += FVector2D(Rested);
		}
		const double WaterZ = Bottom + Draft;
		const int64 FirstX = FMath::FloorToInt64(Extent.Min.X / Cell) - 1;
		const int64 FirstY = FMath::FloorToInt64(Extent.Min.Y / Cell) - 1;
		const int64 CountX = FMath::FloorToInt64(Extent.Max.X / Cell) + 2 - FirstX;
		const int64 CountY = FMath::FloorToInt64(Extent.Max.Y / Cell) + 2 - FirstY;
		if (CountX <= 0 || CountY <= 0 || CountX * CountY > 4 * 1024 * 1024)
		{
			return false;
		}
		// The footprint: every triangle below the water marks the cell centres it covers where it lies below the water there.
		TArray<uint8> Foot;
		Foot.SetNumZeroed(static_cast<int32>(CountX * CountY));
		for (const FIntVector3& Triangle : Hull.Triangles)
		{
			const FVector& A = Points[Triangle.X];
			const FVector& B = Points[Triangle.Y];
			const FVector& C = Points[Triangle.Z];
			if (FMath::Min3(A.Z, B.Z, C.Z) >= WaterZ)
			{
				continue;   // wholly above the water
			}
			const double Twice = (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X);
			if (FMath::Abs(Twice) < 1e-9)
			{
				continue;   // a wall standing upright: no vertical line crosses it
			}
			const int64 X0 = FMath::Max<int64>(FMath::FloorToInt64(FMath::Min3(A.X, B.X, C.X) / Cell - 0.5), FirstX);
			const int64 X1 = FMath::Min<int64>(FMath::CeilToInt64(FMath::Max3(A.X, B.X, C.X) / Cell - 0.5), FirstX + CountX - 1);
			const int64 Y0 = FMath::Max<int64>(FMath::FloorToInt64(FMath::Min3(A.Y, B.Y, C.Y) / Cell - 0.5), FirstY);
			const int64 Y1 = FMath::Min<int64>(FMath::CeilToInt64(FMath::Max3(A.Y, B.Y, C.Y) / Cell - 0.5), FirstY + CountY - 1);
			for (int64 J = Y0; J <= Y1; ++J)
			{
				for (int64 I = X0; I <= X1; ++I)
				{
					const double PX = (static_cast<double>(I) + 0.5) * Cell;
					const double PY = (static_cast<double>(J) + 0.5) * Cell;
					// Barycentric weights from the edge functions, each over the whole triangle's (signed) twice area.
					const double WA = ((B.X - PX) * (C.Y - PY) - (B.Y - PY) * (C.X - PX)) / Twice;
					const double WB = ((C.X - PX) * (A.Y - PY) - (C.Y - PY) * (A.X - PX)) / Twice;
					const double WC = 1.0 - WA - WB;
					if (WA < 0.0 || WB < 0.0 || WC < 0.0)
					{
						continue;
					}
					if (WA * A.Z + WB * B.Z + WC * C.Z < WaterZ)
					{
						Foot[static_cast<int32>((J - FirstY) * CountX + (I - FirstX))] = 1;
					}
				}
			}
		}
		// The footprint's cells, area, centroid and second moments; and its rows (the cells at one y) and columns (at one x), the strips.
		TArray<FVector2D>& Cells = Out.Cells;
		TArray<int32> RowCells;
		TArray<int32> ColumnCells;
		RowCells.SetNumZeroed(static_cast<int32>(CountY));
		ColumnCells.SetNumZeroed(static_cast<int32>(CountX));
		double SumX = 0.0;
		double SumY = 0.0;
		for (int64 J = 0; J < CountY; ++J)
		{
			for (int64 I = 0; I < CountX; ++I)
			{
				if (Foot[static_cast<int32>(J * CountX + I)] != 0)
				{
					const FVector2D Centre((static_cast<double>(FirstX + I) + 0.5) * Cell, (static_cast<double>(FirstY + J) + 0.5) * Cell);
					Cells.Add(Centre);
					SumX += Centre.X;
					SumY += Centre.Y;
					++RowCells[static_cast<int32>(J)];
					++ColumnCells[static_cast<int32>(I)];
				}
			}
		}
		if (Cells.IsEmpty())
		{
			return false;
		}
		const double CellArea = Cell * Cell;
		Out.CellCm = Cell;
		Out.Area = Cells.Num() * CellArea;
		Out.Middle = FVector2D(SumX / Cells.Num(), SumY / Cells.Num());
		const FVector2D& Middle = Out.Middle;
		double MomentAcross = 0.0;   // Σ (y - yc)², roll
		double MomentAlong = 0.0;    // Σ (x - xc)², pitch
		for (const FVector2D& Centre : Cells)
		{
			MomentAcross += FMath::Square(Centre.Y - Middle.Y);
			MomentAlong += FMath::Square(Centre.X - Middle.X);
		}
		Out.Stiffness[0] = Rho * Gravity * Out.Area;
		Out.Stiffness[1] = Rho * Gravity * MomentAcross * CellArea;
		Out.Stiffness[2] = Rho * Gravity * MomentAlong * CellArea;
		const auto AxisInertia = [&InertiaLocal](const FVector& Axis)
		{
			return InertiaLocal.X * Axis.X * Axis.X + InertiaLocal.Y * Axis.Y * Axis.Y + InertiaLocal.Z * Axis.Z * Axis.Z;
		};
		Out.Inertia[0] = Mass;
		Out.Inertia[1] = AxisInertia(OutRollAxis);
		Out.Inertia[2] = AxisInertia(OutPitchAxis);

		// The strips: Σ B² × cell over the columns (B the column's span across y) and over the rows (its span across x), each also
		// weighted by its arm squared from the centroid; and the footprint's extents along x and y.
		double ColumnStrips = 0.0;   // Σ B_x² cell, cm³
		double RowStrips = 0.0;      // Σ B_y² cell, cm³
		double PitchStrips = 0.0;    // Σ B_x² (x - xc)² cell, cm⁵
		double RollStrips = 0.0;     // Σ B_y² (y - yc)² cell, cm⁵
		int64 MinI = CountX;
		int64 MaxI = -1;
		int64 MinJ = CountY;
		int64 MaxJ = -1;
		for (int64 I = 0; I < CountX; ++I)
		{
			const int32 Count = ColumnCells[static_cast<int32>(I)];
			if (Count > 0)
			{
				const double Span = Count * Cell;
				const double Arm = (static_cast<double>(FirstX + I) + 0.5) * Cell - Middle.X;
				ColumnStrips += Span * Span * Cell;
				PitchStrips += Span * Span * Arm * Arm * Cell;
				MinI = FMath::Min(MinI, I);
				MaxI = FMath::Max(MaxI, I);
			}
		}
		for (int64 J = 0; J < CountY; ++J)
		{
			const int32 Count = RowCells[static_cast<int32>(J)];
			if (Count > 0)
			{
				const double Span = Count * Cell;
				const double Arm = (static_cast<double>(FirstY + J) + 0.5) * Cell - Middle.Y;
				RowStrips += Span * Span * Cell;
				RollStrips += Span * Span * Arm * Arm * Cell;
				MinJ = FMath::Min(MinJ, J);
				MaxJ = FMath::Max(MaxJ, J);
			}
		}
		const double LengthX = static_cast<double>(MaxI - MinI + 1) * Cell;
		const double LengthY = static_cast<double>(MaxJ - MinJ + 1) * Cell;
		const double Long = FMath::Max(LengthX, LengthY);
		const double Short = FMath::Min(LengthX, LengthY);
		// ρ π / 8 per strip, and the end correction L / (L + B): heave over the rows across the long axis (the columns when x is long),
		// pitch about y from the columns' arms along x, roll about x from the rows' arms along y.
		const double Strip = Rho * UE_DOUBLE_PI / 8.0 * (Long / FMath::Max(Long + Short, 1e-9));
		Out.AddedMass[0] = Strip * (LengthX >= LengthY ? ColumnStrips : RowStrips);
		Out.AddedMass[1] = Strip * RollStrips;
		Out.AddedMass[2] = Strip * PitchStrips;
		for (int32 Motion = 0; Motion < 3; ++Motion)
		{
			const double Natural = FMath::Sqrt(FMath::Max(Out.Stiffness[Motion], 0.0) / FMath::Max(Out.Inertia[Motion] + FMath::Max(AddedMassScale, 0.0) * Out.AddedMass[Motion], 1e-9));
			Out.Natural[Motion] = FMath::IsFinite(Natural) && Natural > 0.0 ? Natural : 0.0;
		}
		Out.DepthCm = Depth;
		Out.Gravity = Gravity;
		Out.Rho = Rho;
		Out.bValid = true;
		return true;
	}

	/**
	 * Radiation damping of the body's own waves at Omega (rad/s): heave (kg/s), roll or pitch (kg·cm²/s) from its rest footprint,
	 * b = ρ·g·k/(8π·c_g)·∫|f̂(k, θ)|²dθ (Haskind with Froude–Krylov forcing for a small body), k the wavenumber of Omega in water the
	 * footprint's depth deep. A motion whose k is past 0.9 of the grid's Nyquist rings faster than the grid can carry waves: nothing
	 * radiates on this grid, and it is 0. f̂ is summed directly over the footprint's cells on a ring of WaveRadiationAngles directions
	 * (the cost: the cells × the angles, so UpdateRadiation keeps what it has summed). The result is scaled by WaveRadiationCalibration.
	 */
	double WaveRadiationAt(const FFluidNinjaWaveFootprint& Footprint, int32 Motion, double Omega)
	{
		if (!Footprint.bValid || Footprint.Cells.IsEmpty() || !FMath::IsFinite(Omega) || !(Omega > 0.0))
		{
			return 0.0;
		}
		const double Cell = Footprint.CellCm;
		const double CellArea = Cell * Cell;
		const double Depth = Footprint.DepthCm;
		const double Gravity = Footprint.Gravity;
		const double Nyquist = UE_DOUBLE_PI / Cell;
		// k at that frequency, by bisection on ω(k) up to the grid's Nyquist.
		double Low = 1e-6;
		double High = Nyquist;
		for (int32 Step = 0; Step < 80; ++Step)
		{
			const double Mid = 0.5 * (Low + High);
			(WaveOmega(Mid, Depth, Gravity) < Omega ? Low : High) = Mid;
		}
		const double K = 0.5 * (Low + High);
		if (K > 0.9 * Nyquist)
		{
			return 0.0;   // it rings faster than the grid can carry waves: nothing radiates on this grid
		}
		const double DK = 1e-6 * FMath::Max(K, 1e-6);
		const double GroupVelocity = (WaveOmega(K + DK, Depth, Gravity) - WaveOmega(K - DK, Depth, Gravity)) / (2.0 * DK);
		if (!(GroupVelocity > 0.0))
		{
			return 0.0;
		}
		// ∫|f̂(k, θ)|² dθ over the ring, f̂ the continuous transform of the mode (cm² for heave, cm³ for roll and pitch).
		const FVector2D& Middle = Footprint.Middle;
		double Ring = 0.0;
		for (int32 Angle = 0; Angle < WaveRadiationAngles; ++Angle)
		{
			const double Theta = 2.0 * UE_DOUBLE_PI * Angle / WaveRadiationAngles;
			const double KX = K * FMath::Cos(Theta);
			const double KY = K * FMath::Sin(Theta);
			double Real = 0.0;
			double Imaginary = 0.0;
			for (const FVector2D& Centre : Footprint.Cells)
			{
				const double Mode = Motion == 0 ? 1.0 : (Motion == 1 ? Centre.Y - Middle.Y : Centre.X - Middle.X);
				const double Phase = KX * Centre.X + KY * Centre.Y;
				Real += Mode * FMath::Cos(Phase);
				Imaginary -= Mode * FMath::Sin(Phase);
			}
			Ring += (Real * Real + Imaginary * Imaginary) * CellArea * CellArea;
		}
		const double Integral = Ring / WaveRadiationAngles * 2.0 * UE_DOUBLE_PI;
		return WaveRadiationCalibration * Footprint.Rho * Gravity * K / (8.0 * UE_DOUBLE_PI * GroupVelocity) * Integral;
	}

	/** The physics hull's wetted area resting in its rest attitude at its rest draft on flat still water (cm²). */
	float WaveRestWettedArea(const FluidNinjaBuoyancy::FHull& Hull, const FQuat& Attitude, float Draft, float Mass, const FluidNinjaBuoyancy::FParams& Params)
	{
		float Bottom = 0.f;
		float Top = 0.f;
		FluidNinjaBuoyancy::RestedExtent(Hull, Attitude, Bottom, Top);
		FluidNinjaBuoyancy::FParams Static = Params;
		Static.SlamMultiplier = 0.f;
		Static.bPropulsion = false;
		Static.AirDragCoefficient = 0.f;
		FluidNinjaBuoyancy::FBodyState Body;
		Body.Transform = FTransform(Attitude);
		Body.CenterOfMass = Attitude.RotateVector(FVector((Hull.BoundsMin + Hull.BoundsMax) * 0.5f));
		Body.Mass = FMath::Max(Mass, 1e-3f);
		FluidNinjaBuoyancy::FWaterView Flat;
		Flat.bFlat = true;
		Flat.FlatHeight = Bottom + Draft;
		FluidNinjaBuoyancy::FStepState State;
		FluidNinjaBuoyancy::FStepResult Out;
		FluidNinjaBuoyancy::Integrate(Hull, Body, Flat, Static, 0.f, State, Out);
		return Out.WettedArea;
	}
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The atlas's tiles.

void UFluidNinjaBuoyancyWaves::FTileAllocator::Reset(int32 InSize)
{
	Size = InSize;
	Free.Reset();
	Free.FindOrAdd(Size).Add(FIntPoint::ZeroValue);
}

bool UFluidNinjaBuoyancyWaves::FTileAllocator::Allocate(int32 Cells, FIntPoint& Out)
{
	// The smallest free block that holds it, split in four down to its size.
	for (int32 Block = Cells; Block <= Size; Block *= 2)
	{
		TArray<FIntPoint>* Blocks = Free.Find(Block);
		if (!Blocks || Blocks->IsEmpty())
		{
			continue;
		}
		FIntPoint At = Blocks->Pop(EAllowShrinking::No);
		for (int32 Split = Block; Split > Cells; Split /= 2)
		{
			const int32 Half = Split / 2;
			TArray<FIntPoint>& Smaller = Free.FindOrAdd(Half);
			Smaller.Add(At + FIntPoint(Half, 0));
			Smaller.Add(At + FIntPoint(0, Half));
			Smaller.Add(At + FIntPoint(Half, Half));
		}
		Out = At;
		return true;
	}
	return false;
}

void UFluidNinjaBuoyancyWaves::FTileAllocator::Release(int32 Cells, const FIntPoint& At)
{
	// Back into its size's free list, merged with its three buddies into the block above while all four are free.
	FIntPoint Block = At;
	for (int32 Side = Cells; Side < Size; Side *= 2)
	{
		const int32 Parent = Side * 2;
		const FIntPoint Corner((Block.X / Parent) * Parent, (Block.Y / Parent) * Parent);
		const FIntPoint Quarters[4] = {Corner, Corner + FIntPoint(Side, 0), Corner + FIntPoint(0, Side), Corner + FIntPoint(Side, Side)};
		TArray<FIntPoint>& Same = Free.FindOrAdd(Side);
		bool bAllFree = true;
		for (const FIntPoint& Quarter : Quarters)
		{
			bAllFree &= Quarter == Block || Same.Contains(Quarter);
		}
		if (!bAllFree)
		{
			Same.Add(Block);
			return;
		}
		for (const FIntPoint& Quarter : Quarters)
		{
			Same.Remove(Quarter);
		}
		Block = Corner;
	}
	Free.FindOrAdd(Size).AddUnique(Block);
}

// ---------------------------------------------------------------------------------------------------------------------------------

UWorld* UFluidNinjaBuoyancyWaves::GetOwnerWorld() const
{
	const UFluidNinjaBuoyancySubsystem* Hydro = Owner.Get();
	return Hydro ? Hydro->GetWorld() : nullptr;
}

void UFluidNinjaBuoyancyWaves::Initialize(UFluidNinjaBuoyancySubsystem& InOwner)
{
	Owner = &InOwner;
	GpuState = MakeShared<FluidNinjaBuoyancyWaveSource::FState, ESPMode::ThreadSafe>();
	Tiles.Reset(FluidNinjaBuoyancyWaveRuntime::WaveAtlasFirstSize);
	StatusLine = UFluidNinjaBuoyancySettings::Get().bWaves ? TEXT("FluidNinja Buoyancy waves: starting") : TEXT("FluidNinja Buoyancy waves: off (Project Settings → FluidNinja Buoyancy → Waves)");
}

void UFluidNinjaBuoyancyWaves::Shutdown()
{
	Deactivate(TEXT("shut down"));
	// The last reference: FState hands its GPU resources to the render thread (a frame still in flight holds its own reference).
	GpuState.Reset();
	SourceTargets.Reset();
	SelfAtlas = nullptr;
	AtlasSize = 0;
}

bool UFluidNinjaBuoyancyWaves::IsWanted(FString& OutWhy) const
{
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	if (!FApp::CanEverRender())
	{
		OutWhy = TEXT("nothing is drawn in this process");
		return false;
	}
	if (!Settings.IsEnabled())
	{
		OutWhy = TEXT("FluidNinja Buoyancy is off");
		return false;
	}
	if (!Settings.bWaves)
	{
		OutWhy = TEXT("Project Settings → FluidNinja Buoyancy → Waves is off");
		return false;
	}
	return true;
}

void UFluidNinjaBuoyancyWaves::Deactivate(const FString& Why)
{
	if (bRunning)
	{
		UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy waves: off (%s)"), *Why);
	}
	for (FSimState& Sim : Sims)
	{
		StopSim(Sim);
	}
	Sims.Reset();
	for (TPair<int32, FBodyState>& Entry : Bodies)
	{
		ReleaseSelf(Entry.Value);
	}
	Bodies.Reset();
	FadingDeep.Reset();
	DeepHeadsSent = 0;
	bSwimmerTracked = false;
	SwimmerPawn.Reset();
	Puppets.Reset();
	// The released fields (and the stopped simulations' kept deep sums) go to the render thread with the next frame; with none coming, now.
	if ((!ReleasedSelves.IsEmpty() || !ReleasedDeepKeys.IsEmpty()) && GpuState)
	{
		FluidNinjaBuoyancyWaveSource::FFrame Frame;
		Frame.Released = MoveTemp(ReleasedSelves);
		Frame.ReleasedDeep = MoveTemp(ReleasedDeepKeys);
		ENQUEUE_RENDER_COMMAND(FluidNinjaBuoyancyWavesRelease)([RenderState = GpuState, RenderFrame = MoveTemp(Frame)](FRHICommandListImmediate& RHICmdList)
		{
			FluidNinjaBuoyancyWaveSource::Enqueue(RHICmdList, *RenderState, RenderFrame);
		});
	}
	ReleasedSelves.Reset();
	ReleasedDeepKeys.Reset();
	Tiles.Reset(AtlasSize > 0 ? AtlasSize : FluidNinjaBuoyancyWaveRuntime::WaveAtlasFirstSize);
	for (int32& Peak : SelfPoolPeak)
	{
		Peak = 0;   // every field released: the render thread drops the pools
	}
	NextRankTime = 0.0;
	CachedHullKeys.Reset();
	CachedHulls.Reset();
	CachedFirstTriangle.Reset();
	CachedTriangles.Reset();
	PendingPokes.Reset();
	HiddenMaterials.Reset();
	bRunning = false;
	StatusLine = FString::Printf(TEXT("FluidNinja Buoyancy waves: off (%s)"), *Why);
}

UFluidNinjaBuoyancyWaves::FSimState* UFluidNinjaBuoyancyWaves::FindSim(const FObjectKey& Key)
{
	return Sims.FindByPredicate([&Key](const FSimState& Sim) { return Sim.Key == Key; });
}

const AActor* UFluidNinjaBuoyancyWaves::SimActor(const UObject* Ninja)
{
	const AActor* Actor = Cast<AActor>(Ninja);
	if (!Actor && Ninja)
	{
		Actor = Ninja->GetTypedOuter<AActor>();
	}
	return Actor;
}

void UFluidNinjaBuoyancyWaves::ResolveSimSettings(const AActor* NinjaActor, FFluidNinjaWaveSimSettings& Out, const UFluidNinjaWaveSimComponent*& OutComponent, bool& bOutIniToo)
{
	Out = FFluidNinjaWaveSimSettings();
	OutComponent = NinjaActor ? NinjaActor->FindComponentByClass<UFluidNinjaWaveSimComponent>() : nullptr;
	bOutIniToo = false;
	// The Wave Sims entry naming the actor (by its name or label), the way it has always been found; with a component on the actor it only
	// says there is a clash to log.
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	const FFluidNinjaWaveSimSettings* Entry = nullptr;
	for (const FFluidNinjaWaveSimSettings& Candidate : Settings.WaveSims)
	{
		if (NinjaActor && !Candidate.NinjaLive.IsNone() && (Candidate.NinjaLive == NinjaActor->GetFName() || Candidate.NinjaLive.ToString() == NinjaActor->GetActorNameOrLabel()))
		{
			Entry = &Candidate;   // the last entry naming it, as before
		}
	}
	if (OutComponent)
	{
		// The component's settings as the ini list carries them, and its own switches on top (bWaves: not a wave simulation at all; the
		// scale-down and the importance are the component's fields, whatever ToSettings puts in the struct for them).
		Out = OutComponent->ToSettings();
		Out.bEnabled = OutComponent->bWaves && Out.bEnabled;
		Out.ScaleDownOverride = OutComponent->ScaleDownOverride;
		Out.Importance = OutComponent->Importance;
		bOutIniToo = Entry != nullptr;
		return;
	}
	if (Entry)
	{
		Out = *Entry;
	}
}

int32 UFluidNinjaBuoyancyWaves::WantedScaleDown(const FFluidNinjaWaveSimSettings& Override)
{
	// 1, 2 or 4: the tiers and the core's integer division are written for powers of two, so 3 (a value the clamp lets through) is taken as 2.
	const int32 Asked = FMath::Clamp(Override.ScaleDownOverride > 0 ? Override.ScaleDownOverride : FluidNinjaBuoyancyWaveRuntime::WaveScaleDown, 1, 4);
	return Asked >= 4 ? 4 : (Asked >= 2 ? 2 : 1);
}

void UFluidNinjaBuoyancyWaves::RankSims(const FFluidNinjaWaveWorldInput& Input, double Now)
{
	using namespace FluidNinjaBuoyancyWaveRuntime;
	const int32 Limit = WaveMaxActiveSims;
	if (Limit <= 0)
	{
		// No limit: whatever was suppressed comes back (UpdateSim re-arms it).
		for (FSimState& Sim : Sims)
		{
			if (Sim.bSuppressed)
			{
				Sim.bSuppressed = false;
				Sim.SuppressionChanged = Now;
			}
			Sim.RankPlace = 0;
		}
		NextRankTime = 0.0;
		return;
	}
	if (Now < NextRankTime)
	{
		return;
	}
	NextRankTime = Now + WaveActiveSimRankSeconds;
	// The simulations that could run (enabled, on a core the plugin accepts as far as it has looked, their pressure buffer sized): each at
	// the camera's distance to its nearest wave source (a floating body on its surface, awake and with its Waves on, or a swimmer inside its
	// square), over its importance. Waves come from those sources and are seen around them, so that is where a simulation's waves matter: a
	// simulation that follows the player has the camera inside its square wherever its water is, and ranked by the square it beat a pool of
	// floating bodies every time. One with no source ranks behind every simulation with one: its distance to its square (0 inside it; the
	// actor's height for the vertical part) plus its own size.
	struct FCandidate
	{
		FSimState* Sim = nullptr;
		double Rank = 0.0;
		double Effective = 0.0;
		double DistanceCm = 0.0;
		bool bSource = false;
		FString Name;
	};
	TArray<FCandidate, TInlineAllocator<16>> Candidates;
	for (int32 SimIndex = 0; SimIndex < Input.Sims.Num(); ++SimIndex)
	{
		const FFluidNinjaWaveSimInput& SimInput = Input.Sims[SimIndex];
		FSimState* Sim = FindSim(FObjectKey(SimInput.Ninja.Get()));
		if (!Sim)
		{
			continue;
		}
		const AActor* NinjaActor = SimActor(SimInput.Ninja.Get());
		FFluidNinjaWaveSimSettings Override;
		const UFluidNinjaWaveSimComponent* Component = nullptr;
		bool bIniToo = false;
		ResolveSimSettings(NinjaActor, Override, Component, bIniToo);
		const bool bCoreKnown = Sim->CheckedCore.IsValid();
		const bool bCapable = SimInput.bValid && Override.bEnabled && SimInput.SizeCm > 0.0 && !Sim->bWaitingForPressure
			&& (!bCoreKnown || (Sim->bHasWaveParameter && Sim->bHasAuxParameter && Sim->InterfaceVersion >= WaveRequiredInterfaceVersion));
		if (!bCapable)
		{
			// It takes no place: whether it runs is decided by UpdateSim's own reasons.
			if (Sim->bSuppressed)
			{
				Sim->bSuppressed = false;
				Sim->SuppressionChanged = Now;
			}
			Sim->RankPlace = 0;
			continue;
		}
		double DistanceCm = 0.0;
		bool bSource = false;
		if (Input.bHasCamera)
		{
			const FVector2D Camera(Input.Camera);
			const FVector2D Half(SimInput.SizeCm * 0.5);
			const FVector2D Outside = (Camera - SimInput.Centre).GetAbs() - Half;
			const FVector2D Across(FMath::Max(Outside.X, 0.0), FMath::Max(Outside.Y, 0.0));
			const double Up = NinjaActor ? Input.Camera.Z - NinjaActor->GetActorLocation().Z : 0.0;
			const double SquareCm = FMath::Sqrt(Across.SizeSquared() + Up * Up);
			auto InSquare = [&SimInput, &Half](const FVector& Point)
			{
				const FVector2D Off = (FVector2D(Point) - SimInput.Centre).GetAbs();
				return Off.X <= Half.X && Off.Y <= Half.Y;
			};
			double SourceCm = TNumericLimits<double>::Max();
			for (const FFluidNinjaWaveBodyInput& Body : Input.Bodies)
			{
				if (Body.Sim == SimIndex && Body.bWaves && !Body.bAsleep)
				{
					SourceCm = FMath::Min(SourceCm, FVector::Dist(Input.Camera, Body.Transform.GetLocation()));
				}
			}
			// A swimmer counts only while it is in the water (its lowest point under the water it knows): the player's pawn is sent on land
			// too, and a simulation that follows the player always holds it, a few metres from the camera.
			auto InWater = [](const FFluidNinjaWaveSwimmerInput& Swimmer, double Below)
			{
				return Swimmer.bValid && Swimmer.bHasWaterZ && Swimmer.Centre.Z - Below < Swimmer.WaterZ;
			};
			if (InWater(Input.Swimmer, Input.Swimmer.HalfHeight) && InSquare(Input.Swimmer.Centre))
			{
				SourceCm = FMath::Min(SourceCm, FVector::Dist(Input.Camera, Input.Swimmer.Centre));
			}
			for (const FFluidNinjaWaveSwimmerInput& Puppet : Input.Puppets)
			{
				if (InWater(Puppet, Puppet.Radius) && InSquare(Puppet.Centre))
				{
					SourceCm = FMath::Min(SourceCm, FVector::Dist(Input.Camera, Puppet.Centre));
				}
			}
			bSource = SourceCm < TNumericLimits<double>::Max();
			DistanceCm = bSource ? SourceCm : SquareCm + SimInput.SizeCm;
		}
		FCandidate& Candidate = Candidates.AddDefaulted_GetRef();
		Candidate.Sim = Sim;
		Candidate.DistanceCm = DistanceCm;
		Candidate.bSource = bSource;
		// The simulation's name for the log: the first ranking can come before UpdateSim has named it.
		Candidate.Name = Sim->Name.IsEmpty() ? GetNameSafe(NinjaActor) : Sim->Name;
		Candidate.Rank = DistanceCm / FMath::Max(static_cast<double>(Override.Importance), 0.01);
		// Hysteresis: a running simulation ranks a tenth nearer than it is, so a newcomer has to beat it by that much before they swap.
		Candidate.Effective = Sim->bSuppressed ? Candidate.Rank : Candidate.Rank * (1.0 - WaveActiveSimHysteresis);
	}
	Candidates.StableSort([](const FCandidate& A, const FCandidate& B) { return A.Effective < B.Effective; });
	for (int32 Place = 0; Place < Candidates.Num(); ++Place)
	{
		FSimState& Sim = *Candidates[Place].Sim;
		Sim.RankPlace = Place + 1;
		Sim.RankDistanceCm = Candidates[Place].DistanceCm;
		const bool bWantSuppressed = Place >= Limit;
		// The dwell: a simulation stays in its state for at least WaveActiveSimDwellSeconds, so one at the line does not flap.
		if (bWantSuppressed != Sim.bSuppressed && Now - Sim.SuppressionChanged >= WaveActiveSimDwellSeconds)
		{
			Sim.bSuppressed = bWantSuppressed;
			Sim.SuppressionChanged = Now;
			UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy waves: %s is %s (fnb.Waves.MaxActiveSims %d: it ranks %d of %d, %s %.0f m from the camera, over importance %.2f)."),
				*Candidates[Place].Name, bWantSuppressed ? TEXT("off until it comes nearer") : TEXT("on again"), Limit, Place + 1, Candidates.Num(),
				Candidates[Place].bSource ? TEXT("its nearest floating body or swimmer") : TEXT("no floating body or swimmer on it, ranked behind those with one at its square's distance plus its size,"),
				Candidates[Place].DistanceCm * 0.01, Sim.Importance);
		}
	}
}

const UFluidNinjaBuoyancyWaves::FSimState* UFluidNinjaBuoyancyWaves::FindSim(const FObjectKey& Key) const
{
	return Sims.FindByPredicate([&Key](const FSimState& Sim) { return Sim.Key == Key; });
}

bool UFluidNinjaBuoyancyWaves::IsActive() const
{
	return bRunning && Sims.ContainsByPredicate([](const FSimState& Sim) { return Sim.bActive; });
}

bool UFluidNinjaBuoyancyWaves::IsDisplacementOn() const
{
	return UFluidNinjaBuoyancySettings::Get().bWaveDisplacement;
}

bool UFluidNinjaBuoyancyWaves::IsImpulseOn() const
{
	return UFluidNinjaBuoyancySettings::Get().bWaveImpulse;
}

bool UFluidNinjaBuoyancyWaves::WantsImpulses() const
{
	return IsActive() && IsImpulseOn();
}

double UFluidNinjaBuoyancyWaves::GetMergeCellCm() const
{
	double Finest = 0.0;
	for (const FSimState& Sim : Sims)
	{
		if (Sim.bActive && Sim.CellCm > 0.0)
		{
			Finest = Finest > 0.0 ? FMath::Min(Finest, Sim.CellCm) : Sim.CellCm;
		}
	}
	return Finest > 0.0 ? Finest : 1.0;
}

void UFluidNinjaBuoyancyWaves::ReadTicks(FSimState& Sim)
{
	Sim.Delta = 0;
	Sim.bTicksRead = true;
	UNiagaraComponent* Core = Sim.Core.Get();
	const FNiagaraSystemInstanceControllerPtr Controller = Core ? Core->GetSystemInstanceController() : nullptr;
	const FNiagaraSystemInstance* Instance = Controller.IsValid() ? Controller->GetSystemInstance_Unsafe() : nullptr;
	if (!Instance)
	{
		Sim.Instance = nullptr;
		return;
	}
	// Niagara's game-thread tick of this system runs in its tick group: up to Post Physics before the tickable objects FluidNinja Buoyancy ticks
	// with, Post Update Work and Last Demotable after them.
	Sim.bNiagaraFirst = Instance->CalculateTickGroup() <= TG_PostPhysics;
	const int32 Count = Instance->GetTickCount();
	if (Instance != Sim.Instance || Count < Sim.LastTickCount)
	{
		// A new or reset instance: the core's waves start from flat water, so everything here starts over with it.
		Sim.Instance = Instance;
		Sim.LastTickCount = Count;
		Sim.bRestart = true;
		Sim.bAwaitingConfirm = false;
		return;
	}
	Sim.Delta = Count - Sim.LastTickCount;
	Sim.LastTickCount = Count;
}

void UFluidNinjaBuoyancyWaves::BeginFrame()
{
	if (!bRunning)
	{
		return;
	}
	FluidNinjaBuoyancyWaveSource::FFrame Frame;
	TArray<FTextureRenderTargetResource*> AuxResources;   // per self field: its simulation's Wave Aux target
	for (FSimState& Sim : Sims)
	{
		Sim.bTicksRead = false;
		if (!Sim.bActive)
		{
			continue;
		}
		ReadTicks(Sim);
		if (Sim.bNiagaraFirst)
		{
			continue;   // this frame's ticks consume this frame's write: it is all done in Tick
		}
		// Niagara ticks after FluidNinja Buoyancy: last frame's write was consumed by the ticks counted now. None: it is written again.
		if (Sim.bAwaitingConfirm && Sim.Delta == 0 && !Sim.bRestart)
		{
			Sim.PendingImpulses.Insert(Sim.LastImpulses, 0);
			Sim.PendingPokes.Insert(Sim.LastPokes, 0);
			Sim.PendingErase.Insert(Sim.LastErase, 0);
			Sim.bPendingEraseAll |= Sim.bLastEraseAll;
		}
		Sim.bAwaitingConfirm = false;
		Sim.LastImpulses.Reset();
		Sim.LastPokes.Reset();
		Sim.LastErase.Reset();
		Sim.bLastEraseAll = false;
		// The bodies' own fields take the same ticks, with the sources they were given last frame, before the gather reads them.
		if (Sim.Delta > 0)
		{
			for (TPair<int32, FBodyState>& Entry : Bodies)
			{
				FBodyState& Body = Entry.Value;
				if (!Body.bSelf || Body.Sim != Sim.Key)
				{
					continue;
				}
				FluidNinjaBuoyancyWaveSource::FSelf& Self = Frame.Selves.AddDefaulted_GetRef();
				Self.Id = Body.Id;
				Self.Cells = Body.SelfCells;
				Self.CellCm = static_cast<float>(Body.SelfCellCm);
				Self.StepTicks = Sim.Delta;
				SetSelfStep(Self, Sim, Body);
				AuxResources.Add(Sim.Aux.IsValid() ? Sim.Aux->GameThread_GetRenderTargetResource() : nullptr);
			}
		}
	}
	if (Frame.Selves.IsEmpty() || !GpuState || !SelfAtlas)
	{
		return;
	}
	const UWorld* World = GetOwnerWorld();
	Frame.Gravity = World ? FMath::Max(0.f, static_cast<float>(-World->GetGravityZ())) : 980.f;
	Frame.Density = UFluidNinjaBuoyancySettings::Get().WaterDensity * 1.0e-6f;
	Frame.SpongeRate = FluidNinjaBuoyancyWaveRuntime::WaveSelfSpongeRate;
	FTextureRenderTargetResource* AtlasResource = SelfAtlas->GameThread_GetRenderTargetResource();
	ENQUEUE_RENDER_COMMAND(FluidNinjaBuoyancyWavesSelfSteps)([RenderState = GpuState, RenderFrame = MoveTemp(Frame), AtlasResource, AuxResources](FRHICommandListImmediate& RHICmdList) mutable
	{
		RenderFrame.Atlas = AtlasResource ? AtlasResource->GetRenderTargetTexture().GetReference() : nullptr;
		for (int32 Index = 0; Index < RenderFrame.Selves.Num() && Index < AuxResources.Num(); ++Index)
		{
			RenderFrame.Selves[Index].Aux = AuxResources[Index] ? AuxResources[Index]->GetRenderTargetTexture().GetReference() : nullptr;
		}
		FluidNinjaBuoyancyWaveSource::Enqueue(RHICmdList, *RenderState, RenderFrame);
	});
}

bool UFluidNinjaBuoyancyWaves::UpdateSim(FSimState& Sim, const FFluidNinjaWaveSimInput& Input)
{
	using namespace FluidNinjaBuoyancyWaveRuntime;
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	Sim.Ninja = Input.Ninja;
	Sim.Core = Input.Core;
	Sim.Pressure = Input.Pressure;
	const UObject* Ninja = Input.Ninja.Get();
	const AActor* NinjaActor = SimActor(Ninja);
	Sim.Name = NinjaActor ? NinjaActor->GetActorNameOrLabel() : GetNameSafe(Ninja);

	// This simulation's settings: its NinjaLive actor's Wave Sim component when it carries one (a level-carried simulation), else the Wave
	// Sims entry naming the actor (by its name or label), else the defaults.
	FFluidNinjaWaveSimSettings Override;
	const UFluidNinjaWaveSimComponent* Component = nullptr;
	bool bIniToo = false;
	ResolveSimSettings(NinjaActor, Override, Component, bIniToo);
	Sim.bFromComponent = Component != nullptr;
	if (Component && bIniToo)
	{
		if (!Sim.bWarnedIniAndComponent)
		{
			UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("FluidNinja Buoyancy waves: %s carries a FluidNinja Wave Sim component and is also named in Project Settings → FluidNinja Buoyancy → Waves → Wave Sims; the component's settings are used and the entry is ignored. Logged once."),
				*Sim.Name);
			Sim.bWarnedIniAndComponent = true;
		}
	}
	else
	{
		Sim.bWarnedIniAndComponent = false;
	}
	Sim.bEnabled = Override.bEnabled;
	Sim.DepthCm = Override.bOverrideDepth ? FMath::Max(Override.DepthCm, 1.f) : FMath::Max(Settings.WaveDepthCm, 1.f);
	Sim.bDepthFromBed = !Override.bOverrideDepth;
	Sim.bEdgeOpen = Override.bOverrideEdgeOpen ? Override.bEdgeOpen : Settings.bWaveEdgeOpen;
	Sim.WallAbsorption = FMath::Max(Override.bOverrideWallAbsorption ? Override.WallAbsorption : Settings.WaveWallAbsorption, 0.f);
	Sim.WallBandCm = FMath::Max(Override.bOverrideWallBand ? Override.WallBandCm : Settings.WaveWallBandCm, 0.f);
	Sim.NormalScale = Override.bOverrideNormalScale ? FMath::Max(Override.NormalScale, 0.f)
		: (Input.SizeCm > 0.0 ? static_cast<float>(FMath::Max(Settings.WaveNormalScaleFactor, 0.f) / (0.005 * Input.SizeCm)) : 0.f);
	Sim.Importance = FMath::Max(Override.Importance, 0.01f);
	Sim.ScaleDownWanted = WantedScaleDown(Override);

	UNiagaraComponent* Core = Sim.Core.Get();
	// A simulation fnb.Waves.MaxActiveSims keeps off (RankSims) stops as a disabled one does: its core's waves off, its bodies' own fields
	// released; StopSim marks it to start over when it comes back.
	if (!Sim.bEnabled || !Input.bValid || !Core || Sim.bSuppressed)
	{
		StopSim(Sim);
		return false;
	}
	// The modified NinjaLiveCore has User.WaveSimEnabled; a stock one does not, and makes no waves (no stand-in). Its interface version and
	// build say which FluidNinja Waves it is (a core from before the handshake has neither: interface 1, build 0).
	if (Sim.CheckedCore.Get() != Core)
	{
		Sim.CheckedCore = Core;
		Sim.bHasWaveParameter = Core->GetOverrideParameters().IndexOf(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), WaveSimEnabledName)) != INDEX_NONE;
		Sim.bHasAuxParameter = Core->GetOverrideParameters().IndexOf(FNiagaraVariable(FNiagaraTypeDefinition(UTextureRenderTarget::StaticClass()), WaveAuxTargetName)) != INDEX_NONE;
		Sim.InterfaceVersion = WaveReadCoreInt(*Core, WaveInterfaceVersionName, 1);
		Sim.CoreBuild = WaveReadCoreInt(*Core, WaveCoreBuildName, 0);
		Sim.bWarned = false;
	}
	if (!Sim.bHasWaveParameter)
	{
		if (!Sim.bWarned)
		{
			UE_LOG(LogFluidNinjaBuoyancy, Error, TEXT("FluidNinja Buoyancy waves: %s's %s (%s) has no user parameter %s: it is not the modified NinjaLiveCore FluidNinja Waves needs, so this simulation makes no waves. Copy the modified core in, or turn Waves off for it (Project Settings → FluidNinja Buoyancy → Waves → Wave Sims)."),
				*Sim.Name, *GetNameSafe(Core), *GetPathNameSafe(Core->GetAsset()), *WaveSimEnabledName.ToString());
			Sim.bWarned = true;
		}
		StopSim(Sim);
		return false;
	}
	// The bodies' own fields step the core's water with its obstacles, depth and current, which its Wave Aux stage writes: a core
	// without it is an older FluidNinja Waves, whose bodies would feel their own reflected waves. It makes no waves (no stand-in).
	if (!Sim.bHasAuxParameter)
	{
		if (!Sim.bWarned)
		{
			UE_LOG(LogFluidNinjaBuoyancy, Error, TEXT("FluidNinja Buoyancy waves: %s's %s (%s) has no user parameter %s: it is an older FluidNinja Waves core without the Wave Aux stage the bodies' own fields need, so this simulation makes no waves. Re-apply FluidNinja Waves (FluidNinjaBuoyancyDev/Tools/apply_fluidninja_waves.py) and copy the core in."),
				*Sim.Name, *GetNameSafe(Core), *GetPathNameSafe(Core->GetAsset()), *WaveAuxTargetName.ToString());
			Sim.bWarned = true;
		}
		StopSim(Sim);
		return false;
	}
	// The core's interface must be the one this plugin speaks (the step's breaking and k = 0 kill, User.Wave.MeanRelax, and whatever the
	// next version adds): an older core would run the self fields on a different step from the core's, and the bodies would feel the
	// difference as waves. Refused like a stock core (no stand-in).
	if (Sim.InterfaceVersion < WaveRequiredInterfaceVersion)
	{
		if (!Sim.bWarned)
		{
			UE_LOG(LogFluidNinjaBuoyancy, Error, TEXT("FluidNinja Buoyancy waves: %s's %s (%s) is FluidNinja Waves core build %d, interface %d; this plugin needs interface %d (core build %d or later, from %s), so this simulation makes no waves. Install that zip's core (its README.txt), or turn Waves off for %s (Project Settings → FluidNinja Buoyancy → Waves → Wave Sims)."),
				*Sim.Name, *GetNameSafe(Core), *GetPathNameSafe(Core->GetAsset()), Sim.CoreBuild, Sim.InterfaceVersion, WaveRequiredInterfaceVersion, WaveRequiredCoreBuild, WaveCoreZipName, *Sim.Name);
			Sim.bWarned = true;
		}
		StopSim(Sim);
		return false;
	}
	// The wave cells: the pressure buffer's (export) resolution over the simulation's own square.
	UTextureRenderTarget2D* Pressure = Sim.Pressure.Get();
	// Until the core has sized it (its first ticks: after its compile in a fresh editor, after FluidNinja turns the simulation on), the
	// buffer is a 1 x 1 placeholder, whose cells would be the whole square and every body too big for its own field. Not an error: it
	// runs no waves until the buffer has the simulation's size.
	if (Pressure && (Pressure->SizeX < FluidNinjaBuoyancyWaveSource::MinSelfCells || Pressure->SizeY < FluidNinjaBuoyancyWaveSource::MinSelfCells))
	{
		if (!Sim.bWaitingForPressure)
		{
			UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy waves: %s waits for its core to size its pressure buffer %s (%d x %d now; the waves need %d x %d at least)."),
				*Sim.Name, *GetNameSafe(Pressure), Pressure->SizeX, Pressure->SizeY, FluidNinjaBuoyancyWaveSource::MinSelfCells, FluidNinjaBuoyancyWaveSource::MinSelfCells);
			Sim.bWaitingForPressure = true;
		}
		StopSim(Sim);
		return false;
	}
	Sim.bWaitingForPressure = false;
	if (!Pressure || Pressure->SizeX <= 0 || Pressure->SizeY <= 0 || Pressure->SizeX != Pressure->SizeY || !(Input.SizeCm > 0.0))
	{
		if (!Sim.bWarned)
		{
			UE_LOG(LogFluidNinjaBuoyancy, Error, TEXT("FluidNinja Buoyancy waves: %s: %s; this simulation makes no waves."), *Sim.Name,
				!Pressure ? *FString::Printf(TEXT("its surface material has no '%s' render target (Project Settings → FluidNinja Buoyancy → Waves → Pressure Buffer Parameter)"), *Settings.PressureBufferParameter.ToString())
				: *FString::Printf(TEXT("its pressure buffer %s is %d x %d (the waves need a square one over a placed square)"), *GetNameSafe(Pressure), Pressure->SizeX, Pressure->SizeY));
			Sim.bWarned = true;
		}
		StopSim(Sim);
		return false;
	}
	const FIntPoint PressureCells(Pressure->SizeX, Pressure->SizeY);
	const double PressureCellCm = Input.SizeCm / PressureCells.X;
	// The wave grid: the pressure buffer's cells over the scale-down S (the integer division the core's Ninja_WaveParams makes, so both
	// sides size the grid alike; a buffer that is not a multiple of S gets cells a little larger than S pressure cells, the window still
	// covered end to end). A core before interface version 3 has no User.Wave.ScaleDown: it runs at 1, said once per core when more was
	// asked (no other stand-in: the plugin cannot shrink the core's grid for it).
	int32 ScaleDown = Sim.ScaleDownWanted;
	if (Sim.InterfaceVersion < WaveScaleDownInterfaceVersion && ScaleDown > 1)
	{
		if (!Sim.bWarnedScaleDown)
		{
			UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("FluidNinja Buoyancy waves: %s asks for a wave grid scale-down of %d (%s), but its core (build %d, interface %d) predates interface %d and has no %s: it runs at the pressure buffer's resolution (scale-down 1). Re-apply FluidNinja Waves with the current script for the scale-down. Logged once."),
				*Sim.Name, ScaleDown, Override.ScaleDownOverride > 0 ? TEXT("its own override") : TEXT("fnb.Waves.ScaleDown"), Sim.CoreBuild, Sim.InterfaceVersion, WaveScaleDownInterfaceVersion, *WaveScaleDownName.ToString());
			Sim.bWarnedScaleDown = true;
		}
		ScaleDown = 1;
	}
	const FIntPoint Cells(FMath::Max(PressureCells.X / ScaleDown, 1), FMath::Max(PressureCells.Y / ScaleDown, 1));
	const double CellCm = Input.SizeCm / Cells.X;
	// The core makes the pressure buffer RGBA16F in place once its waves run (the same object, so the materials keep it); until then it is
	// FluidNinja's RG16F, which has no B (and whose A reads 1). The render thread checks the texture itself before it reads B (the gather,
	// the probes and the stats); this is for the status line.
	Sim.bPressureReady = Pressure->GetFormat() == PF_FloatRGBA;
	const FVector2D Origin = Input.Centre - FVector2D(Input.SizeCm * 0.5);
	const FVector2D Phase(Origin.X - FMath::FloorToDouble(Origin.X / CellCm) * CellCm, Origin.Y - FMath::FloorToDouble(Origin.Y / CellCm) * CellCm);
	const bool bNewCells = Cells != Sim.Cells || !FMath::IsNearlyEqual(CellCm, Sim.CellCm, CellCm * 1e-6);
	const auto PhaseMoved = [CellCm](double A, double B)
	{
		const double Difference = FMath::Abs(A - B);
		return FMath::Min(Difference, CellCm - Difference) > CellCm * 1e-3;
	};
	if (bNewCells || !Sim.bHasPhase || PhaseMoved(Phase.X, Sim.Phase.X) || PhaseMoved(Phase.Y, Sim.Phase.Y))
	{
		// New cells (a new buffer, or a new scale-down) or a lattice that moved by part of a cell: every body's own field starts over on the
		// new cells.
		Sim.bRestart = true;
	}
	if (bNewCells && Sim.Cells != FIntPoint::ZeroValue)
	{
		UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy waves: %s's wave grid is now %d x %d cells of %.2f cm (scale-down %d of its %d x %d pressure cells; was %d x %d of %.2f cm)."),
			*Sim.Name, Cells.X, Cells.Y, CellCm, ScaleDown, PressureCells.X, PressureCells.Y, Sim.Cells.X, Sim.Cells.Y, Sim.CellCm);
	}
	Sim.PressureCells = PressureCells;
	Sim.PressureCellCm = PressureCellCm;
	Sim.Cells = Cells;
	Sim.CellCm = CellCm;
	Sim.ScaleDown = ScaleDown;
	Sim.Origin = Origin;
	Sim.Centre = Input.Centre;
	Sim.SizeCm = Input.SizeCm;
	Sim.Phase = Phase;
	Sim.bHasPhase = true;
	const UNiagaraSystem* System = Core->GetAsset();
	Sim.bFixedTick = System && System->HasFixedTickDelta() && System->GetFixedTickDeltaTime() > 0.f;
	Sim.TickSeconds = Sim.bFixedTick ? System->GetFixedTickDeltaTime() : WaveDefaultTickSeconds;
	// The k = 0 rates, once here for the core's parameters below, the bodies' own fields (SetSelfStep) and the status line, so all three
	// say the same numbers.
	Sim.bVPin = WaveVPinRuns(Sim.InterfaceVersion, Sim.bEdgeOpen);
	Sim.CoreMeanRelax = WaveCoreMeanRelax(Sim.bFixedTick, Sim.TickSeconds, Sim.InterfaceVersion, Sim.bEdgeOpen);
	Sim.SelfMeanRelax = WaveSelfMeanRelaxRate(Sim.TickSeconds);
	Sim.VMeanRelax = Sim.InterfaceVersion >= WaveVPinInterfaceVersion ? FMath::Max(WaveVMeanRelax, 0.f) : 0.f;
	if (!Sim.bFixedTick && !bWarnedTick)
	{
		UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("FluidNinja Buoyancy waves: %s's NinjaLiveCore has no fixed tick; the bodies' own fields step at %.4f s a tick, and the core's mean relax is clamped to %.1f /s (stable up to Niagara's %.3f s step). Logged once."),
			*Sim.Name, WaveDefaultTickSeconds, 0.5f / WaveMaxVariableStepSeconds, WaveMaxVariableStepSeconds);
		bWarnedTick = true;
	}

	// Its source texture: W x (H + 1) RGBA32F, clamped and bilinear, written through a UAV.
	UTextureRenderTarget2D* Target = Sim.Target.Get();
	if (!Target || Target->SizeX != Cells.X || Target->SizeY != Cells.Y + 1)
	{
		if (!Target)
		{
			Target = NewObject<UTextureRenderTarget2D>(this);
			SourceTargets.Add(Target);
			Sim.Target = Target;
		}
		Target->RenderTargetFormat = RTF_RGBA32f;
		Target->ClearColor = FLinearColor::Transparent;
		Target->AddressX = TA_Clamp;
		Target->AddressY = TA_Clamp;
		Target->Filter = TF_Bilinear;
		Target->bCanCreateUAV = true;
		Target->InitCustomFormat(Cells.X, Cells.Y + 1, PF_A32B32G32R32F, true);
		Target->UpdateResourceImmediate(true);
		Sim.bRestart = true;
		UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy waves: on for %s, %d x %d cells of %.2f cm (scale-down %d of its %d x %d pressure cells; source texture %d x %d RGBA32F)."),
			*Sim.Name, Cells.X, Cells.Y, CellCm, ScaleDown, PressureCells.X, PressureCells.Y, Cells.X, Cells.Y + 1);
	}

	// Its Wave Aux target: W x H RGBA32F at the wave grid's size, which the core's Wave Aux stage writes every tick (it runs only at exactly
	// that size; the plugin's own fields read it on the same cells).
	UTextureRenderTarget2D* Aux = Sim.Aux.Get();
	if (!Aux || Aux->SizeX != Cells.X || Aux->SizeY != Cells.Y)
	{
		if (!Aux)
		{
			Aux = NewObject<UTextureRenderTarget2D>(this);
			SourceTargets.Add(Aux);
			Sim.Aux = Aux;
		}
		Aux->RenderTargetFormat = RTF_RGBA32f;
		Aux->ClearColor = FLinearColor::Transparent;
		Aux->AddressX = TA_Clamp;
		Aux->AddressY = TA_Clamp;
		Aux->Filter = TF_Nearest;
		Aux->bCanCreateUAV = true;
		Aux->InitCustomFormat(Cells.X, Cells.Y, PF_A32B32G32R32F, true);
		Aux->UpdateResourceImmediate(true);
		Sim.bRestart = true;
	}

	// Its foam target (interface version 4): F per wave cell, one channel, written by the core's Wave Aux every tick (0 while the foam is
	// off), sampled by the surfaces' dynamic materials (WaveFoamBuffer). A core before interface 4 has no User.WaveFoamTarget and gets none.
	UTextureRenderTarget2D* FoamTarget = Sim.Foam.Get();
	if (Sim.InterfaceVersion >= WaveFoamInterfaceVersion && (!FoamTarget || FoamTarget->SizeX != Cells.X || FoamTarget->SizeY != Cells.Y))
	{
		if (!FoamTarget)
		{
			FoamTarget = NewObject<UTextureRenderTarget2D>(this);
			SourceTargets.Add(FoamTarget);
			Sim.Foam = FoamTarget;
		}
		FoamTarget->RenderTargetFormat = RTF_R16f;
		FoamTarget->ClearColor = FLinearColor::Transparent;
		FoamTarget->AddressX = TA_Clamp;
		FoamTarget->AddressY = TA_Clamp;
		FoamTarget->Filter = TF_Bilinear;
		FoamTarget->bCanCreateUAV = true;
		FoamTarget->InitCustomFormat(Cells.X, Cells.Y, PF_R16F, true);
		FoamTarget->UpdateResourceImmediate(true);
	}

	// The core's parameters, every frame (they survive a new asset or a re-activation that way).
	Core->SetVariableBool(WaveSimEnabledName, true);
	Core->SetVariableTexture(WaveSourceTextureName, Target);
	Core->SetVariableTextureRenderTarget(WaveAuxTargetName, Aux);
	Core->SetVariableFloat(WaveDepthName, Sim.DepthCm);
	Core->SetVariableFloat(WaveDepthFromBedName, Sim.bDepthFromBed ? 1.f : 0.f);
	Core->SetVariableFloat(WaveDampingName, FMath::Max(Settings.WaveDamping, 0.f));
	Core->SetVariableFloat(WaveSpongeName, FMath::Max(Settings.WaveSpongeCm, 0.f));
	Core->SetVariableFloat(WaveEdgeOpenName, Sim.bEdgeOpen ? 1.f : 0.f);
	Core->SetVariableFloat(WaveWallAbsorptionName, Sim.WallAbsorption);
	Core->SetVariableFloat(WaveWallBandName, Sim.WallBandCm);
	Core->SetVariableFloat(WaveShoreName, FMath::Max(Settings.WaveShoreDepthCm, 0.f));
	Core->SetVariableFloat(WaveBreakingName, FMath::Max(Settings.WaveBreakingSlope, 0.01f));
	Core->SetVariableFloat(WaveHyperviscosityName, FMath::Max(Settings.WaveHyperviscosity, 0.f));
	Core->SetVariableFloat(WaveCurrentCouplingName, FMath::Max(Settings.WaveCurrentCoupling, 0.f));
	Core->SetVariableFloat(WaveMeanRelaxName, Sim.CoreMeanRelax);   // the bodies' own fields take fnb.Waves.SelfMeanRelax (SetSelfStep)
	// The closed-window <v> pin's rate, on a core that has the parameter (interface version 5; setting one a core lacks would add a stray
	// user parameter to its overrides). Its Wave Step skips the pin in an open window by itself, so every interface-5 core gets the rate.
	if (Sim.InterfaceVersion >= WaveVPinInterfaceVersion)
	{
		Core->SetVariableFloat(WaveVMeanRelaxName, Sim.VMeanRelax);
	}
	// The grid's scale-down, on a core that has the parameter (setting one a core lacks would add a stray user parameter to its overrides).
	if (Sim.InterfaceVersion >= WaveScaleDownInterfaceVersion)
	{
		Core->SetVariableInt(WaveScaleDownName, Sim.ScaleDown);
	}
	// The foam channel, on a core that has it (interface version 4). The switch is the setting and the tier's fnb.Waves.Foam together;
	// off, the core accumulates nothing and its Wave Aux writes 0 into the target, so the surfaces go clean within a tick.
	Sim.bFoam = false;
	if (Sim.InterfaceVersion >= WaveFoamInterfaceVersion && FoamTarget)
	{
		const bool bFoamOn = Settings.bFoam && WaveFoamOn != 0;
		Core->SetVariableTextureRenderTarget(WaveFoamTargetName, FoamTarget);
		Core->SetVariableFloat(WaveFoamName, bFoamOn ? 1.f : 0.f);
		Core->SetVariableFloat(WaveFoamDecayName, FMath::Max(Settings.FoamDecay, 0.f));
		Core->SetVariableFloat(WaveFoamBreakingName, FMath::Max(Settings.FoamBreaking, 0.f));
		Core->SetVariableFloat(WaveFoamWakeName, FMath::Max(Settings.FoamWake, 0.f));
		Core->SetVariableFloat(WaveFoamImpactName, FMath::Max(Settings.FoamImpact, 0.f));
		Sim.bFoam = bFoamOn;
	}
	Sim.bSwitchedOn = true;
	if (!Sim.bActive)
	{
		Sim.bRestart = true;
	}
	Sim.bActive = true;
	return true;
}

void UFluidNinjaBuoyancyWaves::StopSim(FSimState& Sim)
{
	if (Sim.bSwitchedOn)
	{
		if (UNiagaraComponent* Core = Sim.Core.Get())
		{
			Core->SetVariableBool(FluidNinjaBuoyancyWaveRuntime::WaveSimEnabledName, false);
		}
		Sim.bSwitchedOn = false;
	}
	if (Sim.bActive)
	{
		for (TPair<int32, FBodyState>& Entry : Bodies)
		{
			if (Entry.Value.Sim == Sim.Key)
			{
				ReleaseSelf(Entry.Value);
				Entry.Value.Sim = FObjectKey();
			}
		}
	}
	// Its deep heads go with its waves (they start afresh from flat water when it runs again), and the render thread forgets their sum.
	FadingDeep.RemoveAll([&Sim](const FDeepRecord& Record) { return Record.Sim == Sim.Key; });
	for (TPair<int32, FBodyState>& Entry : Bodies)
	{
		if (Entry.Value.Deep.bValid && Entry.Value.Deep.Sim == Sim.Key)
		{
			Entry.Value.Deep = FDeepRecord();
			Entry.Value.bPathLatched = false;
			Entry.Value.bDeep = false;
			Entry.Value.bDeepStartsWhole = false;
		}
	}
	if (Sim.DeepKey != 0 && Sim.LastWriteDeep > 0)
	{
		ReleasedDeepKeys.AddUnique(Sim.DeepKey);
	}
	Sim.LastWriteDeep = 0;
	Sim.bActive = false;
	Sim.bPressureReady = false;
	Sim.PendingImpulses.Reset();
	Sim.PendingPokes.Reset();
	Sim.PendingErase.Reset();
	Sim.bPendingEraseAll = false;
	Sim.LastImpulses.Reset();
	Sim.LastPokes.Reset();
	Sim.LastErase.Reset();
	Sim.bAwaitingConfirm = false;
	Sim.bRestart = true;
}

void UFluidNinjaBuoyancyWaves::ReleaseSelf(FBodyState& Body)
{
	if (Body.bSelf)
	{
		Tiles.Release(Body.SelfCells, Body.Tile);
		ReleasedSelves.Add(Body.Id);
	}
	Body.bSelf = false;
	Body.bSelfReady = false;
	Body.SelfCells = 0;
	Body.Tile = FIntPoint(-1, -1);
	Body.bRadiation = false;
	// Without a field it has no footprint, so no frequencies to show.
	if (UFluidNinjaBuoyancyComponent* Shown = Body.Component.Get())
	{
		Shown->SetWaveMotion(FVector::ZeroVector);
	}
}

void UFluidNinjaBuoyancyWaves::RemoveBody(FBodyState& Body)
{
	// The water where it was is cleared, so a body put away, gone to sleep or destroyed leaves no dent or ring behind. A sinker with a deep
	// head clears nothing: its head reaches metres past its hull, so the erase (η = −P over its box) would cut into the waves and the head
	// around it; the head fades where it is instead, too slowly to ring (RetireDeep).
	const bool bDeepHead = Body.bDeep && Body.Deep.bValid;
	if (FSimState* Sim = FindSim(Body.Sim))
	{
		if (Sim->bActive && Body.Bounds.bIsValid && !bDeepHead)
		{
			Sim->PendingErase.Add(Body.Bounds);
		}
	}
	RetireDeep(Body);
	ReleaseSelf(Body);
	Body.Sim = FObjectKey();
}

void UFluidNinjaBuoyancyWaves::RetireDeep(FBodyState& Body)
{
	if (Body.Deep.bValid)
	{
		FDeepRecord& Fading = FadingDeep.Add_GetRef(Body.Deep);
		Fading.FadeTarget = 0.f;
	}
	Body.Deep = FDeepRecord();
	Body.bPathLatched = false;
	Body.bDeep = false;
	Body.bDeepStartsWhole = false;
}

bool UFluidNinjaBuoyancyWaves::StepDeepFade(FDeepRecord& Record, double DeltaSeconds)
{
	using namespace FluidNinjaBuoyancyWaveRuntime;
	const double Keep = FMath::Exp(-FMath::Max(DeltaSeconds, 0.0) / WaveDeepFadeSeconds);
	Record.Fade = Record.FadeTarget + (Record.Fade - Record.FadeTarget) * static_cast<float>(Keep);
	if (Record.FadeTarget > 0.f)
	{
		return true;
	}
	// What it still presses at most: its column's height (the head over a column of the same volume never stands higher), times its fade.
	const double Column = Record.Half.X > 0.0 && Record.Half.Y > 0.0 ? Record.VolumeCm3 / (4.0 * Record.Half.X * Record.Half.Y) : 0.0;
	return Column * Record.Fade >= WaveDeepDropCm;
}

bool UFluidNinjaBuoyancyWaves::MakeDeepHead(const FDeepRecord& Record, const FVector2D& GridMin, FluidNinjaBuoyancyWaveSource::FDeepHead& Out)
{
	using namespace FluidNinjaBuoyancyWaveRuntime;
	if (!Record.bValid || Record.VolumeCm3 < WaveDeepMinVolumeCm3 || !(Record.Fade > 0.f) || !(Record.DepthCm > 0.0))
	{
		return false;
	}
	// The kernel's rectangle and column, never thinner than WaveDeepMinExtentCm, with the column ending at the kernel's bed; P's multiplier
	// keeps the volume whatever the rectangle and column came to.
	const double HalfA = FMath::Max(Record.Half.X, WaveDeepMinExtentCm);
	const double HalfB = FMath::Max(Record.Half.Y, WaveDeepMinExtentCm);
	const double Bottom = FMath::Clamp(Record.BottomCm, WaveDeepMinExtentCm, Record.DepthCm);
	const double Top = FMath::Clamp(Record.TopCm, 0.0, Bottom - FMath::Min(WaveDeepMinExtentCm, Bottom));
	if (!(Bottom > Top))
	{
		return false;
	}
	const double Scale = Record.VolumeCm3 / (4.0 * HalfA * HalfB * (Bottom - Top)) * Record.Renormalisation * Record.Fade;
	Out.Centre = FVector2f(static_cast<float>(Record.Centre.X - GridMin.X), static_cast<float>(Record.Centre.Y - GridMin.Y));
	Out.Axis = FVector2f(static_cast<float>(FMath::Cos(Record.AxisAngle)), static_cast<float>(FMath::Sin(Record.AxisAngle)));
	Out.Half = FVector2f(static_cast<float>(HalfA), static_cast<float>(HalfB));
	Out.TopCm = static_cast<float>(Top);
	Out.BottomCm = static_cast<float>(Bottom);
	Out.DepthCm = static_cast<float>(Record.DepthCm);
	Out.Scale = static_cast<float>(Scale);
	Out.TaperStartCm = static_cast<float>(Record.TaperStartCm);
	Out.TaperEndCm = static_cast<float>(Record.TaperEndCm);
	return FMath::IsFinite(Scale) && Scale > 0.0;
}

void UFluidNinjaBuoyancyWaves::UpdateDeep(FBodyState& Body, const FSimState& Sim, const FFluidNinjaWaveBodyInput& Input, const FFluidNinjaWaveWorldInput& World,
	const FTransform& Frame, float WaterZ, bool bStartWhole, float DeltaSeconds)
{
	using namespace FluidNinjaBuoyancyWaveRuntime;
	FDeepRecord& Record = Body.Deep;
	const double Dt = FMath::Max(static_cast<double>(DeltaSeconds), 0.0);
	// The rest timer: the head fades only once the body has lain still for WaveDeepRestSeconds (a sunk block on the bed), never while it
	// moves, so the whole of its wave event is made with its whole volume.
	const bool bResting = Input.Velocity.SizeSquared() < FMath::Square(WaveDeepRestSpeedCmS) && Input.AngularVelocity.SizeSquared() < FMath::Square(WaveDeepRestSpinRad);
	const FluidNinjaBuoyancy::FHull* Hull = Input.Hull.Get();
	// While the water under it is unknown (the Surface Mirror lost it as the camera moved), the column is measured against the last level
	// it was seen in: the body keeps sinking or moving, and a head held where it was would step to its real pose when the water came back
	// (tens of cm of P in one frame for a block that went on down meanwhile, which rings).
	FWaveDeepShape Shape;
	const bool bWaterKnown = Hull && Input.WaterSource != EFluidNinjaWaveWater::None;
	const bool bLastWater = Hull && !bWaterKnown && Record.bValid && Record.bWaterZ;
	const double ShapeWaterZ = bWaterKnown ? static_cast<double>(WaterZ) : Record.WaterZ;
	const bool bUnder = (bWaterKnown || bLastWater) && WaveSubmergedShape(*Hull, Frame, ShapeWaterZ, Shape);
	if ((bWaterKnown || bLastWater) && !bUnder && Record.bValid)
	{
		// Wholly out of its water again (lifted out): it presses nothing. Its volume went to nothing as it came out, so this is no step.
		Record.VolumeCm3 = 0.0;
	}
	if (bUnder)
	{
		// The depth the waves run at under it (the kernel's bed): the trace's when the project traces the ground, else its simulation's.
		UpdateDepth(Body, Sim, Input, World);
		const double Depth = FMath::Max(static_cast<double>(Body.DepthCm > 0.f ? Body.DepthCm : Sim.DepthCm), 1.0);
		const bool bNew = !Record.bValid;
		if (bNew)
		{
			Record = FDeepRecord();
			Record.bValid = true;
			Record.Sim = Sim.Key;
			// A head that starts whole: the hull is just coming into the water (its head grows from nothing, as the static head would; the
			// body says so from the frame its path was decided), or every cell of its water is erased to η = −P this frame. Else (a body
			// first seen, or put down, already deep in the water) it fades in, so its whole head never appears in one step.
			Record.Fade = bStartWhole || Body.bDeepStartsWhole ? 1.f : 0.f;
			Record.FadeTarget = 1.f;
			if (bResting && !Body.bDeepStartsWhole)
			{
				// Already lying still (a sunk body when its simulation starts or restarts, or seen for the first time): its head would press
				// a static P under which B stays 0, then hold for WaveDeepRestSeconds and fade for over a minute, a full DeepHeadCS pass over
				// its disc every frame of the fade and no breaking judged there, for nothing drawn. It starts faded out, as after its long
				// rest, and fades in only once it moves. An erase of all the water this frame agrees: η = −P with P = 0 is B = 0 too.
				Record.Fade = 0.f;
				Record.FadeTarget = 0.f;
				Record.RestSeconds = WaveDeepRestSeconds;
			}
		}
		if (bWaterKnown)
		{
			Record.WaterZ = WaterZ;
			Record.bWaterZ = true;
		}
		// fnb.Waves.PlungeLag: the column's ends follow the hull through a first-order lag of τ = c √(h / g) (off at 0).
		const double Lag = FMath::Max(static_cast<double>(WavePlungeLag), 0.0);
		double Top = Shape.TopCm;
		double Bottom = Shape.BottomCm;
		if (Lag > 0.0 && !bNew)
		{
			const UWorld* OwnerWorld = GetOwnerWorld();
			const double Gravity = OwnerWorld ? FMath::Max(1.0, -static_cast<double>(OwnerWorld->GetGravityZ())) : 980.0;
			const double Follow = 1.0 - FMath::Exp(-Dt / (Lag * FMath::Sqrt(Depth / Gravity)));
			Top = Record.TopCm + (Top - Record.TopCm) * Follow;
			Bottom = Record.BottomCm + (Bottom - Record.BottomCm) * Follow;
		}
		Record.Sim = Sim.Key;
		Record.Centre = Shape.Centre;
		Record.AxisAngle = Shape.AxisAngle;
		Record.Half = Shape.Half;
		Record.TopCm = Top;
		Record.BottomCm = Bottom;
		Record.VolumeCm3 = Shape.VolumeCm3;
		Record.DepthCm = Depth;
		// The taper, and the renormalisation for what it takes off (worked out again only when the column or the reach changes). Its reach
		// is measured clear of the footprint: it starts no nearer than the rectangle's corner plus two depths, so the cos² never cuts the
		// hull's own footprint (a 3 m crate in 50 cm of water would otherwise press nothing at its corners and double P over a disc) and the
		// far-field loss WaveDeepTaperLoss sums is always taken outside it, where its series holds. Where the corner is already clear (the
		// bench's block: 960 cm against a 173 cm half-diagonal) the numbers are unchanged.
		const double HalfX = FMath::Max(Shape.Half.X, WaveDeepMinExtentCm);
		const double HalfY = FMath::Max(Shape.Half.Y, WaveDeepMinExtentCm);
		const double Reach = FMath::Max(static_cast<double>(WavePlungeTaperDepths), 0.5) * Depth;
		Record.TaperStartCm = FMath::Max(WaveDeepTaperStartShare * Reach, FMath::Sqrt(HalfX * HalfX + HalfY * HalfY) + 2.0 * Depth);
		Record.TaperEndCm = Record.TaperStartCm + (1.0 - WaveDeepTaperStartShare) * Reach;
		const double Bottomed = FMath::Clamp(Bottom, WaveDeepMinExtentCm, Depth);
		const double Topped = FMath::Clamp(Top, 0.0, Bottomed - FMath::Min(WaveDeepMinExtentCm, Bottomed));
		const double For[6] = {HalfX, HalfY, Topped, Bottomed, Depth, Record.TaperEndCm};
		if (FMemory::Memcmp(For, Record.RenormalisedFor, sizeof(For)) != 0)
		{
			FMemory::Memcpy(Record.RenormalisedFor, For, sizeof(For));
			Record.Renormalisation = 1.0 / (1.0 - WaveDeepTaperLoss(For[0], For[1], Topped, Bottomed, Depth, Record.TaperStartCm, Record.TaperEndCm));
		}
		if (bNew)
		{
			const UFluidNinjaBuoyancyComponent* Component = Body.Component.Get();
			UE_LOG(LogFluidNinjaBuoyancy, Log, TEXT("FluidNinja Buoyancy waves: %s sinks (resting draft %.1f cm): it presses a deep volume head from here on (fnb.Waves.PlungeSource), in water %.0f cm deep (%s), reaching %.0f cm; %s; equivalent footprint %.0f x %.0f cm, %.1f L under now; no ripple impulses, no own field."),
				*GetNameSafe(Component ? Component->GetOwner() : nullptr), Component ? Component->GetRestingDraft() : 0.f, Depth,
				Body.DepthCm > 0.f && UFluidNinjaBuoyancySettings::Get().bWaveDepthFromGround ? TEXT("traced to the ground") : TEXT("its simulation's depth"), Record.TaperEndCm,
				Record.Fade >= 1.f ? TEXT("whole from its first contact") : TEXT("fading in (it was already in the water)"), 2.0 * Shape.Half.X, 2.0 * Shape.Half.Y, Shape.VolumeCm3 * 0.001);
		}
	}
	if (Record.bValid)
	{
		Record.RestSeconds = bResting ? Record.RestSeconds + Dt : 0.0;
		Record.FadeTarget = Record.RestSeconds >= WaveDeepRestSeconds ? 0.f : 1.f;
		// Faded out after its long rest: it presses nothing from here on (so it is not sent, its kept sum goes, and the core judges breaking
		// over its disc again), but it stays the body's, to fade back in if the body moves again.
		if (!StepDeepFade(Record, Dt))
		{
			Record.Fade = 0.f;
		}
	}
}

bool UFluidNinjaBuoyancyWaves::Jumped(const FBodyState& Track, const FVector& Location, const FQuat& Rotation, const FVector& Velocity, const FVector& Spin, float DeltaSeconds) const
{
	using namespace FluidNinjaBuoyancyWaveRuntime;
	const double Dt = FMath::Max(static_cast<double>(DeltaSeconds), 0.0);
	const double Speed = FMath::Max(Velocity.Size(), Track.Velocity.Size());
	if (FVector::Dist(Location, Track.Location) > WaveTeleportSlack * Speed * Dt + WaveTeleportMarginCm)
	{
		return true;
	}
	const double SpinRate = FMath::Max(Spin.Size(), Track.Spin.Size());
	return Track.Rotation.AngularDistance(Rotation) > WaveTeleportSlack * SpinRate * Dt + WaveTeleportMarginRad;
}

bool UFluidNinjaBuoyancyWaves::EnsureAtlas()
{
	using namespace FluidNinjaBuoyancyWaveRuntime;
	if (SelfAtlas && SelfAtlas->GameThread_GetRenderTargetResource())
	{
		return true;
	}
	if (!UE::PixelFormat::HasCapabilities(PF_R16F, EPixelFormatCapabilities::TypedUAVStore))
	{
		if (!bWarnedSelfAtlas)
		{
			UE_LOG(LogFluidNinjaBuoyancy, Error, TEXT("FluidNinja Buoyancy waves: this GPU cannot write R16F through a UAV, so the bodies get no own fields and feel no waves (they still make them)."));
			bWarnedSelfAtlas = true;
		}
		return false;
	}
	// The first size, or the size it had grown to (the tile allocator is already that size).
	const int32 Size = AtlasSize > 0 ? AtlasSize : WaveAtlasFirstSize;
	if (!SelfAtlas)
	{
		SelfAtlas = NewObject<UTextureRenderTarget2D>(this);
		SelfAtlas->RenderTargetFormat = RTF_R16f;
		SelfAtlas->ClearColor = FLinearColor::Transparent;
		SelfAtlas->AddressX = TA_Clamp;
		SelfAtlas->AddressY = TA_Clamp;
		SelfAtlas->Filter = TF_Nearest;
		SelfAtlas->bCanCreateUAV = true;
	}
	SelfAtlas->InitCustomFormat(Size, Size, PF_R16F, true);
	SelfAtlas->UpdateResourceImmediate(true);
	AtlasSize = Size;
	return SelfAtlas->GameThread_GetRenderTargetResource() != nullptr;
}

bool UFluidNinjaBuoyancyWaves::GrowAtlas()
{
	using namespace FluidNinjaBuoyancyWaveRuntime;
	if (!SelfAtlas || AtlasSize <= 0 || AtlasSize >= WaveAtlasMaxSize)
	{
		return false;
	}
	const int32 Grown = FMath::Min(AtlasSize * 2, WaveAtlasMaxSize);
	// Every tile is in the old layout: every field is released (the render thread forgets its state with this frame's release list) and
	// places itself again on the new atlas, starting over from still water, as a new body does.
	int32 Released = 0;
	for (TPair<int32, FBodyState>& Entry : Bodies)
	{
		Released += Entry.Value.bSelf ? 1 : 0;
		ReleaseSelf(Entry.Value);
	}
	Tiles.Reset(Grown);
	// The same object, resized in place (the materials and the gather keep their pointer; the render thread sees the new texture from the
	// next command on).
	SelfAtlas->InitCustomFormat(Grown, Grown, PF_R16F, true);
	SelfAtlas->UpdateResourceImmediate(true);
	AtlasSize = Grown;
	UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy waves: the self-field atlas is full; grown to %d x %d (%d MB), the %d own fields start over on it%s."),
		Grown, Grown, Grown * Grown * 2 / (1024 * 1024), Released, Grown >= WaveAtlasMaxSize ? TEXT(" (the largest it gets)") : TEXT(""));
	return SelfAtlas->GameThread_GetRenderTargetResource() != nullptr;
}

void UFluidNinjaBuoyancyWaves::SetSelfStep(FluidNinjaBuoyancyWaveSource::FSelf& Self, const FSimState& Sim, const FBodyState& Body) const
{
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	Self.DepthCm = Body.DepthCm > 0.f ? Body.DepthCm : Sim.DepthCm;
	Self.Damping = FMath::Max(Settings.WaveDamping, 0.f);
	Self.Hyperviscosity = FMath::Max(Settings.WaveHyperviscosity, 0.f);
	Self.ShoreDepthCm = FMath::Max(Settings.WaveShoreDepthCm, 0.f);
	Self.BreakingSlope = FMath::Max(Settings.WaveBreakingSlope, 0.01f);   // as the core is given it
	Self.SpongeCm = Body.SpongeCm;
	Self.TickSeconds = Sim.TickSeconds;
	Self.Tile = Body.Tile;
	// The field's cell (0, 0) in the simulation's cells (the window's own lattice, which the field's cells share: whole cells apart).
	const FVector2D SelfMin(Sim.Phase.X + static_cast<double>(Body.OriginCellX) * Sim.CellCm, Sim.Phase.Y + static_cast<double>(Body.OriginCellY) * Sim.CellCm);
	Self.AuxCell = FIntPoint(FMath::RoundToInt32((SelfMin.X - Sim.Origin.X) / Sim.CellCm), FMath::RoundToInt32((SelfMin.Y - Sim.Origin.Y) / Sim.CellCm));
	Self.SimCells = Sim.Cells;
	Self.bEdgeOpen = Sim.bEdgeOpen;
	Self.SimSpongeCm = FMath::Max(Settings.WaveSpongeCm, 0.f);
	Self.WallAbsorption = Sim.WallAbsorption;
	Self.WallBandCm = Sim.WallBandCm;   // as the core is given it (UpdateSim)
	// Not the core's rate: fnb.Waves.SelfMeanRelax, clamped (UpdateSim). The fields' own band dominates their k = 0 decay, and a strong pin
	// on a small tile would turn each impulse into a uniform kick (FluidNinjaBuoyancyWaveRuntime::WaveSelfMeanRelax).
	Self.MeanRelax = Sim.SelfMeanRelax;
}

bool UFluidNinjaBuoyancyWaves::PlaceSelf(FBodyState& Body, const FSimState& Sim, const FVector2D& Centre, float Radius, FIntPoint& OutShift, bool& bOutNew)
{
	using namespace FluidNinjaBuoyancyWaveRuntime;
	OutShift = FIntPoint::ZeroValue;
	bOutNew = false;
	const double Cell = Sim.CellCm;
	// The hull's reach (its bounding sphere, two cells and some slack), the sponge outside it, and the smallest size that holds both.
	const double Reach = static_cast<double>(Radius) + 2.0 * Cell + WaveSelfSlackCm;
	const double WantedSponge = FMath::Max(static_cast<double>(UFluidNinjaBuoyancySettings::Get().WaveSpongeCm), 0.0);
	int32 Size = FluidNinjaBuoyancyWaveSource::MinSelfCells;
	while (Size < FluidNinjaBuoyancyWaveSource::MaxSelfCells && Size * Cell < 2.0 * (Reach + WantedSponge))
	{
		Size *= 2;
	}
	const double Sponge = FMath::Min(WantedSponge, 0.5 * Size * Cell - Reach);
	if (Sponge < WaveSelfMinSpongeCells * Cell)
	{
		if (!Body.bSelfWarned)
		{
			const UFluidNinjaBuoyancyComponent* Component = Body.Component.Get();
			UE_LOG(LogFluidNinjaBuoyancy, Error, TEXT("FluidNinja Buoyancy waves: %s (%.0f cm across) is too big for its own field on %s's %.2f cm cells (%d at most): it makes waves but feels none."),
				*GetNameSafe(Component ? Component->GetOwner() : nullptr), 2.f * Radius, *Sim.Name, Cell, FluidNinjaBuoyancyWaveSource::MaxSelfCells);
			Body.bSelfWarned = true;
		}
		ReleaseSelf(Body);
		return false;
	}
	// It fits these cells: a later refusal (other cells) is logged again.
	Body.bSelfWarned = false;
	// A sponge under half the wanted width still reflects some of the body's own waves back at it (a 3.4 m boat on 1.27 cm cells gets
	// about 100 cm, under one wavelength of its own waves): said once per body, so a fine simulation degrading the fields is seen.
	if (Sponge < 0.5 * WantedSponge)
	{
		if (!Body.bSpongeWarned)
		{
			const UFluidNinjaBuoyancyComponent* Component = Body.Component.Get();
			UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("FluidNinja Buoyancy waves: %s's own field on %s is %d cells of %.2f cm (%.0f cm across, the hull %.0f cm): its sponge is %.0f cm, under half the %.0f cm wanted (Wave Sponge Cm), so some of its own waves come back at it. Coarser cells or a smaller body keep the sponge."),
				*GetNameSafe(Component ? Component->GetOwner() : nullptr), *Sim.Name, Size, Cell, Size * Cell, 2.f * Radius, Sponge, WantedSponge);
			Body.bSpongeWarned = true;
		}
	}
	else
	{
		Body.bSpongeWarned = false;
	}
	if (!EnsureAtlas())
	{
		ReleaseSelf(Body);
		return false;
	}
	const auto CornerFor = [Cell, Size](double At, double PhaseOf)
	{
		return FMath::FloorToInt64((At - PhaseOf) / Cell) - Size / 2;
	};
	if (!Body.bSelf || Body.SelfCells != Size || !FMath::IsNearlyEqual(Body.SelfCellCm, Cell, Cell * 1e-6))
	{
		ReleaseSelf(Body);
		FIntPoint Tile;
		// No room: the atlas doubles (every field starts over, this one included) until the largest size, which is then full.
		if (!Tiles.Allocate(Size, Tile) && !(GrowAtlas() && Tiles.Allocate(Size, Tile)))
		{
			if (!bWarnedSelfAtlas)
			{
				UE_LOG(LogFluidNinjaBuoyancy, Error, TEXT("FluidNinja Buoyancy waves: the self-field atlas (%d x %d, the largest it gets) is full; the bodies that find no room in it make waves but feel none. Logged once."),
					AtlasSize, AtlasSize);
				bWarnedSelfAtlas = true;
			}
			return false;
		}
		Body.bSelf = true;
		Body.bSelfReady = false;
		Body.SelfCells = Size;
		Body.SelfCellCm = Cell;
		Body.Tile = Tile;
		Body.OriginCellX = CornerFor(Centre.X, Sim.Phase.X);
		Body.OriginCellY = CornerFor(Centre.Y, Sim.Phase.Y);
		bOutNew = true;
	}
	Body.SpongeCm = static_cast<float>(Sponge);
	// Recentred by whole cells when the hull would reach into the sponge.
	const double Half = 0.5 * Size * Cell;
	const FVector2D Middle(Sim.Phase.X + (static_cast<double>(Body.OriginCellX) + Size / 2) * Cell, Sim.Phase.Y + (static_cast<double>(Body.OriginCellY) + Size / 2) * Cell);
	const double Allowed = Half - Sponge - static_cast<double>(Radius) - Cell;
	if (!bOutNew && (FMath::Abs(Centre.X - Middle.X) > Allowed || FMath::Abs(Centre.Y - Middle.Y) > Allowed))
	{
		const int64 NewX = CornerFor(Centre.X, Sim.Phase.X);
		const int64 NewY = CornerFor(Centre.Y, Sim.Phase.Y);
		OutShift = FIntPoint(static_cast<int32>(FMath::Clamp<int64>(NewX - Body.OriginCellX, -Size, Size)), static_cast<int32>(FMath::Clamp<int64>(NewY - Body.OriginCellY, -Size, Size)));
		Body.OriginCellX = NewX;
		Body.OriginCellY = NewY;
	}
	return true;
}

void UFluidNinjaBuoyancyWaves::UpdateDepth(FBodyState& Body, const FSimState& Sim, const FFluidNinjaWaveBodyInput& Input, const FFluidNinjaWaveWorldInput& World)
{
	using namespace FluidNinjaBuoyancyWaveRuntime;
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	UWorld* OwnerWorld = GetOwnerWorld();
	const FVector2D At(Input.Transform.GetLocation());
	if (!Settings.bWaveDepthFromGround || !OwnerWorld || Input.WaterSource == EFluidNinjaWaveWater::None || World.bControlled)
	{
		Body.DepthCm = Sim.DepthCm;
		return;
	}
	const double Now = OwnerWorld->GetTimeSeconds();
	if (Body.DepthCm > 0.f && Now < Body.NextDepthTrace && FVector2D::Distance(At, Body.DepthTracedAt) < WaveDepthRetraceCm)
	{
		return;
	}
	Body.NextDepthTrace = Now + WaveDepthRetraceSeconds;
	Body.DepthTracedAt = At;
	// Straight down from the water through everything but the ground: the body itself, the people (pawns are not world static) and the
	// drawn water's own actors.
	FCollisionQueryParams Query(SCENE_QUERY_STAT(FluidNinjaBuoyancyWaveDepth), false);
	if (const UFluidNinjaBuoyancyComponent* Component = Input.Component.Get())
	{
		Query.AddIgnoredActor(Component->GetOwner());
	}
	for (const TWeakObjectPtr<AActor>& Surface : World.SurfaceActors)
	{
		if (const AActor* SurfaceActor = Surface.Get())
		{
			Query.AddIgnoredActor(SurfaceActor);
		}
	}
	FHitResult Hit;
	const FVector Start(At.X, At.Y, Input.WaterZ + 5.0);
	const FVector End(At.X, At.Y, Input.WaterZ - WaveDepthTraceCm);
	if (OwnerWorld->LineTraceSingleByObjectType(Hit, Start, End, FCollisionObjectQueryParams(ECC_WorldStatic), Query))
	{
		// At most the simulation's depth: the core runs beds its height clamp flattened at Wave Depth, only shallower ones at their own.
		Body.DepthCm = FMath::Clamp(static_cast<float>(Input.WaterZ - Hit.ImpactPoint.Z), 1.f, Sim.DepthCm);
	}
	else
	{
		Body.DepthCm = Sim.DepthCm;   // no ground within reach: the depth the core takes where it knows none
	}
}

void UFluidNinjaBuoyancyWaves::UpdateRadiation(FBodyState& Body, const FSimState& Sim, const UFluidNinjaBuoyancyComponent& Component)
{
	using namespace FluidNinjaBuoyancyWaveRuntime;
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	const UWorld* World = GetOwnerWorld();
	const double Cell = Sim.CellCm;
	const float Depth = Body.DepthCm > 0.f ? Body.DepthCm : Sim.DepthCm;
	constexpr int32 Bins = FBodyState::RadiationBins;
	constexpr int32 NaturalBin = Bins / 2;
	// The footprint, rastered once per cell size and depth (within a fifth): its added mass, its natural frequencies, and the damping
	// at them (the bin every motion starts in; the others are summed as the body's motion asks for them).
	if (!(Body.bRadiation && FMath::IsNearlyEqual(Body.RadiationCellCm, Cell, Cell * 1e-3) && FMath::Abs(Depth - Body.RadiationDepthCm) <= 0.2f * Body.RadiationDepthCm))
	{
		Body.bRadiation = true;
		Body.RadiationCellCm = Cell;
		Body.RadiationDepthCm = Depth;
		Body.Footprint = FFluidNinjaWaveFootprint();
		Body.Radiation = FVector::ZeroVector;
		Body.RestWettedArea = 0.f;
		for (int32 Motion = 0; Motion < 3; ++Motion)
		{
			Body.RadiationBin[Motion] = -1;
			for (int32 Bin = 0; Bin < Bins; ++Bin)
			{
				Body.RadiationTable[Motion][Bin] = -1.0;
			}
		}
		const UPrimitiveComponent* Primitive = Component.GetSimulatedPrimitive();
		const FBodyInstance* Instance = Primitive ? Primitive->GetBodyInstance() : nullptr;
		const TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe> Surface = Component.GetWaveHull();
		const TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe> Physics = Component.GetHull(0);
		if (!Instance || !Surface.IsValid() || !Physics.IsValid() || !World)
		{
			return;
		}
		const double Rho = Settings.WaterDensity * 1e-6;
		const double Gravity = FMath::Max(1.0, -static_cast<double>(World->GetGravityZ()));
		const FQuat Attitude = Component.RestAttitude.Quaternion();
		const float Draft = Component.GetRestingDraft();
		const float Mass = Instance->GetBodyMass();
		WaveFootprint(*Surface, Attitude, Draft, Mass, Instance->GetBodyInertiaTensor(), Cell, Depth, Gravity, Rho, FMath::Max(Settings.WaveAddedMassScale, 0.f),
			Body.Footprint, Body.RollAxis, Body.PitchAxis);
		FluidNinjaBuoyancy::FParams Params;
		Component.FillParams(Params);
		Params.Rho = static_cast<float>(Rho);
		Params.Gravity = static_cast<float>(Gravity);
		Body.RestWettedArea = WaveRestWettedArea(*Physics, Attitude, Draft, Mass, Params);
		if (Settings.bRadiationDamping)
		{
			for (int32 Motion = 0; Motion < 3; ++Motion)
			{
				Body.RadiationTable[Motion][NaturalBin] = WaveRadiationAt(Body.Footprint, Motion, Body.Footprint.Natural[Motion]);
			}
		}
		const FFluidNinjaWaveFootprint& Foot = Body.Footprint;
		UE_LOG(LogFluidNinjaBuoyancy, Log, TEXT("FluidNinja Buoyancy waves: %s footprint on %.2f cm cells at %.0f cm depth: added mass heave %.3g kg (%.0f%% of the body), roll %.3g, pitch %.3g kg cm² (scale %.2f); natural %.2f Hz heave, %.2f roll, %.2f pitch; radiation damping there heave %.3g kg/s, roll %.3g, pitch %.3g kg cm²/s (rest wetted area %.0f cm²)."),
			*GetNameSafe(Component.GetOwner()), Cell, Depth, Foot.AddedMass[0], Mass > 0.f ? 100.0 * Foot.AddedMass[0] / Mass : 0.0, Foot.AddedMass[1], Foot.AddedMass[2], Settings.WaveAddedMassScale,
			Foot.Natural[0] / (2.0 * UE_DOUBLE_PI), Foot.Natural[1] / (2.0 * UE_DOUBLE_PI), Foot.Natural[2] / (2.0 * UE_DOUBLE_PI),
			FMath::Max(Body.RadiationTable[0][NaturalBin], 0.0), FMath::Max(Body.RadiationTable[1][NaturalBin], 0.0), FMath::Max(Body.RadiationTable[2][NaturalBin], 0.0), Body.RestWettedArea);
	}
	if (!Settings.bRadiationDamping || !Body.Footprint.bValid)
	{
		Body.Radiation = FVector::ZeroVector;
		return;
	}
	// The damping at the frequency each motion runs at: the running estimate (UpdateMotion) in quarter-octave bins about the natural
	// frequency, each summed over the footprint when first needed and kept. A motion moves to another bin only when its estimate is
	// three quarters of a bin past the one in use (so it does not flap at a bin's edge) and the body has not moved bins for a while.
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	const bool bMaySwitch = Now >= Body.NextRadiationSwitch;
	bool bSwitched = false;
	for (int32 Motion = 0; Motion < 3; ++Motion)
	{
		const double Natural = Body.Footprint.Natural[Motion];
		if (!(Natural > 0.0))
		{
			Body.Radiation[Motion] = 0.0;
			continue;
		}
		const double Omega = Body.MotionOmega[Motion] > 0.f ? Body.MotionOmega[Motion] : Natural;
		const double Position = 4.0 * FMath::Log2(Omega / Natural) + NaturalBin;
		int32 Bin = Body.RadiationBin[Motion];
		if (Bin < 0 || (bMaySwitch && FMath::Abs(Position - Bin) > 0.75))
		{
			Bin = FMath::Clamp(FMath::RoundToInt32(Position), 0, Bins - 1);
		}
		if (Bin != Body.RadiationBin[Motion])
		{
			if (Body.RadiationTable[Motion][Bin] < 0.0)
			{
				Body.RadiationTable[Motion][Bin] = WaveRadiationAt(Body.Footprint, Motion, Natural * FMath::Pow(2.0, (Bin - NaturalBin) / 4.0));
			}
			bSwitched |= Body.RadiationBin[Motion] >= 0;
			Body.RadiationBin[Motion] = Bin;
		}
		Body.Radiation[Motion] = FMath::Max(Body.RadiationTable[Motion][Bin], 0.0);
	}
	if (bSwitched)
	{
		Body.NextRadiationSwitch = Now + WaveRadiationSwitchSeconds;
	}
}

void UFluidNinjaBuoyancyWaves::UpdateMotion(FBodyState& Body, const FFluidNinjaWaveBodyInput& Input, float DeltaSeconds)
{
	using namespace FluidNinjaBuoyancyWaveRuntime;
	// This frame's speeds: heave along world Z, roll and pitch about the rest attitude's axes turned with the body. The frame's velocity
	// is what the waves see of the body (the physics steps between frames are not), good enough for an average over a second.
	const FVector Roll = Body.Rotation.RotateVector(Body.RollAxis);
	const FVector Pitch = Body.Rotation.RotateVector(Body.PitchAxis);
	const double Speeds[3] = {Input.Velocity.Z, FVector::DotProduct(Input.AngularVelocity, Roll), FVector::DotProduct(Input.AngularVelocity, Pitch)};
	const double Dt = DeltaSeconds;
	if (Body.bMotionPrimed && Dt > 0.0)
	{
		// Exponential averages of v² and a² (a the change of v since last frame) over WaveMotionAverageSeconds.
		const double Alpha = 1.0 - FMath::Exp(-Dt / WaveMotionAverageSeconds);
		for (int32 Motion = 0; Motion < 3; ++Motion)
		{
			const double Acceleration = (Speeds[Motion] - Body.MotionLastV[Motion]) / Dt;
			Body.MotionV2[Motion] += Alpha * (Speeds[Motion] * Speeds[Motion] - Body.MotionV2[Motion]);
			Body.MotionA2[Motion] += Alpha * (Acceleration * Acceleration - Body.MotionA2[Motion]);
		}
	}
	for (int32 Motion = 0; Motion < 3; ++Motion)
	{
		Body.MotionLastV[Motion] = Speeds[Motion];
	}
	Body.bMotionPrimed = true;
	// A sinusoid at ω has ⟨a²⟩ / ⟨v²⟩ = ω² (a mix of frequencies leans to the faster ones, which the radiation damping cares most
	// about). Clamped to half to twice the natural frequency, the range the footprint's damping is tabulated over; a body too still to
	// measure (or without a footprint yet) is at its natural frequency.
	for (int32 Motion = 0; Motion < 3; ++Motion)
	{
		const double Natural = Body.Footprint.bValid ? Body.Footprint.Natural[Motion] : 0.0;
		double Omega = Natural;
		if (Natural > 0.0 && Body.MotionV2[Motion] > (Motion == 0 ? WaveMotionHeaveFloor : WaveMotionSpinFloor))
		{
			Omega = FMath::Clamp(FMath::Sqrt(Body.MotionA2[Motion] / Body.MotionV2[Motion]), 0.5 * Natural, 2.0 * Natural);
		}
		Body.MotionOmega[Motion] = FMath::IsFinite(Omega) ? Omega : Natural;
	}
}

void UFluidNinjaBuoyancyWaves::UpdateTriangleCache(const TArray<TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe>>& Hulls)
{
	TArray<UPTRINT> Keys;
	Keys.Reserve(Hulls.Num());
	for (const TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe>& Used : Hulls)
	{
		Keys.Add(reinterpret_cast<UPTRINT>(Used.Get()));
	}
	Keys.Sort();
	if (CachedTriangles.IsValid() && Keys == CachedHullKeys)
	{
		return;   // the same hulls as before: their triangles are already up
	}
	// Kept alive while cached, so no other hull can come to have a cached one's address.
	CachedHulls = Hulls;
	CachedFirstTriangle.Reset();
	int32 Corners = 0;
	for (const TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe>& Used : Hulls)
	{
		Corners += Used->Triangles.Num() * 3;
	}
	TSharedRef<TArray<FVector4f>, ESPMode::ThreadSafe> Built = MakeShared<TArray<FVector4f>, ESPMode::ThreadSafe>();
	Built->Reserve(Corners);
	for (const TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe>& Used : Hulls)
	{
		const FluidNinjaBuoyancy::FHull& Surface = *Used;
		CachedFirstTriangle.Add(reinterpret_cast<UPTRINT>(&Surface), static_cast<uint32>(Built->Num() / 3));
		for (const FIntVector3& Triangle : Surface.Triangles)
		{
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const FVector3f& Vertex = Surface.Vertices[Triangle[Corner]];
				Built->Add(FVector4f(Vertex.X, Vertex.Y, Vertex.Z, 0.f));
			}
		}
	}
	CachedHullKeys = MoveTemp(Keys);
	CachedTriangles = Built;
	// New contents: the GPU uploads them once and keeps them while this stays the same.
	++TriangleSerial;
}

void UFluidNinjaBuoyancyWaves::SetMirrorWavesHidden(bool bHidden, const TArray<TWeakObjectPtr<UPrimitiveComponent>>& Shown)
{
	const FName Parameter = UFluidNinjaBuoyancySettings::Get().WaveHeightScaleParameter;
	if (!bHidden)
	{
		for (const TPair<TWeakObjectPtr<UMaterialInstanceDynamic>, float>& Kept : HiddenMaterials)
		{
			if (UMaterialInstanceDynamic* Material = Kept.Key.Get())
			{
				Material->SetScalarParameterValue(Parameter, Kept.Value);
			}
		}
		HiddenMaterials.Reset();
		return;
	}
	UnhiddenSurfaces = 0;
	if (!IsActive() || Parameter.IsNone())
	{
		return;
	}
	// The dynamic materials are found again every frame: FluidNinja makes new ones during play (Recollect Output Material Targets During
	// Game Time).
	TArray<UMaterialInterface*> Used;
	for (const TWeakObjectPtr<UPrimitiveComponent>& Weak : Shown)
	{
		const UPrimitiveComponent* Primitive = Weak.Get();
		if (!Primitive)
		{
			continue;
		}
		Used.Reset();
		Primitive->GetUsedMaterials(Used);
		bool bSurfaceHidden = false;
		for (UMaterialInterface* Interface : Used)
		{
			UMaterialInstanceDynamic* Material = Cast<UMaterialInstanceDynamic>(Interface);
			if (!Material)
			{
				continue;
			}
			if (HiddenMaterials.ContainsByPredicate([Material](const TPair<TWeakObjectPtr<UMaterialInstanceDynamic>, float>& Kept) { return Kept.Key.Get() == Material; }))
			{
				bSurfaceHidden = true;   // shared with a surface already done
				continue;
			}
			float Value = 0.f;
			if (!Material->GetScalarParameterValue(FHashedMaterialParameterInfo(Parameter), Value))
			{
				continue;
			}
			HiddenMaterials.Emplace(Material, Value);
			Material->SetScalarParameterValue(Parameter, 0.f);
			bSurfaceHidden = true;
		}
		// A surface of a running wave simulation that could not hide its waves: the mirror shows them, and the floaters on it would feel their
		// own waves through W besides B - B_self (the self-excitation the own fields are there to remove). Said, not worked around.
		const FVector2D At(Primitive->Bounds.Origin);
		const FSimState* Under = Sims.FindByPredicate([&At](const FSimState& Sim)
		{
			return Sim.bActive && FMath::Abs(At.X - Sim.Centre.X) < Sim.SizeCm * 0.5 && FMath::Abs(At.Y - Sim.Centre.Y) < Sim.SizeCm * 0.5;
		});
		if (!bSurfaceHidden && Under)
		{
			++UnhiddenSurfaces;
			if (!WarnedUnhidden.Contains(FObjectKey(Primitive)))
			{
				WarnedUnhidden.Add(FObjectKey(Primitive));
				UE_LOG(LogFluidNinjaBuoyancy, Error, TEXT("FluidNinja Buoyancy waves: the captured surface %s of %s (%s) has no dynamic material with %s, so the Surface Mirror shows its waves and the floaters on it feel their own waves twice. Give its material the WaveHeightScale scalar (MF_NinjaOutput_BaseFunction_SURFACE) as a dynamic material, or turn Waves off for %s (Wave Sims)."),
					*Primitive->GetName(), *GetNameSafe(Primitive->GetOwner()), *Under->Name, *Parameter.ToString(), *Under->Name);
			}
		}
	}
}

bool UFluidNinjaBuoyancyWaves::GetGatherSlot(int32 BodyId, FFluidNinjaWaveGatherSlot& Out) const
{
	const FBodyState* Body = Bodies.Find(BodyId);
	const FSimState* Sim = Body ? FindSim(Body->Sim) : nullptr;
	UTextureRenderTarget2D* Pressure = Sim ? Sim->Pressure.Get() : nullptr;
	if (!bRunning || !Body || !Body->bSelf || !Body->bSelfReady || !Sim || !Sim->bActive || !Pressure || !SelfAtlas)
	{
		return false;
	}
	// B is read on the pressure buffer's texels (the core exports it there whatever the wave grid), B_self on the body's own field's cells,
	// the wave grid's (the pressure cells over the scale-down).
	Out.Pressure = Pressure;
	Out.PressureCells = Sim->PressureCells;
	Out.SimOrigin = Sim->Origin;
	Out.CellCm = Sim->PressureCellCm;
	Out.SelfOrigin = FVector2D(Sim->Phase.X + static_cast<double>(Body->OriginCellX) * Sim->CellCm, Sim->Phase.Y + static_cast<double>(Body->OriginCellY) * Sim->CellCm);
	Out.SelfCellCm = Sim->CellCm;
	Out.SelfCells = Body->SelfCells;
	Out.Tile = Body->Tile;
	Out.SpongeCells = static_cast<float>(Body->SpongeCm / Sim->CellCm);
	return true;
}

bool UFluidNinjaBuoyancyWaves::GetBodyFeel(int32 BodyId, FFluidNinjaWaveBodyFeel& Out) const
{
	Out = FFluidNinjaWaveBodyFeel();
	const FBodyState* Body = Bodies.Find(BodyId);
	const UFluidNinjaBuoyancyComponent* Component = Body ? Body->Component.Get() : nullptr;
	const FSimState* Sim = Body ? FindSim(Body->Sim) : nullptr;
	// A body without an own field (κ 0, far from the camera, no room, too big for its cells) feels no waves at all, Feel 0: B holds its own
	// waves, which only its field takes out.
	if (!bRunning || !Component || !Component->bWaves || !Body->bSelf || !Body->bSelfReady || !Sim || !Sim->bActive)
	{
		return false;
	}
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	Out.Feel = FMath::Max(Component->WaveFeelScale, 0.f) * FMath::Max(Settings.WaveFeelScale, 0.f);
	if (Body->bRadiation && Body->Footprint.bValid && Body->RestWettedArea > 0.f)
	{
		if (Settings.bRadiationDamping)
		{
			Out.Radiation = Body->Radiation * FMath::Max(FluidNinjaBuoyancyCVars::WaveRadiationScale, 0.f);
		}
		const double AddedMassScale = FMath::Max(Settings.WaveAddedMassScale, 0.f);
		Out.AddedMass = FVector(Body->Footprint.AddedMass[0], Body->Footprint.AddedMass[1], Body->Footprint.AddedMass[2]) * AddedMassScale;
		Out.RollAxis = Body->RollAxis;
		Out.PitchAxis = Body->PitchAxis;
		Out.RestWettedArea = Body->RestWettedArea;
		Out.MotionHz = Body->MotionOmega / (2.0 * UE_DOUBLE_PI);
	}
	return true;
}

void UFluidNinjaBuoyancyWaves::Tick(float DeltaSeconds, const FFluidNinjaWaveWorldInput& Input)
{
	using namespace FluidNinjaBuoyancyWaveRuntime;
	SCOPE_CYCLE_COUNTER(STAT_FNB_WavesBuild);
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	UWorld* World = GetOwnerWorld();
	++TickStamp;
	FString Why;
	if (!World || !GpuState || !IsWanted(Why))
	{
		if (bRunning || StatusLine.IsEmpty() || !Why.IsEmpty())
		{
			Deactivate(Why.IsEmpty() ? FString(TEXT("no world")) : Why);
		}
		PendingPokes.Reset();
		SET_DWORD_STAT(STAT_FNB_SelfFields, 0);
		SET_DWORD_STAT(STAT_FNB_SelfCells, 0);
		SET_DWORD_STAT(STAT_FNB_ImpulsePieces, 0);
		SET_DWORD_STAT(STAT_FNB_WaveSimsActive, 0);
		// The same figures for the CSV profiler (category FNB, FluidNinjaBuoyancyStats.h), so a -csvCaptureFrames run reads 0 while the
		// waves are off rather than leaving the columns empty.
		CSV_CUSTOM_STAT(FNB, SelfFields, 0, ECsvCustomStatOp::Set);
		CSV_CUSTOM_STAT(FNB, SelfCells, 0, ECsvCustomStatOp::Set);
		CSV_CUSTOM_STAT(FNB, ImpulsePieces, 0, ECsvCustomStatOp::Set);
		CSV_CUSTOM_STAT(FNB, WaveSimsActive, 0, ECsvCustomStatOp::Set);
		return;
	}
	if (!bRunning)
	{
		UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy waves: on (FluidNinja Waves on every simulation whose NinjaLiveCore has %s)."), *WaveSimEnabledName.ToString());
	}
	bRunning = true;
	const double Now = World->GetTimeSeconds();
	const bool bClear = bClearRequested;
	bClearRequested = false;

	// The simulations: matched to last frame's by their NinjaLive (a state for each, first, so the ranking below sees new ones too), ranked
	// for fnb.Waves.MaxActiveSims, then placed and their cores' parameters set.
	for (FSimState& Sim : Sims)
	{
		Sim.bSeen = false;
	}
	TArray<int32, TInlineAllocator<8>> SimFor;   // per input simulation: its index in Sims, or INDEX_NONE when it runs no waves
	for (const FFluidNinjaWaveSimInput& SimInput : Input.Sims)
	{
		const FObjectKey Key(SimInput.Ninja.Get());
		FSimState* Sim = FindSim(Key);
		if (!Sim)
		{
			Sim = &Sims.AddDefaulted_GetRef();
			Sim->Key = Key;
			// Its name for the render thread's kept deep sums (never 0, so a source without one is never taken for it).
			Sim->DeepKey = NextDeepKey;
			NextDeepKey = NextDeepKey == MAX_uint32 ? 1u : NextDeepKey + 1u;
		}
		Sim->bSeen = true;
		SimFor.Add(static_cast<int32>(Sim - Sims.GetData()));
	}
	RankSims(Input, Now);
	for (int32 Index = 0; Index < Input.Sims.Num(); ++Index)
	{
		FSimState& Sim = Sims[SimFor[Index]];
		const bool bRuns = UpdateSim(Sim, Input.Sims[Index]);
		if (!bRuns)
		{
			SimFor[Index] = INDEX_NONE;
		}
		else if (!Sim.bTicksRead)
		{
			ReadTicks(Sim);   // new this frame (BeginFrame did not know it)
		}
		Sim.bTicksRead = false;
	}
	for (int32 Index = Sims.Num() - 1; Index >= 0; --Index)
	{
		if (!Sims[Index].bSeen)
		{
			StopSim(Sims[Index]);
			Sims.RemoveAt(Index);
			for (int32& Found : SimFor)
			{
				Found = Found == Index ? INDEX_NONE : (Found > Index ? Found - 1 : Found);
			}
		}
	}
	for (FSimState& Sim : Sims)
	{
		if (Sim.bActive && (Sim.bRestart || bClear))
		{
			Sim.bPendingEraseAll = true;
		}
	}
	// Each simulation's surfaces show its waves' slope at its own WaveNormalScale, and sample its foam target as WaveFoamBuffer (about once
	// a second: FluidNinja makes new dynamic materials during play).
	if (Now >= NextNormalScale)
	{
		NextNormalScale = Now + 1.0;
		const FName NormalParameter = Settings.WaveNormalScaleParameter;
		TArray<UMaterialInterface*> Used;
		for (const TPair<TWeakObjectPtr<UPrimitiveComponent>, int32>& Surface : Input.Surfaces)
		{
			const UPrimitiveComponent* Primitive = Surface.Key.Get();
			const int32 Shown = SimFor.IsValidIndex(Surface.Value) ? SimFor[Surface.Value] : INDEX_NONE;
			if (!Primitive || !Sims.IsValidIndex(Shown))
			{
				continue;
			}
			const float Wanted = Sims[Shown].NormalScale;
			UTextureRenderTarget2D* FoamWanted = Sims[Shown].Foam.Get();
			Used.Reset();
			Primitive->GetUsedMaterials(Used);
			for (UMaterialInterface* Interface : Used)
			{
				UMaterialInstanceDynamic* Material = Cast<UMaterialInstanceDynamic>(Interface);
				if (!Material)
				{
					continue;
				}
				float Value = 0.f;
				if (!NormalParameter.IsNone() && Material->GetScalarParameterValue(FHashedMaterialParameterInfo(NormalParameter), Value) && !FMath::IsNearlyEqual(Value, Wanted, 1e-6f))
				{
					Material->SetScalarParameterValue(NormalParameter, Wanted);
				}
				// The foam buffer, where the material has the parameter (the interface-4 material edit; unbound it is the function's black
				// default). Bound once per dynamic material: the target object is stable, only its contents change.
				UTexture* FoamBound = nullptr;
				if (FoamWanted && Material->GetTextureParameterValue(FHashedMaterialParameterInfo(WaveFoamBufferParameterName), FoamBound) && FoamBound != FoamWanted)
				{
					Material->SetTextureParameterValue(WaveFoamBufferParameterName, FoamWanted);
				}
			}
		}
	}
	// The window the test bench reads: the simulation holding its focus, else the one around the camera (the smallest), else the first
	// running.
	BenchCamera = FVector2D(Input.Camera);
	bBenchCamera = Input.bHasCamera;
	BenchSimKey = PickBenchSim();
	const auto SimAt = [this](const FVector2D& Point) -> FSimState*
	{
		FSimState* Best = nullptr;
		for (FSimState& Sim : Sims)
		{
			if (Sim.bActive && FMath::Abs(Point.X - Sim.Centre.X) < Sim.SizeCm * 0.5 && FMath::Abs(Point.Y - Sim.Centre.Y) < Sim.SizeCm * 0.5
				&& (!Best || Sim.SizeCm < Best->SizeCm))
			{
				Best = &Sim;
			}
		}
		return Best;
	};

	// The bodies: each one's static head record in its simulation and its own field. A new, teleported, re-hulled or re-watered body
	// (or one whose simulation started over) is reset: the water is cleared where it was and is, and its own field starts over, so putting
	// a body somewhere makes no wave; one that leaves clears where it was.
	const bool bDisplacement = IsDisplacementOn();
	struct FPlaced
	{
		int32 Sim = INDEX_NONE;
		FluidNinjaBuoyancyWaveSource::FBody Record;
		bool bRecord = false;
		int32 Hull = INDEX_NONE;
		/** The body's id (0 for the swimmer): looked up again where needed, as adding bodies may move them. */
		int32 BodyId = 0;
		bool bReset = false;
		FIntPoint Shift = FIntPoint::ZeroValue;
		FTransform Transform = FTransform::Identity;
		FBox2D Bounds = FBox2D(ForceInit);
		float WaterZ = WaveNoWaterZ;
		bool bSubmerged = false;
	};
	TArray<FPlaced> Placed;
	TArray<TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe>> UsedHulls;
	int32 Resets = 0;
	int32 Leaving = 0;
	// The graded release of a submerging hull's head (fnb.Waves.SubmergeRelease) runs on the world's gravity (cm/s², as the frame's) and
	// this frame's time (s; 0 while paused, so nothing fades then).
	const bool bSubmergeRelease = WaveSubmergeRelease != 0;
	const double ReleaseGravity = FMath::Max(1.0, -static_cast<double>(World->GetGravityZ()));
	const double ReleaseDt = FMath::Max(static_cast<double>(DeltaSeconds), 0.0);
	// fnb.Waves.PlungeSource works on the static head (Wave Displacement). With either off every body takes today's path at once and the
	// deep heads go with it, a step, for A/B only.
	const bool bPlunge = WavePlungeSource != 0 && bDisplacement;
	if (!bPlunge)
	{
		FadingDeep.Reset();
		for (TPair<int32, FBodyState>& Entry : Bodies)
		{
			FBodyState& Each = Entry.Value;
			Each.Deep = FDeepRecord();
			Each.bPathLatched = false;
			Each.bDeep = false;
			Each.bDeepStartsWhole = false;
			Each.bSinkerNow = false;
		}
	}
	for (const FFluidNinjaWaveBodyInput& BodyInput : Input.Bodies)
	{
		FBodyState* Body = Bodies.Find(BodyInput.Id);
		const UFluidNinjaBuoyancyComponent* Component = BodyInput.Component.Get();
		const FluidNinjaBuoyancy::FHull* Hull = BodyInput.Hull.Get();
		const int32 SimIndex = Input.Sims.IsValidIndex(BodyInput.Sim) && SimFor.IsValidIndex(BodyInput.Sim) ? SimFor[BodyInput.Sim] : INDEX_NONE;
		// A body whose simulation FluidNinja has switched off (activated by the player's nearness, and the player far) is left out, its own
		// field freed, until the simulation runs again.
		const UNiagaraComponent* SimCore = Sims.IsValidIndex(SimIndex) ? Sims[SimIndex].Core.Get() : nullptr;
		const bool bWanted = Component && Hull && !Hull->Triangles.IsEmpty() && !Hull->Vertices.IsEmpty() && BodyInput.bWaves && !BodyInput.bAsleep
			&& Sims.IsValidIndex(SimIndex) && SimCore && SimCore->IsActive();
		if (!bWanted)
		{
			// A sinker asleep (dormant) or held keeps its deep head as it was, in its simulation: a sunk body that goes to sleep on the bed
			// must not take its volume out of the water in one go (a release falls straight into the long-wave band; an erase would cut over
			// the water around it). Asleep it is at rest, so its head fades after the long rest, as an awake one's does.
			if (Body && Body->bDeep && Body->Deep.bValid && BodyInput.bAsleep && Component && BodyInput.bWaves && Sims.IsValidIndex(SimIndex) && SimCore
				&& SimCore->IsActive() && Body->Sim == Sims[SimIndex].Key)
			{
				Body->Stamp = TickStamp;
				Body->bSinkerNow = bPlunge && WaveBodySinks(*Component);
				Body->Deep.RestSeconds += ReleaseDt;
				Body->Deep.FadeTarget = Body->Deep.RestSeconds >= WaveDeepRestSeconds ? 0.f : 1.f;
				if (!StepDeepFade(Body->Deep, ReleaseDt))
				{
					Body->Deep.Fade = 0.f;   // faded out after its long rest: nothing more is sent (UpdateDeep says why)
				}
				ReleaseSelf(*Body);
				continue;
			}
			if (Body)
			{
				RemoveBody(*Body);
				Bodies.Remove(BodyInput.Id);
				++Leaving;
			}
			continue;
		}
		FSimState& Sim = Sims[SimIndex];
		const FBox2D Box = WaveHullBox(*Hull, BodyInput.Transform);
		const FVector Location = BodyInput.Transform.GetLocation();
		const FQuat Rotation = BodyInput.Transform.GetRotation();
		const UPTRINT HullKey = reinterpret_cast<UPTRINT>(Hull);
		if (Body && Body->Sim != Sim.Key)
		{
			RemoveBody(*Body);   // into another simulation: it leaves the old one, and starts afresh in the new
			Bodies.Remove(BodyInput.Id);
			Body = nullptr;
		}
		// A sinker on the deep path starts over only when it is put somewhere (its teleport counter, a jump) or its water is erased (a
		// restart, the bench's fresh ripples). A change of the water it is measured against (the Surface Mirror finding or losing it as the
		// camera moves) or of its hull is no break in the water around it, and the reset's erase over its box mid-plunge would cut into its
		// own waves. Every other body resets on all of them, as before.
		const bool bTeleported = !Body || Body->Counter != BodyInput.TeleportCounter || Sim.bRestart || bClear
			|| Jumped(*Body, Location, Rotation, BodyInput.Velocity, BodyInput.AngularVelocity, DeltaSeconds);
		const bool bReset = bTeleported || (!Body->bDeep && (Body->Hull != HullKey || Body->Water != BodyInput.WaterSource));
		if (!Body)
		{
			Body = &Bodies.Add(BodyInput.Id);
			Body->Id = BodyInput.Id;
		}
		if (bReset)
		{
			// Its source is decided afresh (below); a deep head it had fades where it was.
			RetireDeep(*Body);
			++Resets;
			if (Body->Bounds.bIsValid)
			{
				Sim.PendingErase.Add(Body->Bounds);
			}
			Sim.PendingErase.Add(Box);
			// Its motion estimate starts over: a jump's change of velocity is no acceleration of its bobbing.
			Body->bMotionPrimed = false;
			for (int32 Motion = 0; Motion < 3; ++Motion)
			{
				Body->MotionV2[Motion] = 0.0;
				Body->MotionA2[Motion] = 0.0;
			}
		}
		Body->Component = BodyInput.Component;
		Body->Sim = Sim.Key;
		Body->Bounds = Box;
		Body->Location = Location;
		Body->Rotation = Rotation;
		Body->Velocity = BodyInput.Velocity;
		Body->Spin = BodyInput.AngularVelocity;
		Body->Counter = BodyInput.TeleportCounter;
		Body->Hull = HullKey;
		Body->Water = BodyInput.WaterSource;
		Body->Stamp = TickStamp;
		UpdateMotion(*Body, BodyInput, DeltaSeconds);

		FPlaced& Place = Placed.AddDefaulted_GetRef();
		Place.Sim = SimIndex;
		Place.BodyId = BodyInput.Id;
		Place.bReset = bReset;
		Place.Transform = FTransform(Rotation, Location);
		Place.Bounds = Box;
		Place.WaterZ = BodyInput.WaterSource != EFluidNinjaWaveWater::None ? BodyInput.WaterZ : WaveNoWaterZ;
		// Wholly under the water it floats on: no static head of its own (its impulses still go in). With fnb.Waves.SubmergeRelease on, a
		// hull that came down through the surface lets go of the head it went under with gradually instead (below).
		const bool bNoWater = BodyInput.WaterSource == EFluidNinjaWaveWater::None;
		const double HullTop = WaveHullTop(*Hull, Place.Transform);
		Place.bSubmerged = bNoWater || HullTop < static_cast<double>(Place.WaterZ);
		// fnb.Waves.PlungeSource: the body's source is decided on the first frame any of its hull is in its water, from whether it sinks
		// then (WaveBodySinks: its resting draft, which the subsystem works out at the body's mass every frame, is below 0, as
		// FluidNinjaBuoyancy::DraftForMass found no draft its whole hull can carry it at, and its Buoyancy Multiplier does not float it after
		// all): a sinker presses the deep head; else today's static head, below, exactly as before. Not from bFloats below, which clamps the
		// draft to 0 first and so holds for a sinker too (it is why the bench's block got an 87 cm cap).
		const bool bSinks = WaveBodySinks(*Component);
		Body->bSinkerNow = bPlunge && bSinks;
		const double Entered = bPlunge && !Body->bPathLatched && !bNoWater ? static_cast<double>(Place.WaterZ) - WaveHullBottom(*Hull, Place.Transform) : 0.0;
		if (Entered > 0.0)
		{
			Body->bPathLatched = true;
			Body->bDeep = bSinks;
			// Coming into the water now: its lowest point no deeper than it moves in a frame (with the teleport test's slack) and two wave
			// cells, so its head grows from nothing, as the static head's would. Judged by the depth, not by whether the body was reset:
			// the water it is measured against may change (None to Captured, the far field's flat height coming within reach) on the very
			// frame it meets the water, which must not make a plunge's head fade in. A body put down, or first seen, already deeper fades in.
			Body->bDeepStartsWhole = Entered <= WaveTeleportSlack * FMath::Abs(BodyInput.Velocity.Z) * FMath::Max(static_cast<double>(DeltaSeconds), 0.0) + 2.0 * Sim.CellCm;
		}
		if (Body->bDeep)
		{
			// The deep head (UpdateDeep), from the hull's submerged part against the wave-free water; it starts whole as the hull comes in,
			// or with this frame's erase of all its water. No static head record, no graded release (the head is held through the event
			// instead) and no own field (below): the record and the release below are skipped for it.
			UpdateDeep(*Body, Sim, BodyInput, Input, Place.Transform, Place.WaterZ, Sim.bRestart || bClear, DeltaSeconds);
			if (Body->Deep.bValid)
			{
				Body->bDeepStartsWhole = false;
			}
		}
		// No pit deeper than half the hull is wide where it comes through the water (walls past 45°, more than the linear waves carry): a
		// log speared in at an angle pressed one 90 cm deep over its 40 cm waterline, which the water cannot follow, so the drawn surface
		// (η + P) stood up there as a glassy column a metre high. A floating body keeps at least a little over its resting draft (an upright
		// tall box floats deeper than half its width), so its static head keeps its whole displaced volume; a sinking ball's cap closes to
		// nothing as its top goes under. A wholly submerged hull has no waterline section (0 cm): its cap is not used.
		const float Draft = FMath::Max(Component->GetRestingDraft(), 0.f);
		const FVector3f HullExtent = Hull->BoundsMax - Hull->BoundsMin;
		const bool bFloats = Draft < 0.9f * HullExtent.GetMax();
		const double SectionWidth = Place.bSubmerged || Body->bDeep ? 0.0 : WaveSectionWidth(*Hull, Place.Transform, Place.WaterZ);
		const float MaxHead = static_cast<float>(FMath::Max3(0.5 * SectionWidth, bFloats ? 1.2 * Draft : 0.0, 1.0));
		// The graded release (fnb.Waves.SubmergeRelease). Dropped in the one tick its top went under, a plunged 15 t block's 87 cm cap (40 %
		// of its volume) vanished in 16.7 ms and the hollow it had pressed sprang back as a spike. Instead the hull keeps pressing the head
		// it went under with, times HeadScale, which falls by e^(−dt/τ) each frame: τ = √(w / g) is the time a hollow w wide takes to fill
		// (w = 174 cm: 0.42 s), so the water closes over the hull on its own time scale; below 1 % the record is dropped. w is the waterline
		// width it went under with, at least twice its cap (a hollow is never narrower than twice its depth: the cap's own rule; a rounded
		// or tilted hull's last section shrinks to nothing) and two wave cells (the narrowest hollow the grid holds). Coming up through the
		// surface again, it takes its head back the same way, at the τ of the waterline it has then, so surfacing is not a step either.
		// A reset (the water there is erased to B = 0) starts it at its full head if it comes through the surface, at none if it is under;
		// nothing is held across a reset, nor while the release or the displacement is off, so turning either on never replays a stale head.
		// A sinker on the deep path has neither (its head is held through the event): its release state stays clear.
		bool bHeld = false;
		if (!bSubmergeRelease || !bDisplacement || Body->bDeep)
		{
			Body->HeadScale = 1.f;
			Body->bPierced = false;
		}
		else if (bReset)
		{
			Body->HeadScale = Place.bSubmerged ? 0.f : 1.f;
			Body->bPierced = false;
		}
		else if (Place.bSubmerged)
		{
			// Under: the head it went under with fades. One that has not come through the surface since its reset has none to let go.
			if (Body->bPierced && !bNoWater)
			{
				const double Width = FMath::Max3(static_cast<double>(Body->PiercedWidthCm), 2.0 * static_cast<double>(Body->PiercedMaxHeadCm), 2.0 * Sim.CellCm);
				Body->HeadScale *= static_cast<float>(FMath::Exp(-ReleaseDt / FMath::Sqrt(Width / ReleaseGravity)));
				bHeld = Body->HeadScale >= WaveSubmergeReleaseFloor;
			}
			if (!bHeld)
			{
				Body->HeadScale = 0.f;
				Body->bPierced = false;
			}
		}
		else if (Body->HeadScale < 1.f)
		{
			// Through the surface again (or for the first time since a reset under water): the head comes back at this waterline's τ.
			const double Width = FMath::Max3(SectionWidth, 2.0 * static_cast<double>(MaxHead), 2.0 * Sim.CellCm);
			Body->HeadScale = 1.f - (1.f - Body->HeadScale) * static_cast<float>(FMath::Exp(-ReleaseDt / FMath::Sqrt(Width / ReleaseGravity)));
			if (Body->HeadScale > 1.f - WaveSubmergeReleaseFloor)
			{
				Body->HeadScale = 1.f;
			}
		}
		if (bSubmergeRelease && bDisplacement && !Place.bSubmerged && !Body->bDeep)
		{
			// The hull as it comes through the surface now: what a release starting next frame holds.
			Body->bPierced = true;
			Body->PiercedTopCm = static_cast<float>(HullTop - static_cast<double>(Place.WaterZ));
			Body->PiercedMaxHeadCm = MaxHead;
			Body->PiercedWidthCm = static_cast<float>(SectionWidth);
			Body->PiercedRotation = Rotation;
		}
		if (bDisplacement && (!Place.bSubmerged || bHeld) && !Body->bDeep)
		{
			Place.bRecord = true;
			Place.Hull = UsedHulls.AddUnique(BodyInput.Hull);
			Place.Record.Kind = 0;
			// P's multiplier: 1 with the release off, and while the hull comes through the surface at its full head.
			Place.Record.Extra = FVector4f(0.f, 0.f, Place.WaterZ, Body->HeadScale);
			if (bHeld)
			{
				// The released head: the hull rastered where it is across, but in the attitude it had when it last came through the
				// surface and lifted back to the height above the water its top had then, so every column keeps the head and the column
				// fade it went under with, whatever the hull's shape. Rastered where it is, a column whose top is under water lets its head
				// go within two cells (the raster's fade), which a sinking block falls in a few frames, long before τ; a longer fade instead
				// would press the whole footprint of a sinking ball, whose head had already closed to a small cap, a step up. Rastered in
				// its attitude now, a hull that pitches or tumbles under water would change its footprint and its columns' depths as fast as
				// it turns, not at τ: a log speared in end first that tips towards level would spread its ~40 × 40 cm footprint to
				// ~400 × 40 cm at up to the held cap within a few frames (a P rising faster than the water follows: the glass and spike
				// mechanism), and one that tips to 45° would leave a corner column piercing and the fade would empty the rest (the one-tick
				// drop again). Its box is the held pose's (the record's bounds: the raster's cell box and cull), and the body's bounds take
				// both boxes, so a reset or its removal next frame erases the water where the held head pressed too. Its own field still
				// holds it: turned about where it is, no point of the hull lies further from there than in its attitude now, and the field
				// is placed around where it is. It keeps its place across (not where it went under): the field follows the body, and a
				// hollow left behind could fall outside it, so B - B_self would no longer cancel there. Its cap is the one it went under
				// with (the section it has now is none), and it sheds no waterline foam: nothing of it pierces the surface.
				const FTransform HeldPose(Body->PiercedRotation, Location);
				const double Lift = static_cast<double>(Body->PiercedTopCm) - (WaveHullTop(*Hull, HeldPose) - static_cast<double>(Place.WaterZ));
				Place.Transform = FTransform(Body->PiercedRotation, Location + FVector(0.0, 0.0, Lift));
				Place.Bounds = WaveHullBox(*Hull, Place.Transform);
				Body->Bounds = Box + Place.Bounds;
				Place.Record.MaxHeadCm = Body->PiercedMaxHeadCm;
				Place.Record.SpeedCmS = 0.f;
			}
			else
			{
				Place.Record.MaxHeadCm = MaxHead;
				// The hull's speed through the water (horizontal, against the current under it): the foam's waterline shear (the source
				// texture's A, interface version 4; a pre-4 core ignores the channel).
				Place.Record.SpeedCmS = static_cast<float>((FVector2D(BodyInput.Velocity) - Component->GetReadout().FlowCmS).Size());
			}
		}
		// Its own field, its depth and its radiation damping: only for a body that feels the waves (κ above 0, its own scale times the
		// project's or the bench's) and is within fnb.Waves.SelfFieldDistanceCm of the camera (a tenth of hysteresis, so one at the line
		// does not start over every other frame). The others make waves and feel none (GetBodyFeel: Feel 0), and cost no field: the
		// fields' cost scales with the sum of their cells squared, and the far ones are most of it. Nothing is logged for it.
		const float Feel = FMath::Max(Component->WaveFeelScale, 0.f) * FMath::Max(Settings.WaveFeelScale, 0.f);
		bool bFar = false;
		if (Input.bHasCamera && WaveSelfFieldDistanceCm > 0.f)
		{
			const double Limit = static_cast<double>(WaveSelfFieldDistanceCm) * (Body->bSelf ? 1.0 : 0.9);
			bFar = FVector::DistSquared(Location, Input.Camera) > Limit * Limit;
		}
		bool bNewSelf = false;
		// A sinker on the deep path has none (Feel 0): its own waves would have to be its deep head's, which no self field steps. Its physics
		// step still feels the slam, the drag and FluidNinja's own surface.
		if (Feel <= 0.f || bFar || Body->bDeep)
		{
			ReleaseSelf(*Body);
		}
		else if (PlaceSelf(*Body, Sim, FVector2D(Location), BodyInput.Radius, Place.Shift, bNewSelf))
		{
			UpdateDepth(*Body, Sim, BodyInput, Input);
			if (Settings.bRadiationDamping || Settings.WaveAddedMassScale > 0.f)
			{
				UpdateRadiation(*Body, Sim, *Component);
				// The frequencies it moves at, for its status line (fnb.Buoyancy 2).
				if (UFluidNinjaBuoyancyComponent* Shown = BodyInput.Component.Get())
				{
					Shown->SetWaveMotion(Body->MotionOmega / (2.0 * UE_DOUBLE_PI));
				}
			}
			Place.bReset |= bNewSelf;
		}
	}
	// Bodies gone since last frame (destroyed, unregistered): the water where they were is cleared.
	for (auto It = Bodies.CreateIterator(); It; ++It)
	{
		if (It.Value().Stamp != TickStamp)
		{
			RemoveBody(It.Value());
			It.RemoveCurrent();
			++Leaving;
		}
	}
	// The deep heads whose bodies left or were put elsewhere: each fades where it is and is dropped once it presses nothing measurable. One
	// whose water is erased this frame (the bench's fresh ripples, its simulation starting over) goes at once, before this frame's sources
	// are built, so the erase sets η = −P without it and nothing is left behind (drop 1's fading head must not act as a sink under drop 2);
	// so does one whose simulation stopped.
	for (int32 Index = FadingDeep.Num() - 1; Index >= 0; --Index)
	{
		FDeepRecord& Fading = FadingDeep[Index];
		const FSimState* FadingSim = FindSim(Fading.Sim);
		if (!FadingSim || !FadingSim->bActive || FadingSim->bRestart || bClear || !StepDeepFade(Fading, ReleaseDt))
		{
			FadingDeep.RemoveAt(Index);
		}
	}

	// Player and registered humanoid pawns use this one shape calculation. Animated head/chest/hips spheres concentrate their volume
	// into small, bobbing columns and produced much taller crests than the player's tapered body during the same freestyle stroke.
	auto PawnBodyBounds = [](const FFluidNinjaWaveSwimmerInput& Body)
	{
		const double HalfLength = FMath::Max(0.95 * static_cast<double>(Body.HalfHeight), 1.0);
		return FBox2D(FVector2D(Body.Centre) - FVector2D(HalfLength), FVector2D(Body.Centre) + FVector2D(HalfLength));
	};
	auto PlacePawnBody = [&Settings](const FFluidNinjaWaveSwimmerInput& Body, FPlaced& Place)
	{
		const APawn* BodyPawn = Body.Pawn.Get();
		Place.Transform = FTransform(FRotator(0.0, BodyPawn ? BodyPawn->GetActorRotation().Yaw : 0.0, 0.0), Body.Centre);
		Place.WaterZ = Body.bHasWaterZ ? Body.WaterZ : WaveNoWaterZ;
		const double WaterZ = Body.bHasWaterZ ? static_cast<double>(Body.WaterZ) : Body.Centre.Z + Body.HalfHeight;
		const FFluidNinjaSwimmerBodyShape Shape = FFluidNinjaSwimmerBodyShape::Make(Body.Radius, Body.HalfHeight, Body.Centre.Z, WaterZ, Settings.SwimmerVolumeLitres);
		Place.Record.Kind = 2;
		Place.Record.Extra = FVector4f(static_cast<float>(Shape.HalfLength), static_cast<float>(Shape.HalfWidth), Place.WaterZ, Shape.WetScale);
		Place.Record.HalfThicknessCm = static_cast<float>(Shape.HalfThickness);
		Place.Record.SubmergeFadeCm = WaveSwimmerSubmergeFadeCm;
		Place.Record.SpeedCmS = static_cast<float>(FVector2D(Body.Velocity).Size());
		return Shape.WetScale;
	};

	// The swimmer: the local player's capsule reference, in the simulation around it. It is measured against the Surface Mirror where that
	// shows water under it, else the water the swim found this frame; with neither (on land) it has no static head. It has no own field
	// (nothing floats it on the waves).
	const APawn* SwimmingPawn = Input.Swimmer.Pawn.Get();
	FSimState* SwimmerSim = Settings.bSwimmerWaves && bDisplacement && Input.Swimmer.bValid && SwimmingPawn && Input.Swimmer.Radius > 0.f
		? SimAt(FVector2D(Input.Swimmer.Centre)) : nullptr;
	float SwimmerScale = 1.f;
	bool bSwimmerSent = false;
	if (SwimmerSim)
	{
		// The swimming body as a lying ellipsoid (the raster's kind 2): as long as the capsule is tall less its head's share, about 1.7 m,
		// as wide as the shoulders, about 45 cm, turned to the pawn's facing, and as thick as Swimmer Volume Litres makes it. A swimmer lies
		// along the water: the upright capsule packed the body's 76 L into a 70 cm disc, 20 cm deep with a vertical wall at its rim, and a
		// moving swimmer threw waves far too tall and spikes; the ellipsoid spreads the same volume over its length, deepest along its centre
		// line, and tapers to nothing at its rim. (At 76 L: semi-axes 85 x 23 cm and 9.4 cm thick each way.)
		const FBox2D SwimmerBox = PawnBodyBounds(Input.Swimmer);
		const EFluidNinjaWaveWater SwimmerWater = Input.Swimmer.bHasWaterZ ? EFluidNinjaWaveWater::Published : EFluidNinjaWaveWater::None;
		const bool bReset = !bSwimmerTracked || SwimmerPawn.Get() != SwimmingPawn || Swimmer.Water != SwimmerWater || Swimmer.Sim != SwimmerSim->Key
			|| SwimmerSim->bRestart || Jumped(Swimmer, Input.Swimmer.Centre, FQuat::Identity, Input.Swimmer.Velocity, FVector::ZeroVector, DeltaSeconds);
		if (bReset)
		{
			if (bSwimmerTracked && Swimmer.Bounds.bIsValid)
			{
				if (FSimState* Was = FindSim(Swimmer.Sim))
				{
					Was->PendingErase.Add(Swimmer.Bounds);
				}
			}
			SwimmerSim->PendingErase.Add(SwimmerBox);
		}
		FPlaced& Place = Placed.AddDefaulted_GetRef();
		Place.Sim = static_cast<int32>(SwimmerSim - Sims.GetData());
		Place.bRecord = true;
		Place.Bounds = SwimmerBox;
		// Wading, the body is not afloat: its legs displace a few litres, not the capsule's 70 cm column, and in a current the core carries
		// that column's hollow away while its head stays, so a wader stood in a glassy bell of P over the water. The head comes in only as
		// the water reaches the chest (75 % of the capsule's height, full from there up: swimming), from nothing at the waist (50 %); the
		// wading wakes at the feet draw the wader's disturbance.
		SwimmerScale = PlacePawnBody(Input.Swimmer, Place);
		Swimmer.Bounds = SwimmerBox;
		Swimmer.Location = Input.Swimmer.Centre;
		Swimmer.Velocity = Input.Swimmer.Velocity;
		Swimmer.Water = SwimmerWater;
		Swimmer.Sim = SwimmerSim->Key;
		SwimmerPawn = Input.Swimmer.Pawn;
		bSwimmerTracked = true;
		bSwimmerSent = true;
	}
	else if (bSwimmerTracked)
	{
		if (FSimState* Was = FindSim(Swimmer.Sim))
		{
			if (Was->bActive && Swimmer.Bounds.bIsValid)
			{
				Was->PendingErase.Add(Swimmer.Bounds);
			}
		}
		bSwimmerTracked = false;
		SwimmerPawn.Reset();
	}

	// Registered swimmers: opted-in humanoid pawns match the player; other swimmers retain their profile's individual spheres.
	TSet<TPair<FObjectKey, int32>> PuppetsSeen;
	int32 PuppetsSent = 0;
	for (const FFluidNinjaWaveSwimmerInput& Puppet : Input.Puppets)
	{
		FSimState* PuppetSim = Settings.bSwimmerWaves && bDisplacement && Puppet.bValid && Puppet.Source.IsValid() && Puppet.Radius > 0.f
			? SimAt(FVector2D(Puppet.Centre)) : nullptr;
		if (!PuppetSim)
		{
			continue;
		}
		const TPair<FObjectKey, int32> Key(FObjectKey(Puppet.Source.Get()), Puppet.Part);
		PuppetsSeen.Add(Key);
		FBodyState* State = Puppets.Find(Key);
		const bool bNew = State == nullptr;
		if (bNew)
		{
			State = &Puppets.Add(Key);
		}
		const bool bPawnBody = Puppet.Pawn.IsValid();
		const FBox2D PuppetBox = bPawnBody ? PawnBodyBounds(Puppet)
			: FBox2D(FVector2D(Puppet.Centre) - FVector2D(Puppet.Radius), FVector2D(Puppet.Centre) + FVector2D(Puppet.Radius));
		if (bNew || State->Sim != PuppetSim->Key || PuppetSim->bRestart || Jumped(*State, Puppet.Centre, FQuat::Identity, Puppet.Velocity, FVector::ZeroVector, DeltaSeconds))
		{
			if (!bNew && State->Bounds.bIsValid)
			{
				if (FSimState* Was = FindSim(State->Sim))
				{
					Was->PendingErase.Add(State->Bounds);
				}
			}
			PuppetSim->PendingErase.Add(PuppetBox);
		}
		FPlaced& Place = Placed.AddDefaulted_GetRef();
		Place.Sim = static_cast<int32>(PuppetSim - Sims.GetData());
		Place.bRecord = true;
		Place.Bounds = PuppetBox;
		if (bPawnBody)
		{
			PlacePawnBody(Puppet, Place);
		}
		else
		{
			Place.Transform = FTransform(Puppet.Centre);
			Place.WaterZ = Puppet.bHasWaterZ ? Puppet.WaterZ : WaveNoWaterZ;
			Place.Record.Kind = 1;
			Place.Record.Extra = FVector4f(Puppet.Radius, Puppet.HalfHeight, Place.WaterZ, 1.f);
			Place.Record.SpeedCmS = static_cast<float>(FVector2D(Puppet.Velocity).Size());
		}
		State->Bounds = PuppetBox;
		State->Location = Puppet.Centre;
		State->Velocity = Puppet.Velocity;
		State->Water = Puppet.bHasWaterZ ? EFluidNinjaWaveWater::Published : EFluidNinjaWaveWater::None;
		State->Sim = PuppetSim->Key;
		++PuppetsSent;
	}
	for (auto It = Puppets.CreateIterator(); It; ++It)
	{
		if (!PuppetsSeen.Contains(It.Key()))
		{
			if (FSimState* Was = FindSim(It.Value().Sim))
			{
				if (Was->bActive && It.Value().Bounds.bIsValid)
				{
					Was->PendingErase.Add(It.Value().Bounds);
				}
			}
			It.RemoveCurrent();
		}
	}

	// Each hull's triangles (one list, rebuilt only when the hulls in use change, and uploaded only then: the GPU keeps it).
	UpdateTriangleCache(UsedHulls);

	// The physics steps' impulses: into the pending impulses of each body's simulation, and its own field.
	TMap<int32, TArray<FVector4f>> OwnImpulses;
	int32 NumImpulses = 0;
	if (IsImpulseOn())
	{
		const float Scale = FMath::Max(Settings.WaveImpulseScale, 0.f);
		for (const FFluidNinjaWaveBodyImpulses& Pieces : Input.Impulses)
		{
			const FBodyState* Body = Bodies.Find(Pieces.Id);
			FSimState* Sim = Body ? FindSim(Body->Sim) : nullptr;
			if (!Sim || !Sim->bActive || Scale <= 0.f)
			{
				continue;
			}
			// A sinker sends none (fnb.Waves.PlungeSource): the impulses stood in for the displacement a lagging surface head missed in a
			// fast entry, and the deep head carries that displacement itself, so sending them too counts the entry twice at the surface (in
			// the port its troughs came out 35-58 % over Evers' with them, 28 of 54 measures; filtered to their depth, no better). Its
			// physics still feels every one of them. Left out from the frame it sinks, before its path is decided (the physics may meet the
			// water a frame before the waves see its hull in it), but only while its water is known: a sinker whose water stays unknown never
			// latches its path (that needs its hull seen in known water), and would otherwise send no waves at all.
			if (Body->bDeep || (Body->bSinkerNow && !Body->bPathLatched && Body->Water != EFluidNinjaWaveWater::None))
			{
				continue;
			}
			TArray<FVector4f>* Own = Body->bSelf ? &OwnImpulses.FindOrAdd(Pieces.Id) : nullptr;
			for (const FVector4f& Piece : Pieces.Pieces)
			{
				if (!FMath::IsFinite(Piece.Z) || Piece.Z == 0.f || !FMath::IsFinite(Piece.X) || !FMath::IsFinite(Piece.Y))
				{
					continue;
				}
				const FVector4f Scaled(Piece.X, Piece.Y, Piece.Z * Scale, Piece.W);
				Sim->PendingImpulses.Add(Scaled);
				if (Own)
				{
					Own->Add(Scaled);
				}
				++NumImpulses;
			}
		}
	}
	// The test bench's pokes, into the simulation each falls in.
	for (const FVector4& Poke : PendingPokes)
	{
		if (FSimState* Sim = SimAt(FVector2D(Poke.X, Poke.Y)))
		{
			Sim->PendingPokes.Add(Poke);
		}
	}
	PendingPokes.Reset();

	// The frame.
	FluidNinjaBuoyancyWaveSource::FFrame Frame;
	Frame.Gravity = FMath::Max(0.f, static_cast<float>(-World->GetGravityZ()));
	Frame.Density = Settings.WaterDensity * 1.0e-6f;   // kg/m³ to kg/cm³
	Frame.SmoothCm = FMath::Max(Settings.WaveSourceSmoothCm, 0.f);
	Frame.SpongeRate = WaveSelfSpongeRate;
	Frame.bControlled = Input.bControlled;
	Frame.ControlledHeight = Input.ControlledHeight;
	Frame.Triangles = CachedTriangles;
	Frame.TriangleSerial = TriangleSerial;
	Frame.WorldTime = Now;
	const bool bMirror = Input.Mirror.Target && Input.Mirror.Resolution > 0 && Input.Mirror.Texel > 0.0;
	const auto MirrorFor = [&Input, bMirror](const FVector2D& GridMin)
	{
		FluidNinjaBuoyancyWaveSource::FMirror Mirror;
		Mirror.bValid = bMirror;
		if (bMirror)
		{
			Mirror.Origin = FVector2f(static_cast<float>(Input.Mirror.Origin.X - GridMin.X), static_cast<float>(Input.Mirror.Origin.Y - GridMin.Y));
			Mirror.Texel = static_cast<float>(Input.Mirror.Texel);
			Mirror.Size = Input.Mirror.Resolution;
			Mirror.Top = static_cast<float>(Input.Mirror.Top);
			Mirror.Far = static_cast<float>(Input.Mirror.Far);
		}
		return Mirror;
	};
	const auto FinishRecord = [this, &UsedHulls](FPlaced& Place, FluidNinjaBuoyancyWaveSource::FBody& Record, const FVector2D& GridMin)
	{
		Record = Place.Record;
		WaveSetFrame(Record, Place.Transform.GetRotation(), Place.Transform.GetLocation(), GridMin);
		Record.Bounds = WaveRelativeBox(Place.Bounds, GridMin);
		if (Place.Hull != INDEX_NONE)
		{
			const FluidNinjaBuoyancy::FHull& Surface = *UsedHulls[Place.Hull];
			Record.FirstTriangle = CachedFirstTriangle.FindChecked(reinterpret_cast<UPTRINT>(&Surface));
			Record.NumTriangles = static_cast<uint32>(Surface.Triangles.Num());
		}
	};

	// Each simulation's write, when a tick will read it: Niagara first, only on frames whose ticks are counted already (a frame with none
	// keeps everything pending for the next); Niagara after, every frame (BeginFrame writes again what a frame with none did not consume).
	TArray<TPair<FTextureRenderTargetResource*, int32>> TargetResources;   // per source: its target's resource
	int32 Written = 0;
	// The deep heads alive this frame (sinkers' and fading), for the status line.
	DeepHeadsSent = FadingDeep.Num();
	for (const TPair<int32, FBodyState>& Entry : Bodies)
	{
		DeepHeadsSent += Entry.Value.bDeep && Entry.Value.Deep.bValid ? 1 : 0;
	}
	for (int32 SimIndex = 0; SimIndex < Sims.Num(); ++SimIndex)
	{
		FSimState& Sim = Sims[SimIndex];
		UTextureRenderTarget2D* Target = Sim.Target.Get();
		if (!Sim.bActive || !Target)
		{
			continue;
		}
		const bool bWrite = !Sim.bNiagaraFirst || Sim.Delta > 0;
		Sim.FixedAge += FMath::Max(static_cast<double>(DeltaSeconds), 0.0);
		const double Budget = FMath::Fmod(Sim.FixedAge - FMath::Max(static_cast<double>(DeltaSeconds), 0.0), static_cast<double>(Sim.TickSeconds)) + FMath::Max(static_cast<double>(DeltaSeconds), 0.0);
		const int32 Predicted = FMath::FloorToInt32(Budget / Sim.TickSeconds);
		if (!bWrite)
		{
			continue;
		}
		FluidNinjaBuoyancyWaveSource::FSource& Source = Frame.Sources.AddDefaulted_GetRef();
		Source.Cells = Sim.Cells;
		Source.CellCm = static_cast<float>(Sim.CellCm);
		Sim.Serial = (Sim.Serial + 1) & 0xFFFFFF;
		Source.Header0 = FVector4f(static_cast<float>(Sim.Origin.X), static_cast<float>(Sim.Origin.Y), static_cast<float>(Sim.CellCm), static_cast<float>(Sim.Serial));
		Source.Header1 = FVector4f(static_cast<float>(Sim.bNiagaraFirst ? Sim.Delta : Predicted), 0.f, 0.f, 0.f);
		Source.Mirror = MirrorFor(Sim.Origin);
		for (FPlaced& Place : Placed)
		{
			if (Place.Sim == SimIndex && Place.bRecord)
			{
				FinishRecord(Place, Source.Bodies.AddDefaulted_GetRef(), Sim.Origin);
			}
		}
		// The sinkers' deep heads in it, live (asleep ones included) and fading, added to P on the GPU (none: P is the raster's alone).
		Source.DeepKey = Sim.DeepKey;
		for (const TPair<int32, FBodyState>& Entry : Bodies)
		{
			FluidNinjaBuoyancyWaveSource::FDeepHead Head;
			if (Entry.Value.bDeep && Entry.Value.Deep.Sim == Sim.Key && MakeDeepHead(Entry.Value.Deep, Sim.Origin, Head))
			{
				Source.DeepHeads.Add(Head);
			}
		}
		for (const FDeepRecord& Fading : FadingDeep)
		{
			FluidNinjaBuoyancyWaveSource::FDeepHead Head;
			if (Fading.Sim == Sim.Key && MakeDeepHead(Fading, Sim.Origin, Head))
			{
				Source.DeepHeads.Add(Head);
			}
		}
		Sim.LastWriteDeep = Source.DeepHeads.Num();
		Source.Impulses.Reserve(Sim.PendingImpulses.Num());
		for (const FVector4f& Piece : Sim.PendingImpulses)
		{
			Source.Impulses.Add(FVector4f(static_cast<float>(Piece.X - Sim.Origin.X), static_cast<float>(Piece.Y - Sim.Origin.Y), Piece.Z, Piece.W));
		}
		for (const FVector4& Poke : Sim.PendingPokes)
		{
			Source.Pokes.Add(FVector4f(static_cast<float>(Poke.X - Sim.Origin.X), static_cast<float>(Poke.Y - Sim.Origin.Y), static_cast<float>(Poke.Z), static_cast<float>(Poke.W)));
		}
		for (const FBox2D& Box : Sim.PendingErase)
		{
			FIntVector4 CellBox;
			if (WaveCellBox(Box, Sim.Origin, Sim.CellCm, Sim.Cells, CellBox))
			{
				Source.EraseBoxes.Add(CellBox);
			}
		}
		Source.bEraseAll = Sim.bPendingEraseAll || Source.EraseBoxes.Num() > 256;
		Sim.LastWriteBodies = Source.Bodies.Num();
		Sim.LastWriteImpulses = Source.Impulses.Num();
		if (!Sim.bNiagaraFirst)
		{
			// Kept until the next frame says whether a tick consumed it.
			Sim.LastImpulses = MoveTemp(Sim.PendingImpulses);
			Sim.LastPokes = MoveTemp(Sim.PendingPokes);
			Sim.LastErase = MoveTemp(Sim.PendingErase);
			Sim.bLastEraseAll = Sim.bPendingEraseAll;
			Sim.bAwaitingConfirm = true;
		}
		Sim.PendingImpulses.Reset();
		Sim.PendingPokes.Reset();
		Sim.PendingErase.Reset();
		Sim.bPendingEraseAll = false;
		TargetResources.Emplace(Target->GameThread_GetRenderTargetResource(), Frame.Sources.Num() - 1);
		++Written;
	}

	// Each body's own field: recentred or reset, this frame's static head and impulses, and (Niagara first) stepped with this frame's ticks.
	int32 Fields = 0;
	int64 FieldCells = 0;
	TArray<FTextureRenderTargetResource*> SelfAuxResources;   // per self field: its simulation's Wave Aux target
	for (FPlaced& Place : Placed)
	{
		FBodyState* Body = Place.BodyId != 0 ? Bodies.Find(Place.BodyId) : nullptr;
		if (!Body || !Body->bSelf || !Sims.IsValidIndex(Place.Sim))
		{
			continue;
		}
		const FSimState& Sim = Sims[Place.Sim];
		const FVector2D SelfMin(Sim.Phase.X + static_cast<double>(Body->OriginCellX) * Sim.CellCm, Sim.Phase.Y + static_cast<double>(Body->OriginCellY) * Sim.CellCm);
		FluidNinjaBuoyancyWaveSource::FSelf& Self = Frame.Selves.AddDefaulted_GetRef();
		Self.Id = Body->Id;
		Self.Cells = Body->SelfCells;
		Self.CellCm = static_cast<float>(Sim.CellCm);
		Self.bSources = true;
		Self.Shift = Place.Shift;
		Self.bReset = Place.bReset;
		Self.bHasBody = Place.bRecord;
		if (Place.bRecord)
		{
			FinishRecord(Place, Self.Body, SelfMin);
		}
		Self.Mirror = MirrorFor(SelfMin);
		if (const TArray<FVector4f>* Own = OwnImpulses.Find(Body->Id))
		{
			Self.Impulses.Reserve(Own->Num());
			for (const FVector4f& Piece : *Own)
			{
				Self.Impulses.Add(FVector4f(static_cast<float>(Piece.X - SelfMin.X), static_cast<float>(Piece.Y - SelfMin.Y), Piece.Z, Piece.W));
			}
		}
		Self.StepTicks = Sim.bNiagaraFirst ? Sim.Delta : 0;
		SetSelfStep(Self, Sim, *Body);
		SelfAuxResources.Add(Sim.Aux.IsValid() ? Sim.Aux->GameThread_GetRenderTargetResource() : nullptr);
		// Its tile holds B_self from this frame on (reset writes 0 at once), so the next gather may read it.
		Body->bSelfReady = true;
		++Fields;
		FieldCells += static_cast<int64>(Body->SelfCells) * Body->SelfCells;
	}
	Frame.Released = MoveTemp(ReleasedSelves);
	ReleasedSelves.Reset();
	Frame.ReleasedDeep = MoveTemp(ReleasedDeepKeys);
	ReleasedDeepKeys.Reset();
	for (FSimState& Sim : Sims)
	{
		Sim.bRestart = false;
	}
	// The self-field pools' sizes as the render thread keeps them, for the memory report: a size's pool holds as many slots as it ever held
	// fields at once, until the last field of that size is released and it goes.
	{
		int32 Alive[4] = {0, 0, 0, 0};
		for (const TPair<int32, FBodyState>& Entry : Bodies)
		{
			const FBodyState& Body = Entry.Value;
			if (Body.bSelf && Body.SelfCells >= FluidNinjaBuoyancyWaveSource::MinSelfCells)
			{
				const int32 Size = static_cast<int32>(FMath::FloorLog2(static_cast<uint32>(Body.SelfCells))) - static_cast<int32>(FMath::FloorLog2(static_cast<uint32>(FluidNinjaBuoyancyWaveSource::MinSelfCells)));
				if (Size >= 0 && Size < 4)
				{
					++Alive[Size];
				}
			}
		}
		for (int32 Size = 0; Size < 4; ++Size)
		{
			SelfPoolPeak[Size] = Alive[Size] > 0 ? FMath::Max(SelfPoolPeak[Size], Alive[Size]) : 0;
		}
	}

	// The test bench's probes (each in the simulation it falls in) and the window's stats.
	TArray<FTextureRenderTargetResource*> ProbeResources;
	if (bProbesChanged)
	{
		ProbesSince = Now;   // readings stamped from this frame on are of these probes
		bProbesChanged = false;
	}
	if (bStatsChanged)
	{
		StatsSince = Now;
		bStatsChanged = false;
	}
	if (!ProbePoints.IsEmpty())
	{
		Frame.NumProbes = FMath::Min(ProbePoints.Num(), WaveMaxProbes);
		for (int32 Index = 0; Index < Frame.NumProbes; ++Index)
		{
			const FSimState* Sim = SimAt(ProbePoints[Index]);
			UTextureRenderTarget2D* Pressure = Sim ? Sim->Pressure.Get() : nullptr;
			if (!Pressure)
			{
				continue;   // outside every wave simulation: reads 0
			}
			FluidNinjaBuoyancyWaveSource::FProbeSet* Set = nullptr;
			for (int32 Existing = 0; Existing < Frame.Probes.Num(); ++Existing)
			{
				if (ProbeResources[Existing] == Pressure->GameThread_GetRenderTargetResource())
				{
					Set = &Frame.Probes[Existing];
				}
			}
			if (!Set)
			{
				// The probes read B on the pressure buffer's texels (the core exports it there whatever the wave grid).
				Set = &Frame.Probes.AddDefaulted_GetRef();
				Set->Cells = Sim->PressureCells;
				Set->CellCm = static_cast<float>(Sim->PressureCellCm);
				ProbeResources.Add(Pressure->GameThread_GetRenderTargetResource());
			}
			Set->Points.Add(FVector2f(static_cast<float>(ProbePoints[Index].X - Sim->Origin.X), static_cast<float>(ProbePoints[Index].Y - Sim->Origin.Y)));
			Set->Slots.Add(Index);
		}
	}
	FTextureRenderTargetResource* StatsResource = nullptr;
	if (bStatsWanted)
	{
		if (const FSimState* Sim = BenchSim())
		{
			if (UTextureRenderTarget2D* Pressure = Sim->Pressure.Get())
			{
				// The stats sum B and P over the pressure buffer's texels, at their area.
				Frame.bStats = true;
				Frame.StatsCells = Sim->PressureCells;
				Frame.StatsCellCm = static_cast<float>(Sim->PressureCellCm);
				StatsResource = Pressure->GameThread_GetRenderTargetResource();
			}
		}
	}

	// The render thread takes the textures from their resources (as the gather does) and runs the frame on the persistent state.
	FTextureRenderTargetResource* MirrorResource = bMirror ? Input.Mirror.Target->GameThread_GetRenderTargetResource() : nullptr;
	FTextureRenderTargetResource* AtlasResource = SelfAtlas ? SelfAtlas->GameThread_GetRenderTargetResource() : nullptr;
	ENQUEUE_RENDER_COMMAND(FluidNinjaBuoyancyWavesFrame)(
		[RenderState = GpuState, RenderFrame = MoveTemp(Frame), TargetResources, MirrorResource, AtlasResource, ProbeResources, StatsResource, SelfAuxResources](FRHICommandListImmediate& RHICmdList) mutable
		{
			for (int32 Index = 0; Index < RenderFrame.Selves.Num() && Index < SelfAuxResources.Num(); ++Index)
			{
				RenderFrame.Selves[Index].Aux = SelfAuxResources[Index] ? SelfAuxResources[Index]->GetRenderTargetTexture().GetReference() : nullptr;
			}
			for (const TPair<FTextureRenderTargetResource*, int32>& Entry : TargetResources)
			{
				if (RenderFrame.Sources.IsValidIndex(Entry.Value))
				{
					RenderFrame.Sources[Entry.Value].Target = Entry.Key ? Entry.Key->GetRenderTargetTexture().GetReference() : nullptr;
				}
			}
			RenderFrame.Mirror = MirrorResource ? MirrorResource->GetRenderTargetTexture().GetReference() : nullptr;
			RenderFrame.Atlas = AtlasResource ? AtlasResource->GetRenderTargetTexture().GetReference() : nullptr;
			for (int32 Index = 0; Index < RenderFrame.Probes.Num(); ++Index)
			{
				FTextureRenderTargetResource* ProbeResource = ProbeResources.IsValidIndex(Index) ? ProbeResources[Index] : nullptr;
				RenderFrame.Probes[Index].Pressure = ProbeResource ? ProbeResource->GetRenderTargetTexture().GetReference() : nullptr;
			}
			RenderFrame.StatsPressure = StatsResource ? StatsResource->GetRenderTargetTexture().GetReference() : nullptr;
			FluidNinjaBuoyancyWaveSource::Enqueue(RHICmdList, *RenderState, RenderFrame);
		});

	// The frame's figures for stat FluidNinjaBuoyancy.
	int32 ActiveSims = 0;
	for (const FSimState& Sim : Sims)
	{
		ActiveSims += Sim.bActive ? 1 : 0;
	}
	SET_DWORD_STAT(STAT_FNB_SelfFields, Fields);
	SET_DWORD_STAT(STAT_FNB_SelfCells, static_cast<int32>(FMath::Min<int64>(FieldCells, MAX_int32)));
	SET_DWORD_STAT(STAT_FNB_ImpulsePieces, NumImpulses);
	SET_DWORD_STAT(STAT_FNB_WaveSimsActive, ActiveSims);
	// And for the CSV profiler (category FNB), one value a frame. The self cells go in as int32 like the stat (the CSV takes no int64).
	// The waves' GPU passes have no figure the game thread can read (their GPU stats exist only for stat gpu and the engine's own GPU
	// CSV columns), so no GPU time is recorded here.
	CSV_CUSTOM_STAT(FNB, SelfFields, Fields, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(FNB, SelfCells, static_cast<int32>(FMath::Min<int64>(FieldCells, MAX_int32)), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(FNB, ImpulsePieces, NumImpulses, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(FNB, WaveSimsActive, ActiveSims, ECsvCustomStatOp::Set);

	// Status: built every frame only while something shows it (the debug view, the test bench's panel), else about once a second; the
	// readers get the last build in between.
	static const IConsoleVariable* TestHud = IConsoleManager::Get().FindConsoleVariable(TEXT("fnb.TestHUD"));
	const bool bShown = UFluidNinjaBuoyancySettings::IsDebugView() || (TestHud && TestHud->GetInt() != 0);
	if (bShown || Now >= NextStatusBuild)
	{
		NextStatusBuild = Now + 1.0;
		BuildStatusLine(Placed.Num() - (bSwimmerSent ? 1 : 0) - PuppetsSent, Resets, Leaving, Fields, NumImpulses, Written, bSwimmerSent, SwimmerScale, PuppetsSent, bDisplacement);
	}
}

void UFluidNinjaBuoyancyWaves::BuildStatusLine(int32 NumBodies, int32 Resets, int32 Leaving, int32 Fields, int32 NumImpulses, int32 Written, bool bSwimmerSent, float SwimmerScale,
	int32 PuppetsSent, bool bDisplacement)
{
	using namespace FluidNinjaBuoyancyWaveRuntime;
	FString SimText;
	for (const FSimState& Sim : Sims)
	{
		if (!Sim.bActive)
		{
			SimText += FString::Printf(TEXT("; %s: none%s"), *Sim.Name, !Sim.bEnabled ? (Sim.bFromComponent ? TEXT(" (its Wave Sim component: Waves off)") : TEXT(" (Wave Sims: off)"))
				: (Sim.bSuppressed ? *FString::Printf(TEXT(" (fnb.Waves.MaxActiveSims %d: it ranks %d, %.0f m from the camera)"), WaveMaxActiveSims, Sim.RankPlace, Sim.RankDistanceCm * 0.01)
				: (!Sim.bHasWaveParameter ? TEXT(" (stock NinjaLiveCore: no User.WaveSimEnabled)") : (!Sim.bHasAuxParameter ? TEXT(" (older FluidNinja Waves core: no User.WaveAuxTarget)")
				: (Sim.InterfaceVersion < WaveRequiredInterfaceVersion ? *FString::Printf(TEXT(" (core build %d, interface %d: this plugin needs interface %d, core build %d or later)"), Sim.CoreBuild, Sim.InterfaceVersion, WaveRequiredInterfaceVersion, WaveRequiredCoreBuild)
				: (Sim.bWaitingForPressure ? TEXT(" (waiting for its core to size its pressure buffer)") : TEXT("")))))));
			continue;
		}
		// The grid: its cells and cell, the scale-down in use (and the one asked for, when the core cannot give it).
		const FString ScaleText = Sim.ScaleDown == Sim.ScaleDownWanted ? FString::Printf(TEXT("scale-down %d"), Sim.ScaleDown)
			: FString::Printf(TEXT("scale-down %d, %d asked: interface %d core"), Sim.ScaleDown, Sim.ScaleDownWanted, Sim.InterfaceVersion);
		// The foam: running, off (the setting or fnb.Waves.Foam), or not there (a core before interface 4).
		const TCHAR* FoamText = Sim.InterfaceVersion >= WaveFoamInterfaceVersion ? (Sim.bFoam ? TEXT(", foam") : TEXT(", foam off"))
			: TEXT(", no foam (pre-4 core)");
		// The k = 0 rates it runs at (UpdateSim): the core's mean relax, the own fields', and the core's <v> pin (interface 5, walled windows
		// only). After the tick count, and with no " tick" or "; " in it: the test bench reads the tick count as the first " tick" after the
		// grid (CoreTicksOf) and "mean relax core R /s" from here (CoreMeanRelaxOf), within this simulation's part up to the next "; ".
		const FString VPinText = Sim.InterfaceVersion < WaveVPinInterfaceVersion ? FString(TEXT("no v pin (pre-5 core)"))
			: (Sim.bEdgeOpen ? FString(TEXT("no v pin (open edge)")) : (Sim.bVPin ? FString::Printf(TEXT("v pin %.1f /s"), Sim.VMeanRelax) : FString(TEXT("v pin off"))));
		const FString RelaxText = FString::Printf(TEXT(", mean relax core %.2f /s, self %.2f /s, %s"), Sim.CoreMeanRelax, Sim.SelfMeanRelax, *VPinText);
		SimText += FString::Printf(TEXT("; %s%s: core build %d, interface %d, %d x %.2f cm (%s of %d pressure cells), depth %.0f cm, %s edge, normal scale %.3f%s, %d tick%s this frame (Niagara %s FluidNinja Buoyancy), last write %d bodies %d impulses%s%s"),
			*Sim.Name, Sim.bFromComponent ? TEXT(" (component)") : TEXT(""), Sim.CoreBuild, Sim.InterfaceVersion, Sim.Cells.X, Sim.CellCm, *ScaleText, Sim.PressureCells.X, Sim.DepthCm,
			Sim.bEdgeOpen ? TEXT("open") : *FString::Printf(TEXT("walled (absorbing %.2f/s over %.0f cm)"), Sim.WallAbsorption, Sim.WallBandCm), Sim.NormalScale, FoamText, Sim.Delta, Sim.Delta == 1 ? TEXT("") : TEXT("s"),
			Sim.bNiagaraFirst ? TEXT("before") : TEXT("after"), Sim.LastWriteBodies, Sim.LastWriteImpulses, *RelaxText,
			Sim.bPressureReady ? TEXT("") : TEXT(" (its pressure buffer is not RGBA16F: B is not read, the bodies feel no waves)"));
	}
	int32 Felt = 0;
	for (const TPair<int32, FBodyState>& Entry : Bodies)
	{
		Felt += Entry.Value.bSelfReady ? 1 : 0;
	}
	TArray<FFluidNinjaWaveMemoryItem> Memory;
	ReportMemory(Memory);
	double MegaBytes = 0.0;
	for (const FFluidNinjaWaveMemoryItem& Item : Memory)
	{
		MegaBytes += Item.MegaBytes;
	}
	StatusLine = FString::Printf(TEXT("FluidNinja Buoyancy waves: %d bodies (%d reset, %d leaving), %d own fields (%d feel the waves) in a %d atlas, %d impulse pieces, %d writes, %.1f MB of targets%s%s%s%s%s"),
		NumBodies, Resets, Leaving, Fields, Felt, AtlasSize, NumImpulses, Written, MegaBytes,
		UnhiddenSurfaces > 0 ? *FString::Printf(TEXT(", ERROR: %d captured surface%s could not hide their waves from the Surface Mirror (see the log)"), UnhiddenSurfaces, UnhiddenSurfaces == 1 ? TEXT("") : TEXT("s")) : TEXT(""),
		*(FString(bSwimmerSent ? *FString::Printf(TEXT(", swimmer (static head x%.2f)"), SwimmerScale) : TEXT(""))
			+ (PuppetsSent > 0 ? FString::Printf(TEXT(", %d registered swimmer source%s"), PuppetsSent, PuppetsSent == 1 ? TEXT("") : TEXT("s")) : FString())
			+ (DeepHeadsSent > 0 ? FString::Printf(TEXT(", %d deep volume head%s (sinkers and fading ones, fnb.Waves.PlungeSource)"), DeepHeadsSent, DeepHeadsSent == 1 ? TEXT("") : TEXT("s")) : FString())),
		bDisplacement ? TEXT("") : TEXT(", static head off"), IsImpulseOn() ? TEXT("") : TEXT(", impulses off"),
		SimText.IsEmpty() ? TEXT("; no FluidNinja simulation") : *SimText);
}

void UFluidNinjaBuoyancyWaves::ReportMemory(TArray<FFluidNinjaWaveMemoryItem>& OutItems) const
{
	const auto Add = [&OutItems](const FString& Name, const FIntPoint& Size, const TCHAR* Format, double Bytes)
	{
		FFluidNinjaWaveMemoryItem& Item = OutItems.AddDefaulted_GetRef();
		Item.Name = Name;
		Item.Size = Size;
		Item.Format = Format;
		Item.MegaBytes = Bytes / (1024.0 * 1024.0);
	};
	// Each simulation's targets (16 bytes a texel): kept while the simulation is known, running or not.
	for (const FSimState& Sim : Sims)
	{
		if (const UTextureRenderTarget2D* Target = Sim.Target.Get())
		{
			Add(Sim.Name + TEXT(": wave source (User.WaveSourceTexture)"), FIntPoint(Target->SizeX, Target->SizeY), TEXT("RGBA32F"), 16.0 * Target->SizeX * Target->SizeY);
		}
		if (const UTextureRenderTarget2D* Aux = Sim.Aux.Get())
		{
			Add(Sim.Name + TEXT(": Wave Aux (User.WaveAuxTarget)"), FIntPoint(Aux->SizeX, Aux->SizeY), TEXT("RGBA32F"), 16.0 * Aux->SizeX * Aux->SizeY);
		}
		if (const UTextureRenderTarget2D* FoamTarget = Sim.Foam.Get())
		{
			Add(Sim.Name + TEXT(": foam (User.WaveFoamTarget)"), FIntPoint(FoamTarget->SizeX, FoamTarget->SizeY), TEXT("R16F"), 2.0 * FoamTarget->SizeX * FoamTarget->SizeY);
		}
		// The sinkers' summed deep heads (a uint per wave cell), kept by the render thread while the simulation has any.
		if (Sim.LastWriteDeep > 0)
		{
			Add(Sim.Name + TEXT(": deep heads' sum (sinkers, fnb.Waves.PlungeSource)"), Sim.Cells, TEXT("uint"), 4.0 * Sim.Cells.X * Sim.Cells.Y);
		}
	}
	// The self-field atlas (2 bytes a texel) and the pools behind it: per size, the render thread holds max(4, the peak number of fields of
	// that size rounded up to a power of two) slots of N² cells, each cell (η, v) in two floats, P's bits and the pending impulse (16 bytes).
	if (SelfAtlas && AtlasSize > 0)
	{
		Add(TEXT("self-field atlas (B_self tiles)"), FIntPoint(AtlasSize, AtlasSize), TEXT("R16F"), 2.0 * AtlasSize * AtlasSize);
	}
	for (int32 Size = 0; Size < 4; ++Size)
	{
		if (SelfPoolPeak[Size] > 0)
		{
			const int32 N = FluidNinjaBuoyancyWaveSource::MinSelfCells << Size;
			const int32 Slots = FMath::Max(4, static_cast<int32>(FMath::RoundUpToPowerOfTwo(static_cast<uint32>(SelfPoolPeak[Size]))));
			Add(FString::Printf(TEXT("self-field pool, %d slots of %d x %d cells (state, head, impulse)"), Slots, N, N), FIntPoint(N, N), TEXT("float2+2 uint"), 16.0 * static_cast<double>(Slots) * N * N);
		}
	}
	// The bench's readback rings, four buffers each, while probes or stats are wanted (allocated on first use and kept).
	if (!ProbePoints.IsEmpty())
	{
		const int32 Count = FMath::Min(ProbePoints.Num(), FluidNinjaBuoyancyWaveRuntime::WaveMaxProbes);
		Add(FString::Printf(TEXT("probe readbacks, 4 x %d floats"), Count), FIntPoint(Count, 4), TEXT("float"), 4.0 * 4.0 * Count);
	}
	if (bStatsWanted)
	{
		if (const FSimState* Sim = BenchSim())
		{
			const FIntPoint Groups(FMath::DivideAndRoundUp(Sim->PressureCells.X, 16), FMath::DivideAndRoundUp(Sim->PressureCells.Y, 16));
			Add(FString::Printf(TEXT("stats readbacks, 4 x %d partial sums"), Groups.X * Groups.Y), FIntPoint(Groups.X, Groups.Y * 4), TEXT("float4"), 16.0 * 4.0 * Groups.X * Groups.Y);
		}
	}
}

const UFluidNinjaBuoyancyWaves::FSimState* UFluidNinjaBuoyancyWaves::BenchSim() const
{
	const FSimState* Sim = FindSim(BenchSimKey);
	return Sim && Sim->bActive ? Sim : nullptr;
}

FObjectKey UFluidNinjaBuoyancyWaves::PickBenchSim() const
{
	FObjectKey Picked;
	double PickedArea = TNumericLimits<double>::Max();
	for (const FSimState& Sim : Sims)
	{
		if (!Sim.bActive)
		{
			continue;
		}
		const auto Holds = [&Sim](const FVector2D& Point)
		{
			return FMath::Abs(Point.X - Sim.Centre.X) <= Sim.SizeCm * 0.5 && FMath::Abs(Point.Y - Sim.Centre.Y) <= Sim.SizeCm * 0.5;
		};
		if (bBenchFocus && !Holds(BenchFocus))
		{
			continue;
		}
		const bool bFirst = bBenchFocus || (bBenchCamera && Holds(BenchCamera));
		const double Area = bFirst ? Sim.SizeCm * Sim.SizeCm : 1e30 + Sim.SizeCm * Sim.SizeCm;
		if (Area < PickedArea)
		{
			PickedArea = Area;
			Picked = Sim.Key;
		}
	}
	return Picked;
}

void UFluidNinjaBuoyancyWaves::SetBenchFocus(const FVector2D& WorldXY)
{
	BenchFocus = WorldXY;
	bBenchFocus = true;
	BenchSimKey = PickBenchSim();
}

void UFluidNinjaBuoyancyWaves::ClearBenchFocus()
{
	bBenchFocus = false;
	BenchSimKey = PickBenchSim();
}

void UFluidNinjaBuoyancyWaves::GetBenchWindows(TArray<FBox2D>& Out) const
{
	Out.Reset();
	for (const FSimState& Sim : Sims)
	{
		if (Sim.bActive)
		{
			Out.Add(FBox2D(Sim.Origin, Sim.Origin + FVector2D(Sim.SizeCm)));
		}
	}
	Out.Sort([](const FBox2D& A, const FBox2D& B) { return A.GetArea() < B.GetArea(); });
}

bool UFluidNinjaBuoyancyWaves::IsBenchEdgeOpen() const
{
	const FSimState* Sim = BenchSim();
	return Sim ? Sim->bEdgeOpen : true;
}

bool UFluidNinjaBuoyancyWaves::ReadBenchAux(TArray<FLinearColor>& OutCells, FIntPoint& OutSize) const
{
	OutCells.Reset();
	OutSize = FIntPoint::ZeroValue;
	const FSimState* Sim = BenchSim();
	UTextureRenderTarget2D* Aux = Sim ? Sim->Aux.Get() : nullptr;
	FTextureRenderTargetResource* Resource = Aux ? Aux->GameThread_GetRenderTargetResource() : nullptr;
	if (!Resource || Aux->SizeX != Sim->Cells.X || Aux->SizeY != Sim->Cells.Y)
	{
		return false;
	}
	// RCM_MinMax: the float values as they are (the default rescales a float target into 0..1).
	if (!Resource->ReadLinearColorPixels(OutCells, FReadSurfaceDataFlags(RCM_MinMax, CubeFace_MAX)) || OutCells.Num() != Sim->Cells.X * Sim->Cells.Y)
	{
		OutCells.Reset();
		return false;
	}
	OutSize = Sim->Cells;
	return true;
}

void UFluidNinjaBuoyancyWaves::Poke(const FVector2D& WorldXY, float HeightCm, float SigmaCm)
{
	PendingPokes.Add(FVector4(WorldXY.X, WorldXY.Y, HeightCm, FMath::Max(SigmaCm, 0.1f)));
}

void UFluidNinjaBuoyancyWaves::SetProbes(const TArray<FVector2D>& WorldPoints)
{
	ProbePoints = WorldPoints;
	if (ProbePoints.Num() > FluidNinjaBuoyancyWaveRuntime::WaveMaxProbes)
	{
		UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("FluidNinja Buoyancy waves: %d probes asked for; the first %d are read."), ProbePoints.Num(), FluidNinjaBuoyancyWaveRuntime::WaveMaxProbes);
		ProbePoints.SetNum(FluidNinjaBuoyancyWaveRuntime::WaveMaxProbes);
	}
	bProbesChanged = true;
	ProbesSince = TNumericLimits<double>::Max();
}

bool UFluidNinjaBuoyancyWaves::GetProbeResults(double& OutTime, TArray<float>& OutHeights) const
{
	if (!GpuState || ProbePoints.IsEmpty())
	{
		return false;
	}
	const FluidNinjaBuoyancyWaveSource::FResults Latest = GpuState->GetResults();
	if (Latest.ProbeTime < 0.0 || Latest.ProbeTime < ProbesSince || Latest.ProbeHeights.Num() != ProbePoints.Num())
	{
		return false;
	}
	OutTime = Latest.ProbeTime;
	OutHeights = Latest.ProbeHeights;
	return true;
}

void UFluidNinjaBuoyancyWaves::RequestStats(bool bOn)
{
	if (bOn && !bStatsWanted)
	{
		bStatsChanged = true;
		StatsSince = TNumericLimits<double>::Max();
	}
	bStatsWanted = bOn;
}

bool UFluidNinjaBuoyancyWaves::GetStats(double& OutTime, double& OutVolumeCm3, double& OutPotentialJ, double& OutKineticJ, float& OutMaxAbsCm) const
{
	float KineticDepthCm = 0.f;
	return GetStats(OutTime, OutVolumeCm3, OutPotentialJ, OutKineticJ, OutMaxAbsCm, KineticDepthCm);
}

bool UFluidNinjaBuoyancyWaves::GetStats(double& OutTime, double& OutVolumeCm3, double& OutPotentialJ, double& OutKineticJ, float& OutMaxAbsCm, float& OutKineticDepthCm) const
{
	if (!GpuState)
	{
		return false;
	}
	const FluidNinjaBuoyancyWaveSource::FResults Latest = GpuState->GetResults();
	if (Latest.StatsTime < 0.0 || Latest.StatsTime < StatsSince)
	{
		return false;
	}
	OutTime = Latest.StatsTime;
	OutVolumeCm3 = Latest.VolumeCm3;
	OutPotentialJ = Latest.PotentialJ;
	OutKineticJ = 0.0;   // the core exports η + P and P, not ∂η/∂t
	OutMaxAbsCm = Latest.MaxAbsCm;
	OutKineticDepthCm = static_cast<float>(GetDepthCm());
	return true;
}

bool UFluidNinjaBuoyancyWaves::GetSourceVolume(double& OutTime, double& OutSourceVolumeCm3) const
{
	// The same readback and gating as GetStats.
	if (!GpuState)
	{
		return false;
	}
	const FluidNinjaBuoyancyWaveSource::FResults Latest = GpuState->GetResults();
	if (Latest.StatsTime < 0.0 || Latest.StatsTime < StatsSince)
	{
		return false;
	}
	OutTime = Latest.StatsTime;
	OutSourceVolumeCm3 = Latest.SourceVolumeCm3;
	return true;
}

FVector2D UFluidNinjaBuoyancyWaves::GetWindowOrigin() const
{
	const FSimState* Sim = BenchSim();
	return Sim ? Sim->Origin : FVector2D::ZeroVector;
}

double UFluidNinjaBuoyancyWaves::GetWindowCm() const
{
	const FSimState* Sim = BenchSim();
	return Sim ? Sim->SizeCm : 0.0;
}

double UFluidNinjaBuoyancyWaves::GetCellCm() const
{
	const FSimState* Sim = BenchSim();
	return Sim ? Sim->CellCm : 0.0;
}

int32 UFluidNinjaBuoyancyWaves::GetResolution() const
{
	const FSimState* Sim = BenchSim();
	return Sim ? Sim->Cells.X : 0;
}

double UFluidNinjaBuoyancyWaves::GetDepthCm() const
{
	const FSimState* Sim = BenchSim();
	return Sim ? Sim->DepthCm : static_cast<double>(FMath::Max(UFluidNinjaBuoyancySettings::Get().WaveDepthCm, 1.f));
}

double UFluidNinjaBuoyancyWaves::GetStepsPerSecond() const
{
	const FSimState* Sim = BenchSim();
	return 1.0 / (Sim ? Sim->TickSeconds : FluidNinjaBuoyancyWaveRuntime::WaveDefaultTickSeconds);
}
