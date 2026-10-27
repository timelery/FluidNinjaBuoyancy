#include "FluidNinjaBuoyancyWindSea.h"

#include "Engine/Texture2D.h"
#include "FluidNinjaBuoyancySettings.h"
#include "HAL/IConsoleManager.h"
#include "Math/RandomStream.h"
#include "RenderingThread.h"
#include "TextureResource.h"

namespace FluidNinjaBuoyancyWindSea
{
	const FName CoreTextureName(TEXT("User.WaveWindSeaTexture"));
	const FName MaterialTableName(TEXT("WaveWindSeaTable"));

	namespace
	{
		int32 WindSeaOn = 1;
		FAutoConsoleVariableRef CVarWindSea(TEXT("fnb.Waves.WindSea"), WindSeaOn,
			TEXT("The wind sea (Wave Sims: Wind Sea Height Cm): 1 on where a simulation asks for one (the default), 0 off everywhere (A/B): no table ")
			TEXT("is bound, so the core adds nothing to B and the surfaces draw no wavelets. Needs a core at interface version 7."));
		int32 WindSeaLong = 24;
		FAutoConsoleVariableRef CVarWindSeaLong(TEXT("fnb.Waves.WindSeaLong"), WindSeaLong,
			TEXT("Plane waves in the wind sea's long band, the waves the simulation's grid carries: the core adds them to B, so the floating bodies ")
			TEXT("feel them. More: a less regular sea, at one cosine per pressure-buffer texel each (24 on a 1024 x 1024 buffer is about 0.05 ms). ")
			TEXT("Default 24; 0 to 64; 0 leaves only the wavelets. A change rebuilds the sea."));
		int32 WindSeaShort = 16;
		FAutoConsoleVariableRef CVarWindSeaShort(TEXT("fnb.Waves.WindSeaShort"), WindSeaShort,
			TEXT("Plane waves in the wind sea's short band, the wavelets the surface material draws per pixel (WaveWindSeaTable): more is a finer, ")
			TEXT("less regular texture at a texture load and a sine per pixel each. Default 16; 0 to 64; 0 draws no wavelets. A change rebuilds the sea."));
		float WindSeaSplitCells = 8.f;
		FAutoConsoleVariableRef CVarWindSeaSplit(TEXT("fnb.Waves.WindSeaSplitCells"), WindSeaSplitCells,
			TEXT("The shortest wave the wind sea's long band holds, in pressure-buffer texels (B's grid): shorter waves go to the surface's wavelets. ")
			TEXT("Lower puts more of the sea into B (felt by the bodies), but waves only a few texels long draw blocky; higher leaves more to the ")
			TEXT("wavelets. Default 8 (94 cm on a 120 m, 1024-texel simulation); at least 2. A change rebuilds the sea."));

		constexpr double Gravity = 981.0;          // cm/s²
		constexpr double SurfaceTension = 72.8;    // σ / ρ of water, cm³/s²
		constexpr double ShortestCm = 10.0;        // the shortest wavelet
		constexpr double LongestPeakMultiple = 2.0; // the spectrum starts at twice the peak wavelength (0.5 kp): JONSWAP has nothing below
		constexpr double OriginStepCm = 1000.0;     // the table's origin follows the window in steps of this much
		constexpr double CalmWindCmS = 50.0;        // below this a sea that follows the wind keeps its heading
		constexpr double MaxTurnRadS = 3.0 * UE_DOUBLE_PI / 180.0; // the sea turns at most this fast (the field turns about its origin)
		constexpr int32 MaxComponents = 64;

		/** Finite-depth gravity-capillary dispersion, rad/s. */
		double Omega(double K, double DepthCm)
		{
			return FMath::Sqrt((Gravity * K + SurfaceTension * K * K * K) * FMath::Tanh(FMath::Min(K * DepthCm, 20.0)));
		}

		/** The JONSWAP spectrum (gamma 3.3) carried over to wavenumber, S(k) = S(omega) d omega / dk, up to a constant (the sea is scaled to its
		 * height afterwards). */
		double Spectrum(double K, double Kp, double DepthCm)
		{
			const double W = Omega(K, DepthCm);
			const double Wp = Omega(Kp, DepthCm);
			const double Sigma = W <= Wp ? 0.07 : 0.09;
			const double Peak = FMath::Exp(-FMath::Square(W - Wp) / (2.0 * Sigma * Sigma * Wp * Wp));
			const double Sw = FMath::Pow(W, -5.0) * FMath::Exp(-1.25 * FMath::Pow(Wp / W, 4.0)) * FMath::Pow(3.3, Peak);
			const double Dk = 1.0e-4 * K;
			const double Cg = (Omega(K + Dk, DepthCm) - Omega(K - Dk, DepthCm)) / (2.0 * Dk);
			return Sw * Cg;
		}

		double Wrap(double Angle)
		{
			double Out = FMath::Fmod(Angle + UE_DOUBLE_PI, 2.0 * UE_DOUBLE_PI);
			if (Out < 0.0)
			{
				Out += 2.0 * UE_DOUBLE_PI;
			}
			return Out - UE_DOUBLE_PI;
		}
	}

	void FState::Build(const FFluidNinjaWaveSimSettings& Settings, const FInput& Input, int32 NumLong, int32 NumShort, double SplitCm)
	{
		Components.Reset();
		NumLongBand = 0;
		PeakCm = FMath::Max<double>(Settings.WindSeaWavelengthCm, ShortestCm * 1.5);
		SplitWavelengthCm = SplitCm;
		const double Depth = FMath::Max<double>(Input.DepthCm, 1.0);
		const double Kp = 2.0 * UE_DOUBLE_PI / PeakCm;
		const double KMin = Kp / LongestPeakMultiple;
		const double KMax = 2.0 * UE_DOUBLE_PI / ShortestCm;
		const double KSplit = FMath::Clamp(2.0 * UE_DOUBLE_PI / SplitCm, KMin, KMax);
		FRandomStream Random(static_cast<int32>(Input.Seed));
		// Directional spreading cos^2s(theta / 2) about the heading, s from the rms angle the setting gives (sigma^2 = 2 / (s + 1)).
		const double SigmaTheta = FMath::DegreesToRadians(FMath::Clamp<double>(Settings.WindSeaSpreadDegrees, 1.0, 90.0));
		const double S = FMath::Max(2.0 / (SigmaTheta * SigmaTheta) - 1.0, 0.25);
		auto Band = [&](double K0, double K1, int32 Count) -> int32
		{
			if (Count <= 0 || K1 <= K0 * 1.0001)
			{
				return 0;
			}
			const double L0 = FMath::Loge(K0);
			const double L1 = FMath::Loge(K1);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				// One wave per equal step in log k, at a random k inside its step.
				const double A = L0 + (L1 - L0) * Index / Count;
				const double B = L0 + (L1 - L0) * (Index + 1) / Count;
				FComponent C;
				C.K = FMath::Exp(A + (B - A) * Random.FRand());
				C.Omega = Omega(C.K, Depth);
				double Theta = 0.0;
				for (int32 Try = 0; Try < 64; ++Try)
				{
					Theta = Random.FRandRange(-UE_PI, UE_PI);
					if (Random.FRand() <= FMath::Pow(FMath::Cos(0.5 * Theta), 2.0 * S))
					{
						break;
					}
				}
				C.Angle = Theta;
				C.Amplitude = FMath::Sqrt(2.0 * Spectrum(C.K, Kp, Depth) * (FMath::Exp(B) - FMath::Exp(A)));
				C.Phase = Random.FRandRange(0.f, 2.f * UE_PI);
				Components.Add(C);
			}
			return Count;
		};
		NumLongBand = Band(KMin, KSplit, NumLong);
		Band(KSplit, KMax, NumShort);
		// Amplitudes per cm of significant height: sum of a^2 / 2 = (Hs / 4)^2.
		double Variance = 0.0;
		for (const FComponent& C : Components)
		{
			Variance += 0.5 * C.Amplitude * C.Amplitude;
		}
		const double Scale = Variance > 0.0 ? 0.25 / FMath::Sqrt(Variance) : 0.0;
		for (FComponent& C : Components)
		{
			C.Amplitude *= Scale;
		}
		bHasOrigin = false;
	}

	UTexture2D* FState::Update(const FFluidNinjaWaveSimSettings& Settings, const FInput& Input)
	{
		const int32 NumLong = FMath::Clamp(WindSeaLong, 0, MaxComponents);
		const int32 NumShort = FMath::Clamp(WindSeaShort, 0, MaxComponents);
		if (WindSeaOn == 0 || Settings.WindSeaHeightCm <= 0.f || NumLong + NumShort == 0 || Input.PressureCellCm <= 0.0)
		{
			bStarted = false;
			return nullptr;
		}
		const double SplitCm = FMath::Max(WindSeaSplitCells, 2.f) * Input.PressureCellCm;
		uint32 Key = GetTypeHash(Settings.WindSeaWavelengthCm);
		Key = HashCombine(Key, GetTypeHash(Settings.WindSeaSpreadDegrees));
		Key = HashCombine(Key, GetTypeHash(NumLong));
		Key = HashCombine(Key, GetTypeHash(NumShort));
		Key = HashCombine(Key, GetTypeHash(FMath::RoundToInt(SplitCm * 10.0)));
		Key = HashCombine(Key, GetTypeHash(FMath::RoundToInt(Input.DepthCm)));
		Key = HashCombine(Key, Input.Seed);
		if (!bBuilt || Key != BuiltKey)
		{
			Build(Settings, Input, NumLong, NumShort, SplitCm);
			BuiltKey = Key;
			bBuilt = true;
		}
		if (Components.Num() == 0)
		{
			bStarted = false;
			return nullptr;
		}

		// The heading and height the wind asks for, approached with the response lag (and a turn-rate cap: the field turns about its
		// origin, so far from it a fast turn would sweep the waves sideways).
		const double WindCmS = Input.WindCmS.Size();
		double TargetHeading = FMath::DegreesToRadians(static_cast<double>(Settings.WindSeaDirectionDegrees));
		if (Settings.bWindSeaFollowsWind && WindCmS > CalmWindCmS)
		{
			TargetHeading = FMath::Atan2(Input.WindCmS.Y, Input.WindCmS.X);
		}
		TargetHeightCm = Settings.WindSeaHeightCm;
		if (Settings.WindSeaReferenceWindMs > 0.f)
		{
			TargetHeightCm *= FMath::Clamp(WindCmS / (100.0 * Settings.WindSeaReferenceWindMs), 0.0, 2.0);
		}
		const double Dt = bStarted ? FMath::Clamp(Input.Now - LastNow, 0.0, 0.25) : 0.0;
		if (!bStarted)
		{
			HeadingRad = Wrap(TargetHeading);
			HeightCm = TargetHeightCm;
			bStarted = true;
		}
		else
		{
			const double Alpha = Settings.WindSeaResponseSeconds > 0.f ? 1.0 - FMath::Exp(-Dt / Settings.WindSeaResponseSeconds) : 1.0;
			const double Turn = FMath::Clamp(Wrap(TargetHeading - HeadingRad) * Alpha, -MaxTurnRadS * Dt, MaxTurnRadS * Dt);
			HeadingRad = Wrap(HeadingRad + Turn);
			HeightCm += (TargetHeightCm - HeightCm) * Alpha;
		}
		LastNow = Input.Now;

		// The origin follows the window in whole steps; each phase carries the step (phase += k . step), so the waves stay where they are.
		const FVector2D NewOrigin(FMath::RoundToDouble(Input.WindowCentre.X / OriginStepCm) * OriginStepCm,
			FMath::RoundToDouble(Input.WindowCentre.Y / OriginStepCm) * OriginStepCm);
		if (!bHasOrigin)
		{
			Origin = NewOrigin;
			bHasOrigin = true;
		}
		else if (!NewOrigin.Equals(Origin, 0.5))
		{
			const FVector2D Step = NewOrigin - Origin;
			for (FComponent& C : Components)
			{
				const double A = HeadingRad + C.Angle;
				C.Phase = Wrap(C.Phase + C.K * (FMath::Cos(A) * Step.X + FMath::Sin(A) * Step.Y));
			}
			Origin = NewOrigin;
		}
		// Each wave runs along its k at its own speed: cos(k . (x - origin) - omega t + phase0).
		for (FComponent& C : Components)
		{
			C.Phase = Wrap(C.Phase - C.Omega * Dt);
		}
		Write(Origin);
		return Table.Get();
	}

	void FState::Write(const FVector2D& InOrigin)
	{
		const int32 Width = Components.Num() + 1;
		if (!Table.IsValid() || Table->GetSizeX() != Width)
		{
			UTexture2D* Texture = UTexture2D::CreateTransient(Width, 1, PF_A32B32G32R32F, TEXT("FNB_WindSeaTable"));
			if (!Texture)
			{
				return;
			}
			Texture->Filter = TF_Nearest;
			Texture->SRGB = false;
			Texture->CompressionSettings = TC_HDR;   // a Linear Color sampler, as the material's WaveWindSeaTable has
			Texture->AddressX = TA_Clamp;
			Texture->AddressY = TA_Clamp;
			Texture->NeverStream = true;
			Texture->UpdateResource();
			Table.Reset(Texture);
		}
		TArray<FVector4f>* Data = new TArray<FVector4f>();
		Data->SetNumUninitialized(Width);
		(*Data)[0] = FVector4f(static_cast<float>(InOrigin.X), static_cast<float>(InOrigin.Y), static_cast<float>(NumLongBand), static_cast<float>(Components.Num()));
		for (int32 Index = 0; Index < Components.Num(); ++Index)
		{
			const FComponent& C = Components[Index];
			const double A = HeadingRad + C.Angle;
			(*Data)[Index + 1] = FVector4f(static_cast<float>(C.K * FMath::Cos(A)), static_cast<float>(C.K * FMath::Sin(A)),
				static_cast<float>(C.Amplitude * HeightCm), static_cast<float>(C.Phase));
		}
		FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, Width, 1);
		Table->UpdateTextureRegions(0, 1, Region, Width * sizeof(FVector4f), sizeof(FVector4f), reinterpret_cast<uint8*>(Data->GetData()),
			[Data](uint8*, const FUpdateTextureRegion2D* Regions)
			{
				delete Data;
				delete Regions;
			});
	}

	FString FState::Describe() const
	{
		if (!bStarted || Components.Num() == 0)
		{
			return FString();
		}
		return FString::Printf(TEXT("wind sea %.1f cm (wanted %.1f), peak %.0f cm, toward %.0f deg, %d + %d waves (B holds >= %.0f cm)"),
			HeightCm, TargetHeightCm, PeakCm, FMath::RadiansToDegrees(HeadingRad), NumLongBand, Components.Num() - NumLongBand, SplitWavelengthCm);
	}
}
