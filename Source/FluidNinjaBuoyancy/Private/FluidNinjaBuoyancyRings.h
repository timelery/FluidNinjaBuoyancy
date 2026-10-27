#pragma once

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"

class UTexture2D;

/**
 * FluidNinja Waves' ring layer: the fine ring ripples a small contact makes (a bird's beak, feet and body going in, a dip, a take-off),
 * which a simulation's grid is too coarse to carry (a wave cell of 11.7 cm holds nothing shorter than about 47 cm). The same impulse goes
 * in twice, split by wavelength so that nothing is counted twice: the grid gets it splatted as a Gaussian of σ_g = max(2 wave cells, the
 * band's σ_b) (UFluidNinjaBuoyancySubsystem::AddWaterContact's poke), and this layer draws the rest of its spectrum analytically, the band
 * filter F(k) = exp(-k² σ_b² / 2) - exp(-k² σ_g² / 2), as the exact linear Cauchy-Poisson response of a surface pressure impulse:
 *   η̂(k, t) = -(I / ρ) F(k) (k tanh(k d) / ω) sin(ω t) e^(-2 ν k² t),   ω² = (g k + σ/ρ k³) tanh(k d),
 *   η(r, t) = 1/(2π) ∫ η̂ J0(k r) k dk,   ∂η/∂r = -1/(2π) ∫ η̂ J1(k r) k² dk.
 * Each simulation with rings keeps a LUT of that response for an impulse of LutUnitImpulse (one band per source σ, NumBands of them),
 * worked out on the CPU when its grid σ, depth or the viscosity changes, and a small table of its recent rings, written every frame:
 *   LUT   LutRadii x (LutTimes x NumBands) RGBA16F, bilinear: texel (i, b M + j) holds band b at r = LutRadiusCm u², u = (i + 0.5) / LutRadii,
 *         and t = LutSeconds (j + 0.5) / M: R = ∂η/∂r (cm per cm), G = η (cm), B = the local dominant wavelength (cm, 10000 where none);
 *   table (N + 2) x 1 RGBA32F, nearest: texel 0 = (origin x, origin y, N, 0), texel 1 = (LutRadiusCm, LutSeconds, NumBands, M),
 *         texel 2 + i = (x_i - origin x, y_i - origin y, age_i + 16 band_i, amplitude_i), amplitude = I Wave Impulse Scale Wave Ring
 *         Strength / LutUnitImpulse.
 * The surface material (WaveRingTable, WaveRingLUT) sums the rings' slopes per pixel into its normal. Nothing of it enters the wave state,
 * the foam or what the bodies feel: the rings are centimetres high and short, and the grid keeps every wave a body can feel.
 */
namespace FluidNinjaBuoyancyRings
{
	/** The surface material's texture parameters the table and the LUT are bound to. */
	extern const FName MaterialTableName;   // "WaveRingTable"
	extern const FName MaterialLUTName;     // "WaveRingLUT"

	/** The bands (each its own source σ, BandSigmaCm) and the LUT's reach: radius (cm), time (s) and the impulse its values are for (kg·cm/s,
	 * 1 N·s). */
	constexpr int32 NumBands = 3;
	constexpr double LutRadiusCm = 600.0;
	constexpr double LutSeconds = 8.0;
	constexpr double LutUnitImpulse = 100.0;
	/** The LUT's texels: radii across (Nr), time rows per band (M). */
	constexpr int32 LutRadii = 512;
	constexpr int32 LutTimes = 512;

	/** A band's source σ, cm (2.5, 6 and 14). */
	double BandSigmaCm(int32 Band);
	/** The band of a contact RadiusCm wide: its source σ is half its radius (a disc of radius a has a spectrum of about e^(-k² a² / 8)), and
	 * the band is the one whose σ is nearest in log. */
	int32 BandForRadius(float RadiusCm);
	/** The ring layer is on: the project's Wave Rings and fnb.Waves.Rings. */
	bool IsOn();

	/** One simulation's rings: the recent impacts, their table and the LUT for its grid. Game thread. */
	class FState
	{
	public:
		/** A ring of ImpulseDown (kg·cm/s, + presses the water down; the impulse as it happened, with no scale on it) from a contact RadiusCm
		 * wide at WorldXY, born at Now (world seconds). Rings live LutSeconds; past fnb.Waves.RingsMax the oldest is dropped. A ring of the
		 * same band within 1 cm of one added this frame merges into it (their amplitudes add). */
		void Add(const FVector2D& WorldXY, double Now, float ImpulseDown, float RadiusCm);
		/** Ages and drops the rings, (re)builds the LUT if needed (the wave cell the grid runs on, the depth, the viscosity) and writes the
		 * table; returns false when there is nothing to draw (no rings, the layer off): then the table holds N = 0 (written once), so a
		 * material still bound to it draws nothing. */
		bool Update(double Now, const FVector2D& WindowCentre, double WaveCellCm, float DepthCm);
		/** Drops every ring and writes the table empty (N = 0) if it held any: before the state goes (its simulation stopped), since the
		 * surfaces keep the texture bound. */
		void Clear();
		UTexture2D* GetTable() const { return Table.Get(); }
		UTexture2D* GetLUT() const { return Lut.Get(); }
		int32 Num() const { return Rings.Num(); }
		/** One line: the rings, the LUT's grid σ, depth, viscosity and build time; empty before any LUT. */
		FString Describe() const;
		/** fnb.Waves.RingsDump: logs the LUT's R and G (and B) at r = 25, 50, 100, 200 cm and t = 0.5, 1, 2, 4 s for every band, the exact
		 * integral beside what the shader reads (the LUT generated again and sampled bilinearly as the GPU does), with σ_g and the depth.
		 * False (nothing logged) before any LUT. */
		bool LogLut(const FString& SimName) const;
		/** The σ (cm) the grid's share of a contact RadiusCm wide is splatted with on a grid of WaveCellCm cells: max(2 cells, its band's σ). */
		static double GridSigmaCm(double WaveCellCm, float RadiusCm);

	private:
		struct FRing
		{
			FVector2D Location = FVector2D::ZeroVector;   // world cm
			double Born = 0.0;                            // world s
			int32 Band = 0;
			float Amplitude = 0.f;                        // LUT units (the table's w)
			uint64 Frame = 0;                             // GFrameCounter when added (the merge)
		};

		void BuildLut(double WaveCellCm, double DepthCm, double Viscosity);
		void WriteTable(double Now);
		/** The amplitude a ring draws with: its own, held to Wave Ring Max Slope by its band's steepest LUT slope (no LUT yet: its own). */
		float DrawnAmplitude(const FRing& Ring) const;
		/** The ring that matters least now (the smallest drawn amplitude still to come: |a| x the share of its life left). */
		int32 LeastImportant(double Now) const;

		TArray<FRing> Rings;
		TStrongObjectPtr<UTexture2D> Table;
		int32 TableWidth = 0;
		/** The table's last write held rings (so emptying it writes N = 0 once). */
		bool bTableHasRings = false;
		FVector2D Origin = FVector2D::ZeroVector;
		TStrongObjectPtr<UTexture2D> Lut;
		/** What the LUT was built for (the wave cell, the depth rounded to 10 cm, the viscosity) and how long it took. */
		double LutCellCm = 0.0;
		double LutDepthCm = 0.0;
		double LutViscosity = 0.0;
		double LutBuildMs = 0.0;
		/** Each band's steepest |d eta/dr| per LUT unit outside its source (r of at least four σ_b plus 5 cm), over the LUT's whole life. */
		double PeakSlope[NumBands] = {};
		/** The last build for that key could not make its texture (logged): not tried again until the key changes. */
		bool bLutFailed = false;
	};
}
