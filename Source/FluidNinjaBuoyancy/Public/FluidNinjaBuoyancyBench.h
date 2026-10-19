#pragma once

#include "CoreMinimal.h"

class UFluidNinjaBuoyancyComponent;
class UPrimitiveComponent;
class UWorld;
enum class EFluidNinjaBuoyancyStirApproach : uint8;
struct FFluidNinjaBuoyancyBodyDebug;
struct FFluidNinjaBuoyancyWind;

/**
 * The test bench's side of FluidNinja Waves: the window the bench reads (the wave simulation around the camera, or the one a test
 * focuses on), its pokes, probes and stats. Implemented by the runtime module (UFluidNinjaBuoyancySubsystem forwards to its waves);
 * reached through IFluidNinjaBuoyancyBenchHost::GetBenchWaves. The waves' project switches (Wave Feel Scale, Wave Displacement, Wave
 * Impulse, Wave Current Coupling) are not here: a test that needs another value writes the setting itself
 * (UFluidNinjaBuoyancySettings::GetMutable) and puts it back when it ends.
 */
class IFluidNinjaBuoyancyBenchWaves
{
public:
	virtual ~IFluidNinjaBuoyancyBenchWaves() = default;

	/** Some simulation runs waves. */
	virtual bool IsActive() const = 0;
	virtual FString GetStatusLine() const = 0;
	/** A Gaussian pressure head HeightCm high and SigmaCm wide pressing on the water for one of the core's ticks, at the next frame. */
	virtual void Poke(const FVector2D& WorldXY, float HeightCm, float SigmaCm) = 0;
	/** Points (world XY) where the wave height is read back every frame until they change (at most 256). */
	virtual void SetProbes(const TArray<FVector2D>& WorldPoints) = 0;
	/** The latest probe readings (cm) and the world time of the frame they were taken in; false before any. */
	virtual bool GetProbeResults(double& OutTime, TArray<float>& OutHeights) const = 0;
	/** Reads back the window's volume and energy every frame while on. */
	virtual void RequestStats(bool bOn) = 0;
	/** The latest stats (cm³, J, J, cm) since they were requested; false before any. */
	virtual bool GetStats(double& OutTime, double& OutVolumeCm3, double& OutPotentialJ, double& OutKineticJ, float& OutMaxAbsCm) const = 0;
	/** As above, with the depth the waves run at in the window (cm). */
	virtual bool GetStats(double& OutTime, double& OutVolumeCm3, double& OutPotentialJ, double& OutKineticJ, float& OutMaxAbsCm, float& OutKineticDepthCm) const = 0;
	/** The water the bodies' static head displaces (cm³) from the same readback as GetStats; false before any. */
	virtual bool GetSourceVolume(double& OutTime, double& OutSourceVolumeCm3) const = 0;
	/** Clears every simulation's waves and every body's own field, at the next frame. */
	virtual void RequestClear() = 0;
	/** World XY of the window's lower-left corner, the window's width (cm), its cell (cm) and the core's fixed ticks a second. */
	virtual FVector2D GetWindowOrigin() const = 0;
	virtual double GetWindowCm() const = 0;
	virtual double GetCellCm() const = 0;
	virtual double GetStepsPerSecond() const = 0;
	/** The window the bench reads from now on: the smallest running simulation holding WorldXY, until ClearBenchFocus. */
	virtual void SetBenchFocus(const FVector2D& WorldXY) = 0;
	virtual void ClearBenchFocus() = 0;
	/** Every running wave simulation's square seen from above (world XY), smallest first. */
	virtual void GetBenchWindows(TArray<FBox2D>& Out) const = 0;
	/** The window's border is open water (with its sponge), not a wall. */
	virtual bool IsBenchEdgeOpen() const = 0;
	/** The window's Wave Aux target read back now: per cell the core's wet mask, its depth (cm), its drift x and y (cells per tick),
	 * row-major from the window's lower-left cell; false when the core has written none for this window. */
	virtual bool ReadBenchAux(TArray<FLinearColor>& OutCells, FIntPoint& OutSize) const = 0;
};

/**
 * What a test bench may ask of the water: the runtime module's whole surface for testing, implemented by UFluidNinjaBuoyancySubsystem.
 * A bench (IFluidNinjaBuoyancyBench, in the FluidNinjaBuoyancyDeveloper module) talks to the water through this alone, so the runtime
 * ships nothing of the bench and the bench depends on nothing private. The bodies themselves are UFluidNinjaBuoyancyComponent's public
 * API (its readout, start transform, RestartAt, CopyWaveHull).
 */
class IFluidNinjaBuoyancyBenchHost
{
public:
	virtual ~IFluidNinjaBuoyancyBenchHost() = default;

	/** The world the water runs in. */
	virtual UWorld* GetBenchWorld() const = 0;
	/** One line about the water field and one per body (built every frame while a bench wants it). */
	virtual FString GetStatus() const = 0;

	/* The bodies. */
	virtual TArray<UFluidNinjaBuoyancyComponent*> GetBodies() const = 0;
	virtual bool GetBodyDebug(const UFluidNinjaBuoyancyComponent* Component, FFluidNinjaBuoyancyBodyDebug& Out) const = 0;

	/* Controlled water and the test wind. */
	/** Every body floats on flat, still water at Height (no waves, no current, no wakes), for measurements the live water would swamp. */
	virtual void SetControlledWater(bool bOn, float Height = 0.f) = 0;
	virtual bool IsWaterControlled() const = 0;
	virtual float GetControlledWaterHeight() const = 0;
	virtual const FFluidNinjaBuoyancyWind& GetWind() const = 0;
	/** Blows SpeedMs toward DirectionDegrees instead of the project's Wind, until switched off. */
	virtual void SetWindOverride(bool bOn, float SpeedMs = 0.f, float DirectionDegrees = 0.f) = 0;

	/* The current's calibration. */
	virtual void CalibrateFlow(bool bApply) = 0;
	virtual void CalibrateFlowAt(const FVector& Where, bool bApply) = 0;
	virtual FString GetCalibrationReport() const = 0;
	virtual float GetFlowScale() const = 0;

	/* The gather and its verification. */
	/** The GPU gather is in use (the setting, unless the console overrides it). */
	virtual bool IsGathering() const = 0;
	/** Verify mode: every body's water is also copied out on its own and compared with the gather of the same frame
	 * (FFluidNinjaBuoyancyBodyDebug::Verify*). Switching it on clears the statistics. */
	virtual void SetVerifyGather(bool bOn) = 0;

	/* How the bodies stir FluidNinja (EFluidNinjaBuoyancyStirApproach). */
	virtual void SetStirApproach(EFluidNinjaBuoyancyStirApproach Approach) = 0;
	virtual EFluidNinjaBuoyancyStirApproach GetStirApproach() const = 0;
	virtual bool IsStirApproachSet() const = 0;
	virtual FString DescribeStirApproach() const = 0;

	/* Readouts for the panel. */
	/** Smoothed costs (ms): game-thread tick, its patch building, physics pre-tick, physics thread a frame. */
	virtual void GetCosts(float& OutTick, float& OutPatches, float& OutPreTick, float& OutPhysics) const = 0;
	virtual int32 GetWakePoints() const = 0;
	virtual int32 GetCapturedPrimitives() const = 0;
	virtual int32 GetSurfacePrimitives() const = 0;
	virtual const TArray<TWeakObjectPtr<UPrimitiveComponent>>& GetSurfacePrimitiveComponents() const = 0;
	virtual int32 GetGatherBodies() const = 0;
	virtual int32 GetGatherPoints() const = 0;

	/** FluidNinja Waves of this world, or null before Begin Play and where there are none. */
	virtual IFluidNinjaBuoyancyBenchWaves* GetBenchWaves() = 0;
};

/**
 * A test bench the water hosts: created for a world by the factory UFluidNinjaBuoyancySubsystem::BenchFactory (bound by the
 * FluidNinjaBuoyancyDeveloper module, where the bench lives), ticked by the subsystem every frame before it reads the water, and
 * removed when the subsystem goes. Nothing else of the bench is known to the runtime.
 */
class IFluidNinjaBuoyancyBench
{
public:
	virtual ~IFluidNinjaBuoyancyBench() = default;

	/** Every frame, from the subsystem's tick, before the water is read (a test's inputs go in before the frame's physics). */
	virtual void Tick(float DeltaSeconds) = 0;
	/** The world is going: the panel comes down, a running test stops and puts back what it changed. */
	virtual void Remove() = 0;
	/** A test is running. */
	virtual bool IsRunning() const = 0;
	/** The bench reads the status every frame (its panel is up or may come up, or a test runs): the host builds it every frame rather than
	 * once a second. */
	virtual bool WantsStatusEveryFrame() const = 0;
};
