#pragma once

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"

class UTexture2D;
struct FFluidNinjaWaveSimSettings;

/**
 * FluidNinja Waves' wind sea (core interface version 7): the fine, short-crested wind waves of open water, as an incident field the plugin
 * describes and FluidNinja draws. Each simulation with a Wind Sea Height gets a JONSWAP spectrum with directional spreading, sampled into
 * a fixed set of plane waves (stratified in wavenumber, random in direction and phase, the same every run for the same simulation and
 * settings). The waves are anchored in the world: the plugin advances each one's phase with its own frequency every frame (finite-depth
 * gravity-capillary dispersion) and writes them into a small table, (N + 1) x 1 RGBA32F:
 *   texel 0       = (origin x, origin y, L, N): the origin in world cm (near the window, moved in whole steps with the phases carried),
 *                   L the long band's count, N every component's;
 *   texels 1..N   = (kx, ky in rad/cm, amplitude cm, phase rad at the origin this frame), the long band first.
 * The long band (wavelengths of at least fnb.Waves.WindSeaSplitCells pressure-buffer texels) is what the simulation's grid can carry: the
 * core's Wave Export adds it to the exported wave height B (User.WaveWindSeaTexture), so the floating bodies feel it and the surface's
 * waves draw it. The short band, down to 10 cm, is drawn per pixel by the surface material (WaveWindSeaTable) as a normal tilt. Nothing of
 * it enters the wave state, the breaking or the foam: the boats' waves and wakes run as before, and the sea adds to them.
 * The sea's direction and height follow the wind (Wind Sea Follows Wind, Wind Sea Reference Wind) with a lag (Wind Sea Response Seconds);
 * the whole field turns about the origin, so a slow turn reads as the sea veering.
 */
namespace FluidNinjaBuoyancyWindSea
{
	/** The first core interface with the wind sea (User.WaveWindSeaTexture). The plugin sets the parameter only on such a core. */
	constexpr int32 InterfaceVersion = 7;
	/** The core's user parameter the table is bound to. */
	extern const FName CoreTextureName;
	/** The surface material's texture parameter the table is bound to (the material step's wavelets). */
	extern const FName MaterialTableName;

	/** What the wind sea takes from its simulation each frame. */
	struct FInput
	{
		/** World seconds (the phases advance by the time since the last update). */
		double Now = 0.0;
		/** World XY of the simulation's window centre (the table's origin follows it in whole steps). */
		FVector2D WindowCentre = FVector2D::ZeroVector;
		/** The pressure buffer's texel, cm: B's grid, which sets the shortest wave the long band may hold. */
		double PressureCellCm = 0.0;
		/** The water's depth for the dispersion, cm. */
		float DepthCm = 200.f;
		/** The wind over the water, world cm/s (FluidNinja Buoyancy's: the project's Wind, a project's own wind source, the bench's override). */
		FVector2D WindCmS = FVector2D::ZeroVector;
		/** Names the simulation's own random sea (its name's hash). */
		uint32 Seed = 0;
	};

	/** One simulation's wind sea: its components, its state from frame to frame and its table texture. */
	class FState
	{
	public:
		/** Advances the sea and writes its table. Returns the table, or null when the sea is off (Wind Sea Height 0, fnb.Waves.WindSea 0, or
		 * no component in range): then nothing is bound and the core and the surfaces add nothing. */
		UTexture2D* Update(const FFluidNinjaWaveSimSettings& Settings, const FInput& Input);
		/** One line for the status: height, peak wavelength, heading and components; empty while off. */
		FString Describe() const;

	private:
		struct FComponent
		{
			double K = 0.0;            // rad/cm
			double Omega = 0.0;        // rad/s
			double Angle = 0.0;        // rad, from the sea's heading
			double Amplitude = 0.0;    // cm per cm of significant height
			double Phase = 0.0;        // rad at the origin
		};

		/** Builds the components when what they depend on changed (the spectrum's settings, the counts, the split, the depth, the seed). */
		void Build(const FFluidNinjaWaveSimSettings& Settings, const FInput& Input, int32 NumLong, int32 NumShort, double SplitCm);
		void Write(const FVector2D& Origin);

		TArray<FComponent> Components;
		int32 NumLongBand = 0;
		uint32 BuiltKey = 0;
		bool bBuilt = false;
		bool bStarted = false;
		double LastNow = 0.0;
		double HeadingRad = 0.0;
		double HeightCm = 0.0;
		double TargetHeightCm = 0.0;
		double PeakCm = 0.0;
		double SplitWavelengthCm = 0.0;
		FVector2D Origin = FVector2D::ZeroVector;
		bool bHasOrigin = false;
		TStrongObjectPtr<UTexture2D> Table;
	};
}
