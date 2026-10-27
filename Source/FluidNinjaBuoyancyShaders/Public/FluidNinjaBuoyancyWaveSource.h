#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

class FRHICommandListImmediate;
class FRHITexture;

/**
 * FluidNinja Buoyancy's side of FluidNinja Waves on the render thread (FluidNinjaBuoyancyWaveSource.usf): the source texture each wave simulation's modified
 * NinjaLiveCore reads (User.WaveSourceTexture: P, the bodies' static head plus the sinkers' deep volume heads; y, their impulses; the erase flags; a header row with the
 * grid's origin), and each floating body's own field (its self-sim, for the self-coupling fix): the core's own water (its Wave
 * Prepare and Wave Step, ported) driven only by that body, with the core's obstacles, depth and current (its Wave Aux target) and the
 * core's ticks, whose B_self = η + P goes into a tile of the self-field atlas for the gather. Render-thread code with no UObjects; the runtime (UFluidNinjaBuoyancyWaves) fills an FFrame each frame.
 *
 * Units are Unreal's: cm, kg, s. Everything in XY is relative to its grid's lower-left corner (a simulation's, or a self field's);
 * Z is absolute.
 */
namespace FluidNinjaBuoyancyWaveSource
{
	/** A body (or the swimmer's capsule) whose static head the raster writes. Grid-relative XY, absolute Z. */
	struct FBody
	{
		/** Rows of the body->world rotation matrix; Axis[r].W = world translation component r (X, Y grid-relative, Z absolute). */
		FVector4f Axis[3] = {FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero()};
		/** This frame's world XY box of the body (min x, min y, max x, max y), grid-relative cm. */
		FVector4f Bounds = FVector4f::Zero();
		/** x capsule radius (cm), y capsule half-height with the caps (cm), z the water it floats on where the Surface Mirror has none
		 * (cm), w P's multiplier (1 for a hull, less while a hull that went under releases its head or takes it back on surfacing,
		 * fnb.Waves.SubmergeRelease; the swimmer's capsule sends its body's share). */
		FVector4f Extra = FVector4f(0.f, 0.f, 0.f, 1.f);
		/** The deepest P this body presses into any cell (cm; 0: no limit). A hull deeper than it is wide (a log speared in end first)
		 * would press a pit the linear waves cannot carry: let go, it collapses into a spike. */
		float MaxHeadCm = 0.f;
		/** The hull's speed through the water (cm/s, horizontal). It goes into the source texture's A wherever this body presses a cell
		 * (the fastest body per cell wins), for the core's waterline foam (interface version 4; a pre-4 core ignores the channel). */
		float SpeedCmS = 0.f;
		/** A capsule's P lets go over this much (cm) as its top goes under the surface (0: its radius). */
		float SubmergeFadeCm = 0.f;
		/** Kind 2 (a lying ellipsoid): its vertical semi-axis, cm; Extra.x and Extra.y are its semi-axes along and across the body's local
		 * X (Axis' first column: the facing). */
		float HalfThicknessCm = 0.f;
		/** Into FFrame::Triangles (three FVector4f per triangle, body-local cm in xyz). */
		uint32 FirstTriangle = 0;
		uint32 NumTriangles = 0;
		/** 0 triangle hull, 1 upright capsule (centred on the translation), 2 lying ellipsoid (centred on the translation, turned by Axis:
		 * a swimming body). */
		uint32 Kind = 0;
	};

	/**
	 * A sinking body's deep volume head (fnb.Waves.PlungeSource; FluidNinjaBuoyancyWaveSource.usf, DeepHeadCS). Its submerged volume,
	 * as a uniform column over an equivalent rectangle, carried up to the surface through the exact linear transfer of a volume source
	 * at depth over a flat bed, cosh(k (h − z)) / cosh(k h): the surface it would raise by itself. Added to P, so B = η + P is the surface
	 * of that source problem. Tapered to 0 at TaperEndCm, where the finite-depth kernel has fallen to a fraction of a percent. Grid-relative
	 * XY; depths are below the wave-free surface, positive down.
	 */
	struct FDeepHead
	{
		/** The equivalent rectangle's centre (grid-relative cm) and its first axis (the cosine and sine of its angle from world X). */
		FVector2f Centre = FVector2f::ZeroVector;
		FVector2f Axis = FVector2f(1.f, 0.f);
		/** Its half extents along that axis and across it, cm. */
		FVector2f Half = FVector2f::ZeroVector;
		/** The depths of the column's top and bottom, cm (0 <= TopCm < BottomCm <= DepthCm; the top 0 while the hull comes through the surface). */
		float TopCm = 0.f;
		float BottomCm = 0.f;
		/** The depth the waves run at under it (h, the kernel's bed), cm. */
		float DepthCm = 200.f;
		/** P's multiplier: the submerged volume over 4 ab (BottomCm − TopCm), times the taper's renormalisation and the record's fade. */
		float Scale = 0.f;
		/** P is whole out to TaperStartCm from the centre and falls as cos² to 0 at TaperEndCm. */
		float TaperStartCm = 0.f;
		float TaperEndCm = 0.f;
	};

	/** The Surface Mirror as it was drawn this frame (with the waves left out of it), placed for one grid. */
	struct FMirror
	{
		bool bValid = false;
		/** Grid-relative centre, cm. */
		FVector2f Origin = FVector2f::ZeroVector;
		float Texel = 1.f;
		int32 Size = 0;
		float Top = 0.f;
		/** Depths at or beyond this: nothing drawn. */
		float Far = 0.f;
	};

	/** One wave simulation's source texture this frame (R = P, G = y, B = erase, A = the pressing hull's speed through the water). */
	struct FSource
	{
		/** W x (H + 1) RGBA32F (A32B32G32R32F), set on the render thread. */
		FRHITexture* Target = nullptr;
		/** W x H: the simulation's export cells. */
		FIntPoint Cells = FIntPoint::ZeroValue;
		float CellCm = 1.f;
		/** (OriginX, OriginY: absolute world cm of the grid's lower-left corner, Cell cm, Serial). */
		FVector4f Header0 = FVector4f::Zero();
		/** (TicksExpected, 0, 0, 0). */
		FVector4f Header1 = FVector4f::Zero();
		FMirror Mirror;
		TArray<FBody> Bodies;
		/** x, y (grid-relative cm), I_down (kg·cm/s, positive pushes the water down), projected area (cm²). */
		TArray<FVector4f> Impulses;
		/** x, y (grid-relative cm), head (cm), sigma (cm): pressure heads pressing for FFrame::PokeSeconds. */
		TArray<FVector4f> Pokes;
		/** Cells (min x, min y, max x, max y), inclusive, where the core sets η = −P and v = 0 this frame. */
		TArray<FIntVector4> EraseBoxes;
		bool bEraseAll = false;
		/** The sinkers' deep volume heads, added to P (signed fixed point, never through the floaters' deepest-body maximum). None: P is the
		 * floaters' raster alone, exactly as before the deep heads existed. */
		TArray<FDeepHead> DeepHeads;
		/** Names the simulation from frame to frame (never 0): its summed deep heads are kept on the GPU and reused while its records stay
		 * the same (a sunk body at rest costs no dispatch). */
		uint32 DeepKey = 0;
		/** FluidNinja Waves interface version 6: the simulation has a wash texture (User.WaveWashTexture, W x H RGBA16F on these cells), written
		 * here with the source: R the fresh and G the aged foam rates the bodies wash in (Washes: two float4 each, (x, y grid-relative cm,
		 * σ cm, 0) then (fresh rate, aged rate, 0, 0), per second), B the steady part of this write's impulse head y (the impulses whose
		 * area is negative: a planing hull's steady pressure, which the core's impact foam leaves out), A 0. WashTarget is set on the
		 * render thread. */
		bool bWash = false;
		FRHITexture* WashTarget = nullptr;
		TArray<FVector4f> Washes;
	};

	/** One body's self field this frame. */
	struct FSelf
	{
		/** The body's id (persistent GPU state is kept per id until released). */
		int32 Id = 0;
		/** N cells across: 64, 128, 256 or 512. */
		int32 Cells = 128;
		float CellCm = 1.f;
		/** Raster this frame's P, splat its impulses (and recentre or reset first); false: only step (or nothing). */
		bool bSources = false;
		/** Whole cells the window moved since the last frame with sources (new origin minus old). */
		FIntPoint Shift = FIntPoint::ZeroValue;
		/** Start from η = −P, v = 0 (B_self = 0), with nothing pending: new, teleported, or its simulation started over. */
		bool bReset = false;
		/** Raster Body (false: P = 0 this frame, a body wholly under the surface whose head is not, or no longer, being released). */
		bool bHasBody = false;
		FBody Body;
		FMirror Mirror;
		/** This body's own impulses, self-grid-relative (as FSource::Impulses). */
		TArray<FVector4f> Impulses;
		/** The core's ticks to step now (0: no step this frame; the pending impulse is kept). */
		int32 StepTicks = 0;
		/** The depth outside the simulation's window, cm (inside, the core's own, from Aux). */
		float DepthCm = 200.f;
		/** The core's Wave.Damping, Wave.Hyperviscosity (1/s), Wave.ShoreDepthCm (cm) and Wave.BreakingSlope. */
		float Damping = 0.02f;
		float Hyperviscosity = 0.5f;
		float ShoreDepthCm = 10.f;
		float BreakingSlope = 0.4f;
		/** The self field's own mean relax (1/s): fnb.Waves.SelfMeanRelax as SetSelfStep clamps it, deliberately not the core's
		 * User.Wave.MeanRelax. The k = 0 mode kill: the tile's mean of s = eta + P over its wet cells relaxed toward 0 each tick (0 off). Kept
		 * low because the field's absorbing band already dominates its uniform mode, and a strong pin on a small tile would turn each impulse
		 * into a uniform kick. */
		float MeanRelax = 0.5f;
		/** The width of the self field's own absorbing band at its window's edge, cm. */
		float SpongeCm = 300.f;
		/** One fixed tick of the body's simulation (its NinjaLiveCore's), s. */
		float TickSeconds = 1.f / 60.f;
		/** Atlas texel of cell (0, 0). */
		FIntPoint Tile = FIntPoint::ZeroValue;
		/** The simulation's Wave Aux target (RGBA32F at its cells: mask, depth cm, drift x and y in cells per tick, written by the
		 * core every tick), set on the render thread. */
		FRHITexture* Aux = nullptr;
		/** The simulation cell (in Aux) of the self field's cell (0, 0), and the simulation's cells across. */
		FIntPoint AuxCell = FIntPoint::ZeroValue;
		FIntPoint SimCells = FIntPoint::ZeroValue;
		/** The simulation's window border: open with the core's sponge (Wave.SpongeCm wide), or a wall absorbing WallAbsorption (1/s)
		 * within WallBandCm of it (Wave.WallAbsorption, Wave.WallBandCm). */
		bool bEdgeOpen = true;
		float SimSpongeCm = 300.f;
		float WallAbsorption = 0.f;
		float WallBandCm = 50.f;
	};

	/** Test bench readings from one simulation's output (RT_PressureDivergence: B = η + P in z, A = P in w). */
	struct FProbeSet
	{
		FRHITexture* Pressure = nullptr;
		FIntPoint Cells = FIntPoint::ZeroValue;
		float CellCm = 1.f;
		/** Grid-relative cm. */
		TArray<FVector2f> Points;
		/** Where each point's reading goes in FResults::ProbeHeights. */
		TArray<int32> Slots;
	};

	/** Everything one frame of the wave sources needs. */
	struct FFrame
	{
		TArray<FSource> Sources;
		TArray<FSelf> Selves;
		/** Self fields to forget (the body left, or its simulation stopped). */
		TArray<int32> Released;
		/** Kept deep-head sums to forget (FSource::DeepKey: the simulation stopped). */
		TArray<uint32> ReleasedDeep;
		/** The bodies' triangles, laid out as three FVector4f per triangle: uploaded once and kept while TriangleSerial stays the same. An
		 * array handed over here must never change afterwards. */
		TSharedPtr<const TArray<FVector4f>, ESPMode::ThreadSafe> Triangles;
		uint32 TriangleSerial = 0;
		/** The Surface Mirror (R32F scene depth), set on the render thread; each grid has its own FMirror placement. */
		FRHITexture* Mirror = nullptr;
		/** The self-field atlas (R16F with UAV access), set on the render thread. */
		FRHITexture* Atlas = nullptr;
		bool bControlled = false;
		float ControlledHeight = 0.f;
		/** Vertical lines per cell in the hull raster (Wave Raster Lines): the nearest of 1, 4, 8, 16 and 32 is used (SupportedRasterLines). */
		int32 RasterLines = 16;
		float Gravity = 980.f;
		/** kg/cm³. */
		float Density = 0.001f;
		/** σ of the impulse splat, cm (at most one cell is used). */
		float SmoothCm = 0.f;
		float PokeSeconds = 1.f / 60.f;
		/** The self fields' sponge strength at the window's very edge, 1/s. */
		float SpongeRate = 10.f;
		/** Probes (at most 256 points in all) and stats (one simulation). */
		TArray<FProbeSet> Probes;
		int32 NumProbes = 0;
		bool bStats = false;
		FRHITexture* StatsPressure = nullptr;
		FIntPoint StatsCells = FIntPoint::ZeroValue;
		float StatsCellCm = 1.f;
		/** Stamped on the probe and stat results, s. */
		double WorldTime = 0.0;
	};

	/** The latest completed readbacks. */
	struct FResults
	{
		/** World time of the frame the probes were taken in (-1 = none yet). */
		double ProbeTime = -1.0;
		/** B at each probe, cm. */
		TArray<float> ProbeHeights;
		/** World time of the frame the stats were taken in (-1 = none yet). */
		double StatsTime = -1.0;
		/** Σ(B − A)·Δ²: the waves' volume, cm³. */
		double VolumeCm3 = 0.0;
		/** ½·ρ·g·Σ(B − A)²·Δ², J. */
		double PotentialJ = 0.0;
		/** max |B − A|, cm. */
		float MaxAbsCm = 0.f;
		/** ΣA·Δ²: the volume the bodies' static head displaces, cm³. */
		double SourceVolumeCm3 = 0.0;
	};

	/** Persistent GPU state: create on the game thread, use and release on the render thread only. */
	class FLUIDNINJABUOYANCYSHADERS_API FState
	{
	public:
		FState();
		/** Releases the GPU state; if it runs off the render thread, the release is handed to the render thread. */
		~FState();
		FState(const FState&) = delete;
		FState& operator=(const FState&) = delete;

		/** Thread-safe copy of the latest readbacks. */
		FResults GetResults() const;

	private:
		friend struct FStateAccess;
		struct FGpu;
		/** The self fields, the kept triangles and the readback rings; render thread only. */
		TUniquePtr<FGpu> Gpu;
		mutable FCriticalSection ResultsLock;
		FResults Results;
	};

	/** The fixed-point units per kg·cm/s of the impulse accumulators (shared with the runtime's bookkeeping). */
	constexpr float ImpulseFixedScale = 1024.f;
	/** The self fields' sizes: N must be one of these. */
	constexpr int32 MinSelfCells = 64;
	constexpr int32 MaxSelfCells = 512;

	/** Render thread: one frame of the wave sources (sources, self fields, probes and stats). */
	FLUIDNINJABUOYANCYSHADERS_API void Enqueue(FRHICommandListImmediate& RHICmdList, FState& State, const FFrame& Frame);

	/** The hull raster's lines per cell for a wanted count: the nearest of the compiled ones (1, 4, 8, 16, 32), ties to the larger. */
	FLUIDNINJABUOYANCYSHADERS_API int32 SupportedRasterLines(int32 Lines);
}
