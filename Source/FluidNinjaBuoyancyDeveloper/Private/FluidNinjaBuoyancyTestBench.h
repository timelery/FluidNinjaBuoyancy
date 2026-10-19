#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "FluidNinjaBuoyancyBench.h"

class AActor;
class IInputProcessor;
class SWidget;
class UActorComponent;
class UGameViewportClient;
class UPrimitiveComponent;
class UFluidNinjaBuoyancyComponent;
class UFluidNinjaBuoyancySubsystem;

/**
 * FluidNinja Buoyancy test bench: a panel at the top right of the game viewport (PIE by default), labels over every floating body, and one
 * console command, so a person in Play and a script can drive and read the same tests. It is the FluidNinjaBuoyancyDeveloper module
 * (DeveloperTool: the editor and Debug/Development games, never Test or Shipping builds), created per world through
 * UFluidNinjaBuoyancySubsystem::BenchFactory and talking to the water only through FluidNinjaBuoyancyBench.h and public runtime headers.
 *
 * The panel shows the water (live FluidNinja water, or controlled flat still water), the current's scale and calibration, the wind
 * (the project's or a test wind), FluidNinja Buoyancy's costs, a line per body (model, state, draft against its rest draft, tilt, speed, the water's
 * speed, the wind's force) and the selected body in detail, the running test and the latest results.
 *
 * Tests (pass/fail where the answer is clear; a live current swamps such measurements, so these run on controlled water):
 * (in free water: the bodies ignore the scenery for the run, so a bank or rock cannot hold them)
 *   settle    every body put back and settled: draft against its rest draft, and still
 *   capsize   stable bodies heeled 30 degrees: they right themselves
 *   drop      bodies dropped from 3 m: no blow-up, they come back to rest at their draft (slam power reported); the origin's drift
 *             judged, the centre of mass's (horizontal, vertical) and the turn over the same window reported beside it
 *   wind      a test wind of 8 then 16 m/s along the view: bodies drift with it, faster in the stronger wind
 *   throttle  bodies with propulsion: speed at full throttle, heading kept, then turning with full rudder
 * and on the live water:
 *   gather    the water under each body read by the GPU gather and by per-body copies agrees (costs of both reported)
 *   walkaway  the player goes 70 m away and back: no body sinks while out of the capture, all float again after; each pool body
 *             at its rest height on the water it knows (W in the capture, else the far field) within 5 cm or a quarter of its
 *             draft, near and away (either phase outside fails; a pass needs both measured); per phase the waves' own fields and
 *             the open-water probes' mean wave height, from the points inside a running wave simulation (information)
 *   all       every test in turn
 * and FluidNinja Waves (FluidNinjaBuoyancyWaves.h, through the facade the ripple tests were written against when FluidNinja Buoyancy had its own ripple solver;
 * each starts from cleared waves, and stops with a note when the waves are off; ripple-dispersion and ripple-fps are not run, with a note,
 * as their theory is the old solver's, and ripple-weight's energy is reported, not judged, as the core exports no surface speed):
 *   ripple-dispersion  a 4 cm pressure head pressing for one of the core's ticks on controlled water: the rings 1, 2 and 4 m out
 *                      against the impulse response of the core's own dispersion symbol (its Wave Step operator on its grid, from a
 *                      copy of the interface 4 table), stepped at its tick (arrival of the largest crest within 5%, normalised RMS
 *                      within 10%); the continuous theory reported beside it; not measured under irregular frame pacing (more than
 *                      one count of the core's ticks per frame over the reading, as the bench models the core's accumulator: a run
 *                      not on a fixed frame step reads so after a single catch-up frame, so in practice only fixed-step runs get a
 *                      verdict; the frames per count reported, and the core's own count while its status line is rebuilt each frame)
 *   ripple-fps         ripple-dispersion at t.MaxFPS 30, then 60: arrival and height agree within 5%
 *   ripple-kelvin      the row boat towed at 1.75 m/s on controlled water, against the exact linear wake of its own hull towed the same
 *                      way (in the solver's depth bands, blended at the run's depth): each arc's normalised RMS (4 and 6 m behind) at most 25%, and each side's cusp line through the caustics on
 *                      the two arcs within 2 degrees of the exact wake's (the measured caustics read on the theory's lobes; no line where
 *                      the two arcs' caustics are of different lobes, as the exact wake's are after a tow of 7 s or more); Kelvin's 19.47
 *                      degrees reported beside them, with whether it can be read at this tow length
 *   ripple-weight      the crate at 20, 80 and 320 kg and the row boat, each settled at that mass and dropped from 3 m into the calmed
 *                      live pool with every other body lifted away (put back after the test), displacement alone and with the impulse:
 *                      the static head under it at rest (the window's sum of P times the cell area, less the same while it falls)
 *                      against the volume its wave hull has under the water then (within 2%), read once at rest: moving up or down
 *                      under 1 cm/s and the static head's readings spread under 5% of that volume over 1 s, waited for at most 3 s
 *                      (then read and judged anyway, its line saying so); wave energy against m g h, m / rho and the field's own
 *                      volume, early and late, beside it (information); ring heights beside Evers et al. (2019)'s 3D impulse-wave
 *                      equations (each drop read as its image slide, as ripple-evers does), information only: read over the whole
 *                      record, the pool's reflections and all, so ripple-evers judges Evers; a line says on how many distances the
 *                      drops were inside the equations' tested range (Table 3-3)
 *   ripple-rest        the row boat at rest on the calmed live pool for 60 s feeling no waves (κ 0), reported; then 60 s at κ 1, the
 *                      default (it feels the others' waves, its own taken out): no wave builds up (RMS under 0.1 cm), no drift
 *   ripple-coupling    the row boat, its static head alone, dropped from 3 m with a beach ball 3 m away at 45 degrees from its side
 *                      axis (a fixed heading, whatever test ran before) and every other body lifted away: the ball's rise on the
 *                      direct ring against that ring's rise at its place in a second drop with the ball lifted out (80% to 125%), the
 *                      ring read within the direct ring's arrival window (its first crest after the earliest a wave can arrive, at
 *                      sqrt(g h), and no later than the 3 m fall plus the group of a 1.5 m wave; half that crest's period either side
 *                      of it, capped at that latest) and the ball on the same crest; not measured when no direct ring is found there;
 *                      a ring under 3 mm at the ball fails
 *   ripple-evers       a rigid block (relative density 1.338) sized from the pool's traced depth so that every limit of Evers et al.
 *                      (2019)'s Table 3-3 holds, dropped into the calmed live pool, displacement alone and with the impulse: its first
 *                      crest, trough and wave height at r/h 1.5, 2 and 3 (gamma 0 and 45 degrees) within the scatter of Table 4-5, each
 *                      read only before a reflection can arrive (not run, with the reason, where no spot is deep and open enough).
 *                      Only the drop with the impulse is judged: displacement and impulse, the shipped default (Wave Impulse on) and
 *                      the physical case; the displacement-only drop (the static head alone) is reported with the same numbers,
 *                      information only. Judged as the image slide an open-water drop is (half the block, s/2, m/2, Vol/2, down a
 *                      reflecting wall at its centre); the whole block read as one slide is reported beside it. A reflection is
 *                      taken to travel at the faster of the solver's own fastest wave (its longest, in the deepest band it ran) and
 *                      the manual's first crest celerity with its +10% scatter
 *   ripple-advection   a poke in the river's current (started for the test when stopped, and stopped again after): the ring's drift
 *                      against the current under a river floater beside it, and the same poke with the ripples not carried (information)
 *   ripples            the eight in turn (about seven minutes; not part of all)
 * Results go to the panel, the log (LogFluidNinjaBuoyancy "FluidNinja Buoyancy test") and Saved/FluidNinjaBuoyancy/TestResults/<time>_<test>.json (latest.json too).
 *
 * FluidNinja's own water (test levels):
 *   Pool      calm: every NinjaLive no river drives has its turbulence (Velocity Noise, Velocity From Bitmaps) off, so its water is still
 *             and flat; live gives each its own back
 *   River     an actor with StartRiver / StopRiver / IsRunning functions and a SimulationTag naming the NinjaLives it drives (the demo's
 *             FluidNinjaBuoyancyDemoRiver): Start runs it with its simulation's turbulence, Stop stills its water, Reset stops it and puts
 *             the river floaters (tag FNB_RiverFloater) back where they started.
 *
 * Waves: fnb.Test poke [height cm] [sigma cm] presses a Gaussian pressure head (2 cm, 10 cm by default) on the water for one of the
 * core's ticks 3 m ahead of the view, and the panel shows FluidNinja Waves' status line.
 *
 * The panel is off by default: fnb.TestHUD 1 shows it in PIE, 2 in any non-shipping game world, once the world has floating bodies.
 * The console command works either way: fnb.Test <verb> (fnb.Test help lists them). Hotkeys while the panel is up and the game
 * viewport has focus, Ctrl+Shift with: B hide, R reset bodies, PageUp/PageDown select, T run all tests, W cycle the test wind,
 * C controlled water, D debug view, K capsize the selected body, J drop it.
 */
class FFluidNinjaBuoyancyTestBench : public IFluidNinjaBuoyancyBench, public TSharedFromThis<FFluidNinjaBuoyancyTestBench>
{
public:
	/** The bench of one world, or null (shipping, a dedicated server, not a game world): what the FluidNinjaBuoyancyDeveloper module binds
	 * as UFluidNinjaBuoyancySubsystem::BenchFactory. */
	static TSharedPtr<IFluidNinjaBuoyancyBench> Create(UFluidNinjaBuoyancySubsystem& Subsystem);
	virtual ~FFluidNinjaBuoyancyTestBench() override;
	virtual void Remove() override;
	virtual void Tick(float DeltaSeconds) override;
	/** A test runs, or the panel is up or may come up here (fnb.TestHUD): the host builds its status every frame for the bench. */
	virtual bool WantsStatusEveryFrame() const override;

	/** One console verb with its arguments (fnb.Test); returns what it did. */
	FString Command(const TArray<FString>& Args);

	void Select(int32 Step);
	bool SelectByName(const FString& Part);
	UFluidNinjaBuoyancyComponent* GetSelected() const;
	void ResetBodies(bool bAll);
	/** Controlled flat still water at the water height under the bodies (or Height when given), or the live water. */
	void SetControlledWater(bool bOn, const float* Height = nullptr);
	/** A test wind (SpeedMs toward Degrees, or along the view when Degrees is null), or back to the project's Wind. */
	void SetTestWind(bool bOn, float SpeedMs = 0.f, const float* Degrees = nullptr);
	void CycleWind();
	/** The next way of stirring FluidNinja for every body (wakes, tracking by tag, by object type, wakes and tracking). */
	void CycleStir();
	/** Still water in every NinjaLive no river drives (its turbulence off), or each with its own turbulence back. */
	void SetPoolCalm(bool bCalm);
	bool IsPoolCalm() const { return bPoolCalm; }
	/** The river running (its simulation's turbulence on) or stopped (still water). */
	void SetRiverRunning(bool bRun);
	/** Stops the river and puts every river floater (tag FNB_RiverFloater) back where it started. */
	void ResetRiver();
	bool HasRiver() const;
	bool IsRiverRunning() const;
	void Push(float NewtonSeconds);
	void Capsize(float Degrees);
	void Drop(float Metres);
	void BringHere();
	void SetBoat(float Throttle, float Rudder);
	/** A Gaussian pressure head HeightCm high and SigmaCm wide pressing for one of the core's ticks, 3 m ahead of the view target along the
	 * view; returns what it did. */
	FString PokeAhead(float HeightCm, float SigmaCm);
	void ToggleDebugView();
	/** Free water: the bodies ignore the scenery (banks, rocks, the bottom), so a measurement is not cut short by them; they still
	 * meet each other. The controlled-water tests turn it on for their run. */
	void SetSceneryCollision(bool bCollide);
	bool IsSceneryCollision() const { return SceneryOff.IsEmpty(); }
	void Run(const FString& Name);
	void Stop();
	virtual bool IsRunning() const override { return Current.IsValid(); }
	void ToggleVisible() { bVisible = !bVisible; }
	bool IsGameFocused() const;

private:
	struct FTrack;
	struct FPhase;
	struct FRun;
	struct FWaterMap;
	struct FRippleData;
	struct FResult
	{
		FString Test;
		FString Body;
		int32 Verdict = 0;   // 1 pass, -1 fail, 0 information
		FString Text;
	};

	void AttachPanel();
	TSharedRef<SWidget> Build();
	FText Readout() const;
	void DrawLabels() const;
	UWorld* GetWorld() const;
	UFluidNinjaBuoyancySubsystem* GetHydro() const { return Subsystem.Get(); }
	FVector ViewLocation() const;
	float ViewYaw() const;
	/** Water height under the bodies now (mean of those that know theirs). */
	bool BodiesWaterHeight(float& OutHeight) const;
	FString BodyName(const UFluidNinjaBuoyancyComponent* Body) const;
	FString BodyLine(const UFluidNinjaBuoyancyComponent* Body, bool bSelected) const;
	/** The body's tilt from its rest attitude (degrees) and its draft against the water it knows (cm; false when none). */
	float Tilt(const UFluidNinjaBuoyancyComponent* Body) const;
	bool Draft(const UFluidNinjaBuoyancyComponent* Body, float& OutDraft) const;
	/** The rivers (actors with StartRiver / StopRiver and a SimulationTag) and the NinjaLive actors they drive. */
	void FindRivers(TArray<AActor*>& OutRivers, TSet<AActor*>& OutRiverNinjas) const;
	/** A NinjaLive's turbulence off, or back to what it had (kept at the first change), passed on with FluidNinja's SetParams. */
	void SetTurbulence(AActor& Ninja, bool bOn);
	int32 CountRiverFloaters() const;
	/** FluidNinja Waves of this world (the ripple tests' facade), or null. */
	IFluidNinjaBuoyancyBenchWaves* GetWaves() const;
	/** The selected body when its name or class has Kind in it, else the first such body that is not a river floater, else any such. */
	UFluidNinjaBuoyancyComponent* FindTestBody(const TCHAR* Kind) const;
	/** Where the waves run in the window the bench reads, from the core's own Wave Aux (its wet mask and depth); false without one. */
	bool BuildWaterMap(FWaterMap& Out) const;
	/** How far the ground lies below WaterZ at Point (cm, traced down from 3 m above it with the floating bodies, people and the water's
	 * own surfaces left out); below 0 when no ground was found. */
	float TraceBedDepth(const FVector2D& Point, float WaterZ) const;
	/** The ripple tests on the live, calmed pool (in a group they share the calm, which is put back after the last of them). */
	static bool WantsCalmPool(const FString& Test);
	void Report(const FString& Test, const FString& Body, int32 Verdict, const FString& Text);
	void SaveResults(const FString& Test, int32 FirstResult) const;

	// Tests.
	void StartRun(TUniquePtr<FRun> Run);
	void FinishRun(bool bStopped);
	TUniquePtr<FRun> MakeRun(const FString& Name);
	TUniquePtr<FRun> MakeRippleRun(const FString& Name);
	void TickRun();

	TWeakObjectPtr<UFluidNinjaBuoyancySubsystem> Subsystem;
	TWeakObjectPtr<UGameViewportClient> Viewport;
	TSharedPtr<SWidget> Widget;
	TSharedPtr<IInputProcessor> Input;
	bool bVisible = true;
	bool bLabels = true;
	TWeakObjectPtr<UFluidNinjaBuoyancyComponent> Selected;
	int32 WindPreset = 0;
	bool bPoolCalm = false;
	/** World time the pool was last calmed (its water settles over about 40 s), and whether a ripple test calmed it (and puts it back). */
	double PoolCalmSince = -1.0;
	bool bPoolCalmedByTest = false;
	/** This frame's length: a ripple test's poke goes in before the frame's steps, which cover the frame just passed. */
	float FrameSeconds = 0.f;
	/** The still water under the bodies (BodiesWaterHeight: each afloat body's W, FluidNinja's surface drawn without the waves, else its
	 * far field), the last frame any body knew it on live water: controlled water is set there when every body is held for its water
	 * (after a test's reset each body's slot starts over and knows no water), rather than at the view's height less 170 cm. */
	float LastStillWaterZ = 0.f;
	bool bLastStillWater = false;
	/** Each NinjaLive's own Velocity Noise and Velocity From Bitmaps, from before the first change. */
	TMap<TWeakObjectPtr<UActorComponent>, TPair<double, double>> Turbulence;
	/** The start-of-play river stop: 0 not yet, 1 done once, 2 done again at 2 s (FluidNinja may apply its preset after Begin Play). */
	int32 RiverHold = 0;
	/** The rivers and the NinjaLive each drives, looked up once. */
	mutable bool bRiversFound = false;
	mutable TArray<TPair<TWeakObjectPtr<AActor>, TWeakObjectPtr<AActor>>> RiverActors;

	struct FSavedResponse
	{
		ECollisionResponse WorldStatic = ECR_Block;
		ECollisionResponse WorldDynamic = ECR_Block;
	};
	TMap<TWeakObjectPtr<UPrimitiveComponent>, FSavedResponse> SceneryOff;

	TUniquePtr<FRun> Current;
	TArray<FString> Queue;
	FString QueueName;
	int32 QueueFirstResult = 0;
	TArray<FResult> Results;
	mutable FText CachedText;
	mutable double CachedAt = -1.0;
};
