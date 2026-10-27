#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UObject/ObjectKey.h"
#include "FluidNinjaBuoyancyWash.h"
#include "FluidNinjaBuoyancyWaves.generated.h"

class AActor;
class APawn;
class UMaterialInstanceDynamic;
class UNiagaraComponent;
class UPrimitiveComponent;
class UTextureRenderTarget2D;
class UWorld;
class UFluidNinjaBuoyancyComponent;
class UFluidNinjaBuoyancySubsystem;
class UFluidNinjaWaveSimComponent;
struct FFluidNinjaWaveSimSettings;
namespace FluidNinjaBuoyancy { struct FHull; }
namespace FluidNinjaBuoyancyWaveSource { class FState; struct FSelf; struct FDeepHead; }
namespace FluidNinjaBuoyancyWindSea { class FState; }
namespace FluidNinjaBuoyancyRings { class FState; }
class UTexture2D;

/** Where the water a body is measured against comes from, where the Surface Mirror shows none under it (a change resets the body). */
enum class EFluidNinjaWaveWater : uint8
{
	None,         // no water known: no static head there
	Published,    // the far field (creek ribbons, water areas, water volumes)
	Captured,     // the drawn water under the body (FluidNinja's own surface), as last read back
	Controlled,   // the test bench's flat, still water
};

/** One FluidNinja simulation FluidNinja Buoyancy found (UFluidNinjaBuoyancySubsystem's flow source), as the waves take it each frame. */
struct FFluidNinjaWaveSimInput
{
	/** The NinjaLive it belongs to (the velocity buffer's outer; its actor names it for Wave Sims). */
	TWeakObjectPtr<UObject> Ninja;
	/** Its NinjaLiveCore, which runs the wave field (User.WaveSimEnabled and User.Wave.* are set on it every frame). */
	TWeakObjectPtr<UNiagaraComponent> Core;
	/** RT_PressureDivergence (the surface material's Pressure Buffer Parameter): the wave cells' size and the core's B. */
	TWeakObjectPtr<UTextureRenderTarget2D> Pressure;
	/** World XY of its square's centre, and its side (cm). */
	FVector2D Centre = FVector2D::ZeroVector;
	double SizeCm = 0.0;
	bool bValid = false;
};

/** One floating body as the waves see it this frame (game thread). */
struct FFluidNinjaWaveBodyInput
{
	/** UFluidNinjaBuoyancySubsystem's slot id: the gather and the physics step ask for the body by it. */
	int32 Id = 0;
	TWeakObjectPtr<UFluidNinjaBuoyancyComponent> Component;
	/** The finest triangle surface of its hull (body frame, cm), whatever model floats it. */
	TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe> Hull;
	/** The body frame: the floating primitive's location and rotation, no scale. */
	FTransform Transform = FTransform::Identity;
	FVector Velocity = FVector::ZeroVector;          // cm/s
	FVector AngularVelocity = FVector::ZeroVector;   // rad/s
	/** The primitive's bounding sphere (cm): its own field must hold it, well clear of the field's sponge. */
	float Radius = 0.f;
	/** The water it floats on where the Surface Mirror has none under it, cm (Captured: FluidNinja's own surface under its centre). */
	float WaterZ = 0.f;
	EFluidNinjaWaveWater WaterSource = EFluidNinjaWaveWater::None;
	/** The body's Waves switch. */
	bool bWaves = true;
	/** Asleep (dormant) or held out of its water: left out (the water where it was is cleared), and new again when it comes back. */
	bool bAsleep = false;
	/** UFluidNinjaBuoyancyComponent::GetTeleportCounter: a change means it was put somewhere (no waves from the jump). */
	int32 TeleportCounter = 0;
	/** Index into FFluidNinjaWaveWorldInput::Sims of the simulation whose surface lies under it, or INDEX_NONE. */
	int32 Sim = INDEX_NONE;
};

/** A pawn's smooth swimming body, or a sphere of a registered swimmer's body (Source, Part). */
struct FFluidNinjaWaveSwimmerInput
{
	bool bValid = false;
	TWeakObjectPtr<APawn> Pawn;
	/** A registered swimmer's mesh (UFluidNinjaBuoyancySubsystem::RegisterSwimmer) and which of its spheres this is; none for the player. */
	TWeakObjectPtr<UObject> Source;
	int32 Part = 0;
	FVector Centre = FVector::ZeroVector;
	float Radius = 0.f;
	/** Half the capsule's height with its caps, cm. */
	float HalfHeight = 0.f;
	FVector Velocity = FVector::ZeroVector;
	/** The water surface at the pawn (a water volume's top), when one is known, cm. */
	float WaterZ = 0.f;
	bool bHasWaterZ = false;
};

/** The Surface Mirror, when it was drawn this frame (with the waves left out of it). */
struct FFluidNinjaWaveMirrorInput
{
	UTextureRenderTarget2D* Target = nullptr;
	FVector2D Origin = FVector2D::ZeroVector;   // world XY of its centre
	double Top = 0.0;                           // its Z
	double Far = 0.0;                           // depths at or beyond this: nothing drawn
	double Texel = 1.0;                         // cm
	int32 Resolution = 0;
};

/** One body's impulse pieces from the physics steps since last frame: world x, world y, I_down (kg·cm/s, positive presses the water down),
 * area seen from above (cm²). */
struct FFluidNinjaWaveBodyImpulses
{
	int32 Id = 0;
	TArray<FVector4f> Pieces;
	/** The pieces left the physics step scaled already (a planing hull: its steady pieces, their area negative, at Wave Planing Pressure
	 * Scale, the rest at Wave Impulse Scale), so the waves add no scale of their own. */
	bool bScaled = false;
};

/** Everything the waves take from FluidNinja Buoyancy each frame (UFluidNinjaBuoyancySubsystem fills it). */
struct FFluidNinjaWaveWorldInput
{
	TArray<FFluidNinjaWaveSimInput> Sims;
	TArray<FFluidNinjaWaveBodyInput> Bodies;
	FFluidNinjaWaveMirrorInput Mirror;
	bool bControlled = false;
	float ControlledHeight = 0.f;
	FFluidNinjaWaveSwimmerInput Swimmer;
	/** Registered swimmers' spheres, or one player-shaped body for an opted-in humanoid pawn (Pawn valid). */
	TArray<FFluidNinjaWaveSwimmerInput> Puppets;
	TArray<FFluidNinjaWaveBodyImpulses> Impulses;
	/** The whitewater the bodies (their propellers, sterns and spray) and AddWaterFoam add to the foam this frame (interface version 6). */
	TArray<FFluidNinjaWaterWash> Washes;
	/** AddWaterFoam's patches on their last frame (a Seconds 0 patch's only one), apart from Washes: nothing sends them again, so they are
	 * kept until a core tick reads them, as the impulses are (FSimState::PendingFoamPatches). */
	TArray<FFluidNinjaWaterWash> FinalFoamPatches;
	FVector Camera = FVector::ZeroVector;
	bool bHasCamera = false;
	/** The drawn water's own actors: the depth traces under the bodies look through them. */
	TArray<TWeakObjectPtr<AActor>> SurfaceActors;
	/** The drawn water's primitives and the simulation each shows (index into Sims, or INDEX_NONE): their dynamic materials get the
	 * simulation's WaveNormalScale. */
	TArray<TPair<TWeakObjectPtr<UPrimitiveComponent>, int32>> Surfaces;
};

/** What the gather needs for one body: its simulation's B (at the pressure buffer's texels) and its own field's tile (at the wave grid's
 * cells, the pressure buffer's over the simulation's scale-down; the same cells at scale-down 1). */
struct FFluidNinjaWaveGatherSlot
{
	/** RT_PressureDivergence of the body's simulation (B = η + P in z). */
	UTextureRenderTarget2D* Pressure = nullptr;
	FIntPoint PressureCells = FIntPoint::ZeroValue;
	/** World XY of the simulation's lower-left corner, and the pressure buffer's cell (cm): B's texel. */
	FVector2D SimOrigin = FVector2D::ZeroVector;
	double CellCm = 1.0;
	/** World XY of the body's own field's lower-left corner (the wave grid's cells, shifted by whole cells), the field's cell (cm: the wave
	 * cell, CellCm times the simulation's scale-down), its cells across, its first texel in the atlas, and its sponge in its cells. */
	FVector2D SelfOrigin = FVector2D::ZeroVector;
	double SelfCellCm = 1.0;
	int32 SelfCells = 0;
	FIntPoint Tile = FIntPoint::ZeroValue;
	float SpongeCells = 0.f;
};

/** One GPU target the waves own, for the memory report (ReportMemory, fnb.Waves.Memory and the subsystem's fnb.Memory). */
struct FFluidNinjaWaveMemoryItem
{
	FString Name;
	/** Texels across and down (a buffer pool: its cells across and down per slot, the name says how many slots). */
	FIntPoint Size = FIntPoint::ZeroValue;
	FString Format;
	double MegaBytes = 0.0;
};

/** What the physics step needs for one body: how much of the waves it feels, and the radiation damping and added mass of its own. */
struct FFluidNinjaWaveBodyFeel
{
	/** κ (0: it feels none). */
	float Feel = 0.f;
	/** Heave (kg/s), roll and pitch (kg·cm²/s) at the full rest footprint, at the frequencies the body moves at; 0 for none. */
	FVector Radiation = FVector::ZeroVector;
	/** The water the body drags along, at the full rest footprint: heave (kg), roll and pitch (kg·cm², about RollAxis and PitchAxis),
	 * scaled by Wave Added Mass Scale; 0 for none. */
	FVector AddedMass = FVector::ZeroVector;
	/** The roll and pitch axes in the body's frame (the rest attitude's world X and Y). */
	FVector RollAxis = FVector::XAxisVector;
	FVector PitchAxis = FVector::YAxisVector;
	/** The physics hull's wetted area at rest on still water (cm²). */
	float RestWettedArea = 0.f;
	/** The running estimate of how fast the body heaves, rolls and pitches (Hz; its natural frequencies at rest), for debug output. */
	FVector MotionHz = FVector::ZeroVector;
};

/** A body's rest footprint on its simulation's cells (the hull resting in its rest attitude at its rest draft; a cell is in where its
 * vertical line meets the hull below the water), with what the radiation damping and the added mass are worked out from. */
struct FFluidNinjaWaveFootprint
{
	/** The cells' centres (cm, about the body's origin in its rest attitude) and their size. */
	TArray<FVector2D> Cells;
	double CellCm = 0.0;
	double Area = 0.0;
	FVector2D Middle = FVector2D::ZeroVector;
	/** The waterplane's stiffness: heave (ρ g × area, kg/s²), roll and pitch (ρ g × its second moment about the centroid, kg·cm²/s²). */
	double Stiffness[3] = {0.0, 0.0, 0.0};
	/** The body's own mass (kg) and inertia about the roll and pitch axes (kg·cm²). */
	double Inertia[3] = {0.0, 0.0, 0.0};
	/** The physical added mass by strip theory: heave (kg), roll and pitch (kg·cm²), before Wave Added Mass Scale. */
	double AddedMass[3] = {0.0, 0.0, 0.0};
	/** Each motion's natural frequency (rad/s) with the scaled added mass in: √(C / (I + A)); 0 where there is none. */
	double Natural[3] = {0.0, 0.0, 0.0};
	/** The water's depth (cm), gravity (cm/s²) and density (kg/cm³) the damping is worked out with. */
	double DepthCm = 0.0;
	double Gravity = 980.0;
	double Rho = 0.001;
	bool bValid = false;
};

/**
 * FluidNinja Buoyancy's side of FluidNinja Waves (game thread; its interface with the modified NinjaLiveCore is in Docs/FluidNinjaWaves.md):
 * for each FluidNinja simulation FluidNinja Buoyancy finds, while Project Settings → FluidNinja Buoyancy → Waves is on,
 * - every frame it sets the core's User.WaveSimEnabled and User.Wave.* (depth, damping, sponge, open edge, shore, breaking, hyperviscosity,
 *   current coupling, the mean level's relax rate, the grid's scale-down, the foam switch and tuning, and on an interface-5 core the
 *   closed-window <v> pin's rate) and binds User.WaveSourceTexture, its own RGBA32F render target of
 *   the wave grid's size (the core's pressure buffer's cells over the scale-down) plus a header row (FluidNinjaBuoyancyWaveSource.usf), and,
 *   on an interface-4 core, User.WaveFoamTarget, the R16F target the core's foam channel exports into (the surfaces' dynamic materials
 *   sample it as WaveFoamBuffer; fnb.Waves.Foam and the Foam setting switch the accumulation);
 * - a simulation is set up by the UFluidNinjaWaveSimComponent on its NinjaLive actor when it carries one, else by the Wave Sims list;
 *   fnb.Waves.MaxActiveSims keeps only the simulations nearest the camera running;
 * - into that texture it writes each wave body's static head P (rastered from its hull against the wave-free Surface Mirror), its
 *   impulses as y, and erase flags where a body was put, first seen or left; the header row carries the grid's origin, cell and a serial
 *   with the contents, so they travel together whatever the tick order;
 * - a body that sinks (its resting draft below 0) presses a deep volume head instead (fnb.Waves.PlungeSource): its submerged volume
 *   carried up to the surface by the finite-depth kernel, added to P, without ripple impulses, own field or graded release;
 * - it counts the core's ticks (FNiagaraSystemInstance::GetTickCount): an impulse is never lost or doubled when the core runs 0 or 2 ticks
 *   in a frame, whether Niagara ticks before FluidNinja Buoyancy (the count of this frame is known: a frame with none keeps its impulses for the next)
 *   or after it (a frame with none is seen on the next, which writes its impulses again);
 * - each wave body has its own field (the self-coupling fix): the same linear water driven only by that body, on the simulation's own
 *   cells around the body, stepped exactly with the core's ticks and frame, so the gather reads the core's B and the body's B_self for the
 *   same instant and the body feels W + κ·(B − B_self); and the radiation damping of its own waves, from its rest footprint at its heave,
 *   roll and pitch frequencies, goes to the physics step.
 * A core without User.WaveSimEnabled (a stock NinjaLiveCore) is an error once, and its simulation makes no waves. Owned by UFluidNinjaBuoyancySubsystem.
 *
 * The test bench's facade (Poke, probes, stats, the window) reads the simulation around the camera.
 */
UCLASS(Transient)
class UFluidNinjaBuoyancyWaves : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(UFluidNinjaBuoyancySubsystem& InOwner);
	/** Releases the GPU state and the render targets; every core's User.WaveSimEnabled goes back to false. */
	void Shutdown();
	/** Before the Surface Mirror and the gather, every frame: the cores' tick counts read, and (where Niagara ticks after FluidNinja Buoyancy) last
	 * frame's write settled and the bodies' own fields stepped with the ticks that consumed it, so the gather reads them in step. */
	void BeginFrame();
	/** After the Surface Mirror and the gather, every frame: the sources rastered and written, the bodies' own fields fed (and stepped,
	 * where Niagara ticks before FluidNinja Buoyancy). */
	void Tick(float DeltaSeconds, const FFluidNinjaWaveWorldInput& Input);
	/** Some simulation runs waves. */
	bool IsActive() const;
	/** The physics steps should list their impulses (waves running, impulse source on). */
	bool WantsImpulses() const;
	/** The cell (cm) the impulse pieces are merged in when there are too many: the finest wave cell running (1 cm without one). */
	double GetMergeCellCm() const;
	/** The Surface Mirror shows FluidNinja's own surface W: the captured surfaces' dynamic materials get WaveHeightScale 0 before its
	 * capture (bHidden) and their own value back right after it. Materials exposing LFPhysicsWaterCapture also get 1 for this capture
	 * alone, then their previous value back, so visual hull cutouts do not remove the water the physics samples. */
	void SetMirrorWavesHidden(bool bHidden, const TArray<TWeakObjectPtr<UPrimitiveComponent>>& Shown);
	/** The body's simulation's B and its own field's tile for the gather; false when it feels no waves (no own field yet, waves off). */
	bool GetGatherSlot(int32 BodyId, FFluidNinjaWaveGatherSlot& Out) const;
	/** The self-field atlas the gather reads B_self from (null before any body has a field). */
	UTextureRenderTarget2D* GetSelfAtlas() const { return SelfAtlas; }
	/** κ and the radiation damping for the body's physics steps; false (and zeros) when it feels no waves. */
	bool GetBodyFeel(int32 BodyId, FFluidNinjaWaveBodyFeel& Out) const;
	/** The water's depth under the body as its waves take it (cm: its simulation's depth, or the ground traced under it with Wave Depth From
	 * Ground), for a planing hull's wave resistance; 0 (deep water) when no running simulation has it. */
	float GetBodyDepthCm(int32 BodyId) const;
	FString GetStatusLine() const { return StatusLine; }
	/** The GPU memory the waves hold, one item per target the plugin owns: each simulation's source texture and Wave Aux target, the
	 * self-field atlas, the self-field pools (per size: the slots the render thread keeps, mirrored here), and the bench's readback rings
	 * while probes or stats are wanted. Appended to OutItems; the per-frame transients (a simulation's static head and impulse textures,
	 * a batch's WaveOp slices) live in the render graph's pool and are not listed. */
	void ReportMemory(TArray<FFluidNinjaWaveMemoryItem>& OutItems) const;

	/** The ring layer (FluidNinjaBuoyancyRings.h): a ring of ImpulseDown (kg·cm/s, + presses the water down; the impulse as it happened,
	 * unscaled) from a contact RadiusCm wide at WorldXY, drawn by the surfaces of the running simulation holding it (the smallest, as the
	 * pokes find theirs), whose ring state is made on its first ring. Dropped where no running simulation holds it, and while the ring layer
	 * is off (Wave Rings, fnb.Waves.Rings). */
	void AddRing(const FVector2D& WorldXY, float ImpulseDown, float RadiusCm);
	/** The wave cell (cm: the grid the waves run on, the pressure buffer's cell times the scale-down) and the core's fixed ticks a second of
	 * the running simulation holding WorldXY; where none holds it, GetCellCm and GetStepsPerSecond (the bench's window). */
	double GetCellCmAt(const FVector2D& WorldXY) const;
	double GetStepsPerSecondAt(const FVector2D& WorldXY) const;
	/** fnb.Waves.RingsDump: logs the ring response table of the first running simulation that has one (FluidNinjaBuoyancyRings::FState::LogLut),
	 * or that none has. */
	void DumpRings() const;

	/* Test bench (the facade the ripple tests were written against; the window is the wave simulation around the camera). */
	/** A Gaussian pressure head HeightCm high and SigmaCm wide pressing on the water for one of the core's ticks, at the next frame: an
	 * impulse into y (the Cauchy–Poisson impulse problem, not a hump of water from rest). */
	void Poke(const FVector2D& WorldXY, float HeightCm, float SigmaCm);
	/** Points (world XY) where B is read back every frame until they change (at most 256). */
	void SetProbes(const TArray<FVector2D>& WorldPoints);
	/** The latest probe readings (cm) of the current probes and the world time of the frame they were taken in; false before any. */
	bool GetProbeResults(double& OutTime, TArray<float>& OutHeights) const;
	/** Reads back the window's volume and energy every frame while on. */
	void RequestStats(bool bOn);
	/** The latest stats (cm³, J, J, cm) since they were requested; false before any. The core exports no ∂η/∂t, so the kinetic energy
	 * cannot be read: it is 0. */
	bool GetStats(double& OutTime, double& OutVolumeCm3, double& OutPotentialJ, double& OutKineticJ, float& OutMaxAbsCm) const;
	/** As above, with the depth the waves run at in the window (cm). */
	bool GetStats(double& OutTime, double& OutVolumeCm3, double& OutPotentialJ, double& OutKineticJ, float& OutMaxAbsCm, float& OutKineticDepthCm) const;
	/** The water the bodies' static head displaces (Σ P × cell area, cm³) from the same readback as GetStats; false before any. */
	bool GetSourceVolume(double& OutTime, double& OutSourceVolumeCm3) const;
	/** Wave Displacement and Wave Impulse (the settings: the test bench writes them for a run and puts them back). */
	bool IsDisplacementOn() const;
	bool IsImpulseOn() const;
	/** Clears every simulation's waves (the erase flag everywhere) and every body's own field, at the next frame. */
	void RequestClear() { bClearRequested = true; }
	/** World XY of the window's lower-left corner (the wave simulation around the camera). */
	FVector2D GetWindowOrigin() const;
	double GetWindowCm() const;
	double GetCellCm() const;
	int32 GetResolution() const;
	/** The depth the waves run at in the window (Wave Depth or its simulation's override), cm. */
	double GetDepthCm() const;
	/** The core's fixed ticks a second (60 for FluidNinja's 1/60 s). */
	double GetStepsPerSecond() const;
	/** The window the bench reads from now on: the smallest running simulation holding WorldXY (none holds it: no window, which reads 0),
	 * until ClearBenchFocus, which goes back to the simulation around the camera. A test sets it where it measures. */
	void SetBenchFocus(const FVector2D& WorldXY);
	void ClearBenchFocus();
	/** Every running wave simulation's square seen from above (world XY), smallest first. */
	void GetBenchWindows(TArray<FBox2D>& Out) const;
	/** The window's border is open water (with its sponge), not a wall. */
	bool IsBenchEdgeOpen() const;
	/** The window's Wave Aux target read back now (it waits for the GPU): per cell (the core's wet mask, its depth cm, its drift x and y
	 * in cells per tick), row-major from the window's lower-left cell; false when the core has written none for this window. */
	bool ReadBenchAux(TArray<FLinearColor>& OutCells, FIntPoint& OutSize) const;

private:
	/** One wave simulation: its core, its source texture and the bookkeeping that keeps every impulse once. */
	struct FSimState
	{
		/** Names the simulation from frame to frame (its NinjaLive object). */
		FObjectKey Key;
		TWeakObjectPtr<UObject> Ninja;
		TWeakObjectPtr<UNiagaraComponent> Core;
		TWeakObjectPtr<UTextureRenderTarget2D> Pressure;
		TWeakObjectPtr<UTextureRenderTarget2D> Target;
		/** The core's Wave Aux target (User.WaveAuxTarget: its mask, depth and current per cell, for the bodies' own fields). */
		TWeakObjectPtr<UTextureRenderTarget2D> Aux;
		/** The core's foam target (User.WaveFoamTarget, interface version 4: F 0..1 per wave cell, written by its Wave Aux every tick; the
		 * surfaces' dynamic materials sample it as WaveFoamBuffer). None on an older core. */
		TWeakObjectPtr<UTextureRenderTarget2D> Foam;
		/** The foam is accumulating this frame (interface 4 core, the setting and fnb.Waves.Foam on), for the status line. */
		bool bFoam = false;
		/** The wash texture (User.WaveWashTexture, interface version 6: fresh and aged foam rates and the steady impulse head per cell, which
		 * the core's foam takes in), and this frame's whitewater for it (world XY; only while the foam is on, as the core reads it only
		 * then). None on an older core. LastWriteWashes counts the patches its last write washed in, the kept final ones included. */
		TWeakObjectPtr<UTextureRenderTarget2D> Wash;
		TArray<FFluidNinjaWaterWash> FrameWashes;
		int32 LastWriteWashes = 0;
		/** The wind sea (interface version 7, FluidNinjaBuoyancyWindSea.h): its state while the simulation asks for one, and the table it
		 * wrote this frame (User.WaveWindSeaTexture, and the surfaces' WaveWindSeaTable); null while it has none. */
		TSharedPtr<FluidNinjaBuoyancyWindSea::FState> WindSea;
		TWeakObjectPtr<UTexture2D> WindSeaTable;
		/** The ring layer (FluidNinjaBuoyancyRings.h): its state from the simulation's first ring until it stops (kept while it has no rings,
		 * so its response table is not worked out again for every splash), and the table and LUT the surfaces get (WaveRingTable, WaveRingLUT)
		 * once the state has made them; an empty table holds N = 0 and draws nothing, so they stay bound. When the simulation stops the table
		 * is written empty (its surfaces, no longer bound, keep it and draw nothing) and the surfaces go back to their defaults when it runs
		 * again. */
		TSharedPtr<FluidNinjaBuoyancyRings::FState> Rings;
		TWeakObjectPtr<UTexture2D> RingTable;
		TWeakObjectPtr<UTexture2D> RingLUT;
		FString Name;
		/** The core checked for User.WaveSimEnabled, User.WaveAuxTarget and its interface version (by pointer, so a new core is checked
		 * again), and what was found. */
		TWeakObjectPtr<UNiagaraComponent> CheckedCore;
		bool bHasWaveParameter = false;
		bool bHasAuxParameter = false;
		/** User.Wave.InterfaceVersion and User.Wave.CoreBuild (YYYYMMDD) of the core; a core without them is interface 1, build 0. */
		int32 InterfaceVersion = 0;
		int32 CoreBuild = 0;
		/** The core takes the depth from FluidNinja's height field where it has one (User.Wave.DepthFromBed); off with a depth override. */
		bool bDepthFromBed = true;
		/** Where its border is a wall: the wall's absorption, 1/s, and the band it acts in, cm (User.Wave.WallAbsorption, User.Wave.WallBandCm). */
		float WallAbsorption = 0.f;
		float WallBandCm = 50.f;
		/** Its settings come from a UFluidNinjaWaveSimComponent on its NinjaLive actor (else from the Wave Sims list, or the defaults); a
		 * Wave Sims entry naming an actor that carries the component was ignored, and that was logged. */
		bool bFromComponent = false;
		bool bWarnedIniAndComponent = false;
		bool bWarned = false;
		/** Its pressure buffer is still the placeholder the core sizes on its first ticks (logged once a wait, for the status line). */
		bool bWaitingForPressure = false;
		bool bEnabled = true;
		bool bActive = false;
		/** User.WaveSimEnabled was set true (so it is set false when the waves stop). */
		bool bSwitchedOn = false;
		bool bSeen = false;
		/** The pressure buffer (RT_PressureDivergence): its texels over the window, and their size (cm). B is exported, probed and gathered
		 * on them. */
		FIntPoint PressureCells = FIntPoint::ZeroValue;
		double PressureCellCm = 0.0;
		/** The wave grid: the pressure buffer's cells over ScaleDown (User.Wave.ScaleDown, S = 1, 2 or 4; integer division, as the core's),
		 * cells of S pressure cells. The source texture, the Wave Aux target, the bodies' own fields and every cell computation here are on
		 * it. ScaleDownWanted is what the simulation's override or fnb.Waves.ScaleDown asked for; a core before interface version 3 has no
		 * User.Wave.ScaleDown and runs at 1, said once per core (bWarnedScaleDown). */
		FIntPoint Cells = FIntPoint::ZeroValue;
		double CellCm = 0.0;
		int32 ScaleDown = 1;
		int32 ScaleDownWanted = 1;
		bool bWarnedScaleDown = false;
		/** fnb.Waves.MaxActiveSims: how much this simulation matters in the ranking (its distance from the camera is divided by it), its
		 * place in the last ranking (1 the nearest; 0 not ranked) and the distance it was ranked at (cm); suppressed when it is not among the
		 * nearest N: its core's waves are off, its bodies have no own fields, and it re-arms (bRestart) when it comes back. SuppressionChanged
		 * is when it last changed state (world seconds), for the dwell. */
		float Importance = 1.f;
		int32 RankPlace = 0;
		double RankDistanceCm = 0.0;
		bool bSuppressed = false;
		double SuppressionChanged = -1.0e9;
		FVector2D Origin = FVector2D::ZeroVector;
		FVector2D Centre = FVector2D::ZeroVector;
		double SizeCm = 0.0;
		/** The cell lattice's phase (the origin modulo a cell): the bodies' own fields sit on it, so a change starts them over. */
		FVector2D Phase = FVector2D::ZeroVector;
		bool bHasPhase = false;
		float DepthCm = 200.f;
		bool bEdgeOpen = true;
		/** WaveNormalScale for its surfaces' dynamic materials. */
		float NormalScale = 0.f;
		/** How much its current carries its waves and foam (User.Wave.CurrentCoupling): its Wave Sims entry's, else the project's. */
		float CurrentCoupling = 1.f;
		/** How fast its waves lose their energy (User.Wave.Damping, 1/s), its own fields' too: its Wave Sims entry's, else the project's. */
		float Damping = 0.02f;
		/** Its foam: its Wave Sims entry's overrides, else the project's (User.Wave.FoamDecay, FoamAgedDecay, FoamWake, FoamTransfer,
		 * FoamDiffusion, FoamAgedDiffusion). */
		float FoamDecay = 0.7f;
		float FoamAgedDecay = 0.02f;
		float FoamWake = 1.f;
		float FoamTransfer = 0.5f;
		float FoamDiffusion = 30.f;
		float FoamAgedDiffusion = 200.f;
		/** The pressure buffer's object says RGBA16F (B = η + P in z, A = P in w), for the status line. The render thread reads B only from
		 * an RGBA16F texture whatever this says (a stock RG16F one has no B, and its A reads 1). */
		bool bPressureReady = false;
		float TickSeconds = 1.f / 60.f;
		/** The core has a fixed tick (its system's HasFixedTickDelta, above 0), which TickSeconds is. Without one, Wave Step integrates with
		 * the frame's DeltaTime, up to Niagara's 0.125 s clamp, and an explicit mean-relax term must stay stable at that Dt, not at the
		 * 1/60 s TickSeconds then guesses. */
		bool bFixedTick = false;
		/** The k = 0 rates, 1/s, worked out once a frame in UpdateSim: User.Wave.MeanRelax as the core is sent it (fnb.Waves.MeanRelax or its
		 * automatic rate, clamped to 0.5 / the core's longest step), the bodies' own fields' rate (fnb.Waves.SelfMeanRelax, clamped), and
		 * User.Wave.VMeanRelax as an interface-5 core is sent it; bVPin says that core's <v> pin runs (interface 5, a walled window and a rate
		 * above 0). SetSelfStep and the status line read them. */
		float CoreMeanRelax = 0.f;
		float SelfMeanRelax = 0.5f;
		float VMeanRelax = 0.f;
		bool bVPin = false;
		// The core's ticks.
		const void* Instance = nullptr;
		int32 LastTickCount = 0;
		int32 Delta = 0;
		bool bNiagaraFirst = true;
		bool bTicksRead = false;
		/** Started (or the core started over): the next write erases everything and every body's own field starts over. */
		bool bRestart = true;
		/** Not yet written (Niagara first: frames with no tick) or written and not yet seen consumed (Niagara after). */
		TArray<FVector4f> PendingImpulses;   // world x, y, I_down, area
		TArray<FVector4> PendingPokes;       // world x, y, head, sigma
		TArray<FBox2D> PendingErase;         // world XY
		bool bPendingEraseAll = false;
		/** AddWaterFoam's patches on their last frame (FFluidNinjaWaveWorldInput::FinalFoamPatches), kept the same way: the wash is written
		 * whole every write, so one no tick read would be gone. The bodies' washes and a lasting patch's earlier frames come again every
		 * frame and are not kept (FrameWashes). */
		TArray<FFluidNinjaWaterWash> PendingFoamPatches;   // world XY
		TArray<FVector4f> LastImpulses;
		TArray<FVector4> LastPokes;
		TArray<FBox2D> LastErase;
		bool bLastEraseAll = false;
		TArray<FFluidNinjaWaterWash> LastFoamPatches;
		bool bAwaitingConfirm = false;
		uint32 Serial = 0;
		double FixedAge = 0.0;
		int32 LastWriteBodies = 0;
		int32 LastWriteImpulses = 0;
		/** Names it to the render thread, which keeps its summed deep heads between frames (FSource::DeepKey; never 0), and the deep heads
		 * in its last write (the sinkers' and the fading ones'), for the status line and the memory report. */
		uint32 DeepKey = 0;
		int32 LastWriteDeep = 0;
	};

	/**
	 * A sinker's deep volume head (fnb.Waves.PlungeSource): its submerged volume as a uniform column over an equivalent rectangle, the
	 * rectangle and column matching the volume's plan second moments and vertical spread (exact for an upright box), carried up to the
	 * surface by the finite-depth kernel on the GPU (FluidNinjaBuoyancyWaveSource.usf, DeepHeadCS). Held while the body moves and through
	 * its wave event; it fades slowly (WaveDeepFadeSeconds) after a long rest or once the body is gone, never in one step.
	 */
	struct FDeepRecord
	{
		bool bValid = false;
		/** The simulation it presses (FSimState::Ninja). */
		FObjectKey Sim;
		/** World XY of the submerged volume's centroid (cm), and the angle of the rectangle's first axis from world X (rad). */
		FVector2D Centre = FVector2D::ZeroVector;
		double AxisAngle = 0.0;
		/** The rectangle's half extents along that axis and across it, cm. */
		FVector2D Half = FVector2D::ZeroVector;
		/** The column's top and bottom below the wave-free surface (cm, positive down), after fnb.Waves.PlungeLag's filter when it is on. */
		double TopCm = 0.0;
		double BottomCm = 0.0;
		/** The submerged volume (cm³; under WaveDeepMinVolumeCm3 nothing is sent) and the depth the waves run at under it (cm). */
		double VolumeCm3 = 0.0;
		double DepthCm = 0.0;
		/** The last wave-free water level the body was seen in (world Z, cm): while its water is unknown (the Surface Mirror lost it) the
		 * column is still measured against it, so the head follows the body and does not step when the water is known again. */
		double WaterZ = 0.0;
		bool bWaterZ = false;
		/** Where the taper starts and ends (cm from the centre), and 1 / (1 − the share of the volume it takes off), worked out again only
		 * when RenormalisedFor (a, b, top, bottom, depth, taper end) changes. */
		double TaperStartCm = 0.0;
		double TaperEndCm = 0.0;
		double Renormalisation = 1.0;
		double RenormalisedFor[6] = {-1.0, -1.0, -1.0, -1.0, -1.0, -1.0};
		/** P's multiplier for the slow fade (0..1), where it heads (1 while the body is in play, 0 after a long rest or once it is gone),
		 * and how long the body has been at rest (s). */
		float Fade = 1.f;
		float FadeTarget = 1.f;
		double RestSeconds = 0.0;
	};

	/** What the waves remember of a body (or the swimmer) from last frame. */
	struct FBodyState
	{
		int32 Id = 0;
		TWeakObjectPtr<UFluidNinjaBuoyancyComponent> Component;
		/** The simulation it is in (FSimState::Ninja). */
		FObjectKey Sim;
		FBox2D Bounds = FBox2D(ForceInit);   // world XY
		FVector Location = FVector::ZeroVector;
		FQuat Rotation = FQuat::Identity;
		FVector Velocity = FVector::ZeroVector;
		FVector Spin = FVector::ZeroVector;
		int32 Counter = 0;
		UPTRINT Hull = 0;
		EFluidNinjaWaveWater Water = EFluidNinjaWaveWater::None;
		uint64 Stamp = 0;
		// Its own field: N cells of the simulation's own, its lower-left cell on the simulation's lattice, its atlas tile.
		int32 SelfCells = 0;
		double SelfCellCm = 0.0;
		int64 OriginCellX = 0;
		int64 OriginCellY = 0;
		FIntPoint Tile = FIntPoint(-1, -1);
		float SpongeCm = 0.f;
		bool bSelf = false;
		bool bSelfReady = false;
		bool bSelfWarned = false;
		/** Its field's sponge came out under half the wanted width, and that was logged (once per body, until it fits again). */
		bool bSpongeWarned = false;
		// The water's depth under it (a trace), for its own field and its radiation damping.
		float DepthCm = 0.f;
		double NextDepthTrace = 0.0;
		FVector2D DepthTracedAt = FVector2D::ZeroVector;
		// Its rest footprint (radiation damping and added mass), for the cell and depth it was rastered at.
		FFluidNinjaWaveFootprint Footprint;
		FVector RollAxis = FVector::XAxisVector;
		FVector PitchAxis = FVector::YAxisVector;
		float RestWettedArea = 0.f;
		double RadiationCellCm = 0.0;
		float RadiationDepthCm = 0.f;
		bool bRadiation = false;
		/** The radiation damping in use (heave kg/s, roll and pitch kg·cm²/s): the footprint's Haskind damping at each motion's frequency
		 * bin, a quarter octave each about the natural frequency (bin RadiationBins / 2), worked out when a bin is first needed (the ring
		 * sum over the footprint's cells is the cost, so no bin is summed twice) and kept in RadiationTable (set below 0, not yet, when
		 * the footprint is rastered). */
		static constexpr int32 RadiationBins = 9;
		FVector Radiation = FVector::ZeroVector;
		double RadiationTable[3][RadiationBins] = {};
		int32 RadiationBin[3] = {-1, -1, -1};
		/** When the damping may next move to another bin (the estimate wanders; the damping follows it at most every quarter second). */
		double NextRadiationSwitch = 0.0;
		/** The running estimate of each motion's frequency (heave, roll, pitch): ⟨v²⟩ and ⟨a²⟩ of the vertical speed and the roll and
		 * pitch rates over about a second (exponential averages), the last speeds (a is their change frame to frame), and the estimate
		 * itself, √(⟨a²⟩/⟨v²⟩) clamped to half to twice the natural frequency, the natural frequency while the body is still (rad/s; 0
		 * without a footprint). */
		double MotionV2[3] = {0.0, 0.0, 0.0};
		double MotionA2[3] = {0.0, 0.0, 0.0};
		double MotionLastV[3] = {0.0, 0.0, 0.0};
		bool bMotionPrimed = false;
		FVector MotionOmega = FVector::ZeroVector;
		/** The graded release of a hull's static head as its top goes under (fnb.Waves.SubmergeRelease; rigid hulls only, the swimmers'
		 * capsules and spheres keep their own fade). HeadScale is P's multiplier (the record's Extra.w): 1 while the hull comes through
		 * the surface, falling as e^(−t/τ) once its top has gone under (τ = √(w / g), the hollow's own collapse time), rising back the same
		 * way when it comes up again, 0 when it presses nothing. bPierced says the fields after it hold the hull as it last came through the
		 * surface (written on every frame it does, kept through the release, cleared on a reset, at the release's end and while the
		 * release is off): how far its top stood above the water then (cm), its MaxHead then (cm: once nothing pierces, the section width
		 * collapses and the cap with it), its waterline width then (cm) and its attitude then (the held head is rastered in it, so a hull
		 * that pitches or tumbles under water changes neither its footprint nor its columns' depths while the head fades). */
		float HeadScale = 1.f;
		bool bPierced = false;
		float PiercedTopCm = 0.f;
		float PiercedMaxHeadCm = 0.f;
		float PiercedWidthCm = 0.f;
		FQuat PiercedRotation = FQuat::Identity;
		/** fnb.Waves.PlungeSource: which source the body takes, decided on the first frame any of its hull is in its water, from whether it
		 * sinks then (its resting draft below 0 and its Buoyancy Multiplier not floating it after all, WaveBodySinks: the deep head; else
		 * today's static head), and kept until a teleport, its removal or the plunge
		 * source going off, so a mass that changes mid-plunge cannot flip it. bSinkerNow: this frame it sinks and the plunge source is on
		 * (its ripple impulses are left out from then on, even before the path is decided: the deep head carries its entry). */
		bool bPathLatched = false;
		bool bDeep = false;
		bool bSinkerNow = false;
		/** Its path was decided as it came into the water (its lowest point no deeper than it moves in a frame): its deep head starts whole. */
		bool bDeepStartsWhole = false;
		/** Its deep volume head, while it is on the deep path. */
		FDeepRecord Deep;
	};

	/** The atlas's square blocks (a buddy allocator over 64-texel units). */
	struct FTileAllocator
	{
		int32 Size = 0;
		/** Free blocks per size (64, 128, ..., Size), their atlas texels. */
		TMap<int32, TArray<FIntPoint>> Free;
		void Reset(int32 InSize);
		bool Allocate(int32 Cells, FIntPoint& Out);
		void Release(int32 Cells, const FIntPoint& At);
	};

	UWorld* GetOwnerWorld() const;
	bool IsWanted(FString& OutWhy) const;
	/** Off: every core's User.WaveSimEnabled false, every own field released, everything starts afresh when it comes back on. */
	void Deactivate(const FString& Why);
	FSimState* FindSim(const FObjectKey& Key);
	const FSimState* FindSim(const FObjectKey& Key) const;
	/** The cores' tick counts since last frame (and which of Niagara and FluidNinja Buoyancy ticks first). */
	void ReadTicks(FSimState& Sim);
	/** The NinjaLive actor a simulation input names (the velocity buffer's outer, or the actor itself); null without one. */
	static const AActor* SimActor(const UObject* Ninja);
	/** The per-simulation settings of a NinjaLive actor: its UFluidNinjaWaveSimComponent's (ToSettings(), plus the component's own
	 * scale-down and importance), else the Wave Sims entry naming it, else the defaults. OutComponent is the component when there is one;
	 * bOutIniToo says a Wave Sims entry names the actor as well (the component wins). */
	static void ResolveSimSettings(const AActor* NinjaActor, FFluidNinjaWaveSimSettings& Out, const UFluidNinjaWaveSimComponent*& OutComponent, bool& bOutIniToo);
	/** The scale-down a simulation asks for: its override, else fnb.Waves.ScaleDown, as 1, 2 or 4. */
	static int32 WantedScaleDown(const FFluidNinjaWaveSimSettings& Override);
	/** fnb.Waves.MaxActiveSims: about once a second, ranks the simulations that could run by their distance from the camera over their
	 * importance and suppresses all but the nearest N (10 % of hysteresis for the ones running, and at least a second in each state). With
	 * the limit at 0 nothing is suppressed. The states must exist (Tick makes them before it calls this). */
	void RankSims(const FFluidNinjaWaveWorldInput& Input, double Now);
	/** Places the simulation (its cells, lattice, source texture) and sets its core's parameters; false when it runs no waves. */
	bool UpdateSim(FSimState& Sim, const FFluidNinjaWaveSimInput& Input);
	void StopSim(FSimState& Sim);
	/** A body leaves the waves: the water where it was is cleared, and its own field released. A sinker on the deep path clears nothing:
	 * its deep head fades where it is (RetireDeep). */
	void RemoveBody(FBodyState& Body);
	void ReleaseSelf(FBodyState& Body);
	/** The body's deep head goes to the fading list (it fades where it is, WaveDeepFadeSeconds), and the body's source is decided afresh. */
	void RetireDeep(FBodyState& Body);
	/** A sinker's deep head this frame from its hull in the water (the moments of its submerged part, the depth under it), with the rest
	 * timer and the slow fade; bStartWhole: a new head starts at its full value (the hull is just coming into the water, or the water is
	 * erased everywhere this frame), else it fades in. Nothing changes while the water under it is unknown. */
	void UpdateDeep(FBodyState& Body, const FSimState& Sim, const FFluidNinjaWaveBodyInput& Input, const FFluidNinjaWaveWorldInput& World, const FTransform& Frame,
		float WaterZ, bool bStartWhole, float DeltaSeconds);
	/** The fade one frame on (towards FadeTarget, over WaveDeepFadeSeconds); false once a head fading out presses under WaveDeepDropCm. */
	static bool StepDeepFade(FDeepRecord& Record, double DeltaSeconds);
	/** The record as the GPU takes it, relative to the grid's lower-left corner; false when it presses nothing this frame. */
	static bool MakeDeepHead(const FDeepRecord& Record, const FVector2D& GridMin, FluidNinjaBuoyancyWaveSource::FDeepHead& Out);
	/** The body's own field: its size, tile and lattice place (recentred by whole cells); false when it cannot have one. */
	bool PlaceSelf(FBodyState& Body, const FSimState& Sim, const FVector2D& Centre, float Radius, FIntPoint& OutShift, bool& bOutNew);
	/** A self field's step settings: the core's tuning, its window and where the field's cells sit in its Wave Aux target. */
	void SetSelfStep(FluidNinjaBuoyancyWaveSource::FSelf& Self, const FSimState& Sim, const FBodyState& Body) const;
	void UpdateDepth(FBodyState& Body, const FSimState& Sim, const FFluidNinjaWaveBodyInput& Input, const FFluidNinjaWaveWorldInput& World);
	/** The body's rest footprint (its added mass and natural frequencies), rastered again when its cell or depth changes, and its radiation
	 * damping at the frequencies it moves at (UpdateMotion's estimate), moved between bins as the estimate moves. */
	void UpdateRadiation(FBodyState& Body, const FSimState& Sim, const UFluidNinjaBuoyancyComponent& Component);
	/** The running estimate of how fast the body heaves, rolls and pitches, from this frame's velocities (FBodyState::MotionOmega). */
	void UpdateMotion(FBodyState& Body, const FFluidNinjaWaveBodyInput& Input, float DeltaSeconds);
	bool Jumped(const FBodyState& Track, const FVector& Location, const FQuat& Rotation, const FVector& Velocity, const FVector& Spin, float DeltaSeconds) const;
	void UpdateTriangleCache(const TArray<TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe>>& Hulls);
	/** The self-field atlas exists at its current size (made at the first size when there is none). */
	bool EnsureAtlas();
	/** Doubles the atlas (up to the largest size) when its tiles ran out: every body's own field is released and places itself again, so
	 * the fields start over; false at the largest size already. */
	bool GrowAtlas();
	/** The status line from this frame's figures (built every frame only while something shows it, else about once a second). */
	void BuildStatusLine(int32 NumBodies, int32 Resets, int32 Leaving, int32 Fields, int32 NumImpulses, int32 Written, bool bSwimmerSent, float SwimmerScale,
		int32 PuppetsSent, bool bDisplacement);
	const FSimState* BenchSim() const;
	/** The bench's window: the smallest running simulation holding the focus while there is one; else the one around the camera, else
	 * the smallest running. */
	FObjectKey PickBenchSim() const;
	/** The smallest running simulation whose square holds WorldXY (the one the pokes, impulses and foam patches go into), or null. */
	FSimState* FindActiveSimAt(const FVector2D& WorldXY);
	const FSimState* FindActiveSimAt(const FVector2D& WorldXY) const;

	TWeakObjectPtr<UFluidNinjaBuoyancySubsystem> Owner;

	/** Each simulation's User.WaveSourceTexture (RGBA32F, its pressure buffer's size plus a header row). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTextureRenderTarget2D>> SourceTargets;

	/** Every body's own B_self, a tile each (R16F with UAV access), AtlasSize texels across: it starts small and doubles when its tiles
	 * run out (0 before it exists). */
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> SelfAtlas;
	int32 AtlasSize = 0;

	/** The GPU side: made here, used and released on the render thread. */
	TSharedPtr<FluidNinjaBuoyancyWaveSource::FState, ESPMode::ThreadSafe> GpuState;

	TArray<FSimState> Sims;
	TMap<int32, FBodyState> Bodies;
	FTileAllocator Tiles;
	TArray<int32> ReleasedSelves;
	/** Deep heads whose body left or was put elsewhere: each fades where it is until it presses nothing measurable, and goes at once with
	 * an erase of all its simulation's water (the bench's fresh ripples, a restart). */
	TArray<FDeepRecord> FadingDeep;
	/** The next FSimState::DeepKey, and the keys of stopped simulations whose kept deep sums the render thread drops with the next frame. */
	uint32 NextDeepKey = 1;
	TArray<uint32> ReleasedDeepKeys;
	/** Deep heads written this frame, for the status line. */
	int32 DeepHeadsSent = 0;
	/** Per self-field size (64, 128, 256, 512 cells): the most fields of that size alive at once since the size was last unused. The render
	 * thread's pool of that size holds that many slots (rounded up to a power of two, four at least) until every field of the size is
	 * released, so this is what ReportMemory charges for the pools. */
	int32 SelfPoolPeak[4] = {0, 0, 0, 0};
	uint64 TickStamp = 0;
	bool bRunning = false;
	FObjectKey BenchSimKey;
	/** When the simulations are next ranked for fnb.Waves.MaxActiveSims (world seconds). */
	double NextRankTime = 0.0;

	// The swimmer.
	FBodyState Swimmer;
	TWeakObjectPtr<APawn> SwimmerPawn;
	bool bSwimmerTracked = false;
	// Registered swimmers' spheres, by mesh and sphere.
	TMap<TPair<FObjectKey, int32>, FBodyState> Puppets;

	// Every hull in use, its triangles one after another (three body-local corners each); rebuilt only when the set of hulls changes, never
	// changed once built (a rebuild makes a new one and a new serial), so the GPU keeps one upload of it while the serial stays the same.
	TArray<UPTRINT> CachedHullKeys;
	TArray<TSharedPtr<const FluidNinjaBuoyancy::FHull, ESPMode::ThreadSafe>> CachedHulls;
	TMap<UPTRINT, uint32> CachedFirstTriangle;
	TSharedPtr<const TArray<FVector4f>, ESPMode::ThreadSafe> CachedTriangles;
	uint32 TriangleSerial = 0;
	/** The hull raster's lines per cell last noted in the log (Wave Raster Lines); 0 before the first frame. */
	int32 LoggedRasterLines = 0;

	/** The dynamic materials whose WaveHeightScale the Surface Mirror's capture set to 0, with the value each gets back. */
	TArray<TPair<TWeakObjectPtr<UMaterialInstanceDynamic>, float>> HiddenMaterials;
	/** Optional visual-cutout bypass, independent of whether waves are running. Only the physics mirror sets this flag. */
	TArray<TPair<TWeakObjectPtr<UMaterialInstanceDynamic>, float>> PhysicsCaptureMaterials;
	/** Captured surfaces of a wave simulation that had no dynamic material with WaveHeightScale this frame: the mirror shows their waves,
	 * so the floaters there feel their own waves again (an error, logged once per surface, and in the status line). */
	int32 UnhiddenSurfaces = 0;
	TSet<FObjectKey> WarnedUnhidden;

	// Test bench.
	TArray<FVector4> PendingPokes;      // world x, y, head, sigma
	TArray<FVector2D> ProbePoints;      // world XY
	bool bProbesChanged = false;
	double ProbesSince = TNumericLimits<double>::Max();
	bool bStatsWanted = false;
	bool bStatsChanged = false;
	double StatsSince = TNumericLimits<double>::Max();
	bool bClearRequested = false;
	FVector2D BenchFocus = FVector2D::ZeroVector;
	bool bBenchFocus = false;
	FVector2D BenchCamera = FVector2D::ZeroVector;
	bool bBenchCamera = false;

	/** When the surfaces' WaveNormalScale is next set (FluidNinja makes new dynamic materials during play). */
	double NextNormalScale = 0.0;
	/** A simulation's ring table or LUT object changed this frame (its first ring, a table grown, a LUT built again, the simulation
	 * stopped): the surfaces are bound again this frame, not at the next once-a-second pass, so a first ring is drawn at once. */
	bool bRingTexturesChanged = false;
	/** When the status line is next built while nothing shows it every frame. */
	double NextStatusBuild = 0.0;
	bool bWarnedTick = false;
	bool bWarnedSelfAtlas = false;
	FString StatusLine;
};
