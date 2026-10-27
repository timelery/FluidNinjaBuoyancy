#include "FluidNinjaBuoyancyRings.h"

#include "Async/ParallelFor.h"
#include "Engine/Texture2D.h"
#include "FluidNinjaBuoyancyLog.h"
#include "FluidNinjaBuoyancySettings.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Math/Float16Color.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace FluidNinjaBuoyancyRings
{
	const FName MaterialTableName(TEXT("WaveRingTable"));
	const FName MaterialLUTName(TEXT("WaveRingLUT"));

	namespace
	{
		int32 RingsOn = 1;
		FAutoConsoleVariableRef CVarRings(TEXT("fnb.Waves.Rings"), RingsOn,
			TEXT("The analytic ring layer (Project Settings: Wave Rings): 1 on (the default), 0 off everywhere (A/B): water contacts send no rings, ")
			TEXT("every simulation's ring table is written empty, and the surfaces draw only the grid's waves. The contacts still press the grid."));
		int32 RingsMax = 64;
		FAutoConsoleVariableRef CVarRingsMax(TEXT("fnb.Waves.RingsMax"), RingsMax,
			TEXT("The most rings one simulation draws at once (each costs a table load and a LUT sample per water pixel within 6 m of it): a new ring ")
			TEXT("past it drops the one that matters least (the smallest drawn amplitude times the share of its life left), so a paddling bird's ")
			TEXT("small rings do not push out a dive's. Default 64; 0 to 128; 0 draws none."));
		float RingsViewCm = 1500.f;
		FAutoConsoleVariableRef CVarRingsViewCm(TEXT("fnb.Waves.RingsViewCm"), RingsViewCm,
			TEXT("When a simulation's ring table is full, rings far from the camera count for less, so the ripples the player looks at keep their ")
			TEXT("slots: a ring's importance (its drawn slope times the share of its life left) is divided by 1 + (distance / this)^2, in cm along ")
			TEXT("the water. Default 1500 (a ring 15 m away counts half, 30 m a fifth); 0 or less: distance does not matter."));

		constexpr double Gravity = 981.0;          // cm/s²
		constexpr double SurfaceTension = 72.8;    // σ / ρ of water, cm³/s²
		constexpr double Density = 0.001;          // kg/cm³
		constexpr double BandSigmas[NumBands] = {2.5, 6.0, 14.0};
		/** The k integral runs to min(this / σ_b, KMaxCap): six σ_b out the band filter is e^-18 of its peak. */
		constexpr double KMaxSigmas = 6.0;
		constexpr double KMaxCap = 3.0;            // rad/cm
		/** At most this many rings in a table (fnb.Waves.RingsMax is clamped to it). */
		constexpr int32 MaxRings = 128;
		/** The table's origin follows the window in steps of this much (cm), so its offsets stay small. */
		constexpr double OriginStepCm = 1000.0;
		/** Rings of one band added in the same frame closer than this (cm) are one. */
		constexpr double MergeCm = 1.0;
		/** B where no wave stands (no group speed matches r / t, or its weight is negligible): wide enough that the shader never fades it. */
		constexpr float NoWavelengthCm = 10000.f;
		/** A stationary-phase root weighs nothing next to the band's strongest wavenumber below this share of its |F k² / ω|. */
		constexpr double NegligibleWeight = 1.0e-3;
		/** The LUT is worked out this many time rows at a time (each radius's Bessel row is used for all of them). */
		constexpr int32 TimeBlock = 8;
		static_assert(LutTimes % TimeBlock == 0, "the LUT's time rows go in whole blocks");
		/** The half float's range: values past it are clamped, not infinite. */
		constexpr double HalfMax = 65000.0;
		/** The depth the LUT is built for is rounded to this (cm). */
		constexpr double DepthStepCm = 10.0;
		/** The wavelength of the slowest gravity-capillary wave (cm): inside r < c_min t no group speed matches, yet the rings' slow tail is
		 * there, no longer than this or the band's shortest significant wave. */
		constexpr double SlowestWavelengthCm = 4.35;
		/** A band's shortest significant wavelength: where its |F k² / ω| has fallen to this share of its peak on the short side. */
		constexpr double SignificantWeight = 0.05;
		/** The rings fade out over the LUT's last quarter (6-8 s), so a ring still showing at its end does not vanish in one frame. */
		constexpr double TailFadeShare = 0.25;

		double TailFade(double Seconds)
		{
			return 1.0 - FMath::SmoothStep(LutSeconds * (1.0 - TailFadeShare), LutSeconds, Seconds);
		}

		/** A ring's source radius in the LUT: beyond four of its band's σ plus 5 cm is the ring, inside it the crater. */
		double OutsideSourceCm(int32 Band)
		{
			return 4.0 * BandSigmas[FMath::Clamp(Band, 0, NumBands - 1)] + 5.0;
		}

		/** Finite-depth gravity-capillary dispersion, rad/s (tanh's argument held at 20). */
		double Omega(double K, double DepthCm)
		{
			return FMath::Sqrt((Gravity * K + SurfaceTension * K * K * K) * FMath::Tanh(FMath::Min(K * DepthCm, 20.0)));
		}

		/** Its group speed dω/dk, cm/s. */
		double GroupSpeed(double K, double DepthCm)
		{
			const double KD = K * DepthCm;
			const double Th = FMath::Tanh(FMath::Min(KD, 20.0));
			const double Sech2 = KD < 20.0 ? 1.0 - Th * Th : 0.0;
			const double W = Omega(K, DepthCm);
			return W > 0.0 ? ((Gravity + 3.0 * SurfaceTension * K * K) * Th + (Gravity * K + SurfaceTension * K * K * K) * DepthCm * Sech2) / (2.0 * W) : 0.0;
		}

		double BandFilter(double K, double SigmaB, double SigmaG)
		{
			return FMath::Exp(-0.5 * K * K * SigmaB * SigmaB) - FMath::Exp(-0.5 * K * K * SigmaG * SigmaG);
		}

		/** J0(x): Abramowitz & Stegun 9.4.1 (|x| <= 3) and 9.4.3 (beyond), absolute error under 1e-7. */
		double BesselJ0(double X)
		{
			const double Ax = FMath::Abs(X);
			if (Ax <= 3.0)
			{
				const double Y = (Ax / 3.0) * (Ax / 3.0);
				return 1.0 + Y * (-2.2499997 + Y * (1.2656208 + Y * (-0.3163866 + Y * (0.0444479 + Y * (-0.0039444 + Y * 0.0002100)))));
			}
			const double Z = 3.0 / Ax;
			const double F0 = 0.79788456 + Z * (-0.00000077 + Z * (-0.00552740 + Z * (-0.00009512 + Z * (0.00137237 + Z * (-0.00072805 + Z * 0.00014476)))));
			const double Theta0 = Ax - 0.78539816 + Z * (-0.04166397 + Z * (-0.00003954 + Z * (0.00262573 + Z * (-0.00054125 + Z * (-0.00029333 + Z * 0.00013558)))));
			return F0 * FMath::Cos(Theta0) / FMath::Sqrt(Ax);
		}

		/** J1(x): Abramowitz & Stegun 9.4.4 (|x| <= 3) and 9.4.6 (beyond), absolute error under 1e-7. */
		double BesselJ1(double X)
		{
			const double Ax = FMath::Abs(X);
			double Value = 0.0;
			if (Ax <= 3.0)
			{
				const double Y = (Ax / 3.0) * (Ax / 3.0);
				Value = Ax * (0.5 + Y * (-0.56249985 + Y * (0.21093573 + Y * (-0.03954289 + Y * (0.00443319 + Y * (-0.00031761 + Y * 0.00001109))))));
			}
			else
			{
				const double Z = 3.0 / Ax;
				const double F1 = 0.79788456 + Z * (0.00000156 + Z * (0.01659667 + Z * (0.00017105 + Z * (-0.00249511 + Z * (0.00113653 + Z * -0.00020033)))));
				const double Theta1 = Ax - 2.35619449 + Z * (0.12499612 + Z * (0.00005650 + Z * (-0.00637879 + Z * (0.00074348 + Z * (0.00079824 + Z * -0.00029166)))));
				Value = F1 * FMath::Cos(Theta1) / FMath::Sqrt(Ax);
			}
			return X < 0.0 ? -Value : Value;
		}

		/** The radius (cm) and time (s) at a LUT texel's centre. */
		double TexelRadius(int32 Index)
		{
			const double U = (Index + 0.5) / LutRadii;
			return LutRadiusCm * U * U;
		}

		double TexelSeconds(int32 Row)
		{
			return LutSeconds * (Row + 0.5) / LutTimes;
		}

		double GridSigmaForBand(double WaveCellCm, int32 Band)
		{
			return FMath::Max(2.0 * WaveCellCm, BandSigmaCm(Band));
		}

		/** One band's response on its wavenumbers: the midpoint rule from dk / 2 to k_max, dk at most π / (2 LutRadiusCm) (four samples a
		 * period of J0(k r) at the LUT's edge). */
		struct FBandModel
		{
			double SigmaB = 0.0;
			double SigmaG = 0.0;
			double DepthCm = 0.0;
			int32 K = 0;
			double Dk = 0.0;
			TArray<double> Wavenumber;
			TArray<double> Frequency;
			/** -(I / ρ) F (k tanh(k d) / ω) k dk / 2π: η̂'s k dk / 2π share without its sin(ω t) and decay. */
			TArray<double> Coefficient;
			/** 2 ν k², 1/s. */
			TArray<double> Decay;
			TArray<double> Speed;
			/** The slowest group speed's index: the gravity branch (the group speed falling with k) lies before it, the capillary branch (rising)
			 * after it. */
			int32 SlowestIndex = 0;
			double WeightMax = 0.0;
			/** B inside the slow zone (r < c_min t, and at r = 0): the slowest wave's length or the band's shortest significant one. */
			float SlowZoneWavelengthCm = static_cast<float>(SlowestWavelengthCm);

			void Build(int32 Band, double InSigmaG, double InDepthCm, double Viscosity)
			{
				SigmaB = BandSigmaCm(Band);
				SigmaG = InSigmaG;
				DepthCm = InDepthCm;
				const double KMax = FMath::Min(KMaxSigmas / SigmaB, KMaxCap);
				const double DkMax = UE_DOUBLE_PI / (2.0 * LutRadiusCm);
				K = FMath::Max(FMath::CeilToInt32(KMax / DkMax), 1);
				Dk = KMax / K;
				Wavenumber.SetNumUninitialized(K);
				Frequency.SetNumUninitialized(K);
				Coefficient.SetNumUninitialized(K);
				Decay.SetNumUninitialized(K);
				Speed.SetNumUninitialized(K);
				WeightMax = 0.0;
				SlowestIndex = 0;
				for (int32 N = 0; N < K; ++N)
				{
					const double Kn = (N + 0.5) * Dk;
					const double W = Omega(Kn, DepthCm);
					const double F = BandFilter(Kn, SigmaB, SigmaG);
					Wavenumber[N] = Kn;
					Frequency[N] = W;
					Coefficient[N] = W > 0.0 ? -(LutUnitImpulse / Density) * F * (Kn * FMath::Tanh(FMath::Min(Kn * DepthCm, 20.0)) / W) * Kn * Dk / (2.0 * UE_DOUBLE_PI) : 0.0;
					Decay[N] = 2.0 * Viscosity * Kn * Kn;
					Speed[N] = GroupSpeed(Kn, DepthCm);
					WeightMax = FMath::Max(WeightMax, Weight(Kn));
					if (Speed[N] < Speed[SlowestIndex])
					{
						SlowestIndex = N;
					}
				}
				double ShortestK = Wavenumber.IsEmpty() ? 0.0 : Wavenumber.Last();
				for (int32 N = K - 1; N >= 0; --N)
				{
					if (Weight(Wavenumber[N]) >= SignificantWeight * WeightMax)
					{
						ShortestK = Wavenumber[N];
						break;
					}
				}
				SlowZoneWavelengthCm = static_cast<float>(ShortestK > 0.0 ? FMath::Max(SlowestWavelengthCm, 2.0 * UE_DOUBLE_PI / ShortestK) : SlowestWavelengthCm);
			}

			/** How much a wavenumber stands out in the slope's integrand: |F k² / ω|. */
			double Weight(double Kn) const
			{
				const double W = Omega(Kn, DepthCm);
				return W > 0.0 ? FMath::Abs(BandFilter(Kn, SigmaB, SigmaG) * Kn * Kn / W) : 0.0;
			}

			/** η̂'s k dk / 2π share at wavenumber N and time Seconds. */
			double Amplitude(int32 N, double Seconds) const
			{
				return Coefficient[N] * FMath::Sin(Frequency[N] * Seconds) * FMath::Exp(-Decay[N] * Seconds);
			}

			/** The integrals at (r, t), Bessel functions worked out here: ∂η/∂r (cm per cm) and η (cm). */
			void Evaluate(double RadiusCm, double Seconds, double& OutSlope, double& OutEta) const
			{
				OutSlope = 0.0;
				OutEta = 0.0;
				for (int32 N = 0; N < K; ++N)
				{
					const double A = Amplitude(N, Seconds);
					const double X = Wavenumber[N] * RadiusCm;
					OutEta += A * BesselJ0(X);
					OutSlope -= A * Wavenumber[N] * BesselJ1(X);
				}
			}

			/** Between neighbouring wavenumbers Lo and Hi, the one whose group speed is C. */
			double Crossing(int32 Lo, int32 Hi, double C) const
			{
				const double Span = Speed[Hi] - Speed[Lo];
				if (Lo == Hi || FMath::Abs(Span) < 1.0e-12)
				{
					return Wavenumber[Lo];
				}
				return Wavenumber[Lo] + (Wavenumber[Hi] - Wavenumber[Lo]) * FMath::Clamp((C - Speed[Lo]) / Span, 0.0, 1.0);
			}

			/** The local dominant wavelength (cm) where r / t = C: by stationary phase, the wavenumber whose group speed is C; of a gravity and a
			 * capillary root the one with the larger weight; inside the slow zone (C below the slowest group speed: no root, but the rings'
			 * slow tail of short waves) SlowZoneWavelengthCm, so the shader fades it at a distance; NoWavelengthCm where a root weighs nothing. */
			float Wavelength(double C) const
			{
				if (K <= 0)
				{
					return NoWavelengthCm;
				}
				if (!(C > Speed[SlowestIndex]))
				{
					return SlowZoneWavelengthCm;
				}
				double BestK = 0.0;
				double BestWeight = -1.0;
				const auto Consider = [&](double Kn)
				{
					const double W = Weight(Kn);
					if (W > BestWeight)
					{
						BestWeight = W;
						BestK = Kn;
					}
				};
				// The gravity branch: the group speed falls from the first wavenumber to the slowest.
				if (C <= Speed[0] && C >= Speed[SlowestIndex])
				{
					int32 Lo = 0;
					int32 Hi = SlowestIndex;
					while (Hi - Lo > 1)
					{
						const int32 Mid = (Lo + Hi) / 2;
						if (Speed[Mid] >= C)
						{
							Lo = Mid;
						}
						else
						{
							Hi = Mid;
						}
					}
					Consider(Crossing(Lo, Hi, C));
				}
				// The capillary branch: it rises from the slowest to k_max.
				if (C >= Speed[SlowestIndex] && C <= Speed[K - 1])
				{
					int32 Lo = SlowestIndex;
					int32 Hi = K - 1;
					while (Hi - Lo > 1)
					{
						const int32 Mid = (Lo + Hi) / 2;
						if (Speed[Mid] <= C)
						{
							Lo = Mid;
						}
						else
						{
							Hi = Mid;
						}
					}
					Consider(Crossing(Lo, Hi, C));
				}
				if (BestK <= 0.0 || BestWeight < NegligibleWeight * WeightMax)
				{
					return NoWavelengthCm;
				}
				return static_cast<float>(FMath::Min(2.0 * UE_DOUBLE_PI / BestK, static_cast<double>(NoWavelengthCm)));
			}
		};

		float ToHalfRange(double Value)
		{
			return static_cast<float>(FMath::Clamp(Value, -HalfMax, HalfMax));
		}

		/** The whole LUT, row-major (LutRadii texels a row, band b's row j at b LutTimes + j), and the bands' models. ParallelFor over the
		 * radii for the Bessel tables (J0 and J1 at every wavenumber, kept per band for all times) and over blocks of time rows. */
		void GenerateLut(double WaveCellCm, double DepthCm, double Viscosity, TArray<FFloat16Color>& Out, FBandModel (&Models)[NumBands])
		{
			for (int32 Band = 0; Band < NumBands; ++Band)
			{
				Models[Band].Build(Band, GridSigmaForBand(WaveCellCm, Band), DepthCm, Viscosity);
			}
			TArray<double> J0[NumBands];
			TArray<double> J1[NumBands];
			for (int32 Band = 0; Band < NumBands; ++Band)
			{
				J0[Band].SetNumUninitialized(LutRadii * Models[Band].K);
				J1[Band].SetNumUninitialized(LutRadii * Models[Band].K);
			}
			ParallelFor(NumBands * LutRadii, [&](int32 Index)
			{
				const int32 Band = Index / LutRadii;
				const int32 Radius = Index % LutRadii;
				const FBandModel& Model = Models[Band];
				const double R = TexelRadius(Radius);
				double* Row0 = J0[Band].GetData() + static_cast<int64>(Radius) * Model.K;
				double* Row1 = J1[Band].GetData() + static_cast<int64>(Radius) * Model.K;
				for (int32 N = 0; N < Model.K; ++N)
				{
					const double X = Model.Wavenumber[N] * R;
					Row0[N] = BesselJ0(X);
					Row1[N] = BesselJ1(X);
				}
			});
			Out.SetNumUninitialized(LutRadii * LutTimes * NumBands);
			constexpr int32 Blocks = LutTimes / TimeBlock;
			ParallelFor(NumBands * Blocks, [&](int32 Index)
			{
				const int32 Band = Index / Blocks;
				const int32 FirstRow = (Index % Blocks) * TimeBlock;
				const FBandModel& Model = Models[Band];
				double Seconds[TimeBlock];
				for (int32 Q = 0; Q < TimeBlock; ++Q)
				{
					Seconds[Q] = TexelSeconds(FirstRow + Q);
				}
				// Per wavenumber, the block's times side by side: η's weights and the slope's (-k times them).
				TArray<double> EtaWeights;
				TArray<double> SlopeWeights;
				EtaWeights.SetNumUninitialized(Model.K * TimeBlock);
				SlopeWeights.SetNumUninitialized(Model.K * TimeBlock);
				for (int32 N = 0; N < Model.K; ++N)
				{
					for (int32 Q = 0; Q < TimeBlock; ++Q)
					{
						const double A = Model.Amplitude(N, Seconds[Q]);
						EtaWeights[N * TimeBlock + Q] = A;
						SlopeWeights[N * TimeBlock + Q] = -A * Model.Wavenumber[N];
					}
				}
				for (int32 Radius = 0; Radius < LutRadii; ++Radius)
				{
					double Eta[TimeBlock] = {};
					double Slope[TimeBlock] = {};
					const double* Row0 = J0[Band].GetData() + static_cast<int64>(Radius) * Model.K;
					const double* Row1 = J1[Band].GetData() + static_cast<int64>(Radius) * Model.K;
					const double* EtaWeight = EtaWeights.GetData();
					const double* SlopeWeight = SlopeWeights.GetData();
					for (int32 N = 0; N < Model.K; ++N, EtaWeight += TimeBlock, SlopeWeight += TimeBlock)
					{
						const double Bessel0 = Row0[N];
						const double Bessel1 = Row1[N];
						for (int32 Q = 0; Q < TimeBlock; ++Q)
						{
							Eta[Q] += EtaWeight[Q] * Bessel0;
							Slope[Q] += SlopeWeight[Q] * Bessel1;
						}
					}
					const double R = TexelRadius(Radius);
					for (int32 Q = 0; Q < TimeBlock; ++Q)
					{
						const double Fade = TailFade(Seconds[Q]);
						Out[(Band * LutTimes + FirstRow + Q) * LutRadii + Radius] = FFloat16Color(FLinearColor(ToHalfRange(Slope[Q] * Fade), ToHalfRange(Eta[Q] * Fade),
							Model.Wavelength(R / Seconds[Q]), 0.f));
					}
				}
			});
		}

		/** The LUT at (r, t) as the shader samples it: u = √(r / R), the band's row from the age, bilinear between the four nearest texels. */
		FLinearColor SampleLut(const TArray<FFloat16Color>& Data, int32 Band, double RadiusCm, double Seconds)
		{
			const double X = FMath::Sqrt(FMath::Clamp(RadiusCm / LutRadiusCm, 0.0, 1.0)) * LutRadii - 0.5;
			const double Y = Band * LutTimes + FMath::Clamp(Seconds / LutSeconds * LutTimes - 0.5, 0.0, LutTimes - 1.0);
			const int32 X0 = FMath::FloorToInt32(X);
			const int32 Y0 = FMath::FloorToInt32(Y);
			const float Fx = static_cast<float>(X - X0);
			const float Fy = static_cast<float>(Y - Y0);
			const auto Texel = [&Data](int32 Column, int32 Row)
			{
				return FLinearColor(Data[FMath::Clamp(Row, 0, NumBands * LutTimes - 1) * LutRadii + FMath::Clamp(Column, 0, LutRadii - 1)]);
			};
			const FLinearColor Bottom = Texel(X0, Y0) * (1.f - Fx) + Texel(X0 + 1, Y0) * Fx;
			const FLinearColor Top = Texel(X0, Y0 + 1) * (1.f - Fx) + Texel(X0 + 1, Y0 + 1) * Fx;
			return Bottom * (1.f - Fy) + Top * Fy;
		}

		double LutDepth(float DepthCm)
		{
			return FMath::Max(DepthStepCm, FMath::RoundToDouble(static_cast<double>(DepthCm) / DepthStepCm) * DepthStepCm);
		}

		double LutViscosityFrom(const UFluidNinjaBuoyancySettings& Settings)
		{
			return FMath::Clamp(static_cast<double>(Settings.WaveRingViscosityCm2S), 0.01, 1.0);
		}
	}

	double BandSigmaCm(int32 Band)
	{
		return BandSigmas[FMath::Clamp(Band, 0, NumBands - 1)];
	}

	int32 BandForRadius(float RadiusCm)
	{
		const double Sigma = 0.5 * FMath::Clamp(static_cast<double>(RadiusCm), 0.5, 200.0);
		int32 Best = 0;
		double BestDistance = TNumericLimits<double>::Max();
		for (int32 Band = 0; Band < NumBands; ++Band)
		{
			const double Distance = FMath::Abs(FMath::Loge(Sigma / BandSigmas[Band]));
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Best = Band;
			}
		}
		return Best;
	}

	bool IsOn()
	{
		return RingsOn != 0 && UFluidNinjaBuoyancySettings::Get().bWaveRings;
	}

	double FState::GridSigmaCm(double WaveCellCm, float RadiusCm)
	{
		return GridSigmaForBand(WaveCellCm, BandForRadius(RadiusCm));
	}

	void FState::Add(const FVector2D& WorldXY, double Now, float ImpulseDown, float RadiusCm)
	{
		const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
		const int32 Max = FMath::Clamp(RingsMax, 0, MaxRings);
		if (!IsOn() || Max == 0 || !FMath::IsFinite(ImpulseDown) || ImpulseDown == 0.f || WorldXY.ContainsNaN())
		{
			return;
		}
		const int32 Band = BandForRadius(RadiusCm);
		const float Amplitude = static_cast<float>(ImpulseDown * FMath::Max(Settings.WaveImpulseScale, 0.f) * FMath::Max(Settings.WaveRingStrength, 0.f) / LutUnitImpulse);
		if (Amplitude == 0.f)
		{
			return;
		}
		// This frame's rings are the newest, at the end.
		const uint64 Frame = GFrameCounter;
		for (int32 Index = Rings.Num() - 1; Index >= 0 && Rings[Index].Frame == Frame; --Index)
		{
			FRing& Ring = Rings[Index];
			if (Ring.Band == Band && FVector2D::DistSquared(Ring.Location, WorldXY) <= MergeCm * MergeCm)
			{
				Ring.Amplitude += Amplitude;
				return;
			}
		}
		while (Rings.Num() >= Max)
		{
			Rings.RemoveAt(LeastImportant(Now));   // order kept: this frame's rings stay at the end for the merge
		}
		FRing& Ring = Rings.AddDefaulted_GetRef();
		Ring.Location = WorldXY;
		Ring.Born = Now;
		Ring.Band = Band;
		Ring.Amplitude = Amplitude;
		Ring.Frame = Frame;
	}

	bool FState::Update(double Now, const FVector2D& WindowCentre, double WaveCellCm, float DepthCm)
	{
		if (!IsOn() || !(WaveCellCm > 0.0))
		{
			Clear();
			return false;
		}
		Rings.RemoveAll([Now](const FRing& Ring) { return Now - Ring.Born >= LutSeconds; });
		const int32 Max = FMath::Clamp(RingsMax, 0, MaxRings);
		while (Rings.Num() > Max)
		{
			Rings.RemoveAt(LeastImportant(Now));
		}
		if (Rings.IsEmpty())
		{
			if (bTableHasRings)
			{
				WriteTable(Now);   // N = 0: whatever is still bound to it draws nothing
			}
			return false;
		}
		// The LUT for this grid (its σ_g), depth and viscosity; built again only when one of them changes.
		const double Depth = LutDepth(DepthCm);
		const double Viscosity = LutViscosityFrom(UFluidNinjaBuoyancySettings::Get());
		const bool bNewKey = !FMath::IsNearlyEqual(LutCellCm, WaveCellCm, WaveCellCm * 1.0e-4) || LutDepthCm != Depth || LutViscosity != Viscosity;
		if (bNewKey || (!Lut.IsValid() && !bLutFailed))
		{
			BuildLut(WaveCellCm, Depth, Viscosity);
		}
		if (!Lut.IsValid())
		{
			// No LUT to draw them with (its texture could not be made, logged): the rings go and the table is written empty, so a surface
			// still holding an older LUT does not go on drawing the last rings it was given, frozen.
			Clear();
			return false;
		}
		Origin = FVector2D(FMath::RoundToDouble(WindowCentre.X / OriginStepCm) * OriginStepCm, FMath::RoundToDouble(WindowCentre.Y / OriginStepCm) * OriginStepCm);
		WriteTable(Now);
		return Table.IsValid() && bTableHasRings;
	}

	void FState::Clear()
	{
		Rings.Reset();
		if (bTableHasRings && Table.IsValid())
		{
			WriteTable(0.0);
		}
		bTableHasRings = false;
	}

	void FState::BuildLut(double WaveCellCm, double DepthCm, double Viscosity)
	{
		const double Start = FPlatformTime::Seconds();
		// What it is for, failed or not (a failure is not tried again every frame, only when this changes).
		LutCellCm = WaveCellCm;
		LutDepthCm = DepthCm;
		LutViscosity = Viscosity;
		bLutFailed = false;
		TArray<FFloat16Color> Data;
		FBandModel Models[NumBands];
		GenerateLut(WaveCellCm, DepthCm, Viscosity, Data, Models);
		// A new texture each build (rare: a new grid, depth or viscosity), its texels in the mip's bulk data before its resource is made.
		const FName Name = MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(), TEXT("FNB_WaveRingLUT"));
		UTexture2D* Texture = UTexture2D::CreateTransient(LutRadii, LutTimes * NumBands, PF_FloatRGBA, Name);
		if (!Texture || !Texture->GetPlatformData() || Texture->GetPlatformData()->Mips.IsEmpty())
		{
			UE_LOG(LogFluidNinjaBuoyancy, Error, TEXT("FluidNinja Buoyancy rings: could not make the %d x %d RGBA16F ring LUT; no rings are drawn."), LutRadii, LutTimes * NumBands);
			Lut.Reset();
			bLutFailed = true;
			return;
		}
		Texture->Filter = TF_Bilinear;
		Texture->SRGB = false;
		Texture->CompressionSettings = TC_HDR;   // a Linear Color sampler, as the material's WaveRingLUT has
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->NeverStream = true;
		FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
		void* Texels = Mip.BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(Texels, Data.GetData(), Data.Num() * sizeof(FFloat16Color));
		Mip.BulkData.Unlock();
		Texture->UpdateResource();
		Lut.Reset(Texture);
		// Each band's steepest slope outside its source, as the half floats hold it: what Wave Ring Max Slope holds a ring's amplitude to.
		for (int32 Band = 0; Band < NumBands; ++Band)
		{
			PeakSlope[Band] = 0.0;
			int32 FirstRadius = 0;
			while (FirstRadius < LutRadii && TexelRadius(FirstRadius) < OutsideSourceCm(Band))
			{
				++FirstRadius;
			}
			for (int32 Row = Band * LutTimes; Row < (Band + 1) * LutTimes; ++Row)
			{
				for (int32 Radius = FirstRadius; Radius < LutRadii; ++Radius)
				{
					PeakSlope[Band] = FMath::Max(PeakSlope[Band], static_cast<double>(FMath::Abs(Data[Row * LutRadii + Radius].R.GetFloat())));
				}
			}
		}
		LutBuildMs = (FPlatformTime::Seconds() - Start) * 1000.0;
		UE_LOG(LogFluidNinjaBuoyancy, Log, TEXT("FluidNinja Buoyancy rings: LUT built in %.1f ms (%d x %d RGBA16F, %d + %d + %d wavenumbers; wave cell %.2f cm, grid sigma %.2f / %.2f / %.2f cm, depth %.0f cm, viscosity %.3f cm2/s; peak slope per %.0f kg*cm/s %.4f / %.4f / %.4f)."),
			LutBuildMs, LutRadii, LutTimes * NumBands, Models[0].K, Models[1].K, Models[2].K, WaveCellCm, Models[0].SigmaG, Models[1].SigmaG, Models[2].SigmaG, DepthCm, Viscosity,
			LutUnitImpulse, PeakSlope[0], PeakSlope[1], PeakSlope[2]);
	}

	void FState::WriteTable(double Now)
	{
		const int32 Count = Rings.Num();
		const int32 Width = Count + 2;
		// Made again only when it must grow (room for a power of two of rings, 16 at least); the shader reads N from the header, not the width.
		if (!Table.IsValid() || TableWidth < Width)
		{
			const int32 NewWidth = 2 + FMath::Max(16, static_cast<int32>(FMath::RoundUpToPowerOfTwo(static_cast<uint32>(FMath::Max(Count, 1)))));
			const FName Name = MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(), TEXT("FNB_WaveRingTable"));
			UTexture2D* Texture = UTexture2D::CreateTransient(NewWidth, 1, PF_A32B32G32R32F, Name);
			if (!Texture)
			{
				return;
			}
			Texture->Filter = TF_Nearest;
			Texture->SRGB = false;
			Texture->CompressionSettings = TC_HDR;   // a Linear Color sampler, as the material's WaveRingTable has
			Texture->AddressX = TA_Clamp;
			Texture->AddressY = TA_Clamp;
			Texture->NeverStream = true;
			Texture->UpdateResource();
			Table.Reset(Texture);
			TableWidth = NewWidth;
		}
		TArray<FVector4f>* Data = new TArray<FVector4f>();
		Data->SetNumZeroed(TableWidth);
		(*Data)[0] = FVector4f(static_cast<float>(Origin.X), static_cast<float>(Origin.Y), static_cast<float>(Count), 0.f);
		(*Data)[1] = FVector4f(static_cast<float>(LutRadiusCm), static_cast<float>(LutSeconds), static_cast<float>(NumBands), static_cast<float>(LutTimes));
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FRing& Ring = Rings[Index];
			const double Age = FMath::Clamp(Now - Ring.Born, 0.0, LutSeconds * (1.0 - 1.0e-6));
			(*Data)[Index + 2] = FVector4f(static_cast<float>(Ring.Location.X - Origin.X), static_cast<float>(Ring.Location.Y - Origin.Y),
				static_cast<float>(Age + 16.0 * Ring.Band), DrawnAmplitude(Ring));
		}
		FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, TableWidth, 1);
		Table->UpdateTextureRegions(0, 1, Region, TableWidth * sizeof(FVector4f), sizeof(FVector4f), reinterpret_cast<uint8*>(Data->GetData()),
			[Data](uint8*, const FUpdateTextureRegion2D* Regions)
			{
				delete Data;
				delete Regions;
			});
		bTableHasRings = Count > 0;
	}

	float FState::DrawnAmplitude(const FRing& Ring) const
	{
		const double Peak = PeakSlope[FMath::Clamp(Ring.Band, 0, NumBands - 1)];
		if (!Lut.IsValid() || !(Peak > 0.0))
		{
			return Ring.Amplitude;
		}
		const double Cap = FMath::Max(static_cast<double>(UFluidNinjaBuoyancySettings::Get().WaveRingMaxSlope), 0.0) / Peak;
		return static_cast<float>(FMath::Clamp(static_cast<double>(Ring.Amplitude), -Cap, Cap));
	}

	int32 FState::LeastImportant(double Now) const
	{
		int32 Least = 0;
		double LeastWeight = TNumericLimits<double>::Max();
		for (int32 Index = 0; Index < Rings.Num(); ++Index)
		{
			const FRing& Ring = Rings[Index];
			const double Left = FMath::Clamp(1.0 - (Now - Ring.Born) / LutSeconds, 0.0, 1.0);
			double Weight = FMath::Abs(static_cast<double>(DrawnAmplitude(Ring))) * PeakSlope[FMath::Clamp(Ring.Band, 0, NumBands - 1)] * Left;
			if (bHasView && RingsViewCm > 0.f)
			{
				Weight /= 1.0 + FVector2D::DistSquared(Ring.Location, ViewXY) / FMath::Square(static_cast<double>(RingsViewCm));
			}
			if (Weight < LeastWeight)
			{
				LeastWeight = Weight;
				Least = Index;
			}
		}
		return Least;
	}

	FString FState::Describe() const
	{
		if (!Lut.IsValid())
		{
			return FString();
		}
		return FString::Printf(TEXT("rings %d (wave cell %.2f cm: grid sigma %.1f / %.1f / %.1f cm over bands of %.1f / %.1f / %.1f cm, depth %.0f cm, viscosity %.3f cm2/s, LUT built in %.0f ms)"),
			Rings.Num(), LutCellCm, GridSigmaForBand(LutCellCm, 0), GridSigmaForBand(LutCellCm, 1), GridSigmaForBand(LutCellCm, 2),
			BandSigmas[0], BandSigmas[1], BandSigmas[2], LutDepthCm, LutViscosity, LutBuildMs);
	}

	bool FState::LogLut(const FString& SimName) const
	{
		if (!Lut.IsValid())
		{
			return false;
		}
		TArray<FFloat16Color> Data;
		FBandModel Models[NumBands];
		GenerateLut(LutCellCm, LutDepthCm, LutViscosity, Data, Models);
		UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy rings dump: %s, wave cell %.3f cm, depth %.0f cm, viscosity %.3f cm2/s, %d rings now. Values per %.0f kg*cm/s (no Wave Impulse Scale or Wave Ring Strength): R = d eta/dr (cm per cm), G = eta (cm), B = local wavelength (cm, 10000 = none); 'exact' is the integral at (r, t), 'read' the LUT as the shader samples it (bilinear on its half floats)."),
			*SimName, LutCellCm, LutDepthCm, LutViscosity, Rings.Num(), LutUnitImpulse);
		static const double Radii[] = {25.0, 50.0, 100.0, 200.0};
		static const double Times[] = {0.5, 1.0, 2.0, 4.0};
		for (int32 Band = 0; Band < NumBands; ++Band)
		{
			const FBandModel& Model = Models[Band];
			UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy rings dump: band %d: source sigma_b %.2f cm, grid sigma_g %.3f cm, depth %.0f cm, %d wavenumbers of %.6f rad/cm up to %.4f rad/cm."),
				Band, Model.SigmaB, Model.SigmaG, Model.DepthCm, Model.K, Model.Dk, Model.Dk * Model.K);
			for (const double Seconds : Times)
			{
				for (const double Radius : Radii)
				{
					double Slope = 0.0;
					double Eta = 0.0;
					Model.Evaluate(Radius, Seconds, Slope, Eta);
					const float Wavelength = Model.Wavelength(Radius / Seconds);
					const FLinearColor Read = SampleLut(Data, Band, Radius, Seconds);
					UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy rings dump: band %d t %.2f s r %3.0f cm: exact R %+.6e G %+.6e B %.1f | read R %+.6e G %+.6e B %.1f"),
						Band, Seconds, Radius, Slope, Eta, Wavelength, Read.R, Read.G, Read.B);
				}
			}
		}
		return true;
	}
}
