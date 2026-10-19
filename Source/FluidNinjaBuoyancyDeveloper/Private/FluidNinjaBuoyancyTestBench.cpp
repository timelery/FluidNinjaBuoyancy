#include "FluidNinjaBuoyancyTestBench.h"

#include "Camera/PlayerCameraManager.h"
#include "CollisionQueryParams.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DrawDebugHelpers.h"
#include "Engine/CollisionProfile.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/HitResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Volume.h"
#include "HAL/IConsoleManager.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "FluidNinjaBuoyancyComponent.h"
#include "FluidNinjaBuoyancyLog.h"
// FluidNinja Waves' game-thread manager: the facade the ripple tests read (Poke, probes, stats, the window).
#include "FluidNinjaBuoyancySettings.h"
#include "FluidNinjaBuoyancySubsystem.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Styling/CoreStyle.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SViewport.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	/** The waves' project switches (Project Settings → FluidNinja Buoyancy → Waves) are the one authority: a test that needs other values
	 * writes the settings for its run (StartRun saves them, FinishRun puts them back). */
	void SetWaveSources(bool bDisplacement, bool bImpulse)
	{
		UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::GetMutable();
		Settings.bWaveDisplacement = bDisplacement;
		Settings.bWaveImpulse = bImpulse;
	}

	void SetWaveCurrentCoupling(float Coupling)
	{
		UFluidNinjaBuoyancySettings::GetMutable().WaveCurrentCoupling = Coupling;
	}

	void SetWaveFeel(float Feel)
	{
		UFluidNinjaBuoyancySettings::GetMutable().WaveFeelScale = Feel;
	}

	/** One of the runtime module's switch console variables (fnb.Buoyancy and friends), for the panel's toggles and the gather test's A/B:
	 * the bench lives in another module, so it sets them through the console manager, as a person at the console would. */
	IConsoleVariable* SwitchCVar(const TCHAR* Name)
	{
		return IConsoleManager::Get().FindConsoleVariable(Name);
	}

	int32 GetSwitch(const TCHAR* Name)
	{
		const IConsoleVariable* Variable = SwitchCVar(Name);
		return Variable ? Variable->GetInt() : -1;
	}

	void SetSwitch(const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* Variable = SwitchCVar(Name))
		{
			Variable->Set(Value, ECVF_SetByConsole);
		}
	}
}

namespace
{
	TAutoConsoleVariable<int32> CVarTestHUD(
		TEXT("fnb.TestHUD"), 0,
		TEXT("FluidNinja Buoyancy test bench panel (top right) and body labels: 0 hidden (default), 1 in PIE, 2 (or more) in any non-shipping game world. The panel comes up once the world has floating bodies; while this is not 0 the status lines are rebuilt every frame. Commands: fnb.Test help."));

	constexpr int32 MaxPhases = 6;
	constexpr float WindPresets[] = {-1.f, 10.f, 20.f, 0.f};   // the project's Wind, 10 m/s, 20 m/s, calm (Ctrl+Shift+W)

	const TCHAR* ModelName(EFluidNinjaHullModel Model)
	{
		switch (Model)
		{
		case EFluidNinjaHullModel::Voxels: return TEXT("vox");
		case EFluidNinjaHullModel::Spheres: return TEXT("sph");
		default: return TEXT("tri");
		}
	}

	const FName RiverFloaterTag(TEXT("FNB_RiverFloater"));
	const FName VeloDirNoise(TEXT("VeloDirNoise"));
	const FName VeloFromBitmaps(TEXT("VeloFromBitmaps"));

	/** FluidNinja's live component on a NinjaLive actor (null on any other actor). */
	UActorComponent* NinjaLiveComponent(const AActor& Actor)
	{
		if (!Actor.GetClass()->GetName().StartsWith(TEXT("NinjaLive")))
		{
			return nullptr;
		}
		TInlineComponentArray<UActorComponent*> Components(&Actor);
		for (UActorComponent* Component : Components)
		{
			if (Component->GetClass()->GetName().Contains(TEXT("NinjaLive")))
			{
				return Component;
			}
		}
		return nullptr;
	}

	/** A number on Object by name (a Blueprint variable or a FluidNinja setting). */
	bool GetNumber(const UObject& Object, FName Name, double& Out)
	{
		const FNumericProperty* Property = FindFProperty<FNumericProperty>(Object.GetClass(), Name);
		if (!Property)
		{
			return false;
		}
		const void* Value = Property->ContainerPtrToValuePtr<void>(&Object);
		Out = Property->IsFloatingPoint() ? Property->GetFloatingPointPropertyValue(Value) : static_cast<double>(Property->GetSignedIntPropertyValue(Value));
		return true;
	}

	bool SetNumber(UObject& Object, FName Name, double Value)
	{
		const FNumericProperty* Property = FindFProperty<FNumericProperty>(Object.GetClass(), Name);
		if (!Property)
		{
			return false;
		}
		void* Address = Property->ContainerPtrToValuePtr<void>(&Object);
		Property->IsFloatingPoint() ? Property->SetFloatingPointPropertyValue(Address, Value) : Property->SetIntPropertyValue(Address, static_cast<int64>(Value));
		return true;
	}

	/** Ctrl+Shift hotkeys, consumed only while the game viewport has focus. */
	class FFluidNinjaBuoyancyBenchInput : public IInputProcessor
	{
	public:
		explicit FFluidNinjaBuoyancyBenchInput(const TSharedPtr<FFluidNinjaBuoyancyTestBench>& InBench) : Bench(InBench) {}

		virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}

		virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& Event) override
		{
			const TSharedPtr<FFluidNinjaBuoyancyTestBench> Pinned = Bench.Pin();
			if (!Pinned || !Event.IsControlDown() || !Event.IsShiftDown() || Event.IsAltDown() || Event.IsRepeat() || !Pinned->IsGameFocused())
			{
				return false;
			}
			const FKey Key = Event.GetKey();
			if (Key == EKeys::B) { Pinned->ToggleVisible(); return true; }
			if (Key == EKeys::R) { Pinned->ResetBodies(true); return true; }
			if (Key == EKeys::PageDown) { Pinned->Select(1); return true; }
			if (Key == EKeys::PageUp) { Pinned->Select(-1); return true; }
			if (Key == EKeys::T) { Pinned->Run(TEXT("all")); return true; }
			if (Key == EKeys::W) { Pinned->CycleWind(); return true; }
			if (Key == EKeys::A) { Pinned->CycleStir(); return true; }
			if (Key == EKeys::C) { Pinned->Command({TEXT("water"), TEXT("toggle")}); return true; }
			if (Key == EKeys::D) { Pinned->ToggleDebugView(); return true; }
			if (Key == EKeys::K) { Pinned->Capsize(30.f); return true; }
			if (Key == EKeys::J) { Pinned->Drop(3.f); return true; }
			return false;
		}

	private:
		TWeakPtr<FFluidNinjaBuoyancyTestBench> Bench;
	};
}

// Evers, Heller, Fuchs, Hager & Boes (2019), "Landslide-generated Impulse Waves in Reservoirs: Basics and Computation", VAW-Mitteilung
// 254, 2nd ed. (V2.1, June 2023), ETH Zurich (doi:10.3929/ethz-b-000413216). The 3D extreme case (free radial propagation from a slide
// entering at one shore): Eq. 3.12 (the impulse product parameter, Heller & Hager 2010), Eqs. 3.22-3.35, the limits of Table 3-3, the
// scatter of Table 4-5 and the 2023 addendum's impact-angle extension. Every equation is dimensionless, so any consistent units work:
// lengths in cm with g in cm/s² and rho_w in kg/cm³ give amplitudes in cm, exactly as metres with g = 9.81 m/s² and rho_w = 1000 kg/m³
// give metres. This reproduces the manual's Example 1 to rounding. Two errata in the manual change nothing here: the symbol box under
// Eqs. 3.29-3.31 swaps the labels of Eqs. 3.27 and 3.28 (3.27 is a0,t1 and 3.28 is a0,c2, as below), and Example 1 prints
// a_c1(r* = 0, gamma = -48 deg) = 6.6 m where Eq. 3.29 gives 6.07 m.
namespace FluidNinjaEvers3D
{
	constexpr double DegToRad = UE_DOUBLE_PI / 180.0;

	struct FInput
	{
		double SlideVelocity = 0.0;   // V_s: speed of the slide's centre of gravity at impact (length/s)
		double SlideThickness = 0.0;  // s: maximum thickness perpendicular to the slope; for alpha = 90 deg the horizontal extent along gamma = 0
		double SlideWidth = 0.0;      // b: mean width across the slide axis (gamma = 0)
		double SlideMass = 0.0;       // m_s = rho_s * bulk volume (kg); M uses the mass directly
		double SlideVolume = 0.0;     // bulk volume (length^3); used only for the V and D range checks
		double ImpactAngleDeg = 90.0; // alpha: slope angle from horizontal at impact (90 = vertical drop)
		double WaterDepth = 0.0;      // h: still water depth in the impact zone
		double WaterDensity = 0.0;    // rho_w (kg/length^3)
		double Gravity = 0.0;         // g (length/s^2)
	};

	struct FParams
	{
		double F = 0.0;           // slide Froude number V_s / sqrt(g h)
		double S = 0.0;           // relative slide thickness s / h
		double M = 0.0;           // relative slide mass m_s / (rho_w b h^2)
		double B = 0.0;           // relative slide width b / h
		double V = 0.0;           // relative slide volume Vol / (b h^2)
		double D = 0.0;           // relative slide density rho_s / rho_w
		double CosAlphaEff = 0.0; // cos(alpha_eff), alpha_eff = (6/7) alpha
		double P = 0.0;           // impulse product parameter, Eq. 3.12
	};

	inline FParams ComputeParams(const FInput& In)
	{
		FParams Out;
		const double Depth = In.WaterDepth;
		Out.F = In.SlideVelocity / FMath::Sqrt(In.Gravity * Depth);
		Out.S = In.SlideThickness / Depth;
		Out.M = In.SlideMass / (In.WaterDensity * In.SlideWidth * Depth * Depth);
		Out.B = In.SlideWidth / Depth;
		Out.V = In.SlideVolume > 0.0 ? In.SlideVolume / (In.SlideWidth * Depth * Depth) : 0.0;
		Out.D = In.SlideVolume > 0.0 ? (In.SlideMass / In.SlideVolume) / In.WaterDensity : 0.0;
		Out.CosAlphaEff = FMath::Cos((6.0 / 7.0) * In.ImpactAngleDeg * DegToRad);
		// Eq. 3.12: P = F S^(1/2) M^(1/4) {cos[(6/7) alpha]}^(1/2)
		Out.P = Out.F * FMath::Sqrt(Out.S) * FMath::Pow(Out.M, 0.25) * FMath::Sqrt(Out.CosAlphaEff);
		return Out;
	}

	// The manual's 3D case has the slide enter at a straight shore (the origin of r on the slope's still-water line, Sec. 3.2.2; the waves
	// spread into the half-plane gamma -90..90 deg, Fig. 3-6b); at alpha = 90 deg that shore is a vertical, reflecting wall. A body dropped
	// in open water is mirror-symmetric about the vertical plane through its centre across the slide axis, and no water crosses that
	// plane, so each half of it is that lab slide exactly (inviscid flow): half its thickness, mass and volume, the same width, r measured
	// from the plane (its centre). The manual does not state this mapping; it follows from the symmetry. Reading the whole body as one
	// slide instead predicts the waves of one twice as thick and heavy.
	inline FInput ImageSlide(const FInput& OpenWater)
	{
		FInput Half = OpenWater;
		Half.SlideThickness *= 0.5;
		Half.SlideMass *= 0.5;
		Half.SlideVolume *= 0.5;
		return Half;
	}

	// Table 3-3 limits (3D), the 2023 addendum's alpha extension and the manual's extended density range.
	enum EOutOfRange : uint32
	{
		Range_None = 0,
		Range_Froude = 1u << 0,        // 0.40 <= F <= 3.40
		Range_Thickness = 1u << 1,     // 0.15 <= S <= 0.60
		Range_Mass = 1u << 2,          // 0.25 <= M <= 1.00
		Range_Width = 1u << 3,         // 0.83 <= B <= 5.00
		Range_Volume = 1u << 4,        // 0.187 <= V <= 0.750
		Range_Density = 1u << 5,       // 0.59 <= D <= 1.72 (extended; the 3D tests used D = 1.338 only)
		Range_AngleCore = 1u << 6,     // 30 <= alpha <= 90 deg (Table 3-3)
		Range_AngleExtended = 1u << 7, // 15 <= alpha <= 90 deg (2023 addendum, preliminary use, needs P in range)
		Range_Impulse = 1u << 8,       // 0.13 <= P <= 2.08
		Range_Radial = 1u << 9,        // 1 <= r/h <= 16
		Range_Angle = 1u << 10,        // -90 <= gamma <= 90 deg
		Range_ImpactZone = 1u << 11,   // r < r0(gamma): inside the slide impact zone, no prediction
	};

	inline uint32 CheckRanges(const FParams& Prm, double ImpactAngleDeg, double RelativeRadius, double GammaDeg, bool bInsideImpactZone)
	{
		uint32 Flags = bInsideImpactZone ? static_cast<uint32>(Range_ImpactZone) : static_cast<uint32>(Range_None);
		const auto Outside = [](double X, double Lo, double Hi) { return X < Lo || X > Hi; };
		if (Outside(Prm.F, 0.40, 3.40)) { Flags |= Range_Froude; }
		if (Outside(Prm.S, 0.15, 0.60)) { Flags |= Range_Thickness; }
		if (Outside(Prm.M, 0.25, 1.00)) { Flags |= Range_Mass; }
		if (Outside(Prm.B, 0.83, 5.00)) { Flags |= Range_Width; }
		if (Outside(Prm.V, 0.187, 0.750)) { Flags |= Range_Volume; }
		if (Outside(Prm.D, 0.59, 1.72)) { Flags |= Range_Density; }
		if (Outside(ImpactAngleDeg, 30.0, 90.0)) { Flags |= Range_AngleCore; }
		if (Outside(ImpactAngleDeg, 15.0, 90.0)) { Flags |= Range_AngleExtended; }
		if (Outside(Prm.P, 0.13, 2.08)) { Flags |= Range_Impulse; }
		if (Outside(RelativeRadius, 1.0, 16.0)) { Flags |= Range_Radial; }
		if (Outside(GammaDeg, -90.0, 90.0)) { Flags |= Range_Angle; }
		return Flags;
	}

	struct FWave
	{
		bool bInsideImpactZone = true; // r < r0: the manual gives no wave prediction there
		double R00 = 0.0;   // impact radius on the axis, Eq. 3.22
		double R090 = 0.0;  // impact radius across the axis, Eq. 3.23
		double R0 = 0.0;    // impact radius at gamma, Eq. 3.24
		double RStar = 0.0; // r* = r - r0, Eq. 3.25
		double A0C1 = 0.0;  // initial first crest amplitude at r* = 0, gamma = 0, Eq. 3.26
		double A0T1 = 0.0;  // initial first trough amplitude (positive number), Eq. 3.27
		double A0C2 = 0.0;  // initial second crest amplitude, Eq. 3.28
		double AC1 = 0.0;   // first crest amplitude, Eq. 3.29 (scatter +25 % / -45 %)
		double AT1 = 0.0;   // first trough amplitude below still water, Eq. 3.30 (+40 % / -25 %)
		double AC2 = 0.0;   // second crest amplitude, Eq. 3.31 (+50 % / -60 %)
		double H1 = 0.0;    // first wave height a_c1 + a_t1
		double CC1 = 0.0;   // first crest celerity, Eq. 3.32 (+10 % / -15 %)
		double CC2 = 0.0;   // second crest celerity, Eq. 3.33 (+15 % / -25 %)
		double T1 = 0.0;    // first wave period, Eq. 3.34 (+15 % / -10 %)
		double L1 = 0.0;    // first wavelength T1 * cc1, Eq. 3.35
	};

	inline double Sech(double X) { return 1.0 / FMath::Cosh(X); }

	// RadialDistance is measured from the impact location (the slope/still-water intersection in the lab); GammaDeg from the slide
	// axis, -90..90.
	inline FWave Predict(const FParams& Prm, double SlideWidth, double WaterDepth, double Gravity, double RadialDistance, double GammaDeg)
	{
		FWave Out;
		const double Depth = WaterDepth;
		const double PBCos = Prm.P * Prm.B * Prm.CosAlphaEff;
		Out.R00 = 2.5 * FMath::Pow(PBCos, 0.25) * Depth;                                          // Eq. 3.22
		Out.R090 = 0.5 * SlideWidth + 1.5 * FMath::Pow(Prm.P * Prm.CosAlphaEff, 0.25) * Depth;   // Eq. 3.23
		const double SinG = FMath::Sin(GammaDeg * DegToRad);
		const double CosG = FMath::Cos(GammaDeg * DegToRad);
		Out.R0 = FMath::Sqrt((Out.R00 * Out.R00 * Out.R090 * Out.R090)
			/ (Out.R00 * Out.R00 * SinG * SinG + Out.R090 * Out.R090 * CosG * CosG));          // Eq. 3.24
		Out.A0C1 = 0.2 * FMath::Sqrt(Prm.P) * FMath::Pow(Prm.B, 0.75) * FMath::Pow(Prm.CosAlphaEff, 0.25) * Depth; // Eq. 3.26
		Out.A0T1 = 0.35 * FMath::Sqrt(PBCos) * Depth;                                            // Eq. 3.27
		Out.A0C2 = 0.14 * FMath::Pow(PBCos, 0.25) * Depth;                                       // Eq. 3.28
		if (RadialDistance < Out.R0)
		{
			return Out;
		}
		Out.bInsideImpactZone = false;
		Out.RStar = RadialDistance - Out.R0;                                                     // Eq. 3.25
		const double Root = FMath::Sqrt(Out.RStar / Depth);
		const double LateralPower = Prm.CosAlphaEff * FMath::Exp(-0.15 * Root);
		const double GammaN = GammaDeg / 90.0;
		Out.AC1 = Out.A0C1 * FMath::Exp(-0.4 * FMath::Pow(Out.A0C1 / Depth, -0.3) * Root) * FMath::Pow(Sech(3.2 * GammaN), LateralPower); // Eq. 3.29
		Out.AT1 = Out.A0T1 * FMath::Exp(-0.4 * FMath::Pow(Out.A0T1 / Depth, -0.3) * Root) * FMath::Pow(Sech(3.6 * GammaN), LateralPower); // Eq. 3.30
		Out.AC2 = Out.A0C2 * FMath::Exp(-0.1 * FMath::Pow(Out.A0C2 / Depth, -0.3) * Root) * FMath::Pow(Sech(3.0 * GammaN), LateralPower); // Eq. 3.31
		Out.H1 = Out.AC1 + Out.AT1;
		Out.CC1 = 0.95 * FMath::Sqrt(Gravity * (Depth + Out.AC1));                                // Eq. 3.32
		Out.CC2 = 0.7 * FMath::Sqrt(Gravity * (Depth + Out.AC2));                                 // Eq. 3.33
		Out.T1 = (10.0 * FMath::Pow(Out.H1 / Depth, 0.2) + Out.RStar / (2.0 * Depth)) * FMath::Sqrt(Depth / Gravity); // Eq. 3.34
		Out.L1 = Out.T1 * Out.CC1;                                                               // Eq. 3.35
		return Out;
	}

	// Table 4-5 (3D): measured values fell within these fractions above and below the predictions (inside Table 3-3; wider outside it).
	constexpr double CrestAbove = 0.25;
	constexpr double CrestBelow = 0.45;
	constexpr double TroughAbove = 0.40;
	constexpr double TroughBelow = 0.25;

	/** The Table 3-3 checks that failed, by name (empty when the inputs are inside every limit). */
	inline FString RangeNames(uint32 Flags)
	{
		struct FName2 { uint32 Bit; const TCHAR* Label; };
		static const FName2 Names[] = {{Range_Froude, TEXT("F")}, {Range_Thickness, TEXT("S")}, {Range_Mass, TEXT("M")}, {Range_Width, TEXT("B")},
			{Range_Volume, TEXT("V")}, {Range_Density, TEXT("D")}, {Range_AngleCore, TEXT("alpha 30-90")}, {Range_AngleExtended, TEXT("alpha 15-90")},
			{Range_Impulse, TEXT("P")}, {Range_Radial, TEXT("r/h")}, {Range_Angle, TEXT("gamma")}, {Range_ImpactZone, TEXT("inside the impact zone")}};
		FString Out;
		for (const FName2& Entry : Names)
		{
			if (Flags & Entry.Bit)
			{
				Out += (Out.IsEmpty() ? TEXT("") : TEXT(", "));
				Out += Entry.Label;
			}
		}
		return Out;
	}
}

/**
 * The ripple tests' numbers, theory and records, for FluidNinja Waves as the modified NinjaLiveCore runs it (Docs/FluidNinjaWaves.md):
 * one linear field (η, v) per simulation on its pressure buffer's cells, at FluidNinja's fixed ticks (dt = 1/60 s), pressed by the
 * surface pressure head s = η + P + P_dyn (P the bodies' static head, P_dyn = y / (ticks · dt) an impulse y spread over its frame's ticks):
 *   v' = v − dt g L(s) − dt Γ v,   η' = η + dt v',
 * with L's symbol (k + (σ/ρg) k³) tanh(k d) at the depth d the core runs there (its Wave Aux), so ω² = g (k + (σ/ρg) k³) tanh(k d), and
 * Γ(k) = Wave Damping + Wave Hyperviscosity · (1 − B̂(k))² (B̂ the 3 x 3 binomial's symbol). g and σ/ρg are the core's own constants
 * (CoreGravity, CoreCapillaryCm2), whatever the world's gravity. The core exports B = η + P (what the surface draws and the probes read)
 * and A = P. The tests read it through the waves' bench facade (IFluidNinjaBuoyancyBenchWaves), pointed at the simulation each measures in (SetBenchFocus), and
 * find their water in the core's own Wave Aux (wet mask and depth), as the waves see it.
 */
namespace FluidNinjaBuoyancyBenchRipples
{
	/** FluidNinja's pool water stills over about this long once its turbulence is off (0-2 cm/s 40 s after calm). */
	constexpr double CalmSettleSeconds = 40.0;
	/** ripple-dispersion's poke: a Gaussian pressure head H pressing for one of the core's ticks (the impulse y = H dt exp(-r²/(2σ²))),
	 * and the distances its rings are read at. Its steepest slope, H/σ · e^(-1/2) = 0.24, stays under Wave Breaking Slope (0.4), which the
	 * core judges on s = η + P + P_dyn. */
	constexpr float PokeHeightCm = 4.f;
	constexpr float PokeSigmaCm = 10.f;
	constexpr double PokeRadiiCm[] = {100.0, 200.0, 400.0};
	/** ripple-dispersion: the water around the poke, out past the furthest ring, must run at one depth (the theory's) within this share. */
	constexpr double PokeLevelShare = 0.05;
	/** The core's own constants (Wave Step): g (cm/s²) and the capillary length squared, σ/(ρ g) (cm²). */
	constexpr double CoreGravity = 981.0;
	constexpr double CoreCapillaryCm2 = 72.8 / 981.0;
	/** A reading lags the frame it is stamped with by a few of the core's ticks that the bench cannot see (a probe reads the export of an
	 * earlier frame's ticks, and a poke or a pose waits for the next source write): the fits take the best whole lag up to this many. */
	constexpr int32 MaxLagTicks = 3;
	/** ripple-weight: the crate's masses, the drop and the distances its rings are read at. */
	constexpr float CrateMassesKg[] = {20.f, 80.f, 320.f};
	constexpr float DropMetres = 3.f;
	constexpr double DropRadiiCm[] = {200.0, 400.0, 800.0};
	/** ripple-weight: the static head under a body at rest, Σ P · cell area (cm³), against the water it displaces by Archimedes, m / ρ, within
	 * this share. A body floating free at rest displaces its weight in water whatever level the water stands at; the wave hull's volume
	 * clipped at a level the bench read in one frame moves by the hull's waterplane area for every cm that reading is off (the 80 cm
	 * crate: 6.4 L per cm, so 0.6 mm is this 2% of the 20 kg crate's 20 L), so that clipped volume is reported beside it, not judged. */
	constexpr double SourceVolumeTolerance = 0.02;
	/** ripple-weight: a body counts as at rest for its static head once, over the last WeightSettledSeconds, it has moved up or down slower
	 * than WeightSettledCmS (cm/s) and the static head's readings have spread over less than WeightSettledSpread of its wave hull's volume
	 * under the water (a body still bobbing is read at a different draft every frame: the 20 kg crate's readings once spread over 11 L of
	 * its 20). It is waited for at most WeightSettleLimitSeconds after the fresh field's 2 s, the settling the test used to give a body
	 * (s); a body that is not at rest by then is still read and judged, and its line says so. */
	constexpr double WeightSettledCmS = 1.0;
	constexpr double WeightSettledSpread = 0.05;
	constexpr double WeightSettledSeconds = 1.0;
	constexpr float WeightSettleLimitSeconds = 3.f;
	/** ripple-weight: the static-head readings the spread needs over that last second (the readbacks lag and repeat, so fewer could show a
	 * spread of nothing whatever the body does). */
	constexpr int32 WeightSettledReadings = 10;
	/** ripple-rest: κ of each run (the Wave Feel Scale setting, written for the run): 0, feeling no waves, reported; then 1, the default, feeling the others'
	 * waves (its own taken out by its own field, its own waves' drag put back as radiation damping), judged: FluidNinja Waves' self-coupling
	 * fix must keep a resting hull from building waves under it. */
	constexpr float RestCouplings[] = {0.f, 1.f};
	/**
	 * ripple-evers: a rigid block made for the still water found, as shares of its depth h, so that every limit of Evers et al. (2019)'s
	 * Table 3-3 holds for the reading that is judged (s along gamma = 0, b across it, m its mass). Evers' 3D case is a slide entering at a
	 * straight shore, its waves spreading into the half-plane in front of it; at alpha = 90 deg that shore is a vertical, reflecting wall.
	 * A block dropped in open water is mirror-symmetric about the vertical plane through its centre across the gamma = 0 axis (no water
	 * crosses it), so it is that case twice: two image slides of half its thickness, mass and volume and the same width, down a wall at
	 * its centre, r measured from there (FluidNinjaEvers3D::ImageSlide). Read so: S = s/(2h) 0.29, B = b/h 1.0, V = Vol/(2 b h²) 0.21 (its
	 * height 0.72 h), the one density the manual's tests used, D = 1.338 (so M = D V = 0.281), its bottom let go 0.75 h above the water
	 * (F = √(2 · 0.75) = 1.22), alpha 90 deg (P about 0.23). The direct reading of the whole block as one slide (S 0.58, M 0.562, V 0.42,
	 * P 0.38, also inside Table 3-3) predicts about 1.5 times its waves and is reported beside it. Its probes on the gamma = 0 axis and at
	 * gamma = 45 deg at these r/h (Table 3-3: 1 to 16).
	 */
	constexpr double EversThicknessShare = 0.58;
	constexpr double EversWidthShare = 1.0;
	constexpr double EversVolumeShare = 0.42;
	constexpr double EversDensity = 1.338;
	constexpr double EversDropShare = 0.75;
	constexpr double EversRadiusShares[] = {1.5, 2.0, 3.0};
	/** The shallowest water it plans a block for (each image slide, 0.29 h thick, then spans at least 17 cells of the pool's 2.5 cm). */
	constexpr double EversShallowestCm = 150.0;
	/** Table 4-5's upper scatter of the first crest's celerity c_c1 (+10 %, p. 74): the fastest the manual lets a wave come back from a wall.
	 * The reflections' cut-off takes the core's own fastest wave (CoreFastestSpeed) where that is faster. */
	constexpr double EversCelerityAbove = 0.10;
	/** The depth the core runs under a probe within this share of h counts as level (the manual's basin has a horizontal bed). */
	constexpr double EversLevelBed = 0.2;
	/** ripple-advection: the poke's ring followed this long, the river's time to get going when the test starts it, the slowest current it
	 * measures in, and the probe line's reach beyond the ring's path (behind the poke and past where the current takes it). */
	constexpr double AdvectSeconds = 1.5;
	constexpr double AdvectSpinUpSeconds = 8.0;
	constexpr double AdvectSlowestCmS = 20.0;
	constexpr double AdvectLineMarginCm = 200.0;
	/** ripple-advection: the ring's drift against the core's own drift (its Wave Aux, along the ring's path), within this share. */
	constexpr double AdvectTolerance = 0.15;
	/** ripple-coupling: the ball counts as at rest before the drop once it bobs slower than this (cm/s) for a second. */
	constexpr double SettledBallCmS = 0.5;
	/** ripple-coupling: the direct ring at the ball is the first crest, after the earliest a wave can arrive and no later than the latest its
	 * long waves can (RingShortestWaveCm), that rises above this share of the largest rise in that time (a smaller crest ahead of it is the
	 * long-wave precursor; a later, larger one is what the pool sends back or another body stirs). */
	constexpr double RingPickShare = 0.3;
	/** ripple-coupling: the direct ring is looked for, and the scale it is picked against measured, only until its waves this long (cm) or
	 * longer have all reached the ball: the 3 m fall, then the ball's distance from the boat's centre at this wave's group speed at the
	 * core's depth (a longer wave's group is faster). Over the whole record a later crest can set that scale: suite runs read a 16.7 cm
	 * crest 5.9 s after the lift (the old whole-record reading, 2026-10-18), 30 % of which (5.0 cm) would stand above the direct ring's own
	 * first crest as standalone runs read it (4.85 and 4.92 cm at 3.0 s, a 1.78 m wave), so the pick would take the late crest and judge
	 * it (not yet seen in a suite with this pick: an inference from those two readings). At 3 m from the boat and 4 m deep this bound is
	 * about 4.7 s after the lift (0.78 s of fall, then 3.9 s at 77 cm/s): it holds the 3.0 s crest and leaves out the 5.9 s one. It is not
	 * set at a reflection's time at √(g h): a dispersing ring's crest comes after a long wave's reflection would, so that bound would cut
	 * the ring itself out. */
	constexpr double RingShortestWaveCm = 150.0;
	/** ripple-kelvin: the tow and the arcs behind the boat (snapshots of both arcs, each a little further out). */
	constexpr double TowSpeedCmS = 175.0;
	constexpr double TowLongestSeconds = 8.0;
	constexpr double TowShortestSeconds = 5.0;
	constexpr double WakeReadSeconds = 2.0;
	constexpr double WakeArcsCm[] = {400.0, 600.0};
	constexpr int32 WakeArcDegrees = 45;
	constexpr int32 WakeSnapshots = 8;
	/** Kelvin's wedge, asin(1/3) (deep water, far behind a point disturbance): the plan's cusp half-angle, reported beside the lines (it is the
	 * far wake's, and is measurable on the arcs only when the exact linear wake's own line lies within the tolerance of it). The tolerance
	 * is also the judged match of the measured line to the exact linear wake's. */
	constexpr double KelvinAngleDeg = 19.4712206;
	constexpr double KelvinToleranceDeg = 2.0;
	/** The judged match of each arc to the exact linear wake of the same hull towed the same way: normalised RMS at most this. */
	constexpr double WakeRmsLimit = 0.25;
	/** Near a caustic (the wake's cusp line) the envelope follows the Airy function, whose crest Ai(-1.0188) = 0.53566 lies just inside
	 * the line and whose value on it is Ai(0) = 0.35503: the line is where the envelope, outward of its peak, has fallen to this share. */
	constexpr double CausticShare = 0.35502805 / 0.53565666;
	/** The theory's grid for the towed hull: 1024 cells of 10 cm (102 m, so the start's free waves do not come round). */
	constexpr int32 WakeGridCells = 1024;
	constexpr double WakeGridCm = 10.0;
	/** Water the core runs at least this deep counts as open (shallower is shore, which Wave Shore Depth absorbs). */
	constexpr float OpenDepthCm = 30.f;

	/** Bessel J0: Abramowitz & Stegun 9.4.1 up to 3 (error under 5e-8) and 9.4.3 beyond (f0 within 1.6e-8, theta0 within 7e-8). */
	double BesselJ0(double X)
	{
		const double Ax = FMath::Abs(X);
		if (Ax <= 3.0)
		{
			const double T = (Ax / 3.0) * (Ax / 3.0);
			return 1.0 + T * (-2.2499997 + T * (1.2656208 + T * (-0.3163866 + T * (0.0444479 + T * (-0.0039444 + T * 0.0002100)))));
		}
		const double T = 3.0 / Ax;
		const double F0 = 0.79788456 + T * (-0.00000077 + T * (-0.00552740 + T * (-0.00009512 + T * (0.00137237 + T * (-0.00072805 + T * 0.00014476)))));
		const double Theta0 = Ax - 0.78539816 + T * (-0.04166397 + T * (-0.00003954 + T * (0.00262573 + T * (-0.00054125 + T * (-0.00029333 + T * 0.00013558)))));
		return F0 * FMath::Cos(Theta0) / FMath::Sqrt(Ax);
	}

	/** The core's dispersion, ω² = g (k + (σ/ρg) k³) tanh(k d) (rad/s for k in rad/cm, d in cm), with its own g and σ/ρg. */
	double CoreOmega(double K, double DepthCm)
	{
		const double Kd = FMath::Min(K * FMath::Max(DepthCm, 1.0), 20.0);
		return FMath::Sqrt(FMath::Max(CoreGravity * (K + CoreCapillaryCm2 * K * K * K) * FMath::Tanh(Kd), 0.0));
	}

	/** dω/dk of the same (the group velocity, cm/s): √(g d) as k goes to 0. */
	double CoreGroupVelocity(double K, double DepthCm)
	{
		const double Depth = FMath::Max(DepthCm, 1.0);
		const double Kd = K * Depth;
		if (Kd < 1.0e-6)
		{
			return FMath::Sqrt(CoreGravity * Depth);
		}
		const double Tanh = FMath::Tanh(FMath::Min(Kd, 20.0));
		const double Sech = 1.0 / FMath::Cosh(FMath::Min(Kd, 300.0));
		const double Slope = CoreGravity * ((1.0 + 3.0 * CoreCapillaryCm2 * K * K) * Tanh + (K + CoreCapillaryCm2 * K * K * K) * Depth * Sech * Sech);
		return Slope / (2.0 * CoreOmega(K, Depth));
	}

	/** The wavenumber (rad/cm) of a wave of angular frequency Omega (rad/s) at the depth the core runs (cm): CoreOmega inverted by halving
	 * 1e-5 to 10 rad/cm sixty times (it rises with k all the way, capillarity included). A wave's length is 2π over it. */
	double CoreWaveNumber(double Omega, double DepthCm)
	{
		double Low = 1.0e-5;
		double High = 10.0;
		for (int32 Step = 0; Step < 60; ++Step)
		{
			const double Middle = 0.5 * (Low + High);
			(CoreOmega(Middle, DepthCm) < Omega ? Low : High) = Middle;
		}
		return 0.5 * (Low + High);
	}

	/** The core's damping of v at wavenumber k on open water (1/s): Wave Damping, plus Wave Hyperviscosity times the square of the
	 * masked 3 x 3 binomial's T0 = 1 − B̂, B̂ = (1 + cos kx Δ)(1 + cos ky Δ)/4, averaged over directions. A wave's height decays at half of it. */
	double CoreDamping(double K, double Damping, double Hyper, double CellCm)
	{
		double Mean = 0.0;
		constexpr int32 Directions = 8;
		for (int32 Turn = 0; Turn < Directions; ++Turn)
		{
			const double Angle = (Turn + 0.5) * 0.5 * UE_DOUBLE_PI / Directions;
			const double B = 0.25 * (1.0 + FMath::Cos(K * FMath::Cos(Angle) * CellCm)) * (1.0 + FMath::Cos(K * FMath::Sin(Angle) * CellCm));
			Mean += FMath::Square(1.0 - B) / Directions;
		}
		return FMath::Max(Damping, 0.0) + FMath::Max(Hyper, 0.0) * Mean;
	}

	/**
	 * The fastest any wave the window holds can travel (cm/s): the phase speed ω/k of its longest wave, k = 2π / the window (nothing longer
	 * fits in it), at the depth the core runs. The phase speed falls as k grows from there (capillarity turns it back up only below a
	 * centimetre, where the grid holds nothing) and is never below the group speed, so neither a crest nor a wave's energy outruns it.
	 * At 4 m in a 20 m window: 5.4 m/s, where √(g h) is 6.3 m/s.
	 */
	double CoreFastestSpeed(double DepthCm, double WindowCm)
	{
		const double Longest = 2.0 * UE_DOUBLE_PI / FMath::Max(WindowCm, 1.0);
		return CoreOmega(Longest, DepthCm) / Longest;
	}

	/**
	 * FluidNinja Waves' dispersion operator as the core runs it (Tools/CorePatch/FluidNinjaWaves/Ninja_WaveStep.hlsl): L = (1/dx) Σ_l w_l T_l,
	 * T_0 = s - B3[s] (the 3 x 3 binomial on its cells), T_l = s - the 4-fetch cubic B-spline read of mip l, mip l the Niagara Blur2 of mip
	 * l - 1 (five bilinear taps two source texels apart, Gaussian weights exp(-x²/2), normalised) on max(N >> l, 1) texels, w_l lerped between
	 * two rows of its P0 minimax table by the depth in cells, a level left out where its weight is not positive (as the shader does). The
	 * continuous ω(k) is matched within about 1%, but the group speed is 4-5% off either way, so after 4 m the continuous theory's largest
	 * crest is half a period off what the core makes: ripple-dispersion judges against this one. The table is a copy: CoreTableInterface names
	 * the core interface it was copied from, and the test warns when the core that ran has another.
	 */
	constexpr int32 CoreTableInterface = 4;
	constexpr double CoreDispersionWG[12][9] = {
		{2.652007, 0.2231427, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
		{2.139424, 0.5073167, 0.01866169, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
		{2.076839, 0.3952435, 0.1602098, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
		{2.169085, 0.2620277, 0.2055651, 0.04232308, 0.0, 0.0, 0.0, 0.0, 0.0},
		{2.115532, 0.32208, 0.09967527, 0.1033419, 0.01101243, 0.0, 0.0, 0.0, 0.0},
		{2.11266, 0.330048, 0.08598847, 0.07846895, 0.03881073, 0.004212264, 0.0, 0.0, 0.0},
		{2.112846, 0.3295296, 0.08799041, 0.06950843, 0.02918338, 0.02122492, 0.0, 0.0, 0.0},
		{2.112486, 0.3305339, 0.08411518, 0.07931206, 0.01001729, 0.03198638, 0.0, 0.0, 0.00164963},
		{2.112476, 0.3305616, 0.08400983, 0.07963947, 0.009197677, 0.02809094, 0.0, 0.006120074, 0.0},
		{2.112991, 0.3291329, 0.08587961, 0.07864184, 0.00983242, 0.02616774, 0.0, 0.007710436, 0.0},
		{2.109872, 0.3377785, 0.07471284, 0.08403116, 0.008085577, 0.02663098, 0.0, 0.007667287, 0.0},
		{2.109872, 0.3377785, 0.07471284, 0.08403116, 0.008085577, 0.02663098, 0.0, 0.007667287, 0.0}};
	constexpr double CoreDispersionWC[12][9] = {
		{0.5048955, 18.69172, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
		{4.901266, 0.0, 0.07636693, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
		{7.384365, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
		{7.899802, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
		{7.920234, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
		{7.920234, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
		{7.920234, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
		{7.920235, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
		{7.920239, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
		{7.920244, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
		{7.920241, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
		{7.920241, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}};

	/** Wave Step's level weights w_l for cells of CellCm at DepthCm (both cm): the table's rows H_j = 1.28 · 1.683891^j cells lerped at
	 * RowU = ln(d / dx / 1.28) · 1.91899, the capillary column times the Bond number (σ/ρg) / dx². */
	void CoreDispersionWeights(double CellCm, double DepthCm, double (&OutWeights)[9])
	{
		const double RowU = FMath::Clamp(FMath::Loge(FMath::Max(DepthCm / CellCm, 1.0e-6) / 1.28) * 1.91899, 0.0, 11.0);
		const int32 Row = FMath::Min(static_cast<int32>(RowU), 10);
		const double Blend = RowU - Row;
		const double Bond = CoreCapillaryCm2 / (CellCm * CellCm);
		for (int32 Level = 0; Level < 9; ++Level)
		{
			OutWeights[Level] = FMath::Lerp(CoreDispersionWG[Row][Level] + Bond * CoreDispersionWC[Row][Level],
				CoreDispersionWG[Row + 1][Level] + Bond * CoreDispersionWC[Row + 1][Level], Blend);
		}
	}

	/** A periodic line of texels read linearly at texel coordinate X (texel I's centre at X = I), wrapping at both ends: the core's render
	 * targets wrap, and a cell far from the window's edge never meets its UV clamps. */
	double WrapLinear(const TArray<double>& Texels, double X)
	{
		const int32 Num = Texels.Num();
		const double Floor = FMath::FloorToDouble(X);
		const double Fraction = X - Floor;
		const int32 First = ((static_cast<int32>(Floor) % Num) + Num) % Num;
		return (1.0 - Fraction) * Texels[First] + Fraction * Texels[(First + 1) % Num];
	}

	/** The core's dispersion symbol on its own grid, one entry per whole number of waves 1..N/2 - 1 across its N-cell window: the
	 * wavenumber k (rad/cm) and the angular frequency (rad/s) its operator gives that wave, ω² = g λ / dx with λ = dx L(s) / s. */
	struct FCoreSymbol
	{
		TArray<double> K;
		TArray<double> Omega;
		double CellCm = 1.0;
		bool bValid = false;
	};

	/**
	 * FCoreSymbol for a window of N cells of CellCm at DepthCm, by applying the operator to a plane wave along one axis: the field is
	 * constant across it, so every 2D binomial, blur and read reduces to its 1D chain on a periodic line of N cells (the scratchpad's
	 * verified numpy version, symbol1d.py, does the same with matrices). N is the wave grid's cells across, the NX the shader's mips
	 * halve. About 400 waves of N cells each: tens of milliseconds, so it is built when the test is judged, not while the rings are read.
	 */
	FCoreSymbol ComputeCoreSymbol(int32 N, double CellCm, double DepthCm)
	{
		FCoreSymbol Symbol;
		Symbol.CellCm = CellCm;
		if (N < 8 || CellCm <= 0.0 || DepthCm <= 0.0)
		{
			return Symbol;
		}
		double Weights[9];
		CoreDispersionWeights(CellCm, DepthCm, Weights);
		// Blur2's five taps, normalised.
		double Taps[5];
		double TapSum = 0.0;
		for (int32 Tap = 0; Tap < 5; ++Tap)
		{
			Taps[Tap] = FMath::Exp(-0.5 * FMath::Square(Tap - 2.0));
			TapSum += Taps[Tap];
		}
		for (double& Tap : Taps)
		{
			Tap /= TapSum;
		}
		TArray<double> Wave;
		TArray<double> Applied;
		TArray<double> Mip;
		TArray<double> Next;
		Wave.SetNumUninitialized(N);
		Applied.SetNumUninitialized(N);
		for (int32 Cycles = 1; Cycles < N / 2; ++Cycles)
		{
			for (int32 Cell = 0; Cell < N; ++Cell)
			{
				Wave[Cell] = FMath::Cos(2.0 * UE_DOUBLE_PI * Cycles * (Cell + 0.5) / N);
			}
			// Level 0: the 3 x 3 binomial, along the wave [1/4 1/2 1/4].
			for (int32 Cell = 0; Cell < N; ++Cell)
			{
				Applied[Cell] = Weights[0] * (Wave[Cell] - (0.25 * Wave[(Cell + N - 1) % N] + 0.5 * Wave[Cell] + 0.25 * Wave[(Cell + 1) % N]));
			}
			// Levels 1-8: each mip is the last one blurred onto max(N >> l, 1) texels (every level is made, a weighted one or not, since the
			// next is made from it), and read back at every cell by the B-spline's two bilinear fetches.
			Mip = Wave;
			for (int32 Level = 1; Level < 9; ++Level)
			{
				const int32 Size = FMath::Max(N >> Level, 1);
				const int32 SourceSize = Mip.Num();
				Next.SetNumUninitialized(Size);
				for (int32 Texel = 0; Texel < Size; ++Texel)
				{
					const double At = (Texel + 0.5) / Size * SourceSize - 0.5;   // this texel's centre, in source texels
					double Value = 0.0;
					for (int32 Tap = 0; Tap < 5; ++Tap)
					{
						Value += Taps[Tap] * WrapLinear(Mip, At + 2.0 * (Tap - 2));
					}
					Next[Texel] = Value;
				}
				Swap(Mip, Next);
				if (Weights[Level] <= 0.0)
				{
					continue;
				}
				for (int32 Cell = 0; Cell < N; ++Cell)
				{
					const double At = (Cell + 0.5) / N * Size - 0.5;
					const double Floor = FMath::FloorToDouble(At);
					const double F = At - Floor;
					const double F2 = F * F;
					const double F3 = F2 * F;
					const double B0 = (1.0 - F) * (1.0 - F) * (1.0 - F) / 6.0;
					const double B1 = (4.0 - 6.0 * F2 + 3.0 * F3) / 6.0;
					const double B2 = (1.0 + 3.0 * F + 3.0 * F2 - 3.0 * F3) / 6.0;
					const double B3 = F3 / 6.0;
					const double G0 = B0 + B1;
					const double G1 = B2 + B3;
					const double Read = G0 * WrapLinear(Mip, Floor - 1.0 + B1 / G0) + G1 * WrapLinear(Mip, Floor + 1.0 + B3 / G1);
					Applied[Cell] += Weights[Level] * (Wave[Cell] - Read);
				}
			}
			// λ: the operator's output projected on the wave (2/N Σ L s; what is left over is aliasing, under 1e-3 on the pool's grid).
			double Lambda = 0.0;
			for (int32 Cell = 0; Cell < N; ++Cell)
			{
				Lambda += 2.0 / N * Applied[Cell] * Wave[Cell];
			}
			Symbol.K.Add(2.0 * UE_DOUBLE_PI * Cycles / (N * CellCm));
			Symbol.Omega.Add(FMath::Sqrt(FMath::Max(CoreGravity * Lambda / CellCm, 0.0)));
		}
		Symbol.bValid = Symbol.K.Num() > 1;
		return Symbol;
	}

	/** The core's ω at any k (rad/cm): linear between the symbol's waves (evenly spaced, k_c = c k_1); below the longest wave the window
	 * holds, the continuous ω scaled to meet the symbol there. Beyond the shortest (the grid's Nyquist) the caller holds no waves. */
	double SymbolOmega(const FCoreSymbol& Symbol, double K, double DepthCm)
	{
		if (K <= Symbol.K[0])
		{
			return CoreOmega(K, DepthCm) * Symbol.Omega[0] / FMath::Max(CoreOmega(Symbol.K[0], DepthCm), 1.0e-12);
		}
		const double Position = K / Symbol.K[0] - 1.0;
		const int32 Low = FMath::Clamp(FMath::FloorToInt32(Position), 0, Symbol.K.Num() - 2);
		return FMath::Lerp(Symbol.Omega[Low], Symbol.Omega[Low + 1], Position - Low);
	}

	/** Where FluidNinja Waves' status line describes the running simulation whose grid is Cells x CellCm ("core build B, interface I,
	 * Cells x CellCm cm, ..."): the index just past its grid's " x ", with the interface it gives that simulation's core; INDEX_NONE when
	 * none is listed so (the bench facade carries neither of its own). */
	int32 FindSimInStatus(const FString& StatusLine, int32 Cells, double CellCm, int32& OutInterface)
	{
		const TCHAR* Marker = TEXT(", interface ");
		const int32 MarkerLength = FCString::Strlen(Marker);
		for (int32 At = StatusLine.Find(Marker, ESearchCase::CaseSensitive); At != INDEX_NONE;
			At = StatusLine.Find(Marker, ESearchCase::CaseSensitive, ESearchDir::FromStart, At + MarkerLength))
		{
			TCHAR* End = nullptr;
			const int32 Interface = FCString::Strtoi(*StatusLine + At + MarkerLength, &End, 10);
			if (!End || FCString::Strncmp(End, TEXT(", "), 2) != 0)
			{
				continue;   // a simulation that is not running ("core build B, interface I: this plugin needs ...")
			}
			TCHAR* GridEnd = nullptr;
			const int32 Across = FCString::Strtoi(End + 2, &GridEnd, 10);
			if (!GridEnd || FCString::Strncmp(GridEnd, TEXT(" x "), 3) != 0)
			{
				continue;
			}
			if (Across == Cells && FMath::Abs(FCString::Atod(GridEnd + 3) - CellCm) < 0.01)
			{
				OutInterface = Interface;
				return static_cast<int32>(GridEnd + 3 - *StatusLine);
			}
		}
		return INDEX_NONE;
	}

	/** The interface FluidNinja Waves' status line gives the core of the simulation whose grid is Cells x CellCm, or -1 when none is
	 * listed so. */
	int32 CoreInterfaceOf(const FString& StatusLine, int32 Cells, double CellCm)
	{
		int32 Interface = -1;
		return FindSimInStatus(StatusLine, Cells, CellCm, Interface) != INDEX_NONE ? Interface : -1;
	}

	/** The core's ticks in the frame FluidNinja Waves last read them for the simulation whose grid is Cells x CellCm (its Sim.Delta, the
	 * Niagara instance's ticks since the frame before: "N ticks this frame" in that simulation's part of the status line, which runs to the
	 * next "; "), or -1 when the line has none so. */
	int32 CoreTicksOf(const FString& StatusLine, int32 Cells, double CellCm)
	{
		int32 Interface = -1;
		const int32 From = FindSimInStatus(StatusLine, Cells, CellCm, Interface);
		if (From == INDEX_NONE)
		{
			return -1;
		}
		const int32 PartEnd = StatusLine.Find(TEXT("; "), ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
		const int32 Tick = StatusLine.Find(TEXT(" tick"), ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
		if (Tick == INDEX_NONE || (PartEnd != INDEX_NONE && Tick > PartEnd))
		{
			return -1;
		}
		int32 Start = Tick;
		while (Start > 0 && FChar::IsDigit(StatusLine[Start - 1]))
		{
			--Start;
		}
		return Start < Tick ? FCString::Atoi(*StatusLine + Start) : -1;
	}

	/** The core's mean relax rate (1/s: User.Wave.MeanRelax as FluidNinja Waves sends it, after fnb.Waves.MeanRelax's automatic choice and
	 * its clamp) for the simulation whose grid is Cells x CellCm: "mean relax core R /s" in that simulation's part of the status line, which
	 * runs to the next "; ". -1 when the line has none so (the bench facade carries no accessor of its own for it). */
	double CoreMeanRelaxOf(const FString& StatusLine, int32 Cells, double CellCm)
	{
		int32 Interface = -1;
		const int32 From = FindSimInStatus(StatusLine, Cells, CellCm, Interface);
		if (From == INDEX_NONE)
		{
			return -1.0;
		}
		const TCHAR* Marker = TEXT("mean relax core ");
		const int32 PartEnd = StatusLine.Find(TEXT("; "), ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
		const int32 At = StatusLine.Find(Marker, ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
		if (At == INDEX_NONE || (PartEnd != INDEX_NONE && At > PartEnd))
		{
			return -1.0;
		}
		return FCString::Atod(*StatusLine + At + FCString::Strlen(Marker));
	}

	/** FluidNinja Waves rebuilds its status line every frame only while something shows it: the debug view, or fnb.TestHUD not 0 (else
	 * about once a second, its readers getting the last build in between), so only then does the line hold each frame's tick count. */
	bool WavesStatusEveryFrame()
	{
		return UFluidNinjaBuoyancySettings::IsDebugView() || CVarTestHUD.GetValueOnGameThread() != 0;
	}

	/** The bodies' own fields and how many of them feel the waves, from FluidNinja Waves' status line ("N own fields (M feel the waves)");
	 * false when the line has none. */
	bool OwnFieldsOf(const FString& StatusLine, int32& OutFields, int32& OutFelt)
	{
		const TCHAR* Marker = TEXT(" own fields (");
		const int32 At = StatusLine.Find(Marker, ESearchCase::CaseSensitive);
		if (At == INDEX_NONE)
		{
			return false;
		}
		int32 Start = At;
		while (Start > 0 && FChar::IsDigit(StatusLine[Start - 1]))
		{
			--Start;
		}
		if (Start == At)
		{
			return false;
		}
		OutFields = FCString::Atoi(*StatusLine + Start);
		OutFelt = FCString::Atoi(*StatusLine + At + FCString::Strlen(Marker));
		return true;
	}

	/**
	 * The core's answer to ripple-dispersion's poke on open, level water. The poke is an impulse y(r) = H dt exp(-r²/(2σ²)), whose Hankel
	 * pair is Y(k) = H dt σ² e^{-k²σ²/2}, spread evenly over the Ticks ticks of its frame (at t_j = j dt). Each share kicks v by
	 * -ω² Y / Ticks (Cauchy-Poisson's impulse problem: it sets the surface moving, it lifts nothing), so with the damping Γ(k) on v
	 *   η(r, t) = -∫₀^∞ Y(k) ω² Σ_j (1/Ticks) e^{-Γ (t - t_j)/2} sin(ω' (t - t_j)) / ω' J₀(k r) k dk,   ω' = √(ω² - Γ²/4),
	 * at t = 0, Step, 2·Step, ... after the first tick (Out[radius][step]). The integral runs over s = √k (dk = 2s ds), where the deep-water
	 * phase turns at a steady rate; Simpson's rule with panels short enough that neither the phase nor the Bessel function turns more than
	 * a quarter radian across one. bStepped: each wave stepped instead as the core steps it (symplectic Euler at its tick, Step being one
	 * tick: v ← v (1 - dt Γ) - dt ω² η - ω² Y / Ticks while the impulse lasts, η ← η + dt v), the same physics with the core's time step.
	 * Symbol: ω from the core's own operator on its grid (ComputeCoreSymbol) instead of the continuous ω(k), and no wave shorter than the
	 * grid's Nyquist (π / dx), which it cannot hold; null: the continuous theory.
	 */
	void PokeResponse(double HeightCm, double SigmaCm, double TickSeconds, int32 Ticks, double DepthCm, double Damping, double Hyper, double CellCm,
		const TArray<double>& Radii, int32 NumTimes, double StepSeconds, TArray<TArray<double>>& Out, bool bStepped = false, const FCoreSymbol* Symbol = nullptr)
	{
		const bool bSymbol = Symbol && Symbol->bValid;
		Out.SetNum(Radii.Num());
		for (TArray<double>& Series : Out)
		{
			Series.Init(0.0, FMath::Max(NumTimes, 0));
		}
		if (Radii.IsEmpty() || NumTimes <= 0 || SigmaCm <= 0.0)
		{
			return;
		}
		Ticks = FMath::Max(Ticks, 1);
		const double MaxTime = StepSeconds * (NumTimes - 1);
		double MaxRadius = 0.0;
		for (const double Radius : Radii)
		{
			MaxRadius = FMath::Max(MaxRadius, Radius);
		}
		const double SMax = FMath::Sqrt(7.5 / SigmaCm);   // e^{-k²σ²/2} is under 1e-12 beyond k = 7.5/σ
		double TurnRate = 2.0 * SMax * MaxRadius;          // J₀(s² r) turns at d(s² r)/ds
		for (int32 Sample = 1; Sample <= 512; ++Sample)
		{
			const double At = SMax * Sample / 512.0;
			TurnRate = FMath::Max(TurnRate, 2.0 * SMax * MaxRadius + CoreGroupVelocity(At * At, DepthCm) * 2.0 * At * MaxTime);
		}
		int32 Panels = FMath::Clamp(FMath::CeilToInt32(SMax * TurnRate / 0.25), 64, 400000);
		Panels += Panels & 1;   // Simpson's rule needs an even number
		const double Width = SMax / Panels;
		const double Yield = HeightCm * TickSeconds * SigmaCm * SigmaCm;
		TArray<double> Amplitude;
		Amplitude.SetNumZeroed(Radii.Num());
		for (int32 Node = 1; Node <= Panels; ++Node)   // s = 0 adds nothing (the integrand goes as s³ or faster)
		{
			const double S = Node * Width;
			const double K = S * S;
			if (bSymbol && K > UE_DOUBLE_PI / Symbol->CellCm)
			{
				continue;   // shorter than the core's grid holds (the integral runs to 7.5/σ, past Nyquist on a coarse grid)
			}
			const double Omega = bSymbol ? SymbolOmega(*Symbol, K, DepthCm) : CoreOmega(K, DepthCm);
			const double Gamma = CoreDamping(K, Damping, Hyper, CellCm);
			const double Squared = Omega * Omega - 0.25 * Gamma * Gamma;
			if (Squared <= 0.0)
			{
				continue;   // overdamped: only waves far longer than any window
			}
			const double Turning = FMath::Sqrt(Squared);
			const double Weight = (Node == Panels ? 1.0 : ((Node & 1) ? 4.0 : 2.0)) * Width / 3.0;
			if (bStepped)
			{
				const double Base = Weight * 2.0 * K * S * Yield * FMath::Exp(-0.5 * K * K * SigmaCm * SigmaCm);
				for (int32 Index = 0; Index < Radii.Num(); ++Index)
				{
					Amplitude[Index] = Base * BesselJ0(K * Radii[Index]);
				}
				double V = 0.0;
				double Eta = 0.0;
				for (int32 Step = 1; Step < NumTimes; ++Step)
				{
					V = V * (1.0 - TickSeconds * Gamma) - TickSeconds * Omega * Omega * Eta - (Step <= Ticks ? Omega * Omega / Ticks : 0.0);
					Eta += TickSeconds * V;
					for (int32 Index = 0; Index < Radii.Num(); ++Index)
					{
						Out[Index][Step] += Amplitude[Index] * Eta;
					}
				}
				continue;
			}
			const double Base = -Weight * 2.0 * K * S * Yield * FMath::Exp(-0.5 * K * K * SigmaCm * SigmaCm) * Omega * Omega / (Turning * Ticks);
			for (int32 Index = 0; Index < Radii.Num(); ++Index)
			{
				Amplitude[Index] = Base * BesselJ0(K * Radii[Index]);
			}
			for (int32 Step = 0; Step < NumTimes; ++Step)
			{
				const double Time = Step * StepSeconds;
				double Value = 0.0;
				for (int32 Tick = 0; Tick < Ticks; ++Tick)
				{
					const double Since = Time - Tick * TickSeconds;
					if (Since > 0.0)
					{
						Value += FMath::Exp(-0.5 * Gamma * Since) * FMath::Sin(Turning * Since);
					}
				}
				for (int32 Index = 0; Index < Radii.Num(); ++Index)
				{
					Out[Index][Step] += Amplitude[Index] * Value;
				}
			}
		}
	}

	/** A world XY moved to the centre of the window's cell it lies in (Origin: the window's lower-left corner), so a probe there reads that
	 * cell exactly, with no interpolation between cells. */
	FVector2D SnapToCell(const FVector2D& Point, double CellCm, const FVector2D& Origin)
	{
		return Origin + FVector2D((FMath::FloorToDouble((Point.X - Origin.X) / CellCm) + 0.5) * CellCm, (FMath::FloorToDouble((Point.Y - Origin.Y) / CellCm) + 0.5) * CellCm);
	}

	/** A crest: when and how high. */
	struct FCrest
	{
		double Time = 0.0;
		double Height = 0.0;
		bool bFound = false;
	};

	/** The top of the parabola through three samples (their times need not be even); false when they make no crest. */
	bool ParabolaPeak(double T0, double Z0, double T1, double Z1, double T2, double Z2, double& OutTime, double& OutHeight)
	{
		// Times about the middle sample, for well-conditioned sums.
		const double A0 = T0 - T1;
		const double A2 = T2 - T1;
		const double Denominator = A0 * A2 * (A0 - A2);
		if (FMath::Abs(Denominator) < 1.0e-15)
		{
			return false;
		}
		// z = Z1 + b u + c u² through (A0, Z0) and (A2, Z2).
		const double Curve = (A2 * (Z0 - Z1) - A0 * (Z2 - Z1)) / Denominator;
		const double Slope = (A0 * A0 * (Z2 - Z1) - A2 * A2 * (Z0 - Z1)) / Denominator;
		if (Curve >= 0.0)
		{
			return false;
		}
		const double Vertex = -Slope / (2.0 * Curve);
		if (Vertex < FMath::Min(A0, A2) || Vertex > FMath::Max(A0, A2))
		{
			return false;
		}
		OutTime = T1 + Vertex;
		OutHeight = Z1 + Slope * Vertex + Curve * Vertex * Vertex;
		return true;
	}

	/** The largest of Z over [From, To), refined by the parabola through it and its neighbours. */
	FCrest LargestIn(const TArray<double>& Times, const TArray<double>& Z, int32 From, int32 To)
	{
		FCrest Best;
		int32 Top = INDEX_NONE;
		const int32 Last = FMath::Min(To, FMath::Min(Times.Num(), Z.Num()));
		for (int32 Index = FMath::Max(From, 0); Index < Last; ++Index)
		{
			if (Top == INDEX_NONE || Z[Index] > Z[Top])
			{
				Top = Index;
			}
		}
		if (Top == INDEX_NONE)
		{
			return Best;
		}
		Best.Time = Times[Top];
		Best.Height = Z[Top];
		Best.bFound = true;
		double Time = 0.0;
		double Height = 0.0;
		if (Top > FMath::Max(From, 0) && Top + 1 < Last && ParabolaPeak(Times[Top - 1], Z[Top - 1], Times[Top], Z[Top], Times[Top + 1], Z[Top + 1], Time, Height))
		{
			Best.Time = Time;
			Best.Height = FMath::Max(Height, Z[Top]);
		}
		return Best;
	}

	/** Probe readings as they come back: the points, and per reading the world time of its frame and a height per point. */
	struct FProbeLog
	{
		TArray<FVector2D> Points;
		TArray<double> Times;
		TArray<TArray<float>> Heights;
		double LastTime = -1.0;

		/** The newest reading, when it is newer than the last one taken (readbacks lag a few frames; a frame may be skipped). */
		bool Take(const IFluidNinjaBuoyancyBenchWaves& Ripples)
		{
			double Time = 0.0;
			TArray<float> Read;
			if (!Ripples.GetProbeResults(Time, Read) || Time <= LastTime || Read.Num() != Points.Num())
			{
				return false;
			}
			LastTime = Time;
			Times.Add(Time);
			Heights.Add(MoveTemp(Read));
			return true;
		}
	};

	/** One poke of ripple-dispersion (one frame rate of ripple-fps) and what its probes read. */
	struct FPokeRun
	{
		FString Label;
		/** The frame rate it ran at (0: whatever the run had). */
		float FixedFps = 0.f;
		FVector2D Centre = FVector2D::ZeroVector;
		/** Open water around the poke (cm): banks further than the rings' reach do not send anything back in time. */
		double OpenCm = 0.0;
		/** World time the poke went in: the start of the frame whose ticks took it. */
		double PokeTime = -1.0;
		double RecordSeconds = 0.0;
		float HeightCm = PokeHeightCm;
		float SigmaCm = PokeSigmaCm;
		/** The core as it ran, for the theory: its ticks a second, the ticks of the poke's frame (the impulse is spread over them), the
		 * depth it runs at the poke and the least and most within the furthest ring's reach (its Wave Aux), its cell, damping and
		 * hyperviscosity. */
		double StepRate = 60.0;
		int32 Ticks = 1;
		double DepthCm = 0.0;
		double ShallowestCm = 0.0;
		double DeepestCm = 0.0;
		double CellCm = 1.0;
		double Damping = 0.0;
		double Hyper = 0.0;
		/** Per probe, its distance from the poke. */
		TArray<double> Radii;
		FProbeLog Log;
		int32 Frames = 0;
		double FrameSum = 0.0;
		float LongestFrame = 0.f;
		/** The wave grid's cells across the window (the N its dispersion symbol is built on), and the interface its status line gives the
		 * core (-1: not found). */
		int32 CoreCells = 0;
		int32 CoreInterface = -1;
		/** The core's ticks per frame over the poke's frame and the reading, as the bench models them: the core's fixed-step accumulator
		 * carried over the frames' lengths (seeded half a tick in, so a fixed step's float rounding never flips a count; the model's phase
		 * is not the core's). One count throughout is regular pacing; a run at 49 fps reads 1..2, which one fitted phase cannot absorb. A run
		 * not on a fixed frame step reads 1..2 as well once the accumulator has drifted a whole tick, as the core's own does (a capped run
		 * at 59.9 fps within the ~11 s of the reading), so in practice only fixed-step runs are regular all through: TickFrames, the frames
		 * per count (0, 1, 2, 3 or more ticks), tells one catch-up frame from a sustained 1/2 alternation. TicksLeast > TicksMost: no frame
		 * counted. */
		double TickAccumulator = 0.5;
		int32 TicksLeast = TNumericLimits<int32>::Max();
		int32 TicksMost = 0;
		int32 TickFrames[4] = {};
		/** The core's own count per frame beside the model (information): its Sim.Delta in the waves' status line (CoreTicksOf), read one or
		 * two frames late: the bench ticks before the waves build the line, and Sim.Delta is this frame's ticks where Niagara ticks before
		 * FluidNinja Buoyancy, the frame before's where it ticks after (the line says which). It counts only when the line was rebuilt on every
		 * counted frame (bStatusEveryFrame; WavesStatusEveryFrame: the debug view or fnb.TestHUD, which -game runs without the panel lack) and
		 * listed the simulation every time (bCoreTicksEveryFrame). */
		int32 CoreTickFrames[4] = {};
		bool bStatusEveryFrame = true;
		bool bCoreTicksEveryFrame = true;

		/** One frame: its length (s), whether the waves rebuilt their status line this frame, and the core's own count as that line has it
		 * (-1: not read this frame). */
		void CountTicks(double FrameSeconds, bool bStatusRebuilt, int32 CoreTicks)
		{
			TickAccumulator += FrameSeconds * StepRate;
			const int32 FrameTicks = FMath::FloorToInt32(TickAccumulator + 1.0e-4);
			TickAccumulator -= FrameTicks;
			TicksLeast = FMath::Min(TicksLeast, FrameTicks);
			TicksMost = FMath::Max(TicksMost, FrameTicks);
			++TickFrames[FMath::Clamp(FrameTicks, 0, 3)];
			bStatusEveryFrame &= bStatusRebuilt;
			bCoreTicksEveryFrame &= CoreTicks >= 0;
			if (CoreTicks >= 0)
			{
				++CoreTickFrames[FMath::Min(CoreTicks, 3)];
			}
		}
	};

	/** One radius of a poke against the core's impulse response (PokeResponse). */
	struct FRadiusFit
	{
		double Radius = 0.0;
		int32 Probes = 0;
		/** The theory's largest crest, the same crest in the readings (mean of the probes at this radius), and the readings' own largest. */
		FCrest Theory;
		FCrest Measured;
		FCrest Largest;
		double NormalisedRms = 0.0;
	};

	struct FPokeFit
	{
		bool bValid = false;
		FString Problem;
		int32 Readings = 0;
		double MeanFps = 0.0;
		/** Where in its tick the core was when the poke went in, less the readings' lag, in ticks (-MaxLagTicks..1; the bench cannot see
		 * either, so it is the best fit). */
		double StepPhase = 0.0;
		TArray<FRadiusFit> Radii;
	};

	/**
	 * A poke's readings against the core's impulse response (PokeResponse). The core runs whole fixed ticks: a reading stamped T seconds
	 * after the poke went in shows floor(φ + T·rate) ticks of it, where φ is how far into a tick its accumulator was at the poke less the
	 * readback's lag in whole ticks (constant for the run, unknown to the bench: a tick of doubt is a 17% RMS error on 10 rad/s waves).
	 * φ in -MaxLagTicks..1 is the one that fits best. Symbol: the theory on the core's own dispersion symbol (PokeResponse).
	 */
	FPokeFit FitPokeRun(const FPokeRun& Poked, bool bStepped = false, const FCoreSymbol* Symbol = nullptr)
	{
		FPokeFit Fit;
		Fit.Readings = Poked.Log.Times.Num();
		Fit.MeanFps = Poked.FrameSum > 0.0 ? Poked.Frames / Poked.FrameSum : 0.0;
		const double Rate = Poked.StepRate;
		if (Poked.PokeTime < 0.0 || Poked.Log.Points.IsEmpty())
		{
			Fit.Problem = TEXT("the poke did not go in");
			return Fit;
		}
		if (Fit.Readings < 20)
		{
			Fit.Problem = FString::Printf(TEXT("only %d probe readings came back"), Fit.Readings);
			return Fit;
		}
		// The theory is one depth's: the core must run one depth over the rings' reach.
		if (Poked.DepthCm <= 0.0 || Poked.DeepestCm - Poked.ShallowestCm > PokeLevelShare * Poked.DepthCm)
		{
			Fit.Problem = FString::Printf(TEXT("the core runs %.0f to %.0f cm deep within the furthest ring's reach, not one depth (the theory's) within %.0f%%"),
				Poked.ShallowestCm, Poked.DeepestCm, 100.0 * PokeLevelShare);
			return Fit;
		}
		// A frame of more than four ticks: too slow a run to read rings by (an editor in the background runs at 3 fps).
		if (Poked.LongestFrame >= 4.0 / Rate)
		{
			Fit.Problem = FString::Printf(TEXT("a frame took %.0f ms while the rings were read (%.1f fps), more than four of the core's ticks (%.0f ms)"),
				Poked.LongestFrame * 1000.f, Fit.MeanFps, 4000.0 / Rate);
			return Fit;
		}
		// The theory at every step, once per distinct distance.
		TArray<double> Distinct;
		TArray<int32> RadiusOf;
		for (const double Radius : Poked.Radii)
		{
			int32 Found = Distinct.IndexOfByPredicate([Radius](double Known) { return FMath::Abs(Known - Radius) < 0.01; });
			if (Found == INDEX_NONE)
			{
				Found = Distinct.Add(Radius);
			}
			RadiusOf.Add(Found);
		}
		const int32 Steps = FMath::CeilToInt32(Poked.RecordSeconds * Rate) + 8;
		TArray<TArray<double>> Table;
		PokeResponse(Poked.HeightCm, Poked.SigmaCm, 1.0 / Rate, Poked.Ticks, Poked.DepthCm, Poked.Damping, Poked.Hyper, Poked.CellCm, Distinct, Steps + 1, 1.0 / Rate, Table,
			bStepped, Symbol);

		// The fit can only change where a reading's tick count does (φ = n - frac(T·rate) for whole n), so one trial inside each stretch
		// between those breaks finds the best exactly.
		TArray<double> Breaks;
		Breaks.Reserve((Fit.Readings + 1) * (MaxLagTicks + 2));
		Breaks.Add(-MaxLagTicks);
		Breaks.Add(1.0);
		for (int32 Reading = 0; Reading < Fit.Readings; ++Reading)
		{
			const double Steps0 = (Poked.Log.Times[Reading] - Poked.PokeTime) * Rate;
			for (int32 Whole = 1; Whole >= 1 - MaxLagTicks; --Whole)
			{
				Breaks.Add(Whole - (Steps0 - FMath::FloorToDouble(Steps0)));
			}
		}
		Breaks.Sort();
		double BestError = TNumericLimits<double>::Max();
		for (int32 Trial = 0; Trial + 1 < Breaks.Num(); ++Trial)
		{
			if (Breaks[Trial + 1] - Breaks[Trial] < 1.0e-9)
			{
				continue;
			}
			const double Phase = 0.5 * (Breaks[Trial] + Breaks[Trial + 1]);
			double Error = 0.0;
			for (int32 Reading = 0; Reading < Fit.Readings; ++Reading)
			{
				const int32 Step = FMath::FloorToInt32(Phase + (Poked.Log.Times[Reading] - Poked.PokeTime) * Rate);
				if (Step < 0 || Step > Steps)
				{
					continue;
				}
				const TArray<float>& Heights = Poked.Log.Heights[Reading];
				for (int32 Probe = 0; Probe < Heights.Num() && Probe < RadiusOf.Num(); ++Probe)
				{
					const double Miss = Heights[Probe] - Table[RadiusOf[Probe]][Step];
					Error += Miss * Miss;
				}
			}
			if (Error < BestError)
			{
				BestError = Error;
				Fit.StepPhase = Phase;
			}
		}
		TArray<int32> StepOf;
		StepOf.SetNumUninitialized(Fit.Readings);
		for (int32 Reading = 0; Reading < Fit.Readings; ++Reading)
		{
			StepOf[Reading] = FMath::FloorToInt32(Fit.StepPhase + (Poked.Log.Times[Reading] - Poked.PokeTime) * Rate);
		}
		const int32 Recorded = FMath::Clamp(FMath::RoundToInt32(Poked.RecordSeconds * Rate), 2, Steps);
		TArray<double> TheoryTimes;
		TheoryTimes.SetNumUninitialized(Recorded + 1);
		for (int32 Step = 0; Step <= Recorded; ++Step)
		{
			TheoryTimes[Step] = Step / Rate;
		}
		for (int32 Index = 0; Index < Distinct.Num(); ++Index)
		{
			FRadiusFit& At = Fit.Radii.AddDefaulted_GetRef();
			At.Radius = Distinct[Index];
			const TArray<double> Theory(Table[Index].GetData(), Recorded + 1);
			At.Theory = LargestIn(TheoryTimes, Theory, 0, Recorded + 1);
			// The readings' crest that is the theory's largest: the largest between the midpoints to the theory's neighbouring crests. Near a
			// wave group's peak neighbouring crests can be within a per cent of each other, so the largest of each record on its own could
			// be a whole period apart through no fault of the solver.
			int32 Largest = 0;
			for (int32 Step = 1; Step <= Recorded; ++Step)
			{
				Largest = Theory[Step] > Theory[Largest] ? Step : Largest;
			}
			int32 Before = INDEX_NONE;
			int32 After = INDEX_NONE;
			for (int32 Step = Largest - 1; Step > 0 && Before == INDEX_NONE; --Step)
			{
				Before = Theory[Step] > Theory[Step - 1] && Theory[Step] >= Theory[Step + 1] ? Step : INDEX_NONE;
			}
			for (int32 Step = Largest + 1; Step < Recorded && After == INDEX_NONE; ++Step)
			{
				After = Theory[Step] > Theory[Step - 1] && Theory[Step] >= Theory[Step + 1] ? Step : INDEX_NONE;
			}
			const double WindowFrom = Before == INDEX_NONE ? 0.0 : 0.5 * (Before + Largest) / Rate;
			const double WindowTo = After == INDEX_NONE ? Recorded / Rate : 0.5 * (Largest + After) / Rate;
			double ErrorSum = 0.0;
			double NormSum = 0.0;
			int32 Matched = 0;
			int32 Own = 0;
			for (int32 Probe = 0; Probe < RadiusOf.Num(); ++Probe)
			{
				if (RadiusOf[Probe] != Index)
				{
					continue;
				}
				++At.Probes;
				TArray<double> SeriesTimes;
				TArray<double> SeriesZ;
				TArray<double> WindowTimes;
				TArray<double> WindowZ;
				for (int32 Reading = 0; Reading < Fit.Readings; ++Reading)
				{
					const int32 Step = StepOf[Reading];
					if (Step < 0 || Step > Recorded || !Poked.Log.Heights[Reading].IsValidIndex(Probe))
					{
						continue;
					}
					const double Time = Step / Rate;
					const double Height = Poked.Log.Heights[Reading][Probe];
					SeriesTimes.Add(Time);
					SeriesZ.Add(Height);
					ErrorSum += FMath::Square(Height - Theory[Step]);
					NormSum += FMath::Square(Theory[Step]);
					if (Time >= WindowFrom && Time <= WindowTo)
					{
						WindowTimes.Add(Time);
						WindowZ.Add(Height);
					}
				}
				const FCrest Same = LargestIn(WindowTimes, WindowZ, 0, WindowTimes.Num());
				if (Same.bFound)
				{
					At.Measured.Time += Same.Time;
					At.Measured.Height += Same.Height;
					++Matched;
				}
				const FCrest Mine = LargestIn(SeriesTimes, SeriesZ, 0, SeriesTimes.Num());
				if (Mine.bFound)
				{
					At.Largest.Time += Mine.Time;
					At.Largest.Height += Mine.Height;
					++Own;
				}
			}
			if (Matched > 0)
			{
				At.Measured.Time /= Matched;
				At.Measured.Height /= Matched;
				At.Measured.bFound = true;
			}
			if (Own > 0)
			{
				At.Largest.Time /= Own;
				At.Largest.Height /= Own;
				At.Largest.bFound = true;
			}
			At.NormalisedRms = NormSum > 0.0 ? FMath::Sqrt(ErrorSum / NormSum) : 0.0;
		}
		Fit.bValid = true;
		return Fit;
	}

	/** A probe's record once the wave has come (|z| past a tenth of its largest): its first crest and first trough (positive depth), the
	 * largest of each, and which came first. Lobes smaller than that tenth are noise between them and are passed over. */
	struct FLobes
	{
		double FirstCrest = 0.0;
		double FirstTrough = 0.0;
		double MaxCrest = 0.0;
		double MaxTrough = 0.0;
		bool bTroughFirst = false;
		bool bFound = false;
	};

	FLobes MeasureLobes(const TArray<double>& Z)
	{
		FLobes Out;
		double Peak = 0.0;
		for (const double Value : Z)
		{
			Peak = FMath::Max(Peak, FMath::Abs(Value));
			Out.MaxCrest = FMath::Max(Out.MaxCrest, Value);
			Out.MaxTrough = FMath::Max(Out.MaxTrough, -Value);
		}
		if (Peak <= 1.0e-4)
		{
			return Out;
		}
		const double Threshold = 0.1 * Peak;
		int32 Index = 0;
		while (Index < Z.Num() && FMath::Abs(Z[Index]) < Threshold)
		{
			++Index;
		}
		if (Index >= Z.Num())
		{
			return Out;
		}
		Out.bTroughFirst = Z[Index] < 0.0;
		bool bCrest = false;
		bool bTrough = false;
		while (Index < Z.Num() && !(bCrest && bTrough))
		{
			const bool bPositive = Z[Index] >= 0.0;
			double Extreme = 0.0;
			while (Index < Z.Num() && (Z[Index] >= 0.0) == bPositive)
			{
				Extreme = FMath::Max(Extreme, bPositive ? Z[Index] : -Z[Index]);
				++Index;
			}
			if (Extreme >= Threshold)
			{
				if (bPositive && !bCrest)
				{
					Out.FirstCrest = Extreme;
					bCrest = true;
				}
				else if (!bPositive && !bTrough)
				{
					Out.FirstTrough = Extreme;
					bTrough = true;
				}
			}
		}
		Out.bFound = bCrest || bTrough;
		return Out;
	}

	/** A probe's first crest and first trough (positive depth) as MeasureLobes reads them, from the readings before Until alone (Times and
	 * Until on one clock): each only once the record has fallen back to half of it, or crossed zero, before Until, so nothing that arrives
	 * from Until on (a reflection) is in it. A first crest or trough cut off by Until is not found, and no later one stands in for it.
	 * The trough counts only after the first crest, as Evers' a_t1 follows c1 (Fig. 3-6): a depression ahead of the crest (a precursor,
	 * a sinking body's draw-down) is passed over, so the trough and H1 = a_c1 + a_t1 are read on one wave. The crest after that trough
	 * is found the same way, for its time alone (the first wave's period, crest to crest). The times are set even for a lobe cut off by
	 * Until (bCrest, bTrough, bNextCrest false), when they are only where the record stopped. */
	struct FWindowedLobes
	{
		double FirstCrest = 0.0;
		double FirstTrough = 0.0;
		double CrestTime = 0.0;
		double TroughTime = 0.0;
		double NextCrestTime = 0.0;
		bool bCrest = false;
		bool bTrough = false;
		bool bNextCrest = false;
	};

	FWindowedLobes MeasureLobesUntil(const TArray<double>& Times, const TArray<double>& Z, double Until)
	{
		FWindowedLobes Out;
		int32 Count = 0;
		double Peak = 0.0;
		for (int32 Sample = 0; Sample < FMath::Min(Times.Num(), Z.Num()) && Times[Sample] < Until; ++Sample)
		{
			Peak = FMath::Max(Peak, FMath::Abs(Z[Sample]));
			Count = Sample + 1;
		}
		if (Peak <= 1.0e-4)
		{
			return Out;
		}
		const double Threshold = 0.1 * Peak;
		int32 Index = 0;
		while (Index < Count && FMath::Abs(Z[Index]) < Threshold)
		{
			++Index;
		}
		bool bCrestSettled = false;   // measured, or cut off by Until
		bool bTroughSettled = false;
		bool bNextSettled = false;
		while (Index < Count && !(bCrestSettled && bTroughSettled && bNextSettled))
		{
			const bool bPositive = Z[Index] >= 0.0;
			double Extreme = 0.0;
			int32 At = Index;
			bool bFallen = false;
			while (Index < Count && (Z[Index] >= 0.0) == bPositive)
			{
				const double Value = bPositive ? Z[Index] : -Z[Index];
				if (Value > Extreme)
				{
					Extreme = Value;
					At = Index;
					bFallen = false;
				}
				else if (Value <= 0.5 * Extreme)
				{
					bFallen = true;
				}
				++Index;
			}
			if (Extreme < Threshold)
			{
				continue;
			}
			// Whole: back to half of it, or past zero, before Until.
			const bool bWhole = bFallen || Index < Count;
			if (bPositive && !bCrestSettled)
			{
				bCrestSettled = true;
				Out.bCrest = bWhole;
				Out.FirstCrest = bWhole ? Extreme : 0.0;
				Out.CrestTime = Times[At];
			}
			else if (!bPositive && !bTroughSettled && bCrestSettled)
			{
				// Only the trough that follows the first crest (a depression before it is passed over).
				bTroughSettled = true;
				Out.bTrough = bWhole;
				Out.FirstTrough = bWhole ? Extreme : 0.0;
				Out.TroughTime = Times[At];
			}
			else if (bPositive && bCrestSettled && bTroughSettled && !bNextSettled)
			{
				// The crest after that trough, for its time only (the first wave's period, crest to crest).
				bNextSettled = true;
				Out.bNextCrest = bWhole;
				Out.NextCrestTime = Times[At];
			}
		}
		return Out;
	}

	/** The wave hull's triangle surface, copied out of the runtime module (UFluidNinjaBuoyancyComponent::CopyWaveHullSurface): the body
	 * frame's vertices and the triangles indexing them, all a test measures a hull by. */
	struct FBenchHull
	{
		TArray<FVector3f> Vertices;
		TArray<FIntVector3> Triangles;
	};

	bool GetBenchHull(const UFluidNinjaBuoyancyComponent& Body, FBenchHull& Out)
	{
		return Body.CopyWaveHullSurface(Out.Vertices, Out.Triangles);
	}

	/** A floating body's hull as it lies now, from the ripples' own triangle surface: its centre seen from above, its heading (its rest
	 * attitude's forward, level) and side, its extents along both, its height, and its centre from the primitive's origin. */
	struct FHullShape
	{
		FVector2D Centre = FVector2D::ZeroVector;
		FVector Along = FVector::XAxisVector;
		FVector Side = FVector::YAxisVector;
		float Length = 0.f;
		float Beam = 0.f;
		float Height = 0.f;
		/** The hull's plan centre in the primitive's own frame (so it can be followed as the body moves). */
		FVector CentreLocal = FVector::ZeroVector;
	};

	bool MeasureHull(const UFluidNinjaBuoyancyComponent& Body, FHullShape& Out)
	{
		const UPrimitiveComponent* Primitive = Body.GetSimulatedPrimitive();
		FBenchHull Hull;
		if (!Primitive || !GetBenchHull(Body, Hull))
		{
			return false;
		}
		const FTransform Frame(Primitive->GetComponentQuat(), Primitive->GetComponentLocation());
		const FQuat Rested = Primitive->GetComponentQuat() * Body.RestAttitude.Quaternion().Inverse();
		FVector Along = Rested.GetForwardVector();
		Along.Z = 0.0;
		Along = Along.GetSafeNormal(UE_SMALL_NUMBER, FVector::XAxisVector);
		const FVector Side(-Along.Y, Along.X, 0.0);
		double MinAlong = TNumericLimits<double>::Max();
		double MaxAlong = -TNumericLimits<double>::Max();
		double MinSide = MinAlong;
		double MaxSide = MaxAlong;
		double MinZ = MinAlong;
		double MaxZ = MaxAlong;
		const FVector Origin = Frame.GetLocation();
		for (const FVector3f& Vertex : Hull.Vertices)
		{
			const FVector World = Frame.TransformPosition(FVector(Vertex));
			const FVector Offset = World - Origin;
			const double OnAlong = Offset | Along;
			const double OnSide = Offset | Side;
			MinAlong = FMath::Min(MinAlong, OnAlong);
			MaxAlong = FMath::Max(MaxAlong, OnAlong);
			MinSide = FMath::Min(MinSide, OnSide);
			MaxSide = FMath::Max(MaxSide, OnSide);
			MinZ = FMath::Min(MinZ, World.Z);
			MaxZ = FMath::Max(MaxZ, World.Z);
		}
		Out.Along = Along;
		Out.Side = Side;
		Out.Length = static_cast<float>(MaxAlong - MinAlong);
		Out.Beam = static_cast<float>(MaxSide - MinSide);
		Out.Height = static_cast<float>(MaxZ - MinZ);
		const FVector Middle = Along * (0.5 * (MinAlong + MaxAlong)) + Side * (0.5 * (MinSide + MaxSide));
		Out.Centre = FVector2D(Origin + Middle);
		Out.CentreLocal = Frame.GetRotation().UnrotateVector(Middle);
		return true;
	}

	/**
	 * The hull's static head under flat water at Level on a grid, as the core's source rasters it (RasterCS, on the CPU): along each
	 * vertical line, the depth of its lowest crossing of the triangles (the hull's bottom) below the water, open interiors included (a row
	 * boat displaces as if filled to the water line). Cell (i, j) lies (FirstAlong + i)·Cell along Along and (FirstAcross + j)·Cell along
	 * Across from Origin; Out holds CountAlong × CountAcross cells, along first (j * CountAlong + i).
	 */
	void HullFootprint(const FBenchHull& Hull, const FTransform& Frame, float Level, const FVector& Origin, const FVector& Along, const FVector& Across,
		double Cell, int32 FirstAlong, int32 FirstAcross, int32 CountAlong, int32 CountAcross, TArray<float>& Out)
	{
		Out.Init(0.f, CountAlong * CountAcross);
		TArray<float> Bottom;
		Bottom.Init(TNumericLimits<float>::Max(), Out.Num());
		TArray<FVector> Corners;
		Corners.Reserve(Hull.Vertices.Num());
		for (const FVector3f& Vertex : Hull.Vertices)
		{
			const FVector World = Frame.TransformPosition(FVector(Vertex));
			const FVector Offset = World - Origin;
			Corners.Add(FVector((Offset | Along) / Cell - FirstAlong, (Offset | Across) / Cell - FirstAcross, World.Z));   // in cells, and Z
		}
		constexpr double Tolerance = 1.0e-4;   // a line through a shared edge counts for both triangles (only the lowest crossing is kept)
		for (const FIntVector3& Triangle : Hull.Triangles)
		{
			const FVector& P0 = Corners[Triangle.X];
			const FVector& P1 = Corners[Triangle.Y];
			const FVector& P2 = Corners[Triangle.Z];
			const double Det = (P1.Y - P2.Y) * (P0.X - P2.X) + (P2.X - P1.X) * (P0.Y - P2.Y);
			if (FMath::Abs(Det) < 1.0e-9)
			{
				continue;   // a wall seen edge-on from above
			}
			const int32 FromI = FMath::Max(FMath::FloorToInt32(FMath::Min3(P0.X, P1.X, P2.X)) - 1, 0);
			const int32 ToI = FMath::Min(FMath::CeilToInt32(FMath::Max3(P0.X, P1.X, P2.X)) + 1, CountAlong - 1);
			const int32 FromJ = FMath::Max(FMath::FloorToInt32(FMath::Min3(P0.Y, P1.Y, P2.Y)) - 1, 0);
			const int32 ToJ = FMath::Min(FMath::CeilToInt32(FMath::Max3(P0.Y, P1.Y, P2.Y)) + 1, CountAcross - 1);
			for (int32 J = FromJ; J <= ToJ; ++J)
			{
				for (int32 I = FromI; I <= ToI; ++I)
				{
					const double L0 = ((P1.Y - P2.Y) * (I - P2.X) + (P2.X - P1.X) * (J - P2.Y)) / Det;
					const double L1 = ((P2.Y - P0.Y) * (I - P2.X) + (P0.X - P2.X) * (J - P2.Y)) / Det;
					const double L2 = 1.0 - L0 - L1;
					if (L0 < -Tolerance || L1 < -Tolerance || L2 < -Tolerance)
					{
						continue;
					}
					const int32 Index = J * CountAlong + I;
					Bottom[Index] = FMath::Min(Bottom[Index], static_cast<float>(L0 * P0.Z + L1 * P1.Z + L2 * P2.Z));
				}
			}
		}
		for (int32 Index = 0; Index < Out.Num(); ++Index)
		{
			Out[Index] = Bottom[Index] < TNumericLimits<float>::Max() ? FMath::Max(0.f, Level - Bottom[Index]) : 0.f;
		}
	}

	/** A complex number for the bench's FFT. */
	struct FBenchComplex
	{
		double Re = 0.0;
		double Im = 0.0;
	};

	/** In place, N a power of two: forward e^{-i k x}, inverse e^{+i k x} with the 1/N. */
	void Fft1D(FBenchComplex* Data, int32 N, bool bInverse)
	{
		for (int32 I = 1, J = 0; I < N; ++I)
		{
			int32 Bit = N >> 1;
			for (; J & Bit; Bit >>= 1)
			{
				J ^= Bit;
			}
			J ^= Bit;
			if (I < J)
			{
				Swap(Data[I], Data[J]);
			}
		}
		for (int32 Span = 2; Span <= N; Span <<= 1)
		{
			const double Angle = (bInverse ? 2.0 : -2.0) * UE_DOUBLE_PI / Span;
			const double StepRe = FMath::Cos(Angle);
			const double StepIm = FMath::Sin(Angle);
			for (int32 First = 0; First < N; First += Span)
			{
				double TurnRe = 1.0;
				double TurnIm = 0.0;
				for (int32 K = 0; K < Span / 2; ++K)
				{
					FBenchComplex& Even = Data[First + K];
					FBenchComplex& Odd = Data[First + K + Span / 2];
					const double OddRe = Odd.Re * TurnRe - Odd.Im * TurnIm;
					const double OddIm = Odd.Re * TurnIm + Odd.Im * TurnRe;
					Odd.Re = Even.Re - OddRe;
					Odd.Im = Even.Im - OddIm;
					Even.Re += OddRe;
					Even.Im += OddIm;
					const double NextRe = TurnRe * StepRe - TurnIm * StepIm;
					TurnIm = TurnRe * StepIm + TurnIm * StepRe;
					TurnRe = NextRe;
				}
			}
		}
		if (bInverse)
		{
			for (int32 I = 0; I < N; ++I)
			{
				Data[I].Re /= N;
				Data[I].Im /= N;
			}
		}
	}

	/** N x N, row-major (index J * N + I): the rows, then the columns. */
	void Fft2D(TArray<FBenchComplex>& Grid, int32 N, bool bInverse)
	{
		for (int32 Row = 0; Row < N; ++Row)
		{
			Fft1D(Grid.GetData() + Row * N, N, bInverse);
		}
		TArray<FBenchComplex> Column;
		Column.SetNumUninitialized(N);
		for (int32 Col = 0; Col < N; ++Col)
		{
			for (int32 Row = 0; Row < N; ++Row)
			{
				Column[Row] = Grid[Row * N + Col];
			}
			Fft1D(Column.GetData(), N, bInverse);
			for (int32 Row = 0; Row < N; ++Row)
			{
				Grid[Row * N + Col] = Column[Row];
			}
		}
	}

	/**
	 * The exact linear wake of a hull towed from rest at Speed along +x, from the core's own equation: with B = η + P, η_tt = -ω²(η + P) - Γ η_t
	 * is B_tt + Γ B_t + ω² B = P_tt + Γ P_t, with P(x - U t, y) from the start (t = 0) on and B = 0 before it (the hull at rest in its hollow,
	 * η = -P). B_t jumps by P_t at the start (η_t does not), so for a = U k_x and ω'² = ω² - Γ²/4, T seconds after the start
	 *   B̂ = K e^{-iaT} + e^{-ΓT/2} (-K cos ω'T + C sin ω'T),   K = -D̂₀ a (a + iΓ) / (ω² - a² - iaΓ),   C = (K (ia - Γ/2) - ia D̂₀) / ω':
	 * the steady wake the hull carries with it, plus the free waves its start sent out (at Γ = 0, D̂₀ (a² cos ωT - a² e^{-iaT} - i a ω sin ωT)
	 * / (ω² - a²)). Spectrum is D̂₀ (the footprint where it started, forward FFT); Omega and Gamma are ω and Γ on the grid; Field gets the
	 * real field (cm).
	 */
	void TowedWake(const TArray<FBenchComplex>& Spectrum, const TArray<double>& Omega, const TArray<double>& Gamma, int32 N, double Cell, double Speed,
		double Seconds, TArray<FBenchComplex>& Work, TArray<double>& Field)
	{
		Work.SetNumUninitialized(N * N);
		const double Unit = 2.0 * UE_DOUBLE_PI / (N * Cell);
		const auto Mul = [](const FBenchComplex& X, const FBenchComplex& Y) { return FBenchComplex{X.Re * Y.Re - X.Im * Y.Im, X.Re * Y.Im + X.Im * Y.Re}; };
		for (int32 J = 0; J < N; ++J)
		{
			for (int32 I = 0; I < N; ++I)
			{
				const int32 Index = J * N + I;
				const double A = (I < N / 2 ? I : I - N) * Unit * Speed;
				const double W = Omega[Index];
				const double G = Gamma[Index];
				const double Turning = FMath::Sqrt(FMath::Max(W * W - 0.25 * G * G, 0.0));
				const FBenchComplex& D = Spectrum[Index];
				// K = -D a (a + iΓ) / (ω² - a² - iaΓ).
				const FBenchComplex Top = Mul(D, FBenchComplex{-A * A, -A * G});
				const double BottomRe = W * W - A * A;
				const double BottomIm = -A * G;
				const double Size = BottomRe * BottomRe + BottomIm * BottomIm;
				if (Size < 1.0e-30 || Turning < 1.0e-12)
				{
					Work[Index] = FBenchComplex();   // no wave (k = 0, or overdamped)
					continue;
				}
				const FBenchComplex K{(Top.Re * BottomRe + Top.Im * BottomIm) / Size, (Top.Im * BottomRe - Top.Re * BottomIm) / Size};
				// C = (K (ia - Γ/2) - ia D) / ω'.
				const FBenchComplex KTurn = Mul(K, FBenchComplex{-0.5 * G, A});
				const FBenchComplex C{(KTurn.Re + A * D.Im) / Turning, (KTurn.Im - A * D.Re) / Turning};
				const FBenchComplex Steady = Mul(K, FBenchComplex{FMath::Cos(A * Seconds), -FMath::Sin(A * Seconds)});
				const double Fade = FMath::Exp(-0.5 * G * Seconds);
				const double CosW = FMath::Cos(Turning * Seconds);
				const double SinW = FMath::Sin(Turning * Seconds);
				Work[Index] = {Steady.Re + Fade * (-K.Re * CosW + C.Re * SinW), Steady.Im + Fade * (-K.Im * CosW + C.Im * SinW)};
			}
		}
		Work[0] = FBenchComplex();   // no change of volume: the hull's own was in place before it started
		Fft2D(Work, N, true);
		Field.SetNumUninitialized(N * N);
		for (int32 Index = 0; Index < N * N; ++Index)
		{
			Field[Index] = Work[Index].Re;
		}
	}

	/** A field on an N x N grid at a point in cells (bilinear; 0 outside). */
	double SampleGrid(const TArray<double>& Field, int32 N, double X, double Y)
	{
		const int32 I = FMath::FloorToInt32(X);
		const int32 J = FMath::FloorToInt32(Y);
		if (I < 0 || J < 0 || I + 1 >= N || J + 1 >= N)
		{
			return 0.0;
		}
		const double FI = X - I;
		const double FJ = Y - J;
		return Field[J * N + I] * (1.0 - FI) * (1.0 - FJ) + Field[J * N + I + 1] * FI * (1.0 - FJ)
			+ Field[(J + 1) * N + I] * (1.0 - FI) * FJ + Field[(J + 1) * N + I + 1] * FI * FJ;
	}

	/** A series taken over time (times rising) at Time: linear between its samples, held at its ends; 0 when it is empty. */
	double SampleSeries(const TArray<double>& Times, const TArray<double>& Values, double Time)
	{
		const int32 Count = FMath::Min(Times.Num(), Values.Num());
		if (Count == 0)
		{
			return 0.0;
		}
		if (Time <= Times[0])
		{
			return Values[0];
		}
		for (int32 Index = 1; Index < Count; ++Index)
		{
			if (Times[Index] >= Time)
			{
				const double Span = Times[Index] - Times[Index - 1];
				const double Along = Span > 0.0 ? (Time - Times[Index - 1]) / Span : 1.0;
				return Values[Index - 1] + (Values[Index] - Values[Index - 1]) * Along;
			}
		}
		return Values[Count - 1];
	}

	/**
	 * One side of an arc's wake envelope (Envelope[d]: the largest |ζ| d degrees off the track, d = 0 on it): the degree of its largest
	 * value off the track (the track itself is left out: both sides share it). With an Anchor, instead the top of the lobe the anchor lies
	 * in, climbing from it to the nearest local maximum, so a second envelope is read on the same lobe as the first: close behind a hull
	 * the bow's and the stern's lobes can be within a few per cent of each other, and the largest alone may pick either. INDEX_NONE when
	 * there is no envelope to read.
	 */
	int32 EnvelopePeak(const TArray<double>& Envelope, int32 Anchor = INDEX_NONE)
	{
		if (Envelope.Num() < 3)
		{
			return INDEX_NONE;
		}
		if (Anchor == INDEX_NONE)
		{
			int32 Top = 1;
			for (int32 Off = 2; Off < Envelope.Num(); ++Off)
			{
				if (Envelope[Off] > Envelope[Top])
				{
					Top = Off;
				}
			}
			return Top;
		}
		int32 At = FMath::Clamp(Anchor, 1, Envelope.Num() - 1);
		for (;;)
		{
			const bool bOut = At + 1 < Envelope.Num() && Envelope[At + 1] > Envelope[At];
			const bool bIn = At - 1 >= 1 && Envelope[At - 1] > Envelope[At];
			if (bOut && (!bIn || Envelope[At + 1] >= Envelope[At - 1]))
			{
				++At;
			}
			else if (bIn)
			{
				--At;
			}
			else
			{
				return At;
			}
		}
	}

	/**
	 * The caustic of the lobe whose top is at Top (EnvelopePeak): the angle where the envelope falls outward of it to CausticShare of its
	 * value, interpolated between whole degrees. False when it does not fall that far within the arc.
	 */
	bool CausticAngle(const TArray<double>& Envelope, int32 Top, double& OutDegrees)
	{
		if (!Envelope.IsValidIndex(Top) || Envelope[Top] <= 0.0)
		{
			return false;
		}
		const double Level = CausticShare * Envelope[Top];
		for (int32 Off = Top + 1; Off < Envelope.Num(); ++Off)
		{
			if (Envelope[Off] <= Level)
			{
				const double Above = Envelope[Off - 1];
				OutDegrees = (Off - 1) + (Above - Level) / FMath::Max(Above - Envelope[Off], UE_DOUBLE_SMALL_NUMBER);
				return true;
			}
		}
		return false;
	}

	/** The cusp line through the caustics of two arcs behind the hull (radius cm, degrees off the track): its angle to the track, degrees.
	 * The line, not the angle seen from the hull: the bow's and stern's wedges start ahead of and behind the hull's centre. */
	double CuspLineAngle(double NearCm, double NearDegrees, double FarCm, double FarDegrees)
	{
		const double NearRadians = FMath::DegreesToRadians(NearDegrees);
		const double FarRadians = FMath::DegreesToRadians(FarDegrees);
		return FMath::RadiansToDegrees(FMath::Atan2(FarCm * FMath::Sin(FarRadians) - NearCm * FMath::Sin(NearRadians),
			FarCm * FMath::Cos(FarRadians) - NearCm * FMath::Cos(NearRadians)));
	}

	/** Whether a line through two arcs' caustics can be one wave system's cusp line: it opens away from the track, within the arcs' reach.
	 * A line outside that pairs different lobes (the bow's caustic on one arc, the stern's on the other). */
	bool IsCuspLine(double LineDegrees)
	{
		return LineDegrees > 0.0 && LineDegrees < WakeArcDegrees;
	}

	/** ripple-weight: one drop, and what it made. */
	struct FDropRun
	{
		TWeakObjectPtr<UFluidNinjaBuoyancyComponent> Body;
		FString Name;
		/** The mass asked for (0: the body's own); the body settles at it before the drop. */
		float MassKg = 0.f;
		/** Displacement alone, or with the impulse. */
		bool bImpulse = false;
		FVector2D Centre = FVector2D::ZeroVector;
		float WaterZ = 0.f;
		/** The depth the core runs under the hull (its Wave Aux), and the fastest wave its window holds there (CoreFastestSpeed). */
		float DepthCm = 0.f;
		bool bDepthKnown = false;
		double FastestCmS = 0.0;
		/** Evers' slide: thickness s along the gamma = 0 axis (across the hull's shorter side), width b across it, and the hull's height. */
		float ThicknessCm = 0.f;
		float WidthCm = 0.f;
		float HeightCm = 0.f;
		FVector2D Axis = FVector2D(1.0, 0.0);
		/** Per probe: distance from the hull's centre and angle from the axis (0..90 deg). */
		TArray<double> Radii;
		TArray<double> Gammas;
		FProbeLog Log;
		TArray<double> StatTimes;
		TArray<double> Volumes;    // cm³
		TArray<double> Energies;   // J, potential and kinetic
		/** The static head's volume over the window, Σ P · cell area (cm³, from the same readback as the stats), and when. */
		TArray<double> SourceTimes;
		TArray<double> SourceCm3;
		double LastStats = -1.0;
		/** The water the hull pushed aside as the body reported it, every frame (cm³), and when. */
		TArray<double> DisplacedTimes;
		TArray<double> DisplacedCm3;
		/** Open water around the hull's centre before the drop: how far to the nearest wall, shore or the window's sponge (cm). */
		double ClearCm = 0.0;
		/** Half the hull's plan diagonal (cm): its rings leave from its edge, not its centre. */
		double ReachCm = 0.0;
		double DropTime = -1.0;
		double ImpactTime = -1.0;
		float ImpactSpeed = 0.f;   // cm/s
		bool bImpacted = false;
		float LastBottom = 0.f;
		float LastFallSpeed = 0.f;
		float DroppedMassKg = 0.f;
		float DisplacedLitres = 0.f;
		/** ripple-weight: the wave hull's volume under WaterZ every frame before the lift (cm³, its triangles clipped on the CPU at 1 cm), and
		 * when. Information beside the static head's judged reference, m / ρ: WaterZ is one frame's reading of the level, taken as the waves
		 * were cleared, and every mm it is off moves this volume by the hull's waterplane area (the rest's spread limit is still scaled by it). */
		TArray<double> HullTimes;
		TArray<double> HullCm3;
		/** ripple-weight: when the body was put back at its start, when it was first seen let go on its water after that, and when it counted
		 * as at rest (-1: not yet, or not by the limit). */
		double PlacedTime = -1.0;
		double FloatingTime = -1.0;
		double SettledTime = -1.0;
		/** ripple-weight: over the last second when the rest was last judged: the static head's spread and its readings, the wave hull's
		 * mean volume under the water (cm³), and its fastest vertical speed (cm/s; -1: held, no physics). Then, looking back from that
		 * moment, since when it had moved slower than WeightSettledCmS without a break, and since when its static head had stayed within the
		 * spread's limit (no earlier than the first reading of each). */
		double SettleSpreadCm3 = 0.0;
		int32 SettleReadings = 0;
		double SettleHullCm3 = 0.0;
		double SettleFastestCmS = -1.0;
		double StillSince = -1.0;
		double SteadySince = -1.0;
		bool bSettleJudged = false;
		/** ripple-weight: the body's vertical speed every frame from its put-back to the drop (cm/s; -1 while it was held), and when. */
		TArray<double> RiseTimes;
		TArray<double> RiseCmS;
		/** ripple-evers: the block made for the drop (destroyed after it), and the longest frame while it fell and its rings were read. */
		TWeakObjectPtr<AActor> Spawned;
		float LongestFrame = 0.f;
	};

	/** ripple-kelvin: the tow and its wake. */
	struct FTowRun
	{
		TWeakObjectPtr<UFluidNinjaBuoyancyComponent> Boat;
		FVector Direction = FVector::XAxisVector;
		FVector Side = FVector::YAxisVector;
		/** Open water along the run (cm), how long the tow goes on, and the hull's size. */
		double RunCm = 0.0;
		double Seconds = TowLongestSeconds;
		float LengthCm = 0.f;
		float BeamCm = 0.f;
		FVector CentreLocal = FVector::ZeroVector;
		/** The core along the run, for the theory: the depth it runs (its Wave Aux: the mean, the least and the most), its cell, damping and
		 * hyperviscosity. */
		double DepthCm = 0.0;
		double ShallowestCm = 0.0;
		double DeepestCm = 0.0;
		double CellCm = 1.0;
		double Damping = 0.0;
		double Hyper = 0.0;
		double TowStart = -1.0;
		TArray<double> PoseTimes;
		TArray<FVector2D> PoseCentres;
		/** The hull's depth below the water as it went (at the first look at the wake), on the theory's grid around its centre. */
		TArray<float> Footprint;
		int32 FootFirstAlong = 0;
		int32 FootFirstAcross = 0;
		int32 FootAlong = 0;
		int32 FootAcross = 0;
		struct FSnapshot
		{
			double Time = 0.0;
			double Offset = 0.0;
			TArray<FVector2D> Points;
			TArray<float> Heights;
		};
		TArray<FSnapshot> Snapshots;
		bool bPending = false;
		double PendingOffset = 0.0;
		TArray<FVector2D> PendingPoints;
		int32 NextOffset = 0;
	};

	/** ripple-rest: the boat at rest and the water around it, at one coupling. */
	struct FRestRun
	{
		TWeakObjectPtr<UFluidNinjaBuoyancyComponent> Boat;
		/** κ for the run (RestCouplings, written to the Wave Feel Scale setting). */
		float Coupling = 0.f;
		double RingCm = 0.0;
		FProbeLog Log;
		TArray<double> ZTimes;
		TArray<double> Z;
	};

	/** ripple-coupling: the boat's drop and the ball beside it. */
	struct FCouplingRun
	{
		TWeakObjectPtr<UFluidNinjaBuoyancyComponent> Boat;
		TWeakObjectPtr<UFluidNinjaBuoyancyComponent> Ball;
		FVector2D BallPoint = FVector2D::ZeroVector;
		/** The boat's hull centre where it rests and is dropped from (put back at its start before each drop): the ring's distance to the
		 * ball is measured from it. */
		FVector2D BoatCentre = FVector2D::ZeroVector;
		bool bBoatCentre = false;
		FProbeLog Log;
		TArray<double> BallTimes;
		TArray<double> BallZ;
		/** Where the ball floated (world XY) at each height: the probe stays where the ball was at the clear, so a ball the waves push off
		 * it is read against water it no longer floats on. */
		TArray<FVector2D> BallXY;
		/** The ball's state at each height (information): its readout, whether its rigid body is awake, its vertical speed. */
		TArray<FString> BallState;
		/** Since when the ball has bobbed slower than SettledBallCmS before the drop (-1: not yet). */
		double StillSince = -1.0;
		double DropTime = -1.0;
		/** The same drop again with the ball out of the water, read at its place: the ring that reaches it (the reference). */
		FProbeLog Incident;
		double IncidentDropTime = -1.0;
		/** The heading the ball was placed at (degrees from the hull's side axis: 0 broadside, 90 end-on), how far the ball point lies from
		 * the nearest wall or shore (cm, the water map's, at most 1000 read) and how the heading was chosen, for the result line. */
		double GammaDeg = -1.0;
		double WallCm = 0.0;
		FString HeadingText;
	};

	/** ripple-evers: the block, where it falls and its probes, planned once for both drops. */
	struct FEversPlan
	{
		bool bPlanned = false;
		/** Where it falls (its plan centre), the gamma = 0 axis (across its wide face) and its yaw, and the still water there. */
		FVector2D Centre = FVector2D::ZeroVector;
		FVector2D Axis = FVector2D(1.0, 0.0);
		double YawDegrees = 0.0;
		float WaterZ = 0.f;
		double DepthCm = 0.0;
		double ClearCm = 0.0;
		/** The block: s along the axis, b across it, its height, its mass, and how far above the water its bottom is let go (cm, kg). */
		double ThicknessCm = 0.0;
		double WidthCm = 0.0;
		double HeightCm = 0.0;
		double MassKg = 0.0;
		double DropCm = 0.0;
		/** Half its plan diagonal (cm): its waves leave from its edge. */
		double ReachCm = 0.0;
		/** The fastest wave the window holds at the depth the core runs there (cm/s, CoreFastestSpeed): no reflection is taken to come back
		 * slower than that. */
		double FastestCmS = 0.0;
		/** How long each drop is read for (its fall, then until the last probe's reflection can arrive). */
		double RecordSeconds = 0.0;
		/** Per probe: its point, r from the block's centre, gamma, the depth there, and the shortest way a reflection can come to it (cm: from
		 * the block's edge to the nearest wall, shore or the window's sponge, then on to the probe). */
		TArray<FVector2D> Points;
		TArray<double> Radii;
		TArray<double> Gammas;
		TArray<double> Depths;
		TArray<double> ReflectCm;
		/** The probes left out, and why. */
		TArray<FString> LeftOut;
	};

	/** ripple-advection: where the poke goes, the current there and the line of probes down it, chosen once for both pokes. */
	struct FAdvectPlan
	{
		bool bPlanned = false;
		/** The river floater whose water the poke goes into: the current under it is the local current (its readout, every frame). */
		TWeakObjectPtr<UFluidNinjaBuoyancyComponent> Floater;
		FString FloaterName;
		FVector2D Spot = FVector2D::ZeroVector;
		/** Down the current at the poke, and how fast it ran there (cm/s). */
		FVector2D Direction = FVector2D(1.0, 0.0);
		double SpeedCmS = 0.0;
		double ClearCm = 0.0;
		/** The probes, and each one's place along the line from the poke (cm, positive down the current). */
		TArray<FVector2D> Points;
		TArray<double> Along;
		/** The core's own drift down the line at each probe (cm/s, its Wave Aux: cells per tick times the cell and the ticks a second), read
		 * as the carried ring was poked and as it was let go. */
		TArray<double> CoreAtPoke;
		TArray<double> CoreAtEnd;
	};

	/** ripple-advection: one poke, and its ring as the line of probes read it (in the current, or with the ripples not carried). */
	struct FAdvectRun
	{
		bool bCarried = true;
		double PokeTime = -1.0;
		FProbeLog Log;
		/** The current under the floater (cm/s, world XY) every frame of the run, and when. */
		TArray<double> FlowTimes;
		TArray<FVector2D> Flows;
		float LongestFrame = 0.f;
	};

	/** The ripples' status line for the panel: broken after its "; " into lines at most Width long. */
	FString WrapStatus(const FString& Line, int32 Width, const TCHAR* Indent)
	{
		TArray<FString> Parts;
		Line.ParseIntoArray(Parts, TEXT("; "));
		FString Out;
		FString Current;
		for (const FString& Part : Parts)
		{
			if (!Current.IsEmpty() && Current.Len() + 2 + Part.Len() > Width)
			{
				Out += Current + TEXT(";\n") + Indent;
				Current.Reset();
			}
			Current += Current.IsEmpty() ? Part : TEXT("; ") + Part;
		}
		return Out + Current;
	}
}

/** One body followed through a test: its peaks, and per phase its mean velocity, yaw rate, sinking and water over the window. */
struct FFluidNinjaBuoyancyTestBench::FTrack
{
	TWeakObjectPtr<UFluidNinjaBuoyancyComponent> Body;
	FString Name;
	bool bEligible = true;
	float MaxSpeed = 0.f;
	float MaxSlam = 0.f;
	float MaxEntry = 0.f;
	bool bBroke = false;
	bool bSawHeld = false;
	bool bSawDormant = false;
	bool bWasInWater = false;
	/** World time the body last touched the water (a light float on live waves leaves it for a moment now and then). */
	double LastInWater = -1.0;
	float Yaw = 0.f;            // heading, unwrapped
	float LastRawYaw = 0.f;
	FVector WindowLocation = FVector::ZeroVector;
	float WindowYaw = 0.f;
	double WindowTime = -1.0;
	FVector Velocity[MaxPhases];
	float YawRate[MaxPhases] = {};
	float MaxTilt[MaxPhases] = {};
	float MaxSink[MaxPhases] = {};
	float WaterSum[MaxPhases] = {};
	int32 WaterCount[MaxPhases] = {};
	// Height: where each phase began, the deepest it went below that, and its mean over the window.
	float PhaseStartZ[MaxPhases] = {};
	float MaxDrop[MaxPhases] = {};
	double ZSum[MaxPhases] = {};
	int32 ZCount[MaxPhases] = {};
	/** The centre of mass and the attitude when the window opened, and per phase the centre of mass's mean velocity over the window (cm/s),
	 * how far the body turned in all and about the vertical (degrees): Velocity is the primitive's origin's, so a body rocking about a
	 * point off its origin, or bobbing, reads as drift there (drop reports both). */
	FVector WindowCom = FVector::ZeroVector;
	FQuat WindowQuat = FQuat::Identity;
	FVector ComVelocity[MaxPhases] = {};
	float Turned[MaxPhases] = {};
	float YawTurned[MaxPhases] = {};
	/** walkaway: per phase over the window, the body's height against its rest height on the water it knows (Z - (water - rest draft -
	 * rested bottom), cm) summed, that water summed (cm), and the frames counted, with how many of them read it in the capture (W, the
	 * Surface Mirror drawn without the waves) rather than the far field (the water volume's top plus the correction learned in the capture). */
	double RestMissSum[MaxPhases] = {};
	double RestWaterSum[MaxPhases] = {};
	int32 RestCount[MaxPhases] = {};
	int32 RestInCapture[MaxPhases] = {};
};

struct FFluidNinjaBuoyancyTestBench::FPhase
{
	FString Label;
	float Seconds = 0.f;
	/** Seconds into the phase the measuring window opens (velocities, yaw rate, water heights are over the window). */
	float MeasureFrom = 0.f;
	TFunction<void(FFluidNinjaBuoyancyTestBench&, FRun&)> Enter;
	TFunction<void(FFluidNinjaBuoyancyTestBench&, FRun&)> Exit;
	/** Every frame of the phase, the last one too (the ripple tests read their probes and tow the boat here). */
	TFunction<void(FFluidNinjaBuoyancyTestBench&, FRun&)> Tick;
};

/** Where the waves run in the bench's window, as the core sees it (its Wave Aux, read back once per test): pokes and probes must lie over
 * water, clear of the banks. */
struct FFluidNinjaBuoyancyTestBench::FWaterMap
{
	FVector2D Min = FVector2D::ZeroVector;   // world XY of cell (0, 0)'s centre
	double Step = 100.0;
	int32 Size = 0;
	/** Per cell, the least depth the core runs in it (cm): 0 where any of the core's cells in it is dry, or lies in an open window's
	 * sponge. */
	TArray<float> Depth;
	/** The core's own cells as read: (wet mask, depth cm, drift x, drift y in cells per tick), row-major from the window's lower-left. */
	TArray<FLinearColor> Core;
	FIntPoint CoreSize = FIntPoint::ZeroValue;
	FVector2D CoreOrigin = FVector2D::ZeroVector;
	double CoreCellCm = 1.0;

	bool IsValid() const { return Size > 0 && Depth.Num() == Size * Size; }
	FVector2D Centre() const { return Min + FVector2D(0.5 * (Size - 1) * Step); }

	/** The core's cell at a world point, or zeros outside the window. */
	FLinearColor CoreAt(const FVector2D& Point) const
	{
		const int32 X = FMath::FloorToInt32((Point.X - CoreOrigin.X) / CoreCellCm);
		const int32 Y = FMath::FloorToInt32((Point.Y - CoreOrigin.Y) / CoreCellCm);
		return X >= 0 && Y >= 0 && X < CoreSize.X && Y < CoreSize.Y && Core.Num() == CoreSize.X * CoreSize.Y ? Core[Y * CoreSize.X + X] : FLinearColor(0.f, 0.f, 0.f, 0.f);
	}

	/** The least and most depth the core runs over its wet cells within Radius of Point (false: none wet). */
	bool CoreDepthRange(const FVector2D& Point, double Radius, double& OutLeast, double& OutMost) const
	{
		OutLeast = TNumericLimits<double>::Max();
		OutMost = 0.0;
		const int32 Reach = FMath::CeilToInt32(Radius / CoreCellCm);
		const int32 CX = FMath::FloorToInt32((Point.X - CoreOrigin.X) / CoreCellCm);
		const int32 CY = FMath::FloorToInt32((Point.Y - CoreOrigin.Y) / CoreCellCm);
		for (int32 Y = FMath::Max(CY - Reach, 0); Y <= FMath::Min(CY + Reach, CoreSize.Y - 1); ++Y)
		{
			for (int32 X = FMath::Max(CX - Reach, 0); X <= FMath::Min(CX + Reach, CoreSize.X - 1); ++X)
			{
				const FLinearColor& Cell = Core[Y * CoreSize.X + X];
				if (Cell.R > 0.5f && FMath::Square(X - CX) + FMath::Square(Y - CY) <= Reach * Reach)
				{
					OutLeast = FMath::Min(OutLeast, static_cast<double>(Cell.G));
					OutMost = FMath::Max(OutMost, static_cast<double>(Cell.G));
				}
			}
		}
		return OutMost > 0.0;
	}

	/** Beyond the map counts as land. */
	float DepthAt(const FVector2D& Point) const
	{
		if (!IsValid())
		{
			return 0.f;
		}
		const int32 X = FMath::RoundToInt32((Point.X - Min.X) / Step);
		const int32 Y = FMath::RoundToInt32((Point.Y - Min.Y) / Step);
		return X >= 0 && Y >= 0 && X < Size && Y < Size ? Depth[Y * Size + X] : 0.f;
	}

	/** How far from Point the nearest cell that is not open water lies, MaxCm at most. */
	double ClearRadius(const FVector2D& Point, double MaxCm) const
	{
		if (!IsValid())
		{
			return 0.0;
		}
		double Best = MaxCm;
		const int32 Reach = FMath::CeilToInt32(MaxCm / Step);
		const int32 CentreX = FMath::RoundToInt32((Point.X - Min.X) / Step);
		const int32 CentreY = FMath::RoundToInt32((Point.Y - Min.Y) / Step);
		for (int32 DY = -Reach; DY <= Reach; ++DY)
		{
			for (int32 DX = -Reach; DX <= Reach; ++DX)
			{
				const int32 X = CentreX + DX;
				const int32 Y = CentreY + DY;
				const double Distance = FVector2D::Distance(Min + FVector2D(X * Step, Y * Step), Point);
				if (Distance < Best && (X < 0 || Y < 0 || X >= Size || Y >= Size || Depth[Y * Size + X] < FluidNinjaBuoyancyBenchRipples::OpenDepthCm))
				{
					Best = Distance;
				}
			}
		}
		return Best;
	}

	/** Open water at Point with nothing but open water within ClearCm of it. */
	bool IsOpen(const FVector2D& Point, double ClearCm) const
	{
		return DepthAt(Point) >= FluidNinjaBuoyancyBenchRipples::OpenDepthCm && ClearRadius(Point, ClearCm) >= ClearCm;
	}

	/** The most open cell (furthest from any shore, MaxCm at most), the one nearest Near among equals; false without open water. */
	bool MostOpen(const FVector2D& Near, double MaxCm, FVector2D& OutPoint, double& OutClear) const
	{
		if (!IsValid())
		{
			return false;
		}
		TArray<double> Clearance;
		Clearance.Init(-1.0, Size * Size);
		double Most = -1.0;
		for (int32 Index = 0; Index < Size * Size; ++Index)
		{
			if (Depth[Index] >= FluidNinjaBuoyancyBenchRipples::OpenDepthCm)
			{
				Clearance[Index] = ClearRadius(Min + FVector2D((Index % Size) * Step, (Index / Size) * Step), MaxCm);
				Most = FMath::Max(Most, Clearance[Index]);
			}
		}
		if (Most < 0.0)
		{
			return false;
		}
		double BestAway = TNumericLimits<double>::Max();
		for (int32 Index = 0; Index < Size * Size; ++Index)
		{
			const FVector2D Point = Min + FVector2D((Index % Size) * Step, (Index / Size) * Step);
			const double Away = FVector2D::Distance(Point, Near);
			if (Clearance[Index] >= Most - 1.0 && Away < BestAway)
			{
				BestAway = Away;
				OutPoint = Point;
				OutClear = Clearance[Index];
			}
		}
		return true;
	}
};

/** What a ripple test keeps between its phases. */
struct FFluidNinjaBuoyancyTestBench::FRippleData
{
	FWaterMap Water;
	/** ripple-dispersion and ripple-fps: the open spot their pokes go in, and the open water around it (cm). */
	FVector2D Spot = FVector2D::ZeroVector;
	double SpotOpenCm = 0.0;
	TArray<FluidNinjaBuoyancyBenchRipples::FPokeRun> Pokes;
	TArray<FluidNinjaBuoyancyBenchRipples::FDropRun> Drops;
	FluidNinjaBuoyancyBenchRipples::FTowRun Tow;
	/** ripple-rest: one run per coupling (RestCouplings). */
	TArray<FluidNinjaBuoyancyBenchRipples::FRestRun> Rests;
	FluidNinjaBuoyancyBenchRipples::FCouplingRun Coupling;
	FluidNinjaBuoyancyBenchRipples::FEversPlan Evers;
	FluidNinjaBuoyancyBenchRipples::FAdvectPlan AdvectPlan;
	TArray<FluidNinjaBuoyancyBenchRipples::FAdvectRun> Advects;
};

struct FFluidNinjaBuoyancyTestBench::FRun
{
	FString Name;
	TArray<FPhase> Phases;
	TArray<FTrack> Tracks;
	int32 Phase = -1;
	double PhaseStarted = 0.0;
	bool bWindowOpen = false;
	int32 FirstResult = 0;
	float WindDegrees = 0.f;
	float Cost[MaxPhases] = {};
	TFunction<void(FFluidNinjaBuoyancyTestBench&, FRun&)> Evaluate;
	// What the test changes, put back after it.
	bool bWasControlled = false;
	float WasHeight = 0.f;
	bool bWasWindOverride = false;
	float WasWindMs = 0.f;
	float WasWindDegrees = 0.f;
	int32 WasGather = -1;
	/** The waves' project switches as the run found them (a ripple test writes the settings for its measurements), put back after it. */
	float WasFeelScale = 1.f;
	bool bWasDisplacement = true;
	bool bWasImpulse = true;
	float WasCurrentCoupling = 1.f;
	bool bPawnAway = false;
	bool bPawnMoved = false;
	bool bWasSceneryCollision = true;
	/** Put every body back when done (the controlled tests move them, a boat at full throttle a long way). */
	bool bResetAfter = false;
	FTransform PawnReturn;
	FRotator ControlReturn = FRotator::ZeroRotator;
	/** A test outside the ripple tests set the waves' probes and the bench's focus (walkaway): both cleared when it ends, however it ends. */
	bool bOwnsProbes = false;
	/** walkaway, per phase over its window: the waves' own fields and how many feel the waves (least and most, from their status line), and
	 * the mean wave height B (η + P, cm) the open-water probes read, summed over the readings (each the mean of the points inside a running
	 * simulation, the fewest such points in one reading kept), with the readings left out as every point was outside them. */
	struct FWalkawayPhase
	{
		int32 FieldsLeast = TNumericLimits<int32>::Max();
		int32 FieldsMost = -1;
		int32 FeltLeast = TNumericLimits<int32>::Max();
		int32 FeltMost = -1;
		double ProbeSum = 0.0;
		int32 ProbeReadings = 0;
		int32 ProbePointsLeast = TNumericLimits<int32>::Max();
		int32 ProbeOutsideReadings = 0;
	};
	FWalkawayPhase Walkaway[MaxPhases];
	/** walkaway: the open-water probe points (none set: WalkawayProbeNote says why), the newest reading taken, and per point the world time of
	 * the last frame it lay outside every running wave simulation (the waves read such a point as 0; -1: never). */
	TArray<FVector2D> WalkawayProbes;
	FString WalkawayProbeNote;
	double WalkawayProbeTime = -1.0;
	TArray<double> WalkawayProbeOutside;
	/** Stop at the end of this frame: a ripple test that cannot run (it has reported why; nothing is judged). */
	bool bAbort = false;
	// A ripple test, and what it changes, put back after it: the waves' overrides, the frame-rate cap, masses and Waves switches.
	bool bRipple = false;
	/** t.MaxFPS before the test capped it (negative: not capped), and a fixed-step run's step before the test set its own (0 or less: not
	 * set). */
	float WasMaxFps = -1.f;
	double WasFixedDelta = 0.0;
	/** Each body's mass override (kg) and whether it was on, before the test set one. */
	TArray<TPair<TWeakObjectPtr<UPrimitiveComponent>, TPair<float, bool>>> WasMass;
	TArray<TPair<TWeakObjectPtr<UFluidNinjaBuoyancyComponent>, bool>> WasRippleSwitch;
	/** Actors a test made (ripple-evers' block): destroyed when it ends, however it ends. */
	TArray<TWeakObjectPtr<AActor>> Spawned;
	/** The river, when the test started it (ripple-advection): stopped again after, its floaters put back where they were. */
	bool bRiverStartedByTest = false;
	TArray<TPair<TWeakObjectPtr<UFluidNinjaBuoyancyComponent>, FTransform>> RiverPlaced;
	TSharedPtr<FRippleData> Ripple;
};

TSharedPtr<IFluidNinjaBuoyancyBench> FFluidNinjaBuoyancyTestBench::Create(UFluidNinjaBuoyancySubsystem& InSubsystem)
{
#if UE_BUILD_SHIPPING
	return nullptr;
#else
	UWorld* World = InSubsystem.GetWorld();
	if (!World || !World->IsGameWorld() || World->GetNetMode() == NM_DedicatedServer)
	{
		return nullptr;
	}
	TSharedPtr<FFluidNinjaBuoyancyTestBench> Bench = MakeShared<FFluidNinjaBuoyancyTestBench>();
	Bench->Subsystem = &InSubsystem;
	return Bench;
#endif
}

bool FFluidNinjaBuoyancyTestBench::WantsStatusEveryFrame() const
{
	if (IsRunning())
	{
		return true;
	}
	// The panel is up or may come up in this world (fnb.TestHUD): it reads the status every frame.
	const UWorld* World = GetWorld();
	const int32 Mode = CVarTestHUD.GetValueOnGameThread();
	return World && Mode > 0 && (Mode != 1 || World->WorldType == EWorldType::PIE);
}

void FFluidNinjaBuoyancyTestBench::AttachPanel()
{
	// Once the world has floating bodies (a level without any gets no panel), and only where fnb.TestHUD allows it.
	UWorld* World = GetWorld();
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	const int32 Mode = CVarTestHUD.GetValueOnGameThread();
	if (Widget.IsValid() || !World || !Hydro || Mode <= 0 || (Mode == 1 && World->WorldType != EWorldType::PIE) || !FSlateApplication::IsInitialized()
		|| Hydro->GetBodies().IsEmpty())
	{
		return;
	}
	UGameViewportClient* GameViewport = World->GetGameViewport();
	if (!GameViewport)
	{
		return;
	}
	Viewport = GameViewport;
	Widget = Build();
	GameViewport->AddViewportWidgetContent(Widget.ToSharedRef(), 1001);
	Input = MakeShared<FFluidNinjaBuoyancyBenchInput>(AsShared());
	FSlateApplication::Get().RegisterInputPreProcessor(Input);
}

FFluidNinjaBuoyancyTestBench::~FFluidNinjaBuoyancyTestBench()
{
	Remove();
}

void FFluidNinjaBuoyancyTestBench::Remove()
{
	if (Current.IsValid())
	{
		FinishRun(true);
	}
	Queue.Reset();
	SetSceneryCollision(true);
	if (Input.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(Input);
	}
	Input.Reset();
	if (Widget.IsValid())
	{
		if (UGameViewportClient* GameViewport = Viewport.Get())
		{
			GameViewport->RemoveViewportWidgetContent(Widget.ToSharedRef());
		}
	}
	Widget.Reset();
}

UWorld* FFluidNinjaBuoyancyTestBench::GetWorld() const
{
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	return Hydro ? Hydro->GetWorld() : nullptr;
}

bool FFluidNinjaBuoyancyTestBench::IsGameFocused() const
{
	const UGameViewportClient* GameViewport = Viewport.Get();
	const TSharedPtr<SViewport> ViewportWidget = GameViewport ? GameViewport->GetGameViewportWidget() : nullptr;
	return ViewportWidget.IsValid() && (ViewportWidget->HasAnyUserFocusOrFocusedDescendants() || ViewportWidget->HasMouseCapture());
}

FVector FFluidNinjaBuoyancyTestBench::ViewLocation() const
{
	const UWorld* World = GetWorld();
	const APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
	return Controller && Controller->PlayerCameraManager ? Controller->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector;
}

float FFluidNinjaBuoyancyTestBench::ViewYaw() const
{
	const UWorld* World = GetWorld();
	const APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
	return Controller && Controller->PlayerCameraManager ? static_cast<float>(Controller->PlayerCameraManager->GetCameraRotation().Yaw) : 0.f;
}

FString FFluidNinjaBuoyancyTestBench::BodyName(const UFluidNinjaBuoyancyComponent* Body) const
{
	const AActor* Owner = Body ? Body->GetOwner() : nullptr;
	if (!Owner)
	{
		return TEXT("?");
	}
#if WITH_EDITOR
	FString Name = Owner->GetActorLabel();
#else
	FString Name = Owner->GetName();
#endif
	Name.RemoveFromStart(TEXT("Buoyancy Test "));
	return Name;
}

float FFluidNinjaBuoyancyTestBench::Tilt(const UFluidNinjaBuoyancyComponent* Body) const
{
	const UPrimitiveComponent* Primitive = Body ? Body->GetSimulatedPrimitive() : nullptr;
	if (!Primitive)
	{
		return 0.f;
	}
	const FQuat Rested = Primitive->GetComponentQuat() * Body->RestAttitude.Quaternion().Inverse();
	return static_cast<float>(FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Rested.GetUpVector().Z, -1.0, 1.0))));
}

bool FFluidNinjaBuoyancyTestBench::Draft(const UFluidNinjaBuoyancyComponent* Body, float& OutDraft) const
{
	FFluidNinjaBuoyancyBodyDebug Debug;
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (!Hydro || !Body || !Hydro->GetBodyDebug(Body, Debug) || !Debug.bHasWaterZ)
	{
		return false;
	}
	OutDraft = Debug.WaterZ - Body->GetLowestPointZ();
	return true;
}

bool FFluidNinjaBuoyancyTestBench::BodiesWaterHeight(float& OutHeight) const
{
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (!Hydro)
	{
		return false;
	}
	double Sum = 0.0;
	int32 Count = 0;
	for (const UFluidNinjaBuoyancyComponent* Body : Hydro->GetBodies())
	{
		FFluidNinjaBuoyancyBodyDebug Debug;
		if (Hydro->GetBodyDebug(Body, Debug) && Debug.bHasWaterZ && (Debug.bInCapture || Debug.bFlat || Hydro->IsWaterControlled()))
		{
			// Only bodies afloat now (a body stranded on a bank reads the water beside it).
			if (Body->GetReadout().bInWater || Hydro->IsWaterControlled())
			{
				Sum += Debug.WaterZ;
				++Count;
			}
		}
	}
	if (Count == 0)
	{
		return false;
	}
	OutHeight = static_cast<float>(Sum / Count);
	return true;
}

/* ---- Controls ---- */

UFluidNinjaBuoyancyComponent* FFluidNinjaBuoyancyTestBench::GetSelected() const
{
	return Selected.Get();
}

void FFluidNinjaBuoyancyTestBench::Select(int32 Step)
{
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (!Hydro)
	{
		return;
	}
	TArray<UFluidNinjaBuoyancyComponent*> Bodies = Hydro->GetBodies();
	if (Bodies.IsEmpty())
	{
		Selected.Reset();
		return;
	}
	const int32 Index = Bodies.IndexOfByKey(Selected.Get());
	Selected = Bodies[Index == INDEX_NONE ? 0 : (Index + Step % Bodies.Num() + Bodies.Num()) % Bodies.Num()];
}

bool FFluidNinjaBuoyancyTestBench::SelectByName(const FString& Part)
{
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	for (UFluidNinjaBuoyancyComponent* Body : Hydro ? Hydro->GetBodies() : TArray<UFluidNinjaBuoyancyComponent*>())
	{
		if (BodyName(Body).Contains(Part))
		{
			Selected = Body;
			return true;
		}
	}
	return false;
}

void FFluidNinjaBuoyancyTestBench::ResetBodies(bool bAll)
{
	UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (!Hydro)
	{
		return;
	}
	for (UFluidNinjaBuoyancyComponent* Body : Hydro->GetBodies())
	{
		if (bAll || Body == Selected.Get())
		{
			Body->RestartAt(Body->GetStartTransform());
			Body->MarkTeleported();   // put back, not moved there: no ripples from the jump (Restart At marks it too)
		}
	}
}

void FFluidNinjaBuoyancyTestBench::SetControlledWater(bool bOn, const float* Height)
{
	UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (!Hydro)
	{
		return;
	}
	float Level = Hydro->GetControlledWaterHeight();
	if (bOn && !Height)
	{
		// The still water the bodies float on (BodiesWaterHeight: each afloat body's W, the Surface Mirror drawn without the waves, else its
		// far field), never a body's own height, which sits low over a sunk wave field. When no body is afloat, because the test before put
		// every body back and a body put back is held with a slot that knows no water yet, the last such water on live water (Tick keeps
		// it); only with neither, about where the player stands. The view fallback set the 'all' suites' capsize, drop and wind at -31.8 cm
		// on every tier, against -2.7 cm for settle, the first test, whose bodies were still afloat.
		const TCHAR* Source = TEXT("the afloat bodies' still water");
		if (BodiesWaterHeight(Level))
		{
			if (!Hydro->IsWaterControlled())
			{
				LastStillWaterZ = Level;
				bLastStillWater = true;
			}
			else
			{
				Source = TEXT("the controlled water already set");
			}
		}
		else if (bLastStillWater)
		{
			Level = LastStillWaterZ;
			Source = TEXT("the bodies' still water the last time one was afloat on live water (every body is held for its water)");
		}
		else
		{
			Level = static_cast<float>(ViewLocation().Z) - 170.f;
			Source = TEXT("the view's height less 170 cm (no body has known its water yet)");
		}
		UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy test: controlled water at %.1f cm, from %s."), Level, Source);
	}
	Hydro->SetControlledWater(bOn, Height ? *Height : Level);
}

void FFluidNinjaBuoyancyTestBench::SetTestWind(bool bOn, float SpeedMs, const float* Degrees)
{
	if (UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get())
	{
		Hydro->SetWindOverride(bOn, SpeedMs, Degrees ? *Degrees : ViewYaw());
	}
}

void FFluidNinjaBuoyancyTestBench::CycleWind()
{
	WindPreset = (WindPreset + 1) % UE_ARRAY_COUNT(WindPresets);
	const float Speed = WindPresets[WindPreset];
	SetTestWind(Speed >= 0.f, FMath::Max(Speed, 0.f));
}

void FFluidNinjaBuoyancyTestBench::CycleStir()
{
	if (UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get())
	{
		Hydro->SetStirApproach(Hydro->IsStirApproachSet()
			? static_cast<EFluidNinjaBuoyancyStirApproach>((static_cast<uint8>(Hydro->GetStirApproach()) + 1) % 4) : EFluidNinjaBuoyancyStirApproach::Wakes);
		CachedAt = -1.0;
	}
}

void FFluidNinjaBuoyancyTestBench::FindRivers(TArray<AActor*>& OutRivers, TSet<AActor*>& OutRiverNinjas) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	// Looked up once (the panel asks every frame); a level's rivers don't change during play. A river is an actor with StartRiver and
	// StopRiver functions and a SimulationTag naming the NinjaLives it drives (the plugin demo's FluidNinjaBuoyancyDemoRiver).
	if (!bRiversFound)
	{
		bRiversFound = true;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			const FNameProperty* Tag = FindFProperty<FNameProperty>(It->GetClass(), TEXT("SimulationTag"));
			if (!Tag || !It->FindFunction(TEXT("StartRiver")) || !It->FindFunction(TEXT("StopRiver")))
			{
				continue;
			}
			const FName Simulation = Tag->GetPropertyValue_InContainer(*It);
			for (TActorIterator<AActor> Ninja(World); Ninja; ++Ninja)
			{
				if (!Simulation.IsNone() && Ninja->ActorHasTag(Simulation) && NinjaLiveComponent(**Ninja))
				{
					RiverActors.Add({*It, *Ninja});
				}
			}
		}
	}
	for (const TPair<TWeakObjectPtr<AActor>, TWeakObjectPtr<AActor>>& River : RiverActors)
	{
		if (River.Key.IsValid() && River.Value.IsValid())
		{
			OutRivers.Add(River.Key.Get());
			OutRiverNinjas.Add(River.Value.Get());
		}
	}
}

void FFluidNinjaBuoyancyTestBench::SetTurbulence(AActor& Ninja, bool bOn)
{
	UActorComponent* Component = NinjaLiveComponent(Ninja);
	if (!Component)
	{
		return;
	}
	if (!Turbulence.Contains(Component))
	{
		double Noise = 0.0;
		double Bitmaps = 0.0;
		if (!GetNumber(*Component, VeloDirNoise, Noise) || !GetNumber(*Component, VeloFromBitmaps, Bitmaps))
		{
			return;
		}
		Turbulence.Add(Component, {Noise, Bitmaps});
	}
	const TPair<double, double>& Own = Turbulence[Component];
	SetNumber(*Component, VeloDirNoise, bOn ? Own.Key : 0.0);
	SetNumber(*Component, VeloFromBitmaps, bOn ? Own.Value : 0.0);
	// FluidNinja takes its settings up when SetParams runs (not the first run); without it the change does nothing.
	if (UFunction* SetParams = Component->FindFunction(TEXT("SetParams")))
	{
		FStructOnScope Params(SetParams);
		Component->ProcessEvent(SetParams, Params.GetStructMemory());
	}
}

void FFluidNinjaBuoyancyTestBench::SetPoolCalm(bool bCalm)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	TArray<AActor*> Rivers;
	TSet<AActor*> RiverNinjas;
	FindRivers(Rivers, RiverNinjas);
	int32 Count = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!RiverNinjas.Contains(*It) && NinjaLiveComponent(**It))
		{
			SetTurbulence(**It, !bCalm);
			++Count;
		}
	}
	if (bCalm && !bPoolCalm)
	{
		PoolCalmSince = World->GetTimeSeconds();   // its water stills from now (the ripple tests wait for it)
	}
	bPoolCalm = bCalm;
	CachedAt = -1.0;
	UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy test: pool %s (%d NinjaLives)."), bCalm ? TEXT("calm, turbulence off") : TEXT("live, own turbulence"), Count);
}

bool FFluidNinjaBuoyancyTestBench::HasRiver() const
{
	TArray<AActor*> Rivers;
	TSet<AActor*> RiverNinjas;
	FindRivers(Rivers, RiverNinjas);
	return !Rivers.IsEmpty();
}

bool FFluidNinjaBuoyancyTestBench::IsRiverRunning() const
{
	TArray<AActor*> Rivers;
	TSet<AActor*> RiverNinjas;
	FindRivers(Rivers, RiverNinjas);
	UFunction* IsRunning = Rivers.IsEmpty() ? nullptr : Rivers[0]->FindFunction(TEXT("IsRunning"));
	const FBoolProperty* Result = IsRunning ? CastField<FBoolProperty>(IsRunning->GetReturnProperty()) : nullptr;
	if (!Result)
	{
		return false;
	}
	FStructOnScope Params(IsRunning);
	Rivers[0]->ProcessEvent(IsRunning, Params.GetStructMemory());
	return Result->GetPropertyValue_InContainer(Params.GetStructMemory());
}

void FFluidNinjaBuoyancyTestBench::SetRiverRunning(bool bRun)
{
	UWorld* World = GetWorld();
	TArray<AActor*> Rivers;
	TSet<AActor*> RiverNinjas;
	FindRivers(Rivers, RiverNinjas);
	TSet<AActor*> Started;
	for (AActor* River : Rivers)
	{
		if (UFunction* Function = !Started.Contains(River) ? River->FindFunction(bRun ? TEXT("StartRiver") : TEXT("StopRiver")) : nullptr)
		{
			FStructOnScope Params(Function);
			River->ProcessEvent(Function, Params.GetStructMemory());
			Started.Add(River);
		}
	}
	for (AActor* Ninja : RiverNinjas)
	{
		SetTurbulence(*Ninja, bRun);
	}
	CachedAt = -1.0;
	UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy test: river %s (%d rivers)."), bRun ? TEXT("running") : TEXT("stopped, still water"), Started.Num());
}

int32 FFluidNinjaBuoyancyTestBench::CountRiverFloaters() const
{
	int32 Count = 0;
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			Count += It->ActorHasTag(RiverFloaterTag) && It->FindComponentByClass<UFluidNinjaBuoyancyComponent>() ? 1 : 0;
		}
	}
	return Count;
}

void FFluidNinjaBuoyancyTestBench::ResetRiver()
{
	SetRiverRunning(false);
	int32 Count = 0;
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			UFluidNinjaBuoyancyComponent* Body = It->ActorHasTag(RiverFloaterTag) ? It->FindComponentByClass<UFluidNinjaBuoyancyComponent>() : nullptr;
			if (Body)
			{
				Body->RestartAt(Body->GetStartTransform());
				++Count;
			}
		}
	}
	UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy test: %d river floaters back at the head."), Count);
}

void FFluidNinjaBuoyancyTestBench::Push(float NewtonSeconds)
{
	UFluidNinjaBuoyancyComponent* Body = Selected.Get();
	UPrimitiveComponent* Primitive = Body ? Body->GetSimulatedPrimitive() : nullptr;
	if (Primitive && Primitive->IsSimulatingPhysics())
	{
		const FVector Along = FRotator(0.f, ViewYaw(), 0.f).Vector();
		Primitive->AddImpulse(Along * (NewtonSeconds * 100.f));   // N·s = 100 kg·cm/s
	}
}

void FFluidNinjaBuoyancyTestBench::Capsize(float Degrees)
{
	UFluidNinjaBuoyancyComponent* Body = Selected.Get();
	UPrimitiveComponent* Primitive = Body ? Body->GetSimulatedPrimitive() : nullptr;
	if (!Primitive)
	{
		return;
	}
	if (Body->IsAwaitingWater())
	{
		Body->ReleaseHold();
	}
	// Heeled about its own fore-and-aft axis (in its rest attitude), through its centre of mass.
	const FQuat Rested = Primitive->GetComponentQuat() * Body->RestAttitude.Quaternion().Inverse();
	FVector Axis = Rested.GetForwardVector();
	Axis.Z = 0.0;
	Axis = Axis.GetSafeNormal(UE_SMALL_NUMBER, FVector::XAxisVector);
	const FQuat Heel(Axis, FMath::DegreesToRadians(Degrees));
	const FVector Centre = Primitive->IsSimulatingPhysics() ? Primitive->GetCenterOfMass() : Primitive->Bounds.Origin;
	const FVector Location = Centre + Heel.RotateVector(Primitive->GetComponentLocation() - Centre);
	Primitive->SetWorldLocationAndRotation(Location, Heel * Primitive->GetComponentQuat(), false, nullptr, ETeleportType::ResetPhysics);
	Primitive->SetPhysicsLinearVelocity(FVector::ZeroVector);
	Primitive->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector);
	Primitive->WakeRigidBody();
	Body->MarkTeleported();   // turned in place, not rolled there: the ripples start its displacement over
}

void FFluidNinjaBuoyancyTestBench::Drop(float Metres)
{
	UFluidNinjaBuoyancyComponent* Body = Selected.Get();
	UPrimitiveComponent* Primitive = Body ? Body->GetSimulatedPrimitive() : nullptr;
	if (!Primitive)
	{
		return;
	}
	if (Body->IsAwaitingWater())
	{
		Body->ReleaseHold();
	}
	Primitive->SetWorldLocation(Primitive->GetComponentLocation() + FVector(0.0, 0.0, Metres * 100.0), false, nullptr, ETeleportType::ResetPhysics);
	Primitive->SetPhysicsLinearVelocity(FVector::ZeroVector);
	Primitive->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector);
	Primitive->WakeRigidBody();
	Body->MarkTeleported();   // lifted, not flown up: only its fall back into the water makes waves
}

void FFluidNinjaBuoyancyTestBench::BringHere()
{
	UFluidNinjaBuoyancyComponent* Body = Selected.Get();
	if (!Body)
	{
		return;
	}
	// Four metres ahead of the view, turned to it, then settled on the water there as at the start.
	const float Yaw = ViewYaw();
	const FVector Location = ViewLocation() + FRotator(0.f, Yaw, 0.f).Vector() * 400.0;
	Body->RestartAt(FTransform(FQuat(FVector::ZAxisVector, FMath::DegreesToRadians(Yaw)) * Body->RestAttitude.Quaternion(), Location));
	Body->MarkTeleported();   // brought, not moved: no ripples from the jump (Restart At marks it too)
}

void FFluidNinjaBuoyancyTestBench::SetBoat(float Throttle, float Rudder)
{
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	UFluidNinjaBuoyancyComponent* Chosen = Selected.Get();
	for (UFluidNinjaBuoyancyComponent* Body : Hydro ? Hydro->GetBodies() : TArray<UFluidNinjaBuoyancyComponent*>())
	{
		// The selected body when it has propulsion, else every body that has.
		if (Body->bPropulsion && (Body == Chosen || !Chosen || !Chosen->bPropulsion))
		{
			Body->Throttle = FMath::Clamp(Throttle, -1.f, 1.f);
			Body->Rudder = FMath::Clamp(Rudder, -1.f, 1.f);
		}
	}
}

void FFluidNinjaBuoyancyTestBench::ToggleDebugView()
{
	// 2 is the debug view; leaving it goes back to -1, the project's Enabled setting (the bench never turns the water itself off).
	SetSwitch(TEXT("fnb.Buoyancy"), UFluidNinjaBuoyancySettings::IsDebugView() ? -1 : 2);
}

void FFluidNinjaBuoyancyTestBench::SetSceneryCollision(bool bCollide)
{
	if (bCollide)
	{
		for (const TPair<TWeakObjectPtr<UPrimitiveComponent>, FSavedResponse>& Saved : SceneryOff)
		{
			if (UPrimitiveComponent* Primitive = Saved.Key.Get())
			{
				Primitive->SetCollisionResponseToChannel(ECC_WorldStatic, Saved.Value.WorldStatic);
				Primitive->SetCollisionResponseToChannel(ECC_WorldDynamic, Saved.Value.WorldDynamic);
			}
		}
		SceneryOff.Reset();
		return;
	}
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	for (const UFluidNinjaBuoyancyComponent* Body : Hydro ? Hydro->GetBodies() : TArray<UFluidNinjaBuoyancyComponent*>())
	{
		UPrimitiveComponent* Primitive = Body->GetSimulatedPrimitive();
		if (!Primitive || SceneryOff.Contains(Primitive))
		{
			continue;
		}
		SceneryOff.Add(Primitive, {Primitive->GetCollisionResponseToChannel(ECC_WorldStatic), Primitive->GetCollisionResponseToChannel(ECC_WorldDynamic)});
		Primitive->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Ignore);
		Primitive->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Ignore);
	}
}

/* ---- Ripples (FluidNinja Waves) ---- */

IFluidNinjaBuoyancyBenchWaves* FFluidNinjaBuoyancyTestBench::GetWaves() const
{
	UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	return Hydro ? Hydro->GetBenchWaves() : nullptr;
}

FString FFluidNinjaBuoyancyTestBench::PokeAhead(float HeightCm, float SigmaCm)
{
	IFluidNinjaBuoyancyBenchWaves* Ripples = GetWaves();
	if (!Ripples || !Ripples->IsActive())
	{
		return Ripples ? TEXT("no poke, the waves are off: ") + Ripples->GetStatusLine() : FString(TEXT("no poke: this world has no FluidNinja Waves"));
	}
	// From the view target (the player's pawn, else the camera), 3 m on along the view.
	const UWorld* World = GetWorld();
	const APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
	const AActor* Target = Controller && Controller->PlayerCameraManager ? Controller->PlayerCameraManager->GetViewTarget() : nullptr;
	const FVector From = Target ? Target->GetActorLocation() : ViewLocation();
	const FVector Where = From + FRotator(0.f, ViewYaw(), 0.f).Vector() * 300.0;
	Ripples->Poke(FVector2D(Where), HeightCm, SigmaCm);
	return FString::Printf(TEXT("poked a pressure head %.1f cm high (for one of the core's ticks), sigma %.0f cm, at (%.0f, %.0f)"), HeightCm, SigmaCm, Where.X, Where.Y);
}

UFluidNinjaBuoyancyComponent* FFluidNinjaBuoyancyTestBench::FindTestBody(const TCHAR* Kind) const
{
	auto Matches = [this, Kind](const UFluidNinjaBuoyancyComponent* Body)
	{
		const AActor* Owner = Body ? Body->GetOwner() : nullptr;
		return Owner && Body->GetSimulatedPrimitive() && (BodyName(Body).Contains(Kind) || Owner->GetClass()->GetName().Contains(Kind));
	};
	if (Matches(Selected.Get()))
	{
		return Selected.Get();
	}
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (!Hydro)
	{
		return nullptr;
	}
	// One in the pool before the river's own floaters (tag FNB_RiverFloater), which wait at the head of a stopped river.
	const TArray<UFluidNinjaBuoyancyComponent*> Bodies = Hydro->GetBodies();
	for (UFluidNinjaBuoyancyComponent* Body : Bodies)
	{
		if (Matches(Body) && !Body->GetOwner()->ActorHasTag(RiverFloaterTag))
		{
			return Body;
		}
	}
	for (UFluidNinjaBuoyancyComponent* Body : Bodies)
	{
		if (Matches(Body))
		{
			return Body;
		}
	}
	return nullptr;
}

bool FFluidNinjaBuoyancyTestBench::BuildWaterMap(FWaterMap& Out) const
{
	Out = FWaterMap();
	const IFluidNinjaBuoyancyBenchWaves* Layer = GetWaves();
	if (!Layer || Layer->GetWindowCm() <= 0.0 || !Layer->ReadBenchAux(Out.Core, Out.CoreSize))
	{
		return false;
	}
	Out.CoreOrigin = Layer->GetWindowOrigin();
	Out.CoreCellCm = FMath::Max(Layer->GetCellCm(), 1.0e-3);
	const double Window = Layer->GetWindowCm();
	// Map cells of at least 50 cm (about 80 across a large window), each open only where every core cell in it is wet and outside an open
	// window's sponge (a walled window's border cells are dry in the core already).
	Out.Step = FMath::Max(50.0, Window / 80.0);
	Out.Size = FMath::Max(FMath::FloorToInt32(Window / Out.Step), 1);
	Out.Min = Out.CoreOrigin + FVector2D(0.5 * Out.Step);
	const double Sponge = Layer->IsBenchEdgeOpen() ? FMath::Max(static_cast<double>(UFluidNinjaBuoyancySettings::Get().WaveSpongeCm), 0.0) : 0.0;
	Out.Depth.Init(0.f, Out.Size * Out.Size);
	for (int32 Y = 0; Y < Out.Size; ++Y)
	{
		for (int32 X = 0; X < Out.Size; ++X)
		{
			const int32 FromX = FMath::Clamp(FMath::FloorToInt32(X * Out.Step / Out.CoreCellCm), 0, Out.CoreSize.X - 1);
			const int32 ToX = FMath::Clamp(FMath::CeilToInt32((X + 1) * Out.Step / Out.CoreCellCm) - 1, FromX, Out.CoreSize.X - 1);
			const int32 FromY = FMath::Clamp(FMath::FloorToInt32(Y * Out.Step / Out.CoreCellCm), 0, Out.CoreSize.Y - 1);
			const int32 ToY = FMath::Clamp(FMath::CeilToInt32((Y + 1) * Out.Step / Out.CoreCellCm) - 1, FromY, Out.CoreSize.Y - 1);
			float Least = TNumericLimits<float>::Max();
			for (int32 J = FromY; J <= ToY && Least > 0.f; ++J)
			{
				for (int32 I = FromX; I <= ToX && Least > 0.f; ++I)
				{
					const FLinearColor& Cell = Out.Core[J * Out.CoreSize.X + I];
					const double Edge = FMath::Min(FMath::Min(I + 0.5, Out.CoreSize.X - I - 0.5), FMath::Min(J + 0.5, Out.CoreSize.Y - J - 0.5)) * Out.CoreCellCm;
					Least = Cell.R > 0.5f && Edge >= Sponge ? FMath::Min(Least, Cell.G) : 0.f;
				}
			}
			Out.Depth[Y * Out.Size + X] = Least;
		}
	}
	return true;
}

float FFluidNinjaBuoyancyTestBench::TraceBedDepth(const FVector2D& Point, float WaterZ) const
{
	UWorld* World = GetWorld();
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (!World || !Hydro)
	{
		return -1.f;
	}
	// The ground alone: without the floating bodies, people, water volumes and FluidNinja's own actors, nor the drawn water surfaces.
	FCollisionQueryParams Params(TEXT("FluidNinjaBuoyancyBenchBed"), false);
	for (const UFluidNinjaBuoyancyComponent* Body : Hydro->GetBodies())
	{
		if (const AActor* Owner = Body->GetOwner())
		{
			Params.AddIgnoredActor(Owner);
		}
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->IsA<APawn>() || It->IsA<AVolume>() || NinjaLiveComponent(**It) || It->GetClass()->GetName().Contains(TEXT("WaterArea")))
		{
			Params.AddIgnoredActor(*It);
		}
	}
	for (const TWeakObjectPtr<UPrimitiveComponent>& Surface : Hydro->GetSurfacePrimitiveComponents())
	{
		if (const UPrimitiveComponent* Drawn = Surface.Get())
		{
			Params.AddIgnoredComponent(Drawn);
		}
	}
	const FVector Start(Point.X, Point.Y, WaterZ + 300.0);
	const FVector End(Point.X, Point.Y, WaterZ - 5000.0);
	for (int32 Try = 0; Try < 4; ++Try)
	{
		FHitResult Hit;
		if (!World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
		{
			return -1.f;
		}
		if (Hit.bStartPenetrating)
		{
			return 0.f;
		}
		// A flat sheet at the water line (a water plane with collision) is the water itself, not the ground: look past it.
		const UPrimitiveComponent* Struck = Hit.GetComponent();
		if (Struck && FMath::Abs(Hit.ImpactPoint.Z - WaterZ) < 20.0 && Struck->Bounds.BoxExtent.Z < 10.0)
		{
			Params.AddIgnoredComponent(Struck);
			continue;
		}
		return static_cast<float>(WaterZ - Hit.ImpactPoint.Z);
	}
	return -1.f;
}

bool FFluidNinjaBuoyancyTestBench::WantsCalmPool(const FString& Test)
{
	return Test == TEXT("ripple-weight") || Test == TEXT("ripple-rest") || Test == TEXT("ripple-coupling") || Test == TEXT("ripple-evers");
}

/* ---- Tests ---- */

void FFluidNinjaBuoyancyTestBench::Report(const FString& Test, const FString& Body, int32 Verdict, const FString& Text)
{
	Results.Add({Test, Body, Verdict, Text});
	UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy test: %s %s %s: %s"), Verdict > 0 ? TEXT("PASS") : Verdict < 0 ? TEXT("FAIL") : TEXT("INFO"), *Test, *Body, *Text);
}

void FFluidNinjaBuoyancyTestBench::SaveResults(const FString& Test, int32 FirstResult) const
{
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("test"), Test);
	Root->SetStringField(TEXT("time"), FDateTime::Now().ToIso8601());
	int32 Passed = 0;
	int32 Failed = 0;
	TArray<TSharedPtr<FJsonValue>> Items;
	for (int32 Index = FMath::Max(FirstResult, 0); Index < Results.Num(); ++Index)
	{
		const FResult& Result = Results[Index];
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("test"), Result.Test);
		Item->SetStringField(TEXT("body"), Result.Body);
		Item->SetStringField(TEXT("verdict"), Result.Verdict > 0 ? TEXT("pass") : Result.Verdict < 0 ? TEXT("fail") : TEXT("info"));
		Item->SetStringField(TEXT("text"), Result.Text);
		Items.Add(MakeShared<FJsonValueObject>(Item));
		Passed += Result.Verdict > 0;
		Failed += Result.Verdict < 0;
	}
	Root->SetNumberField(TEXT("passed"), Passed);
	Root->SetNumberField(TEXT("failed"), Failed);
	Root->SetArrayField(TEXT("results"), Items);
	if (Hydro)
	{
		Root->SetStringField(TEXT("status"), Hydro->GetStatus());
	}
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Root, Writer);
	const FString Folder = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FluidNinjaBuoyancy"), TEXT("TestResults"));
	FFileHelper::SaveStringToFile(Json, *FPaths::Combine(Folder, FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S_")) + Test + TEXT(".json")));
	FFileHelper::SaveStringToFile(Json, *FPaths::Combine(Folder, TEXT("latest.json")));
	UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy test %s done: %d passed, %d failed (Saved/FluidNinjaBuoyancy/TestResults/latest.json)"), *Test, Passed, Failed);
}

void FFluidNinjaBuoyancyTestBench::Run(const FString& Name)
{
	if (Current.IsValid())
	{
		Stop();
	}
	const bool bAll = Name.Equals(TEXT("all"), ESearchCase::IgnoreCase);
	if (bAll || Name.Equals(TEXT("ripples"), ESearchCase::IgnoreCase))
	{
		// The ripple tests (about seven minutes, so not part of all): those on controlled water first, then those on the calmed live pool,
		// which share its calm, then the one in the river's current.
		Queue = bAll ? TArray<FString>({TEXT("settle"), TEXT("capsize"), TEXT("drop"), TEXT("wind"), TEXT("throttle"), TEXT("gather"), TEXT("walkaway")})
			: TArray<FString>({TEXT("ripple-dispersion"), TEXT("ripple-fps"), TEXT("ripple-kelvin"), TEXT("ripple-weight"), TEXT("ripple-rest"), TEXT("ripple-coupling"),
				TEXT("ripple-evers"), TEXT("ripple-advection")});
		QueueName = bAll ? TEXT("all") : TEXT("ripples");
		QueueFirstResult = Results.Num();
		const FString First = Queue[0];
		Queue.RemoveAt(0);
		StartRun(MakeRun(First));
		return;
	}
	QueueName.Reset();
	StartRun(MakeRun(Name.ToLower()));
}

void FFluidNinjaBuoyancyTestBench::Stop()
{
	Queue.Reset();
	QueueName.Reset();
	if (Current.IsValid())
	{
		Report(Current->Name, TEXT("-"), 0, TEXT("stopped"));
		FinishRun(true);
	}
}

void FFluidNinjaBuoyancyTestBench::StartRun(TUniquePtr<FRun> NewRun)
{
	UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (!NewRun.IsValid() || !Hydro)
	{
		return;
	}
	const FFluidNinjaBuoyancyWind& WindNow = Hydro->GetWind();
	NewRun->bWasControlled = Hydro->IsWaterControlled();
	NewRun->WasHeight = Hydro->GetControlledWaterHeight();
	NewRun->bWasWindOverride = WindNow.bOverride;
	NewRun->WasWindMs = WindNow.SpeedMs;
	NewRun->WasWindDegrees = WindNow.DirectionDegrees;
	NewRun->WasGather = GetSwitch(TEXT("fnb.Buoyancy.Gather"));
	const UFluidNinjaBuoyancySettings& WaveSettings = UFluidNinjaBuoyancySettings::Get();
	NewRun->WasFeelScale = WaveSettings.WaveFeelScale;
	NewRun->bWasDisplacement = WaveSettings.bWaveDisplacement;
	NewRun->bWasImpulse = WaveSettings.bWaveImpulse;
	NewRun->WasCurrentCoupling = WaveSettings.WaveCurrentCoupling;
	NewRun->bWasSceneryCollision = IsSceneryCollision();
	NewRun->WindDegrees = ViewYaw();
	NewRun->FirstResult = Results.Num();
	for (UFluidNinjaBuoyancyComponent* Body : Hydro->GetBodies())
	{
		FTrack& Track = NewRun->Tracks.AddDefaulted_GetRef();
		Track.Body = Body;
		Track.Name = BodyName(Body);
		if (const UPrimitiveComponent* Primitive = Body->GetSimulatedPrimitive())
		{
			Track.LastRawYaw = static_cast<float>((Primitive->GetComponentQuat() * Body->RestAttitude.Quaternion().Inverse()).Rotator().Yaw);
			Track.Yaw = Track.LastRawYaw;
		}
	}
	UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy test %s: started with %d bodies"), *NewRun->Name, NewRun->Tracks.Num());
	Current = MoveTemp(NewRun);
}

void FFluidNinjaBuoyancyTestBench::FinishRun(bool bStopped)
{
	if (!Current.IsValid())
	{
		return;
	}
	TUniquePtr<FRun> Done = MoveTemp(Current);
	if (!bStopped && Done->Evaluate && !Done->bAbort)
	{
		Done->Evaluate(*this, *Done);
	}
	// Put back what the test changed.
	if (UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get())
	{
		Hydro->SetControlledWater(Done->bWasControlled, Done->WasHeight);
		Hydro->SetWindOverride(Done->bWasWindOverride, Done->WasWindMs, Done->WasWindDegrees);
	}
	SetSwitch(TEXT("fnb.Buoyancy.Gather"), Done->WasGather);
	if (Done->bWasSceneryCollision)
	{
		SetSceneryCollision(true);
	}
	if (Done->bRipple)
	{
		// What the test made goes first (before the bodies are put back below, which would otherwise put it back too).
		for (const TWeakObjectPtr<AActor>& Made : Done->Spawned)
		{
			if (AActor* Actor = Made.Get(); Actor && !Actor->IsActorBeingDestroyed())
			{
				Actor->Destroy();
			}
		}
		// The waves' project switches the test wrote (feel, the sources, the current's carry), its probes and stats; the frame-rate cap;
		// the bodies' own masses and Waves switches (before they are put back below, so they settle with their own mass).
		{
			UFluidNinjaBuoyancySettings& WaveSettings = UFluidNinjaBuoyancySettings::GetMutable();
			WaveSettings.WaveFeelScale = Done->WasFeelScale;
			WaveSettings.bWaveDisplacement = Done->bWasDisplacement;
			WaveSettings.bWaveImpulse = Done->bWasImpulse;
			WaveSettings.WaveCurrentCoupling = Done->WasCurrentCoupling;
		}
		if (IFluidNinjaBuoyancyBenchWaves* Ripples = GetWaves())
		{
			Ripples->SetProbes(TArray<FVector2D>());
			Ripples->RequestStats(false);
			Ripples->ClearBenchFocus();
		}
		if (Done->WasMaxFps >= 0.f && GEngine)
		{
			GEngine->SetMaxFPS(Done->WasMaxFps);
		}
		if (Done->WasFixedDelta > 0.0)
		{
			FApp::SetFixedDeltaTime(Done->WasFixedDelta);
		}
		for (const TPair<TWeakObjectPtr<UPrimitiveComponent>, TPair<float, bool>>& Saved : Done->WasMass)
		{
			UPrimitiveComponent* Primitive = Saved.Key.Get();
			if (FBodyInstance* Instance = Primitive ? Primitive->GetBodyInstance() : nullptr)
			{
				Instance->SetMassOverride(Saved.Value.Key, Saved.Value.Value);
				Instance->UpdateMassProperties();
			}
		}
		for (const TPair<TWeakObjectPtr<UFluidNinjaBuoyancyComponent>, bool>& Saved : Done->WasRippleSwitch)
		{
			if (UFluidNinjaBuoyancyComponent* Body = Saved.Key.Get())
			{
				Body->bWaves = Saved.Value;
			}
		}
		// The pool's calm, unless the next test of the group wants it too (it then waits only for what is left of the settling).
		const UWorld* World = GetWorld();
		const bool bNextCalm = !bStopped && !Queue.IsEmpty() && WantsCalmPool(Queue[0]);
		if (bPoolCalmedByTest && !bNextCalm && World && !World->bIsTearingDown)
		{
			SetPoolCalm(false);
			bPoolCalmedByTest = false;
		}
		// The river as it was: stopped again, and its floaters where they waited.
		if (Done->bRiverStartedByTest && World && !World->bIsTearingDown)
		{
			SetRiverRunning(false);
			for (const TPair<TWeakObjectPtr<UFluidNinjaBuoyancyComponent>, FTransform>& Placed : Done->RiverPlaced)
			{
				if (UFluidNinjaBuoyancyComponent* Floater = Placed.Key.Get())
				{
					Floater->RestartAt(Placed.Value);
					Floater->MarkTeleported();   // put back, not moved there (Restart At marks it too)
				}
			}
		}
	}
	// The probes and the focus a test outside the ripple tests set (walkaway; a ripple test's go in the block above).
	if (Done->bOwnsProbes)
	{
		if (IFluidNinjaBuoyancyBenchWaves* Ripples = GetWaves())
		{
			Ripples->SetProbes(TArray<FVector2D>());
			Ripples->ClearBenchFocus();
		}
	}
	if (Done->bResetAfter)
	{
		ResetBodies(true);
	}
	if (UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get())
	{
		Hydro->SetVerifyGather(false);
	}
	for (FTrack& Track : Done->Tracks)
	{
		if (UFluidNinjaBuoyancyComponent* Body = Track.Body.Get(); Body && Body->bPropulsion && Done->Name == TEXT("throttle"))
		{
			Body->Throttle = 0.f;
			Body->Rudder = 0.f;
		}
	}
	if (Done->bPawnAway)
	{
		if (UWorld* World = GetWorld())
		{
			if (APlayerController* Controller = World->GetFirstPlayerController())
			{
				if (APawn* Pawn = Controller->GetPawn())
				{
					Pawn->TeleportTo(Done->PawnReturn.GetLocation(), Done->PawnReturn.Rotator());
				}
				Controller->SetControlRotation(Done->ControlReturn);
			}
		}
	}
	if (!bStopped)
	{
		SaveResults(Done->Name, Done->FirstResult);
	}
	if (!bStopped && !Queue.IsEmpty())
	{
		const FString Next = Queue[0];
		Queue.RemoveAt(0);
		StartRun(MakeRun(Next));
	}
	else if (!bStopped && !QueueName.IsEmpty())
	{
		SaveResults(QueueName, QueueFirstResult);
		QueueName.Reset();
	}
}

void FFluidNinjaBuoyancyTestBench::TickRun()
{
	FRun& Active = *Current;
	UWorld* World = GetWorld();
	UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (!World || !Hydro || Active.Phases.IsEmpty())
	{
		FinishRun(true);
		return;
	}
	const double Now = World->GetTimeSeconds();
	auto EnterPhase = [this, &Active, Now](int32 Index)
	{
		Active.Phase = Index;
		Active.PhaseStarted = Now;
		Active.bWindowOpen = false;
		for (FTrack& Track : Active.Tracks)
		{
			const UPrimitiveComponent* Primitive = Track.Body.IsValid() ? Track.Body->GetSimulatedPrimitive() : nullptr;
			Track.PhaseStartZ[FMath::Min(Index, MaxPhases - 1)] = Primitive ? static_cast<float>(Primitive->GetComponentLocation().Z) : 0.f;
		}
		if (Active.Phases[Index].Enter)
		{
			Active.Phases[Index].Enter(*this, Active);
		}
	};
	if (Active.Phase < 0)
	{
		EnterPhase(0);
	}
	// A test that found it cannot run (in a phase's Enter or Exit) ends here, with what it reported.
	if (Active.bAbort)
	{
		FinishRun(false);
		return;
	}
	const int32 P = FMath::Min(Active.Phase, MaxPhases - 1);
	const FPhase& Phase = Active.Phases[Active.Phase];
	const double Elapsed = Now - Active.PhaseStarted;

	// Follow every body.
	for (FTrack& Track : Active.Tracks)
	{
		const UFluidNinjaBuoyancyComponent* Body = Track.Body.Get();
		const UPrimitiveComponent* Primitive = Body ? Body->GetSimulatedPrimitive() : nullptr;
		if (!Primitive)
		{
			continue;
		}
		const FVector Location = Primitive->GetComponentLocation();
		const FVector Velocity = Primitive->IsSimulatingPhysics() ? Primitive->GetPhysicsLinearVelocity() : FVector::ZeroVector;
		if (Location.ContainsNaN() || Velocity.ContainsNaN() || Velocity.Size() > 3000.0)
		{
			Track.bBroke = true;
		}
		Track.MaxSpeed = FMath::Max(Track.MaxSpeed, static_cast<float>(Velocity.Size()));
		Track.MaxSlam = FMath::Max(Track.MaxSlam, Body->GetReadout().SlamWatts);
		Track.MaxEntry = FMath::Max(Track.MaxEntry, Body->GetReadout().EntryWatts);
		const float RawYaw = static_cast<float>((Primitive->GetComponentQuat() * Body->RestAttitude.Quaternion().Inverse()).Rotator().Yaw);
		Track.Yaw += FMath::FindDeltaAngleDegrees(Track.LastRawYaw, RawYaw);
		Track.LastRawYaw = RawYaw;
		FFluidNinjaBuoyancyBodyDebug Debug;
		if (Hydro->GetBodyDebug(Body, Debug))
		{
			Track.bSawHeld |= Debug.bHeld;
			Track.bSawDormant |= Debug.bDormant;
			if (Active.bWindowOpen && Debug.bHasWaterZ && Debug.bInCapture)
			{
				Track.WaterSum[P] += Debug.WaterZ;
				++Track.WaterCount[P];
			}
		}
		Track.MaxDrop[P] = FMath::Max(Track.MaxDrop[P], Track.PhaseStartZ[P] - static_cast<float>(Location.Z));
		if (Active.bWindowOpen)
		{
			Track.MaxTilt[P] = FMath::Max(Track.MaxTilt[P], Tilt(Body));
			Track.MaxSink[P] = FMath::Max(Track.MaxSink[P], static_cast<float>(Track.WindowLocation.Z - Location.Z));
			Track.ZSum[P] += Location.Z;
			++Track.ZCount[P];
		}
	}
	if (!Active.bWindowOpen && Elapsed >= Phase.MeasureFrom)
	{
		Active.bWindowOpen = true;
		for (FTrack& Track : Active.Tracks)
		{
			const UPrimitiveComponent* Primitive = Track.Body.IsValid() ? Track.Body->GetSimulatedPrimitive() : nullptr;
			Track.WindowLocation = Primitive ? Primitive->GetComponentLocation() : FVector::ZeroVector;
			Track.WindowYaw = Track.Yaw;
			Track.WindowTime = Now;
			Track.WindowCom = Primitive ? (Primitive->IsSimulatingPhysics() ? Primitive->GetCenterOfMass() : Primitive->Bounds.Origin) : FVector::ZeroVector;
			Track.WindowQuat = Primitive ? Primitive->GetComponentQuat() : FQuat::Identity;
		}
	}
	if (Phase.Tick)
	{
		Phase.Tick(*this, Active);
		if (Active.bAbort)
		{
			FinishRun(false);
			return;
		}
	}
	if (Elapsed < Phase.Seconds)
	{
		return;
	}
	// The phase is over: what each body did over its window.
	for (FTrack& Track : Active.Tracks)
	{
		const UPrimitiveComponent* Primitive = Track.Body.IsValid() ? Track.Body->GetSimulatedPrimitive() : nullptr;
		const double Span = Now - Track.WindowTime;
		if (Primitive && Track.WindowTime >= 0.0 && Span > 0.05)
		{
			Track.Velocity[P] = (Primitive->GetComponentLocation() - Track.WindowLocation) / Span;
			Track.YawRate[P] = static_cast<float>((Track.Yaw - Track.WindowYaw) / Span);
			const FVector Com = Primitive->IsSimulatingPhysics() ? Primitive->GetCenterOfMass() : Primitive->Bounds.Origin;
			Track.ComVelocity[P] = (Com - Track.WindowCom) / Span;
			// The whole turn between the two attitudes, 2 acos |q0 · q1| (clamped: rounding can put a unit dot product past 1).
			const FQuat Attitude = Primitive->GetComponentQuat();
			const double Dot = Track.WindowQuat.X * Attitude.X + Track.WindowQuat.Y * Attitude.Y + Track.WindowQuat.Z * Attitude.Z + Track.WindowQuat.W * Attitude.W;
			Track.Turned[P] = static_cast<float>(FMath::RadiansToDegrees(2.0 * FMath::Acos(FMath::Clamp(FMath::Abs(Dot), 0.0, 1.0))));
			Track.YawTurned[P] = Track.Yaw - Track.WindowYaw;
		}
	}
	float GameMs = 0.f;
	float PatchesMs = 0.f;
	float PreTickMs = 0.f;
	float PhysicsMs = 0.f;
	Hydro->GetCosts(GameMs, PatchesMs, PreTickMs, PhysicsMs);
	Active.Cost[P] = GameMs + PreTickMs;
	if (Phase.Exit)
	{
		Phase.Exit(*this, Active);
	}
	if (Active.bAbort || Active.Phase + 1 >= Active.Phases.Num())
	{
		FinishRun(false);
		return;
	}
	EnterPhase(Active.Phase + 1);
}

TUniquePtr<FFluidNinjaBuoyancyTestBench::FRun> FFluidNinjaBuoyancyTestBench::MakeRun(const FString& Name)
{
	if (Name.StartsWith(TEXT("ripple-")))
	{
		return MakeRippleRun(Name);
	}
	TUniquePtr<FRun> Made = MakeUnique<FRun>();
	Made->Name = Name;
	// Controlled still water at the bodies' water, calm air, every body put back.
	auto Prepare = [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
	{
		R.bResetAfter = true;
		Bench.SetSceneryCollision(false);
		Bench.SetControlledWater(true);
		const float Zero = 0.f;
		Bench.SetTestWind(true, 0.f, &Zero);
		Bench.ResetBodies(true);
	};
	auto Settling = [Prepare](float Seconds) { return FPhase{TEXT("settling"), Seconds, Seconds, Prepare, nullptr}; };
	auto ForEach = [](FRun& R, TFunction<void(FTrack&, UFluidNinjaBuoyancyComponent&)> Action)
	{
		for (FTrack& Track : R.Tracks)
		{
			if (UFluidNinjaBuoyancyComponent* Body = Track.Body.Get())
			{
				Action(Track, *Body);
			}
		}
	};
	auto Speed2D = [](const FVector& Velocity) { return static_cast<float>(Velocity.Size2D()); };

	if (Name == TEXT("settle"))
	{
		Made->Phases.Add({TEXT("settling"), 12.f, 9.f, Prepare, nullptr});
		Made->Evaluate = [ForEach](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			ForEach(R, [&Bench, &R](FTrack& Track, UFluidNinjaBuoyancyComponent& Body)
			{
				float Measured = 0.f;
				const float Expected = Body.GetRestingDraft();
				const bool bDraft = Bench.Draft(&Body, Measured);
				const float Tolerance = FMath::Max(2.f, 0.1f * Expected);
				const float Drift = static_cast<float>(Track.Velocity[0].Size());
				const FString Text = FString::Printf(TEXT("draft %.1f cm (rest %.1f, within %.1f), drifting %.1f cm/s, tilt %.1f deg, peak speed %.0f cm/s"),
					Measured, Expected, Tolerance, Drift, Bench.Tilt(&Body), Track.MaxSpeed);
				const bool bPass = !Track.bBroke && bDraft && FMath::Abs(Measured - Expected) <= Tolerance && Drift < 3.f;
				Bench.Report(R.Name, Track.Name, bPass ? 1 : -1, Track.bBroke ? TEXT("flung or NaN; ") + Text : Text);
			});
		};
	}
	else if (Name == TEXT("capsize"))
	{
		Made->Phases.Add(Settling(6.f));
		Made->Phases.Add({TEXT("heeled 30 deg"), 12.f, 8.f, [ForEach](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			ForEach(R, [&Bench](FTrack& Track, UFluidNinjaBuoyancyComponent& Body)
			{
				// Only bodies stable both ways in their rest attitude (a round log rolls freely about its axis).
				Track.bEligible = Body.GetRollStability() > 1.f && Body.GetPitchStability() > 1.f;
				if (Track.bEligible)
				{
					const TWeakObjectPtr<UFluidNinjaBuoyancyComponent> Was = Bench.Selected;
					Bench.Selected = &Body;
					Bench.Capsize(30.f);
					Bench.Selected = Was;
				}
			});
		}, nullptr});
		Made->Evaluate = [ForEach](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			ForEach(R, [&Bench, &R](FTrack& Track, UFluidNinjaBuoyancyComponent& Body)
			{
				if (!Track.bEligible)
				{
					Bench.Report(R.Name, Track.Name, 0, FString::Printf(TEXT("not heeled: metacentric height roll %.0f cm, pitch %.0f cm (neutral or unstable)"),
						Body.GetRollStability(), Body.GetPitchStability()));
					return;
				}
				// Righted: in the last 4 s it swings less than half the heel (a ball buoy keeps rocking a while; a stuck body does not come back).
				const float TiltNow = Bench.Tilt(&Body);
				const bool bPass = !Track.bBroke && Track.MaxTilt[1] < 15.f;
				Bench.Report(R.Name, Track.Name, bPass ? 1 : -1, FString::Printf(TEXT("%s: tilt %.1f deg 12 s after a 30 deg heel, at most %.0f deg in the last 4 s"),
					bPass ? TEXT("righted") : TEXT("did not right"), TiltNow, Track.MaxTilt[1]));
			});
		};
	}
	else if (Name == TEXT("drop"))
	{
		Made->Phases.Add(Settling(6.f));
		Made->Phases.Add({TEXT("dropped from 3 m"), 10.f, 7.f, [ForEach](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			ForEach(R, [&Bench](FTrack& Track, UFluidNinjaBuoyancyComponent& Body)
			{
				Track.MaxSpeed = 0.f;
				Track.MaxSlam = 0.f;
				Track.MaxEntry = 0.f;
				const TWeakObjectPtr<UFluidNinjaBuoyancyComponent> Was = Bench.Selected;
				Bench.Selected = &Body;
				Bench.Drop(3.f);
				Bench.Selected = Was;
			});
		}, nullptr});
		Made->Evaluate = [ForEach](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			const float Impact = FMath::Sqrt(2.f * 980.f * 300.f);
			ForEach(R, [&Bench, &R, Impact](FTrack& Track, UFluidNinjaBuoyancyComponent& Body)
			{
				float Measured = 0.f;
				const float Expected = Body.GetRestingDraft();
				const bool bDraft = Bench.Draft(&Body, Measured);
				const float Drift = static_cast<float>(Track.Velocity[1].Size());
				// Drift: the sideways part of a slam on a hull not quite level (a per cent of the impact speed on a V hull) is allowed; a body
				// that slides off is not.
				const bool bPass = !Track.bBroke && Track.MaxSpeed < Impact * 1.3f + 100.f && bDraft && FMath::Abs(Measured - Expected) <= FMath::Max(3.f, 0.25f * Expected) && Drift < FMath::Max(5.f, 0.01f * Impact);
				// The judged drift is the primitive's origin's: a hull rocking about a point off its origin, or bobbing, counts there too. The
				// centre of mass's own motion over the same window, split, and how far the body turned, are reported beside it (information).
				const FVector& Com = Track.ComVelocity[1];
				Bench.Report(R.Name, Track.Name, bPass ? 1 : -1, FString::Printf(TEXT("peak speed %.0f cm/s (free fall %.0f), slam %.0f W, entry %.0f W (splash %.1f, %.0f kg), then draft %.1f cm (rest %.1f), drifting %.1f cm/s (its origin, judged; centre of mass %.1f cm/s: horizontal %.1f, vertical %+.1f; turned %.1f deg over the window, %+.1f about the vertical)"),
					Track.MaxSpeed, Impact, Track.MaxSlam, Track.MaxEntry, UFluidNinjaBuoyancySettings::Get().SplashStrength(Track.MaxEntry), Body.GetReadout().MassKg,
					Measured, Expected, Drift, Com.Size(), Com.Size2D(), Com.Z, Track.Turned[1], Track.YawTurned[1]));
			});
		};
	}
	else if (Name == TEXT("wind"))
	{
		auto Blow = [](float Speed)
		{
			return [Speed](FFluidNinjaBuoyancyTestBench& Bench, FRun& R) { Bench.SetTestWind(true, Speed, &R.WindDegrees); };
		};
		Made->Phases.Add(Settling(5.f));
		Made->Phases.Add({TEXT("wind 8 m/s"), 9.f, 3.f, Blow(8.f), nullptr});
		Made->Phases.Add(Settling(5.f));   // back to the start, so the stronger wind is not measured against a bank
		Made->Phases.Add({TEXT("wind 16 m/s"), 9.f, 3.f, Blow(16.f), [ForEach](FFluidNinjaBuoyancyTestBench&, FRun& R)
		{
			ForEach(R, [](FTrack& Track, UFluidNinjaBuoyancyComponent& Body) { Track.Velocity[5] = Body.GetReadout().WindForceN; });   // the wind's force at 16 m/s
		}});
		Made->Evaluate = [ForEach](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			const FVector Along = FRotator(0.f, R.WindDegrees, 0.f).Vector();
			ForEach(R, [&Bench, &R, &Along](FTrack& Track, UFluidNinjaBuoyancyComponent&)
			{
				const FVector Slow(Track.Velocity[1].X, Track.Velocity[1].Y, 0.0);
				const FVector Fast(Track.Velocity[3].X, Track.Velocity[3].Y, 0.0);
				const float AlongSlow = static_cast<float>(Slow | Along);
				const float AlongFast = static_cast<float>(Fast | Along);
				const float Cosine = Fast.Size() > 0.01 ? static_cast<float>((Fast.GetSafeNormal() | Along)) : 0.f;
				// A long hull slips more easily along its length than across it, so it drifts between the wind and its own axis (leeway):
				// up to about 40 degrees off for a lying barrel turned 55 degrees to the wind. Up to 45 passes; a wind force with the
				// wrong sign or frame goes further off than that.
				const bool bPass = !Track.bBroke && AlongSlow > 0.5f && AlongFast > AlongSlow * 1.1f && Cosine > 0.7f;
				Bench.Report(R.Name, Track.Name, bPass ? 1 : -1, FString::Printf(TEXT("drift at 8 m/s %.1f cm/s, at 16 m/s %.1f cm/s (toward %.0f deg, %.0f deg off the wind, which blows toward %.0f deg; turning %.1f deg/s); wind force %.0f N at 16 m/s"),
					AlongSlow, AlongFast, FMath::RadiansToDegrees(FMath::Atan2(Fast.Y, Fast.X)), FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Cosine, -1.f, 1.f))),
					R.WindDegrees, Track.YawRate[3], Track.Velocity[5].Size2D()));
			});
		};
	}
	else if (Name == TEXT("throttle"))
	{
		auto Drive = [ForEach](float Throttle, float Rudder)
		{
			return [ForEach, Throttle, Rudder](FFluidNinjaBuoyancyTestBench&, FRun& R)
			{
				ForEach(R, [Throttle, Rudder](FTrack& Track, UFluidNinjaBuoyancyComponent& Body)
				{
					Track.bEligible = Body.bPropulsion;
					if (Body.bPropulsion)
					{
						Body.Throttle = Throttle;
						Body.Rudder = Rudder;
					}
				});
			};
		};
		Made->Phases.Add(Settling(5.f));
		Made->Phases.Add({TEXT("full ahead"), 14.f, 8.f, Drive(1.f, 0.f), nullptr});
		Made->Phases.Add({TEXT("full ahead, full rudder"), 10.f, 5.f, Drive(1.f, 1.f), nullptr});
		Made->Evaluate = [ForEach, Speed2D](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			bool bAny = false;
			ForEach(R, [&Bench, &R, &bAny, Speed2D](FTrack& Track, UFluidNinjaBuoyancyComponent&)
			{
				if (!Track.bEligible)
				{
					return;
				}
				bAny = true;
				const float Speed = Speed2D(Track.Velocity[1]);
				const bool bPass = !Track.bBroke && Speed > 50.f && FMath::Abs(Track.YawRate[1]) < 2.f && FMath::Abs(Track.YawRate[2]) > 5.f;
				Bench.Report(R.Name, Track.Name, bPass ? 1 : -1, FString::Printf(TEXT("full ahead %.2f m/s, heading drifting %.1f deg/s; with full rudder turning %.1f deg/s at %.2f m/s"),
					Speed / 100.f, Track.YawRate[1], Track.YawRate[2], Speed2D(Track.Velocity[2]) / 100.f));
			});
			if (!bAny)
			{
				Bench.Report(R.Name, TEXT("-"), 0, TEXT("no body with propulsion"));
			}
		};
	}
	else if (Name == TEXT("gather"))
	{
		auto Mode = [](int32 Gather, bool bVerify)
		{
			return [Gather, bVerify](FFluidNinjaBuoyancyTestBench& Bench, FRun&)
			{
				Bench.SetControlledWater(false);
				SetSwitch(TEXT("fnb.Buoyancy.Gather"), Gather);
				if (UFluidNinjaBuoyancySubsystem* H = Bench.GetHydro())
				{
					H->SetVerifyGather(bVerify);
				}
			};
		};
		Made->Phases.Add({TEXT("gather and copies, same frames"), 6.f, 6.f, Mode(1, true), [](FFluidNinjaBuoyancyTestBench& Bench, FRun&)
		{
			UFluidNinjaBuoyancySubsystem* H = Bench.GetHydro();
			if (!H)
			{
				return;
			}
			for (UFluidNinjaBuoyancyComponent* Body : H->GetBodies())
			{
				FFluidNinjaBuoyancyBodyDebug Debug;
				if (!H->GetBodyDebug(Body, Debug))
				{
					continue;
				}
				const FString Who = Bench.BodyName(Body);
				if (Debug.VerifyCount == 0)
				{
					Bench.Report(TEXT("gather"), Who, 0, TEXT("nothing compared (out of the capture or asleep)"));
					continue;
				}
				const float FarShare = 100.f * Debug.VerifyFar / Debug.VerifyCount;
				const bool bPass = Debug.VerifyMeanCm < 0.5f && FarShare < 1.f && Debug.VerifyFlowMeanCmS < 5.f;
				Bench.Report(TEXT("gather"), Who, bPass ? 1 : -1, FString::Printf(TEXT("gather vs copies over %d points: height %.2f cm mean, %.1f cm at most, %.2f%% over 2 cm; current %.1f cm/s mean (%d points only the gather has)"),
					Debug.VerifyCount, Debug.VerifyMeanCm, Debug.VerifyMaxCm, FarShare, Debug.VerifyFlowMeanCmS, Debug.VerifyFlowOnlyGathered));
			}
			H->SetVerifyGather(false);
		}});
		Made->Phases.Add({TEXT("GPU gather only"), 4.f, 4.f, Mode(1, false), nullptr});
		Made->Phases.Add({TEXT("per-body copies only"), 4.f, 4.f, Mode(0, false), nullptr});
		Made->Evaluate = [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			Bench.Report(R.Name, TEXT("-"), 0, FString::Printf(TEXT("game thread %.2f ms with the gather, %.2f ms with copies (GPU: ProfileGPU, pass FluidNinjaBuoyancyGather)"), R.Cost[1], R.Cost[2]));
		};
	}
	else if (Name == TEXT("walkaway"))
	{
		// The waves' open-water probes: up to four points, 3 m from every hull, every shore or wall (the water map's) and each other, on rings
		// around the pool bodies (river floaters left out), in the simulation holding them: the bench's focus, since the window that follows
		// the camera goes 70 m away with the player. Set once (setting them again starts their readback over), and only when no other probes
		// are set (GetProbeResults has nothing: a ripple test clears its own when it ends, so another test's probes are never taken over).
		auto PlaceProbes = [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
			const UFluidNinjaBuoyancySubsystem* Hydro = Bench.GetHydro();
			if (!Layer || !Layer->IsActive() || !Hydro)
			{
				R.WalkawayProbeNote = TEXT("no waves running");
				return;
			}
			double Time = 0.0;
			TArray<float> Heights;
			if (Layer->GetProbeResults(Time, Heights))
			{
				R.WalkawayProbeNote = TEXT("the waves' probes are in use (set by something else), so none were set");
				return;
			}
			FVector2D Centre = FVector2D::ZeroVector;
			int32 Count = 0;
			TArray<FBox2D> Hulls;
			for (const UFluidNinjaBuoyancyComponent* Body : Hydro->GetBodies())
			{
				const UPrimitiveComponent* Primitive = Body->GetSimulatedPrimitive();
				if (!Primitive)
				{
					continue;
				}
				const FBox Box = Primitive->Bounds.GetBox();
				Hulls.Add(FBox2D(FVector2D(Box.Min), FVector2D(Box.Max)));
				if (Body->GetOwner() && !Body->GetOwner()->ActorHasTag(RiverFloaterTag))
				{
					Centre += FVector2D(Box.GetCenter());
					++Count;
				}
			}
			if (Count == 0)
			{
				R.WalkawayProbeNote = TEXT("no pool body to probe around");
				return;
			}
			Centre /= Count;
			Layer->SetBenchFocus(Centre);
			R.bOwnsProbes = true;   // the focus is the run's from here: FinishRun puts it back
			FWaterMap Map;
			if (!Bench.BuildWaterMap(Map))
			{
				R.WalkawayProbeNote = TEXT("the pool bodies' simulation has no Wave Aux to find open water in");
				return;
			}
			constexpr double ClearCm = 300.0;
			for (double Radius = ClearCm; Radius <= 1500.0 && R.WalkawayProbes.Num() < 4; Radius += 200.0)
			{
				for (int32 Turn = 0; Turn < 16 && R.WalkawayProbes.Num() < 4; ++Turn)
				{
					const FVector2D Point = Centre + FVector2D(1.0, 0.0).GetRotated(Turn * 22.5) * Radius;
					bool bClear = Map.IsOpen(Point, ClearCm);
					for (const FBox2D& Hull : Hulls)
					{
						bClear &= Hull.ComputeSquaredDistanceToPoint(Point) > ClearCm * ClearCm;
					}
					for (const FVector2D& Taken : R.WalkawayProbes)
					{
						bClear &= FVector2D::Distance(Taken, Point) >= ClearCm;
					}
					if (bClear)
					{
						R.WalkawayProbes.Add(Point);
					}
				}
			}
			if (R.WalkawayProbes.IsEmpty())
			{
				R.WalkawayProbeNote = TEXT("no open water 3 m from every hull and shore within 15 m of the pool bodies");
				return;
			}
			R.WalkawayProbeOutside.Init(-1.0, R.WalkawayProbes.Num());
			Layer->SetProbes(R.WalkawayProbes);
		};
		// Every frame of each phase's window (those TickRun's mean height counts: the window opened on an earlier frame): each body's height
		// against its rest height on the water it knows, the waves' own fields, and the open-water probes' mean wave height.
		auto FollowWalkaway = [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			const UWorld* World = Bench.GetWorld();
			const UFluidNinjaBuoyancySubsystem* Hydro = Bench.GetHydro();
			if (!World || !Hydro)
			{
				return;
			}
			const double Now = World->GetTimeSeconds();
			// Each probe point's last frame outside every running wave simulation: the waves read such a point as 0 (its readback cleared, and
			// stamped like any other), as when the pool's simulation stops with the player 70 m away (one that follows the camera, or one
			// fnb.Waves.MaxActiveSims ranks out). Kept on every frame of the phases, not only their windows: a reading counts only the points
			// last found outside before its own frame. The bench ticks before the waves pick a frame's simulations, so a simulation that stops
			// shows here a frame later, and a reading arrives at least a frame after it was taken: its zeros are always caught.
			const IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
			const bool bProbing = Layer && !R.WalkawayProbes.IsEmpty() && R.WalkawayProbeOutside.Num() == R.WalkawayProbes.Num();
			if (bProbing)
			{
				TArray<FBox2D> Windows;
				Layer->GetBenchWindows(Windows);
				for (int32 Index = 0; Index < R.WalkawayProbes.Num(); ++Index)
				{
					const FVector2D& Point = R.WalkawayProbes[Index];
					if (!Windows.ContainsByPredicate([&Point](const FBox2D& Window) { return Window.IsInside(Point); }))
					{
						R.WalkawayProbeOutside[Index] = Now;
					}
				}
			}
			if (!R.bWindowOpen)
			{
				return;
			}
			const int32 P = FMath::Min(R.Phase, MaxPhases - 1);
			for (FTrack& Track : R.Tracks)
			{
				const UFluidNinjaBuoyancyComponent* Body = Track.Body.Get();
				const UPrimitiveComponent* Primitive = Body ? Body->GetSimulatedPrimitive() : nullptr;
				FFluidNinjaBuoyancyBodyDebug Debug;
				if (!Primitive || Track.WindowTime >= Now || !Hydro->GetBodyDebug(Body, Debug) || !Debug.bHasWaterZ || !(Debug.bInCapture || Debug.bFlat))
				{
					continue;
				}
				// As SettleOnWater sets a body on its water: its rested bottom, its rest draft below the water.
				const float RestDraft = Body->GetRestingDraft() > 0.f ? Body->GetRestingDraft() : Body->RestDraftCm;
				const double RestZ = Debug.WaterZ - RestDraft - Body->GetBottomZ();
				Track.RestMissSum[P] += Primitive->GetComponentLocation().Z - RestZ;
				Track.RestWaterSum[P] += Debug.WaterZ;
				++Track.RestCount[P];
				Track.RestInCapture[P] += Debug.bInCapture ? 1 : 0;
			}
			if (!Layer)
			{
				return;
			}
			FRun::FWalkawayPhase& Waves = R.Walkaway[P];
			int32 Fields = 0;
			int32 Felt = 0;
			if (FluidNinjaBuoyancyBenchRipples::OwnFieldsOf(Layer->GetStatusLine(), Fields, Felt))
			{
				Waves.FieldsLeast = FMath::Min(Waves.FieldsLeast, Fields);
				Waves.FieldsMost = FMath::Max(Waves.FieldsMost, Fields);
				Waves.FeltLeast = FMath::Min(Waves.FeltLeast, Felt);
				Waves.FeltMost = FMath::Max(Waves.FeltMost, Felt);
			}
			double Time = 0.0;
			TArray<float> Heights;
			if (bProbing && Layer->GetProbeResults(Time, Heights) && Time > R.WalkawayProbeTime && Heights.Num() == R.WalkawayProbes.Num())
			{
				R.WalkawayProbeTime = Time;
				double Sum = 0.0;
				int32 Inside = 0;
				for (int32 Index = 0; Index < Heights.Num(); ++Index)
				{
					if (Time > R.WalkawayProbeOutside[Index])
					{
						Sum += Heights[Index];
						++Inside;
					}
				}
				if (Inside > 0)
				{
					Waves.ProbeSum += Sum / Inside;
					++Waves.ProbeReadings;
					Waves.ProbePointsLeast = FMath::Min(Waves.ProbePointsLeast, Inside);
				}
				else
				{
					++Waves.ProbeOutsideReadings;
				}
			}
		};
		Made->Phases.Add({TEXT("player here"), 6.f, 1.f, [PlaceProbes](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			Bench.SetControlledWater(false);
			PlaceProbes(Bench, R);
		}, nullptr, FollowWalkaway});
		Made->Phases.Add({TEXT("player 70 m away"), 14.f, 9.f, [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			Bench.SetControlledWater(false);
			UWorld* World = Bench.GetWorld();
			APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
			APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
			if (!Pawn)
			{
				return;
			}
			// Away from the bodies, looking away, on the ground there.
			FVector Centre = FVector::ZeroVector;
			int32 Count = 0;
			for (const FTrack& Track : R.Tracks)
			{
				if (const UPrimitiveComponent* Primitive = Track.Body.IsValid() ? Track.Body->GetSimulatedPrimitive() : nullptr)
				{
					Centre += Primitive->GetComponentLocation();
					++Count;
				}
			}
			const FVector From = Pawn->GetActorLocation();
			FVector Away = Count > 0 ? From - Centre / Count : FVector::XAxisVector;
			Away.Z = 0.0;
			Away = Away.GetSafeNormal(UE_SMALL_NUMBER, FVector::XAxisVector);
			FVector Target = From + Away * 7000.0;
			FHitResult Hit;
			FCollisionQueryParams Params(TEXT("FluidNinjaBuoyancyWalkAway"), false, Pawn);
			if (!World->LineTraceSingleByChannel(Hit, Target + FVector(0.0, 0.0, 6000.0), Target - FVector(0.0, 0.0, 20000.0), ECC_Visibility, Params))
			{
				// No ground out there (a small test level): stay, rather than drop the player into nothing.
				Bench.Report(R.Name, TEXT("-"), 0, TEXT("no ground 70 m away: the player stays (nothing measured)"));
				return;
			}
			Target.Z = Hit.ImpactPoint.Z + 150.0;
			R.bPawnAway = true;
			R.bPawnMoved = true;
			R.PawnReturn = Pawn->GetActorTransform();
			R.ControlReturn = Controller->GetControlRotation();
			Pawn->TeleportTo(Target, Away.Rotation());
			Controller->SetControlRotation(Away.Rotation());
		}, nullptr, FollowWalkaway});
		Made->Phases.Add({TEXT("player back"), 8.f, 5.f, [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			UWorld* World = Bench.GetWorld();
			APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
			if (APawn* Pawn = Controller ? Controller->GetPawn() : nullptr; Pawn && R.bPawnAway)
			{
				Pawn->TeleportTo(R.PawnReturn.GetLocation(), R.PawnReturn.Rotator());
				Controller->SetControlRotation(R.ControlReturn);
				R.bPawnAway = false;
			}
		}, nullptr, FollowWalkaway});
		Made->Evaluate = [ForEach](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			if (!R.bPawnMoved)
			{
				return;   // nowhere to walk to
			}
			ForEach(R, [&Bench, &R](FTrack& Track, UFluidNinjaBuoyancyComponent& Body)
			{
				// Afloat: touching the water now or within the last second (a light float bobbing on live waves leaves it for a moment).
				const UWorld* World = Bench.GetWorld();
				const bool bAfloat = Body.GetReadout().bInWater || (World && Track.LastInWater >= 0.0 && World->GetTimeSeconds() - Track.LastInWater < 1.0);
				// Where it floats once settled out there (the far field is flat: no waves), against where it floated on average before.
				const float Settled = Track.ZCount[0] > 0 && Track.ZCount[1] > 0 ? static_cast<float>(Track.ZSum[0] / Track.ZCount[0] - Track.ZSum[1] / Track.ZCount[1]) : 0.f;
				const bool bPass = !Track.bBroke && FMath::Abs(Settled) < 15.f && (bAfloat || !Track.bWasInWater);
				FString Away = Track.bSawHeld ? FString(TEXT("held")) : FString();
				if (Track.bSawDormant)
				{
					Away += Away.IsEmpty() ? TEXT("asleep") : TEXT(", asleep");
				}
				// What the far field was built from: the correction learned from the capture persists after the player is back, so a body that
				// floated off out there shows the offset it floated on.
				FFluidNinjaBuoyancyBodyDebug Debug;
				const UFluidNinjaBuoyancySubsystem* Hydro = Bench.Subsystem.Get();
				const FString FarField = Hydro && Hydro->GetBodyDebug(&Body, Debug)
					? (Debug.bFarFieldLearned ? FString::Printf(TEXT("far field %+.0f cm off the volume, water now %.0f cm"), Debug.FarFieldCorrection, Debug.WaterZ)
						: FString::Printf(TEXT("far field not learned, water now %.0f cm"), Debug.WaterZ))
					: FString(TEXT("no debug readout"));
				// The two means themselves (world Z, cm) and where the body sits, so a mean that is off can be told from a far field that is off.
				const UPrimitiveComponent* Primitive = Body.GetSimulatedPrimitive();
				const FVector At = Primitive ? Primitive->GetComponentLocation() : FVector::ZeroVector;
				Bench.Report(R.Name, Track.Name, bPass ? 1 : -1, FString::Printf(TEXT("floated %.0f cm lower (mean) once settled while the player was away (%.0f cm below the start at the deepest; %s), %s after; %s; mean Z here %.0f, away %.0f, now at (%.0f, %.0f, %.0f)"),
					Settled, Track.MaxDrop[1], Away.IsEmpty() ? TEXT("awake") : *Away, bAfloat ? TEXT("afloat") : TEXT("not in the water"), *FarField,
					Track.ZCount[0] > 0 ? Track.ZSum[0] / Track.ZCount[0] : 0.0, Track.ZCount[1] > 0 ? Track.ZSum[1] / Track.ZCount[1] : 0.0, At.X, At.Y, At.Z));
			});
			// Absolute, per pool body: its mean height in each window against its rest height on the water it knows there (W under it in the
			// capture, else the far field: the water volume's top plus the correction learned in the capture), less its rest draft and its
			// rested bottom, as SettleOnWater sets a body: within 5 cm or a quarter of its draft, in both phases. The comparison above passes a
			// body that is equally wrong here and away. A phase with no water known (away, a body that is neither in the capture nor on a flat
			// far field) does not hide the other: either phase off its rest height fails, and a pass needs both phases measured and within.
			// River floaters are reported the same, not judged.
			ForEach(R, [&Bench, &R](FTrack& Track, UFluidNinjaBuoyancyComponent& Body)
			{
				const bool bPool = !(Body.GetOwner() && Body.GetOwner()->ActorHasTag(RiverFloaterTag));
				const float RestDraft = Body.GetRestingDraft() > 0.f ? Body.GetRestingDraft() : Body.RestDraftCm;
				const float Tolerance = FMath::Max(5.f, 0.25f * RestDraft);
				bool bBothMeasured = true;
				bool bAnyOutside = false;
				auto PhaseText = [&Track, Tolerance, &bBothMeasured, &bAnyOutside](int32 P, const TCHAR* Label)
				{
					if (Track.RestCount[P] == 0)
					{
						bBothMeasured = false;
						return FString::Printf(TEXT("%s: no water known"), Label);
					}
					const double Miss = Track.RestMissSum[P] / Track.RestCount[P];
					bAnyOutside |= FMath::Abs(Miss) > Tolerance;
					return FString::Printf(TEXT("%s %+.1f cm (on water at %.1f, %.0f%% of the window in the capture, the rest far field)"), Label, Miss,
						Track.RestWaterSum[P] / Track.RestCount[P], 100.0 * Track.RestInCapture[P] / Track.RestCount[P]);
				};
				const FString Here = PhaseText(0, TEXT("here"));
				const FString There = PhaseText(1, TEXT("away"));
				const bool bJudged = bPool && Track.bWasInWater;
				const int32 Verdict = !bJudged ? 0 : bAnyOutside ? -1 : bBothMeasured ? (Track.bBroke ? -1 : 1) : 0;
				const TCHAR* Lead = !bPool ? TEXT("river floater, information: ") : !Track.bWasInWater ? TEXT("not afloat at the start, information: ")
					: Verdict == 0 ? TEXT("not measured: ") : TEXT("");
				Bench.Report(R.Name, Track.Name + TEXT(" rest height"), Verdict, FString::Printf(TEXT("%sits mean height off its rest height on the water it knows (water - rest draft %.1f cm - rested bottom %+.1f cm): %s; %s (pass: within %.1f cm, 5 cm or a quarter of its draft, in both phases; either phase measured outside it fails)"),
					Lead, RestDraft, Body.GetBottomZ(), *Here, *There, Tolerance));
			});
			// The phases themselves: the waves' own fields and the mean wave height B the open-water probes read in each window (B = η + P;
			// open water about 20 cm low near the player puts the near-player sink in the pool's waves, about 0 with the bodies still low puts
			// it in their own fields).
			auto WavesText = [&R](int32 P, const TCHAR* Label)
			{
				const FRun::FWalkawayPhase& Waves = R.Walkaway[P];
				const FString Fields = Waves.FieldsMost >= 0 ? FString::Printf(TEXT("%d..%d own fields (%d..%d feel the waves)"), Waves.FieldsLeast, Waves.FieldsMost, Waves.FeltLeast, Waves.FeltMost)
					: FString(TEXT("own fields not in the waves' status line"));
				// The readings taken while every point lay outside the running simulations are left out (the waves read such a point as 0,
				// not as still water), and said so.
				const FString Outside = Waves.ProbeOutsideReadings > 0
					? FString::Printf(TEXT("; %d reading%s left out, every point outside the running wave simulations"), Waves.ProbeOutsideReadings, Waves.ProbeOutsideReadings == 1 ? TEXT("") : TEXT("s"))
					: FString();
				const FString Some = Waves.ProbeReadings > 0 && Waves.ProbePointsLeast < R.WalkawayProbes.Num()
					? FString::Printf(TEXT(" (as few as %d of them inside a running simulation)"), Waves.ProbePointsLeast) : FString();
				const FString Open = R.WalkawayProbes.IsEmpty() ? FString(TEXT("open water not probed"))
					: Waves.ProbeReadings > 0 ? FString::Printf(TEXT("open-water B %+.2f cm over %d readings at %d points%s%s"), Waves.ProbeSum / Waves.ProbeReadings, Waves.ProbeReadings, R.WalkawayProbes.Num(), *Some, *Outside)
					: Waves.ProbeOutsideReadings > 0 ? FString::Printf(TEXT("open-water B: not read, the %d points outside the running wave simulations in all %d readings"), R.WalkawayProbes.Num(), Waves.ProbeOutsideReadings)
					: FString::Printf(TEXT("open-water B: none read at the %d points (the pool's simulation may not run waves then)"), R.WalkawayProbes.Num());
				return FString::Printf(TEXT("%s: %s, %s"), Label, *Fields, *Open);
			};
			Bench.Report(R.Name, TEXT("-"), 0, WavesText(0, TEXT("here")) + TEXT("; ") + WavesText(1, TEXT("away")) + TEXT("; ") + WavesText(2, TEXT("back"))
				+ (R.WalkawayProbes.IsEmpty() ? TEXT("; no probes: ") + R.WalkawayProbeNote : FString()));
		};
	}
	else
	{
		Report(Name, TEXT("-"), 0, TEXT("no such test (settle, capsize, drop, wind, throttle, gather, walkaway, all; ripple-dispersion, ripple-fps, ripple-kelvin, ripple-weight, ripple-rest, ripple-coupling, ripple-evers, ripple-advection, ripples)"));
		return nullptr;
	}
	return Made;
}

TUniquePtr<FFluidNinjaBuoyancyTestBench::FRun> FFluidNinjaBuoyancyTestBench::MakeRippleRun(const FString& Name)
{
	using namespace FluidNinjaBuoyancyBenchRipples;
	TUniquePtr<FRun> Made = MakeUnique<FRun>();
	Made->Name = Name;
	Made->bRipple = true;
	Made->Ripple = MakeShared<FRippleData>();

	// Every ripple test first: the ripples must be running (their render target and collection made, FluidNinja Buoyancy on, a camera).
	auto Begin = [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
	{
		const IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
		if (!Layer || !Layer->IsActive())
		{
			Bench.Report(R.Name, TEXT("-"), 0, Layer ? TEXT("not run, the ripples are off: ") + Layer->GetStatusLine() : FString(TEXT("not run: this world has no ripple layer")));
			R.bAbort = true;
			return false;
		}
		return true;
	};
	auto Abort = [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R, const FString& Why)
	{
		Bench.Report(R.Name, TEXT("-"), 0, TEXT("not run: ") + Why);
		R.bAbort = true;
	};
	// A flat, still field to measure from: every wave simulation cleared (the erase flag everywhere) with every body's own field, then 2 s.
	// Then runs just before the clear is asked for, so the sources it switches are the ones the erase takes (η = -P, with P as it will be:
	// a body whose static head it switches off must lose its hollow with it, or the hollow springs back as waves); Each every frame of the 2 s.
	auto AddClear = [&Made](const TCHAR* Label, TFunction<void(FFluidNinjaBuoyancyTestBench&, FRun&)> Then, TFunction<void(FFluidNinjaBuoyancyTestBench&, FRun&)> Each)
	{
		Made->Phases.Add({TEXT("clearing the waves"), 0.1f, 0.f, [Then](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			if (Then)
			{
				Then(Bench, R);
			}
			if (IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves(); Layer && !R.bAbort)
			{
				Layer->RequestClear();
			}
		}, nullptr});
		Made->Phases.Add({Label, 2.f, 0.f, nullptr, [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			const IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
			if (!Layer || !Layer->IsActive())
			{
				Bench.Report(R.Name, TEXT("-"), 0, TEXT("stopped: the ripples did not come back on (") + (Layer ? Layer->GetStatusLine() : FString()) + TEXT(")"));
				R.bAbort = true;
			}
		}, Each});
	};
	// Controlled flat still water at the bodies' water, in calm air: the solver alone.
	auto Controlled = [](FFluidNinjaBuoyancyTestBench& Bench)
	{
		Bench.SetControlledWater(true);
		const float Zero = 0.f;
		Bench.SetTestWind(true, 0.f, &Zero);
	};
	// The live pool with FluidNinja's turbulence off, in calm air; returns the seconds left until its water has stilled.
	auto LivePool = [](FFluidNinjaBuoyancyTestBench& Bench) -> float
	{
		Bench.SetControlledWater(false);
		const float Zero = 0.f;
		Bench.SetTestWind(true, 0.f, &Zero);
		if (!Bench.IsPoolCalm())
		{
			Bench.SetPoolCalm(true);
			Bench.bPoolCalmedByTest = true;
		}
		const UWorld* World = Bench.GetWorld();
		const double Since = World && Bench.PoolCalmSince >= 0.0 ? World->GetTimeSeconds() - Bench.PoolCalmSince : 0.0;
		return static_cast<float>(FMath::Max(CalmSettleSeconds - Since, 0.1));
	};
	// The bench reads the simulation holding Point from now on (its window, stats and Wave Aux; put back when the test ends), and the
	// water map is read again from it.
	auto Focus = [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R, const FVector2D& Point)
	{
		if (IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves())
		{
			Layer->SetBenchFocus(Point);
		}
		R.Ripple->Water = FWaterMap();
	};
	// Where the waves run in the window the bench reads, from the core's own Wave Aux (its wet mask and depth), read once per window.
	auto EnsureWater = [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
	{
		FWaterMap& Water = R.Ripple->Water;
		if (!Water.IsValid() && Bench.BuildWaterMap(Water))
		{
			// What the core shows, for the log: its wet cells' box and depths (a check of the map against the level's own water).
			FBox2D Wet(ForceInit);
			double Least = TNumericLimits<double>::Max();
			double Most = 0.0;
			int32 Count = 0;
			for (int32 Y = 0; Y < Water.CoreSize.Y; ++Y)
			{
				for (int32 X = 0; X < Water.CoreSize.X; ++X)
				{
					const FLinearColor& Cell = Water.Core[Y * Water.CoreSize.X + X];
					if (Cell.R > 0.5f)
					{
						Wet += Water.CoreOrigin + FVector2D(X + 0.5, Y + 0.5) * Water.CoreCellCm;
						Least = FMath::Min(Least, static_cast<double>(Cell.G));
						Most = FMath::Max(Most, static_cast<double>(Cell.G));
						++Count;
					}
				}
			}
			const int32 Open = Water.Depth.FilterByPredicate([](float Depth) { return Depth >= OpenDepthCm; }).Num();
			UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy test %s: water from the core's Wave Aux: window (%.0f, %.0f) %.0f cm, %d x %d cells of %.2f cm; %d wet, X %.0f..%.0f, Y %.0f..%.0f, %.0f-%.0f cm deep; %d of %d map cells of %.0f cm open"),
				*R.Name, Water.CoreOrigin.X, Water.CoreOrigin.Y, Water.CoreSize.X * Water.CoreCellCm, Water.CoreSize.X, Water.CoreSize.Y, Water.CoreCellCm, Count,
				Wet.Min.X, Wet.Max.X, Wet.Min.Y, Wet.Max.Y, Count > 0 ? Least : 0.0, Most, Open, Water.Depth.Num(), Water.Step);
		}
		return Water.IsValid();
	};
	// Why EnsureWater found none, for the reports.
	auto NoWater = [](FFluidNinjaBuoyancyTestBench& Bench)
	{
		const IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
		return Layer && Layer->GetWindowCm() > 0.0 ? FString(TEXT("the core wrote no Wave Aux for this simulation (its NinjaLiveCore predates Wave Aux, or the target is not bound yet)"))
			: FString(TEXT("the place is in no simulation running waves"));
	};
	// An open spot for pokes over every simulation running waves (Focus is left on its window): the finest cells among the windows whose
	// most open spot has at least RoomCm of open water around it (a poke is best resolved on the finest grid), else the most open spot
	// anywhere; false without open water anywhere.
	auto MostOpenAnywhere = [Focus, EnsureWater](FFluidNinjaBuoyancyTestBench& Bench, FRun& R, double RoomCm, FVector2D& OutSpot, double& OutOpen)
	{
		IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
		TArray<FBox2D> Windows;
		if (Layer)
		{
			Layer->GetBenchWindows(Windows);
		}
		bool bFound = false;
		bool bRoomy = false;
		double BestCell = TNumericLimits<double>::Max();
		for (const FBox2D& Window : Windows)
		{
			Focus(Bench, R, Window.GetCenter());
			FVector2D Spot;
			double Open = 0.0;
			if (!EnsureWater(Bench, R) || !R.Ripple->Water.MostOpen(R.Ripple->Water.Centre(), 1500.0, Spot, Open))
			{
				continue;
			}
			const bool bHasRoom = Open >= RoomCm;
			const double Cell = R.Ripple->Water.CoreCellCm;
			const bool bBetter = !bFound || (bHasRoom && !bRoomy) || (bHasRoom == bRoomy && (bHasRoom ? Cell < BestCell : Open > OutOpen));
			if (bBetter)
			{
				bFound = true;
				bRoomy = bHasRoom;
				BestCell = Cell;
				OutSpot = Spot;
				OutOpen = Open;
			}
		}
		if (!bFound)
		{
			return false;
		}
		Focus(Bench, R, OutSpot);
		return EnsureWater(Bench, R);
	};
	// The other bodies' boxes seen from above (their own motion is not the test's).
	auto OtherBoxes = [](const FFluidNinjaBuoyancyTestBench& Bench, TArrayView<const UFluidNinjaBuoyancyComponent* const> Leave)
	{
		TArray<FBox2D> Boxes;
		const UFluidNinjaBuoyancySubsystem* Hydro = Bench.GetHydro();
		for (const UFluidNinjaBuoyancyComponent* Body : Hydro ? Hydro->GetBodies() : TArray<UFluidNinjaBuoyancyComponent*>())
		{
			const UPrimitiveComponent* Primitive = Body->GetSimulatedPrimitive();
			if (Primitive && !Leave.Contains(Body))
			{
				const FBox Box = Primitive->Bounds.GetBox();
				Boxes.Add(FBox2D(FVector2D(Box.Min), FVector2D(Box.Max)));
			}
		}
		return Boxes;
	};
	// Directions out from Centre whose points at the radii lie over open water, best first: the most points over water, then the furthest
	// from the other bodies; at most Count of them, at least 60 degrees apart. Candidates every 22.5 degrees from Axis.
	auto ChooseDirections = [](const FWaterMap& Water, const FVector2D& Centre, const FVector2D& Axis, TArrayView<const double> Radii,
		const TArray<FBox2D>& Others, int32 Count)
	{
		struct FCandidate
		{
			FVector2D Direction;
			double Score = 0.0;
		};
		TArray<FCandidate> Candidates;
		for (int32 Turn = 0; Turn < 16; ++Turn)
		{
			const FVector2D Direction = Axis.GetRotated(Turn * 22.5);
			int32 Open = 0;
			double Nearest = 1000.0;
			for (const double Radius : Radii)
			{
				const FVector2D Point = Centre + Direction * Radius;
				Open += Water.DepthAt(Point) >= OpenDepthCm && Water.ClearRadius(Point, 100.0) >= 50.0 ? 1 : 0;
				for (const FBox2D& Box : Others)
				{
					Nearest = FMath::Min(Nearest, FMath::Sqrt(Box.ComputeSquaredDistanceToPoint(Point)));
				}
			}
			Candidates.Add({Direction, Open * 1.0e6 + Nearest});
		}
		Candidates.Sort([](const FCandidate& A, const FCandidate& B) { return A.Score > B.Score; });
		TArray<FVector2D> Chosen;
		for (const FCandidate& Candidate : Candidates)
		{
			if (Chosen.Num() >= Count || Candidate.Score < 1.0e6)
			{
				break;   // not one point of it over water
			}
			bool bApart = true;
			for (const FVector2D& Taken : Chosen)
			{
				bApart &= (Taken | Candidate.Direction) < FMath::Cos(FMath::DegreesToRadians(59.0));
			}
			if (bApart)
			{
				Chosen.Add(Candidate.Direction);
			}
		}
		return Chosen;
	};
	// Every body but Keep out of the way (lifted 50 m, still, its Waves off; put back after the test): their own static heads and waves,
	// and those they send back, are not the test's.
	auto LiftOthers = [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R, TFunctionRef<bool(const UFluidNinjaBuoyancyComponent&)> Keep)
	{
		UFluidNinjaBuoyancySubsystem* Hydro = Bench.GetHydro();
		for (UFluidNinjaBuoyancyComponent* Other : Hydro ? Hydro->GetBodies() : TArray<UFluidNinjaBuoyancyComponent*>())
		{
			UPrimitiveComponent* Primitive = Other->GetSimulatedPrimitive();
			if (!Primitive || !Other->GetOwner() || Keep(*Other))
			{
				continue;
			}
			R.WasRippleSwitch.Add({Other, Other->bWaves});
			Other->bWaves = false;
			// A body still held for its water (Restart At holds it, and the last test of a suite put every body back so) is let go first:
			// the subsystem's settling would otherwise set it back on the water at its own place about a second later, physics on, in the
			// middle of the test (suite runs logged every lifted body "settled on water" a second into ripple-coupling). Let go before its
			// physics is turned off below, since Release Hold turns it on.
			if (Other->IsAwaitingWater())
			{
				Other->ReleaseHold();
			}
			Primitive->SetSimulatePhysics(false);
			Primitive->SetWorldLocation(Primitive->GetComponentLocation() + FVector(0.0, 0.0, 5000.0), false, nullptr, ETeleportType::ResetPhysics);
			Other->MarkTeleported();
		}
	};
	// A body's own mass, kept once before a test sets another (put back after the test, and before each settling).
	auto KeepMass = [](FRun& R, UPrimitiveComponent& Primitive)
	{
		if (!R.WasMass.ContainsByPredicate([&Primitive](const TPair<TWeakObjectPtr<UPrimitiveComponent>, TPair<float, bool>>& Saved) { return Saved.Key.Get() == &Primitive; }))
		{
			const FBodyInstance* Instance = Primitive.GetBodyInstance();
			R.WasMass.Add({&Primitive, TPair<float, bool>(Instance ? Instance->GetMassOverride() : 0.f, Instance && Instance->bOverrideMass != 0)});
		}
	};
	auto OwnMass = [](FRun& R, UPrimitiveComponent& Primitive)
	{
		for (const TPair<TWeakObjectPtr<UPrimitiveComponent>, TPair<float, bool>>& Saved : R.WasMass)
		{
			FBodyInstance* Instance = Saved.Key.Get() == &Primitive ? Primitive.GetBodyInstance() : nullptr;
			if (Instance)
			{
				Instance->SetMassOverride(Saved.Value.Key, Saved.Value.Value);
				Instance->UpdateMassProperties();
			}
		}
	};
	// Drops a body from 3 m as the bench's Drop does (lifted, marked as teleported, let fall).
	auto DropBody = [](FFluidNinjaBuoyancyTestBench& Bench, UFluidNinjaBuoyancyComponent& Body)
	{
		const TWeakObjectPtr<UFluidNinjaBuoyancyComponent> Was = Bench.Selected;
		Bench.Selected = &Body;
		Bench.Drop(DropMetres);
		Bench.Selected = Was;
	};
	// A dropped body until it meets the water, every frame: the speed it hit the water at is the last dry frame's fall carried on to the
	// water line.
	auto FollowFall = [](FDropRun& Fall, const UWorld& World)
	{
		const UFluidNinjaBuoyancyComponent* Body = Fall.Body.Get();
		const UPrimitiveComponent* Primitive = Body ? Body->GetSimulatedPrimitive() : nullptr;
		if (Fall.DropTime < 0.0 || Fall.bImpacted || !Primitive)
		{
			return;
		}
		const float Bottom = Body->GetLowestPointZ();
		if (Bottom <= Fall.WaterZ)
		{
			const double Gravity = FMath::Max(-static_cast<double>(World.GetGravityZ()), 1.0);
			Fall.ImpactSpeed = static_cast<float>(FMath::Sqrt(FMath::Square(static_cast<double>(Fall.LastFallSpeed)) + 2.0 * Gravity * FMath::Max(Fall.LastBottom - Fall.WaterZ, 0.f)));
			Fall.ImpactTime = World.GetTimeSeconds();
			Fall.bImpacted = true;
		}
		else
		{
			Fall.LastBottom = Bottom;
			Fall.LastFallSpeed = Primitive->IsSimulatingPhysics() ? FMath::Max(-static_cast<float>(Primitive->GetPhysicsLinearVelocity().Z), 0.f) : 0.f;
		}
	};

	// ripple-dispersion and ripple-fps: a Gaussian pressure head pressing for one of the core's ticks (an impulse) on controlled water at
	// the most open spot of any simulation running waves, with no hull or swimmer source; its rings read 1, 2 and 4 m out in four directions
	// until the largest crest has passed the furthest, against the core's own impulse response there (PokeResponse).
	// Judged against the core's own discrete theory (its dispersion symbol on its grid, stepped at its tick: what the numpy port of Wave
	// Prepare and Wave Step that reproduced the GPU's crests to the millisecond runs), and only when the frames' pacing was regular; the
	// continuous theory, whose group speed is 4-5% off the core's (its largest crest half a period off by 4 m), is reported beside it. Returns
	// the continuous fit: ripple-fps compares its two runs' crests as picked against it, measured against measured.
	auto ReportPoke = [](FFluidNinjaBuoyancyTestBench& Bench, const FString& Test, const FPokeRun& Poked, bool bJudge)
	{
		const FPokeFit Fit = FitPokeRun(Poked);
		// The same theory stepped as the core steps it (information): what of a miss is its time step's, and what its grid's.
		const FPokeFit Stepped = FitPokeRun(Poked, true);
		// Built here, when the test is judged (tens of milliseconds), not in the poke's frame, whose length the pacing guard counts.
		const FCoreSymbol Symbol = ComputeCoreSymbol(Poked.CoreCells, Poked.CellCm, Poked.DepthCm);
		const FPokeFit Discrete = Symbol.bValid ? FitPokeRun(Poked, true, &Symbol) : FPokeFit();
		// Regular pacing: one count of the core's ticks in every frame of the reading. Under frames of 1 and 2 ticks the readings' lag behind
		// their stamps varies, which one fitted phase cannot absorb (the P4 runs' r101 at 49 fps), so nothing is judged then. No stray frame
		// is let through: a run not on a fixed frame step whose accumulator drifts a whole tick over the reading (one catch-up frame) is not
		// judged either, so in practice only fixed-step runs (-UseFixedTimeStep, or ripple-fps's own step) get a verdict; the frames per
		// count are reported so that one catch-up frame can be told from a sustained alternation.
		const bool bCounted = Poked.TicksLeast <= Poked.TicksMost;
		const bool bRegular = bCounted && Poked.TicksLeast == Poked.TicksMost;
		const FString Pacing = bCounted ? FString::Printf(TEXT("%d..%d core ticks per frame"), Poked.TicksLeast, Poked.TicksMost) : FString(TEXT("no frame counted"));
		// Frames per count of ticks ("1 tick: 658 frames, 2 ticks: 1 frame").
		auto Tally = [](const int32 (&PerCount)[4])
		{
			FString Text;
			for (int32 Each = 0; Each < 4; ++Each)
			{
				if (PerCount[Each] > 0)
				{
					Text += FString::Printf(TEXT("%s%d%s tick%s: %d frame%s"), Text.IsEmpty() ? TEXT("") : TEXT(", "), Each, Each == 3 ? TEXT(" or more") : TEXT(""),
						Each == 1 ? TEXT("") : TEXT("s"), PerCount[Each], PerCount[Each] == 1 ? TEXT("") : TEXT("s"));
				}
			}
			return Text.IsEmpty() ? FString(TEXT("none")) : Text;
		};
		const FString ModelTally = Tally(Poked.TickFrames);
		const FString CoreTally = !bCounted ? FString(TEXT("no frame counted"))
			: !Poked.bStatusEveryFrame ? FString(TEXT("not read on every frame (the waves rebuild their status line every frame only while the debug view is on or fnb.TestHUD is not 0)"))
			: !Poked.bCoreTicksEveryFrame ? FString::Printf(TEXT("not read on every frame (the status line did not list a running simulation of %d x %.2f cm each time)"), Poked.CoreCells, Poked.CellCm)
			: Tally(Poked.CoreTickFrames);
		const FString Who = Poked.Label.IsEmpty() ? FString(TEXT("poke")) : Poked.Label;
		if (!Fit.bValid)
		{
			Bench.Report(Test, Who, 0, TEXT("not measured: ") + Fit.Problem);
			return Fit;
		}
		if (Poked.CoreInterface >= 0 && Poked.CoreInterface != CoreTableInterface)
		{
			UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("FluidNinja Buoyancy test %s: the bench's copy of the core's dispersion table is interface %d's, and this core is interface %d: copy Ninja_WaveStep.hlsl's WG and WC tables into CoreDispersionWG/WC again and set CoreTableInterface."),
				*Test, CoreTableInterface, Poked.CoreInterface);
		}
		const FString InterfaceText = Poked.CoreInterface < 0
			? FString::Printf(TEXT("its table copied from interface %d; the running core's interface is not in the waves' status line"), CoreTableInterface)
			: Poked.CoreInterface == CoreTableInterface ? FString::Printf(TEXT("its table copied from interface %d, the running core's"), CoreTableInterface)
			: FString::Printf(TEXT("WARNING: its table copied from interface %d, but the running core is interface %d"), CoreTableInterface, Poked.CoreInterface);
		const double MeanTicks = Poked.Frames > 0 ? Poked.FrameSum * Poked.StepRate / Poked.Frames : 0.0;
		Bench.Report(Test, Who, 0, FString::Printf(TEXT("a %.1f cm pressure head, sigma %.0f cm, pressing for one of the core's ticks (an impulse, spread over the %d tick%s of its frame) at (%.0f, %.0f), %.1f m of open water around; the core runs %.0f cm deep there (%.0f-%.0f within the rings' reach), %.0f ticks/s, %.2f cm cells; theories: the impulse response of the core's own dispersion symbol on its %d cells, stepped at its tick (discrete, judged; %s%s), and of omega^2 = g (k + (sigma/rho g) k^3) tanh(k d) (continuous, information), both with the core's g, capillarity, damping and hyperviscosity; %d readings at %.1f fps (longest frame %.0f ms); pacing %s (%.2f on average; %s; the bench's model of the core's fixed-step accumulator from the frames' lengths, the guard's count: a single frame off the one count leaves the fits unjudged; the core's own count from the waves' status line, one or two frames late, information: %s); fitted phase %+.2f ticks continuous, %+.2f discrete (the tick's phase less the readings' lag)"),
			Poked.HeightCm, Poked.SigmaCm, Poked.Ticks, Poked.Ticks == 1 ? TEXT("") : TEXT("s"), Poked.Centre.X, Poked.Centre.Y, Poked.OpenCm / 100.0, Poked.DepthCm, Poked.ShallowestCm,
			Poked.DeepestCm, Poked.StepRate, Poked.CellCm, Poked.CoreCells, *InterfaceText, Symbol.bValid ? TEXT("") : TEXT("; it could not be built"), Fit.Readings, Fit.MeanFps,
			Poked.LongestFrame * 1000.f, *Pacing, MeanTicks, *ModelTally, *CoreTally, Fit.StepPhase, Discrete.bValid ? Discrete.StepPhase : 0.0));
		// One radius against one theory: its largest crest, the same crest in the readings, and the normalised RMS over the record.
		auto Describe = [](const FRadiusFit& At)
		{
			const double Arrival = At.Theory.Time > 0.0 ? (At.Measured.Time - At.Theory.Time) / At.Theory.Time : 0.0;
			const double Height = At.Theory.Height != 0.0 ? (At.Measured.Height - At.Theory.Height) / At.Theory.Height : 0.0;
			const bool bOwnDiffers = At.Largest.bFound && FMath::Abs(At.Largest.Time - At.Measured.Time) > 0.1;
			return FString::Printf(TEXT("largest crest at %.3f s, %.3f cm (theory %.3f s, %.3f cm: arrival %+.1f%%, height %+.1f%%); normalised RMS %.1f%% over %d probes%s"),
				At.Measured.Time, At.Measured.Height, At.Theory.Time, At.Theory.Height, 100.0 * Arrival, 100.0 * Height, 100.0 * At.NormalisedRms, At.Probes,
				bOwnDiffers ? *FString::Printf(TEXT("; the readings' own largest crest is the one at %.3f s, %.3f cm"), At.Largest.Time, At.Largest.Height) : TEXT(""));
		};
		for (int32 Index = 0; Index < Fit.Radii.Num(); ++Index)
		{
			const FRadiusFit& Continuous = Fit.Radii[Index];
			const FRadiusFit* Core = Discrete.bValid && Discrete.Radii.IsValidIndex(Index) ? &Discrete.Radii[Index] : nullptr;
			const FString SteppedText = Stepped.bValid && Stepped.Radii.IsValidIndex(Index)
				? FString::Printf(TEXT("; continuous stepped as the core steps it (symplectic Euler at its tick, information): normalised RMS %.1f%%"), 100.0 * Stepped.Radii[Index].NormalisedRms)
				: FString();
			const double Arrival = Core && Core->Theory.Time > 0.0 ? (Core->Measured.Time - Core->Theory.Time) / Core->Theory.Time : 0.0;
			const bool bPass = Core && Core->Measured.bFound && Core->Theory.bFound && FMath::Abs(Arrival) <= 0.05 && Core->NormalisedRms <= 0.10;
			int32 Verdict = 0;
			FString Lead;
			if (bJudge && !bRegular)
			{
				Lead = FString::Printf(TEXT("not measured: irregular frame pacing (%s); "), *Pacing);
			}
			else if (bJudge && !Core)
			{
				Lead = FString::Printf(TEXT("not measured: the core's dispersion symbol could not be built (%d cells of %.2f cm at %.0f cm); "), Poked.CoreCells, Poked.CellCm, Poked.DepthCm);
			}
			else if (bJudge)
			{
				Verdict = bPass ? 1 : -1;
			}
			const FString CoreText = Core ? FString::Printf(TEXT("discrete theory (%s): %s (pass: 5%%, 10%%); "), Verdict != 0 ? TEXT("judged") : TEXT("information"), *Describe(*Core)) : FString();
			Bench.Report(Test, FString::Printf(TEXT("%s r %.0f cm"), *Who, Continuous.Radius), Verdict,
				Lead + CoreText + TEXT("continuous theory (information): ") + Describe(Continuous) + SteppedText);
		}
		return Fit;
	};
	// The open spot for the pokes, chosen once per test over every simulation running waves (the bench reads its window from then on).
	auto AddFindSpot = [&Made, Abort, MostOpenAnywhere]()
	{
		Made->Phases.Add({TEXT("finding open water"), 0.2f, 0.f, [Abort, MostOpenAnywhere](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			if (R.bAbort)
			{
				return;
			}
			// Room for the rings out past the furthest probe before anything comes back from a wall.
			if (!MostOpenAnywhere(Bench, R, 1.5 * PokeRadiiCm[2], R.Ripple->Spot, R.Ripple->SpotOpenCm))
			{
				Abort(Bench, R, TEXT("no open water in any simulation running waves (their Wave Aux shows none at least 30 cm deep)"));
			}
		}, nullptr});
	};
	auto AddPokeRun = [&Made, &AddClear, Abort](const FString& Label, float Fps)
	{
		if (Fps > 0.f)
		{
			// A fixed-step run (-UseFixedTimeStep) takes the frame rate as its fixed step; any other as t.MaxFPS.
			Made->Phases.Add({FString::Printf(TEXT("%.0f fps"), Fps), 1.5f, 0.f, [Fps](FFluidNinjaBuoyancyTestBench&, FRun& R)
			{
				if (FApp::UseFixedTimeStep())
				{
					R.WasFixedDelta = R.WasFixedDelta > 0.0 ? R.WasFixedDelta : FApp::GetFixedDeltaTime();
					FApp::SetFixedDeltaTime(1.0 / Fps);
				}
				else if (GEngine)
				{
					GEngine->SetMaxFPS(Fps);
				}
			}, nullptr});
		}
		// No hull or swimmer source, and the waves not carried by FluidNinja's current (the theory is still water's; ripple-advection
		// measures the carrying).
		AddClear(TEXT("fresh waves, no hull sources, not carried"), [](FFluidNinjaBuoyancyTestBench& Bench, FRun&)
		{
			SetWaveCurrentCoupling(0.f);
			SetWaveSources(false, false);
			if (IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves())
			{
				Layer->SetProbes(TArray<FVector2D>());
			}
		}, nullptr);
		Made->Phases.Add({TEXT("poked: reading the rings"), 6.f, 0.f, [Label, Fps, Abort](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
			const UWorld* World = Bench.GetWorld();
			const FWaterMap& Water = R.Ripple->Water;
			if (!Layer || !World || !Water.IsValid())
			{
				Abort(Bench, R, TEXT("no waves to poke"));
				return;
			}
			FPokeRun& Poked = R.Ripple->Pokes.AddDefaulted_GetRef();
			Poked.Label = Label;
			Poked.FixedFps = Fps;
			const double Cell = Layer->GetCellCm();
			const FVector2D Origin = Layer->GetWindowOrigin();
			Poked.Centre = SnapToCell(R.Ripple->Spot, Cell, Origin);
			Poked.OpenCm = R.Ripple->SpotOpenCm;
			const FVector2D Directions[] = {FVector2D(1.0, 0.0), FVector2D(0.0, 1.0), FVector2D(-1.0, 0.0), FVector2D(0.0, -1.0)};
			for (const double Radius : PokeRadiiCm)
			{
				for (const FVector2D& Direction : Directions)
				{
					const FVector2D Point = SnapToCell(Poked.Centre + Direction * Radius, Cell, Origin);
					Poked.Log.Points.Add(Point);
					Poked.Radii.Add(FVector2D::Distance(Point, Poked.Centre));
				}
			}
			// The core as it runs here: the depth (its Wave Aux at the poke, and its least and most out past the furthest ring), the ticks
			// this frame's write is spread over, its cell, damping and hyperviscosity (as Ninja_WaveParams clamps it).
			const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
			Poked.StepRate = Layer->GetStepsPerSecond();
			Poked.Ticks = FMath::Max(FMath::RoundToInt32(Bench.FrameSeconds * Poked.StepRate), 1);
			// The wave grid the core steps (its cells across: the N its mips halve) and its interface, for its dispersion symbol.
			Poked.CoreCells = FMath::RoundToInt32(Layer->GetWindowCm() / FMath::Max(Cell, 1.0e-3));
			Poked.CoreInterface = CoreInterfaceOf(Layer->GetStatusLine(), Poked.CoreCells, Cell);
			if (Poked.CoreCells != Water.CoreSize.X)
			{
				UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("FluidNinja Buoyancy test %s: the window is %d wave cells of %.2f cm across, its Wave Aux %d: the dispersion symbol is built on %d."),
					*R.Name, Poked.CoreCells, Cell, Water.CoreSize.X, Poked.CoreCells);
			}
			Poked.DepthCm = Water.CoreAt(Poked.Centre).G;
			Water.CoreDepthRange(Poked.Centre, PokeRadiiCm[2] + 4.0 * Poked.SigmaCm, Poked.ShallowestCm, Poked.DeepestCm);
			Poked.CellCm = Cell;
			// The poke's frame counted with the reading's (its grid known now, for the core's own count in the status line).
			const bool bStatusRebuilt = WavesStatusEveryFrame();
			Poked.CountTicks(Bench.FrameSeconds, bStatusRebuilt, bStatusRebuilt ? CoreTicksOf(Layer->GetStatusLine(), Poked.CoreCells, Poked.CellCm) : -1);
			Poked.Damping = FMath::Max(Settings.WaveDamping, 0.f);
			Poked.Hyper = FMath::Clamp(Settings.WaveHyperviscosity, 0.f, 50.f);
			// Until the largest crest has passed the furthest ring: the impulse's rings carry the most near k = √2/σ, arriving at r / c_g
			// (9.6 s at 4 m in deep water).
			Poked.RecordSeconds = FMath::Max(6.0, 1.25 * PokeRadiiCm[2] / CoreGroupVelocity(FMath::Sqrt(2.0) / Poked.SigmaCm, FMath::Max(Poked.DepthCm, 1.0)) + 1.0);
			R.Phases[R.Phase].Seconds = static_cast<float>(Poked.RecordSeconds);
			Layer->SetProbes(Poked.Log.Points);
			Layer->Poke(Poked.Centre, Poked.HeightCm, Poked.SigmaCm);
			// The poke goes in before this frame's ticks, and those ticks cover the frame just passed.
			Poked.PokeTime = World->GetTimeSeconds() - Bench.FrameSeconds;
		}, nullptr, [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			if (R.Ripple->Pokes.IsEmpty())
			{
				return;
			}
			FPokeRun& Poked = R.Ripple->Pokes.Last();
			const IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
			if (Layer)
			{
				Poked.Log.Take(*Layer);
			}
			++Poked.Frames;
			Poked.FrameSum += Bench.FrameSeconds;
			Poked.LongestFrame = FMath::Max(Poked.LongestFrame, Bench.FrameSeconds);
			const bool bStatusRebuilt = WavesStatusEveryFrame();
			Poked.CountTicks(Bench.FrameSeconds, bStatusRebuilt, Layer && bStatusRebuilt ? CoreTicksOf(Layer->GetStatusLine(), Poked.CoreCells, Poked.CellCm) : -1);
		}});
	};

	if (Name == TEXT("ripple-dispersion"))
	{
		Made->Phases.Add({TEXT("controlled water"), 0.2f, 0.f, [Begin, Controlled](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			if (Begin(Bench, R))
			{
				Controlled(Bench);
			}
		}, nullptr});
		AddFindSpot();
		AddPokeRun(FString(), 0.f);
		Made->Evaluate = [ReportPoke](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			for (const FPokeRun& Poked : R.Ripple->Pokes)
			{
				ReportPoke(Bench, R.Name, Poked, true);
			}
		};
	}
	else if (Name == TEXT("ripple-fps"))
	{
		Made->Phases.Add({TEXT("controlled water"), 0.2f, 0.f, [Begin, Controlled](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			R.WasMaxFps = GEngine ? GEngine->GetMaxFPS() : -1.f;
			if (Begin(Bench, R))
			{
				Controlled(Bench);
			}
		}, nullptr});
		AddFindSpot();
		AddPokeRun(TEXT("30 fps"), 30.f);
		AddPokeRun(TEXT("60 fps"), 60.f);
		Made->Evaluate = [ReportPoke](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			if (R.Ripple->Pokes.Num() < 2)
			{
				Bench.Report(R.Name, TEXT("-"), 0, TEXT("not measured: a poke did not go in"));
				return;
			}
			// Each against the theory for information; the test is the two against each other.
			const FPokeFit Slow = ReportPoke(Bench, R.Name, R.Ripple->Pokes[0], false);
			const FPokeFit Fast = ReportPoke(Bench, R.Name, R.Ripple->Pokes[1], false);
			if (!Slow.bValid || !Fast.bValid)
			{
				Bench.Report(R.Name, TEXT("-"), 0, TEXT("no comparison: a run could not be measured"));
				return;
			}
			// The same waves at both frame rates: the core at the same depth in both runs.
			if (!FMath::IsNearlyEqual(R.Ripple->Pokes[0].DepthCm, R.Ripple->Pokes[1].DepthCm, 0.01))
			{
				Bench.Report(R.Name, TEXT("-"), 0, FString::Printf(TEXT("no comparison: the core ran %.0f cm deep at the first poke and %.0f cm at the second"),
					R.Ripple->Pokes[0].DepthCm, R.Ripple->Pokes[1].DepthCm));
				return;
			}
			if (Fast.MeanFps < 1.3 * Slow.MeanFps)
			{
				Bench.Report(R.Name, TEXT("-"), 0, FString::Printf(TEXT("no comparison: both runs went at about the same frame rate (%.1f and %.1f fps; the machine may not reach 60, or the editor is throttled)"),
					Slow.MeanFps, Fast.MeanFps));
				return;
			}
			for (int32 Index = 0; Index < Slow.Radii.Num() && Index < Fast.Radii.Num(); ++Index)
			{
				const FRadiusFit& AtSlow = Slow.Radii[Index];
				const FRadiusFit& AtFast = Fast.Radii[Index];
				const bool bBoth = AtSlow.Measured.bFound && AtFast.Measured.bFound && AtFast.Measured.Time > 0.0 && AtFast.Measured.Height != 0.0;
				const double Arrival = bBoth ? (AtSlow.Measured.Time - AtFast.Measured.Time) / AtFast.Measured.Time : 0.0;
				const double Height = bBoth ? (AtSlow.Measured.Height - AtFast.Measured.Height) / AtFast.Measured.Height : 0.0;
				const bool bPass = bBoth && FMath::Abs(Arrival) <= 0.05 && FMath::Abs(Height) <= 0.05;
				Bench.Report(R.Name, FString::Printf(TEXT("r %.0f cm"), AtSlow.Radius), bPass ? 1 : -1,
					FString::Printf(TEXT("at %.1f fps the largest crest arrives at %.3f s, %.3f cm; at %.1f fps at %.3f s, %.3f cm: arrival %+.1f%%, height %+.1f%% (pass: both within 5%%)"),
						Slow.MeanFps, AtSlow.Measured.Time, AtSlow.Measured.Height, Fast.MeanFps, AtFast.Measured.Time, AtFast.Measured.Height, 100.0 * Arrival, 100.0 * Height));
			}
		};
	}
	else if (Name == TEXT("ripple-weight"))
	{
		// The crate at three masses and the row boat at its own, each dropped from 3 m, displacement alone and then with the impulse. Only
		// the body dropped is in the water: each drop's settling puts its own body back and lifts every other one out of the way, so a
		// run on its own reads the same water as a run in a suite, wherever the tests before it left the other bodies.
		UFluidNinjaBuoyancyComponent* Crate = FindTestBody(TEXT("Crate"));
		UFluidNinjaBuoyancyComponent* Boat = FindTestBody(TEXT("Boat"));
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			auto Add = [&Made, Pass](UFluidNinjaBuoyancyComponent* Body, float Mass, const FString& Label)
			{
				FDropRun& Fall = Made->Ripple->Drops.AddDefaulted_GetRef();
				Fall.Body = Body;
				Fall.MassKg = Mass;
				Fall.bImpulse = Pass == 1;
				Fall.Name = Label;
			};
			if (Crate)
			{
				for (const float Mass : CrateMassesKg)
				{
					Add(Crate, Mass, FString::Printf(TEXT("%s %.0f kg"), *BodyName(Crate), Mass));
				}
			}
			if (Boat)
			{
				Add(Boat, 0.f, BodyName(Boat));
			}
		}
		Made->Phases.Add({TEXT("calming the pool"), 0.1f, 0.f, [Begin, Abort, LivePool, LiftOthers](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			if (!Begin(Bench, R))
			{
				return;
			}
			if (R.Ripple->Drops.IsEmpty())
			{
				Abort(Bench, R, TEXT("no crate and no row boat among the floating bodies"));
				return;
			}
			R.bResetAfter = true;
			R.Phases[R.Phase].Seconds = LivePool(Bench);
			// Every body but the first one dropped out of the way now, so the pool calms without them: one leaning on a dropped body would
			// carry part of its weight, and one floating beside it would bob, make waves of its own and send the drop's back. The other
			// dropped body is lifted too, and put back for its own drops (below).
			const UFluidNinjaBuoyancyComponent* First = R.Ripple->Drops[0].Body.Get();
			LiftOthers(Bench, R, [First](const UFluidNinjaBuoyancyComponent& Body) { return &Body == First; });
		}, nullptr});
		// A body this run lifted, back in play: the Waves switch LiftOthers saved for it restored and the saved entry dropped, so a later
		// lift saves it afresh. The caller then puts it back on the water with Restart At, which turns its physics on again.
		auto EndLift = [](FRun& R, UFluidNinjaBuoyancyComponent& Body)
		{
			for (const TPair<TWeakObjectPtr<UFluidNinjaBuoyancyComponent>, bool>& Saved : R.WasRippleSwitch)
			{
				if (Saved.Key.Get() == &Body)
				{
					Body.bWaves = Saved.Value;
				}
			}
			R.WasRippleSwitch.RemoveAll([&Body](const TPair<TWeakObjectPtr<UFluidNinjaBuoyancyComponent>, bool>& Saved) { return Saved.Key.Get() == &Body; });
		};
		// Whether this run has lifted Body and not put it back: lifting it again would save its Waves switch as off, and the test's end,
		// which restores the saved switches in order, would leave it off.
		auto IsLifted = [](const FRun& R, const UFluidNinjaBuoyancyComponent& Body)
		{
			return R.WasRippleSwitch.ContainsByPredicate([&Body](const TPair<TWeakObjectPtr<UFluidNinjaBuoyancyComponent>, bool>& Saved) { return Saved.Key.Get() == &Body; });
		};
		// Whether a body is at rest for its static head at Now: over the last WeightSettledSeconds (watched all through) it moved up or down
		// slower than WeightSettledCmS every frame and was never held, and the static head's readings (at least WeightSettledReadings of
		// them) spread over less than WeightSettledSpread of its wave hull's mean volume under the water then. What it found is kept on the
		// drop for its report line, whatever the answer.
		auto JudgeRest = [](FDropRun& Fall, double Now)
		{
			const double From = Now - WeightSettledSeconds;
			bool bHeld = false;
			double Fastest = 0.0;
			for (int32 Sample = 0; Sample < Fall.RiseTimes.Num() && Sample < Fall.RiseCmS.Num(); ++Sample)
			{
				if (Fall.RiseTimes[Sample] >= From)
				{
					bHeld |= Fall.RiseCmS[Sample] < 0.0;
					Fastest = FMath::Max(Fastest, Fall.RiseCmS[Sample]);
				}
			}
			const bool bWatched = !Fall.RiseTimes.IsEmpty() && Fall.RiseTimes[0] <= From;
			double Least = TNumericLimits<double>::Max();
			double Most = -TNumericLimits<double>::Max();
			int32 Readings = 0;
			for (int32 Sample = 0; Sample < Fall.SourceTimes.Num() && Sample < Fall.SourceCm3.Num(); ++Sample)
			{
				if (Fall.SourceTimes[Sample] >= From && Fall.SourceTimes[Sample] <= Now)
				{
					Least = FMath::Min(Least, Fall.SourceCm3[Sample]);
					Most = FMath::Max(Most, Fall.SourceCm3[Sample]);
					++Readings;
				}
			}
			double HullSum = 0.0;
			int32 HullCount = 0;
			for (int32 Sample = 0; Sample < Fall.HullTimes.Num() && Sample < Fall.HullCm3.Num(); ++Sample)
			{
				if (Fall.HullTimes[Sample] >= From && Fall.HullTimes[Sample] <= Now)
				{
					HullSum += Fall.HullCm3[Sample];
					++HullCount;
				}
			}
			Fall.SettleSpreadCm3 = Readings > 0 ? Most - Least : 0.0;
			Fall.SettleReadings = Readings;
			Fall.SettleHullCm3 = HullCount > 0 ? HullSum / HullCount : 0.0;
			Fall.SettleFastestCmS = bHeld ? -1.0 : Fastest;
			Fall.bSettleJudged = true;
			// Looking back from Now, for the report's settle time (the check itself can only run once the fresh field's readings exist):
			// the first frame of the unbroken run of free frames slower than the limit, and the earliest static-head reading from which on
			// the readings stayed within the spread's limit.
			Fall.StillSince = Now;
			for (int32 Sample = FMath::Min(Fall.RiseTimes.Num(), Fall.RiseCmS.Num()) - 1;
				Sample >= 0 && Fall.RiseCmS[Sample] >= 0.0 && Fall.RiseCmS[Sample] < WeightSettledCmS; --Sample)
			{
				Fall.StillSince = Fall.RiseTimes[Sample];
			}
			Fall.SteadySince = Now;
			double RunLeast = TNumericLimits<double>::Max();
			double RunMost = -TNumericLimits<double>::Max();
			for (int32 Sample = FMath::Min(Fall.SourceTimes.Num(), Fall.SourceCm3.Num()) - 1; Sample >= 0 && Fall.SettleHullCm3 > 0.0; --Sample)
			{
				if (Fall.SourceTimes[Sample] > Now)
				{
					continue;
				}
				RunLeast = FMath::Min(RunLeast, Fall.SourceCm3[Sample]);
				RunMost = FMath::Max(RunMost, Fall.SourceCm3[Sample]);
				if (RunMost - RunLeast >= WeightSettledSpread * Fall.SettleHullCm3)
				{
					break;
				}
				Fall.SteadySince = Fall.SourceTimes[Sample];
			}
			return bWatched && !bHeld && Fastest < WeightSettledCmS && Readings >= WeightSettledReadings && Fall.SettleHullCm3 > 0.0
				&& Fall.SettleSpreadCm3 < WeightSettledSpread * Fall.SettleHullCm3;
		};
		for (int32 Index = 0; Index < Made->Ripple->Drops.Num(); ++Index)
		{
			const FDropRun& Planned = Made->Ripple->Drops[Index];
			// Put back at its start and settled at the test's mass (its static head at rest is read before the drop), the bench reading
			// the simulation it floats in. Every frame: its vertical speed, and when the subsystem lets it go on its water (its settle time
			// counts from there). Only it is in the water: it is put back from its lift when another body's drops lifted it, and every other
			// body still in the water is lifted (the crate for the boat's drops, the boat for the crate's), all put back after the test.
			Made->Phases.Add({FString::Printf(TEXT("%s settling"), *Planned.Name), 3.f, 0.f, [Index, OwnMass, KeepMass, Focus, LiftOthers, EndLift, IsLifted](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
			{
				FDropRun& Fall = R.Ripple->Drops[Index];
				UFluidNinjaBuoyancyComponent* Body = Fall.Body.Get();
				if (Body)
				{
					// Its Waves switch back first (the subsystem reads it every frame), so its static head goes into the field from the
					// put-back on, as for a body that was never lifted.
					EndLift(R, *Body);
					LiftOthers(Bench, R, [Body, &R, IsLifted](const UFluidNinjaBuoyancyComponent& Other) { return &Other == Body || IsLifted(R, Other); });
				}
				if (UPrimitiveComponent* Primitive = Body ? Body->GetSimulatedPrimitive() : nullptr)
				{
					if (Fall.MassKg > 0.f)
					{
						KeepMass(R, *Primitive);
						Primitive->SetMassOverrideInKg(NAME_None, Fall.MassKg, true);
					}
					else
					{
						OwnMass(R, *Primitive);
					}
					Body->RestartAt(Body->GetStartTransform());
					Focus(Bench, R, FVector2D(Body->GetStartTransform().GetLocation()));
					if (const UWorld* World = Bench.GetWorld())
					{
						Fall.PlacedTime = World->GetTimeSeconds();
					}
				}
			}, nullptr, [Index](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
			{
				// Its vertical speed from the put-back on (as Log keeps it later), so the settle time can reach back into this phase.
				FDropRun& Fall = R.Ripple->Drops[Index];
				const UFluidNinjaBuoyancyComponent* Body = Fall.Body.Get();
				const UPrimitiveComponent* Primitive = Body ? Body->GetSimulatedPrimitive() : nullptr;
				const UWorld* World = Bench.GetWorld();
				if (Fall.PlacedTime >= 0.0 && Primitive && World)
				{
					const bool bFree = !Body->IsAwaitingWater() && Primitive->IsSimulatingPhysics();
					Fall.RiseTimes.Add(World->GetTimeSeconds());
					Fall.RiseCmS.Add(bFree ? FMath::Abs(Primitive->GetPhysicsLinearVelocity().Z) : -1.0);
					if (bFree && Fall.FloatingTime < 0.0)
					{
						Fall.FloatingTime = World->GetTimeSeconds();
					}
				}
			}});
			// The water it falls into, the probes 2, 4 and 8 m out in three directions over open water, and the field's volume and energy
			// with the volume its displacement source put in (the same readback).
			auto Log = [Index, FollowFall](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
			{
				FDropRun& Fall = R.Ripple->Drops[Index];
				const IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
				const UWorld* World = Bench.GetWorld();
				if (!Layer || !World)
				{
					return;
				}
				Fall.Log.Take(*Layer);
				double Time = 0.0;
				double Volume = 0.0;
				double Potential = 0.0;
				double Kinetic = 0.0;
				float MaxAbs = 0.f;
				if (Layer->GetStats(Time, Volume, Potential, Kinetic, MaxAbs) && Time > Fall.LastStats)
				{
					Fall.LastStats = Time;
					Fall.StatTimes.Add(Time);
					Fall.Volumes.Add(Volume);
					Fall.Energies.Add(Potential + Kinetic);
					double SourceTime = 0.0;
					double Source = 0.0;
					if (Layer->GetSourceVolume(SourceTime, Source))
					{
						Fall.SourceTimes.Add(SourceTime);
						Fall.SourceCm3.Add(Source);
					}
				}
				// The water it pushes aside as it goes (the field's early volume and the source's are read against it; the readbacks lag a
				// few frames).
				if (const UFluidNinjaBuoyancyComponent* Body = Fall.Body.Get())
				{
					Fall.DisplacedTimes.Add(World->GetTimeSeconds());
					Fall.DisplacedCm3.Add(Body->GetReadout().DisplacedLitres * 1000.0);
				}
				FollowFall(Fall, *World);
				// Before the lift: the wave hull's volume under its water, from the same triangles the source rasters, clipped here at 1 cm.
				const UFluidNinjaBuoyancyComponent* Body = Fall.Body.Get();
				const UPrimitiveComponent* Primitive = Body ? Body->GetSimulatedPrimitive() : nullptr;
				FBenchHull Hull;
				if (Fall.DropTime < 0.0 && Primitive && Body && GetBenchHull(*Body, Hull))
				{
					const FTransform Frame(Primitive->GetComponentQuat(), Primitive->GetComponentLocation());
					const int32 First = -FMath::CeilToInt32(Primitive->Bounds.SphereRadius + 2.0);
					TArray<float> Foot;
					HullFootprint(Hull, Frame, Fall.WaterZ, Primitive->Bounds.Origin, FVector::XAxisVector, FVector::YAxisVector, 1.0, First, First, 1 - 2 * First, 1 - 2 * First, Foot);
					double HullVolume = 0.0;
					for (const float Head : Foot)
					{
						HullVolume += Head;   // 1 cm² cells
					}
					Fall.HullTimes.Add(World->GetTimeSeconds());
					Fall.HullCm3.Add(HullVolume);
				}
				// Before the lift: how fast it moves up or down, for its rest (-1 while it is held: a held body has no physics, and its zero
				// speed says nothing), and when it was first let go on its water, should that come after its settling phase.
				if (Fall.DropTime < 0.0 && Primitive && Body)
				{
					const bool bFree = !Body->IsAwaitingWater() && Primitive->IsSimulatingPhysics();
					Fall.RiseTimes.Add(World->GetTimeSeconds());
					Fall.RiseCmS.Add(bFree ? FMath::Abs(Primitive->GetPhysicsLinearVelocity().Z) : -1.0);
					if (bFree && Fall.FloatingTime < 0.0 && Fall.PlacedTime >= 0.0)
					{
						Fall.FloatingTime = World->GetTimeSeconds();
					}
				}
			};
			AddClear(Planned.bImpulse ? TEXT("fresh ripples, displacement and impulse") : TEXT("fresh ripples, displacement only"),
				[Index, EnsureWater, OtherBoxes, ChooseDirections](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
			{
				FDropRun& Fall = R.Ripple->Drops[Index];
				UFluidNinjaBuoyancyComponent* Body = Fall.Body.Get();
				IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
				const UFluidNinjaBuoyancySubsystem* Hydro = Bench.GetHydro();
				FHullShape Shape;
				if (!Body || !Layer || !Hydro || !MeasureHull(*Body, Shape))
				{
					return;
				}
				SetWaveSources(true, Fall.bImpulse);
				FFluidNinjaBuoyancyBodyDebug Debug;
				float Level = 0.f;
				if (Hydro->GetBodyDebug(Body, Debug) && Debug.bHasWaterZ)
				{
					Level = Debug.WaterZ;
				}
				else if (!Bench.BodiesWaterHeight(Level))
				{
					Level = static_cast<float>(Body->GetSimulatedPrimitive()->GetComponentLocation().Z);
				}
				Fall.WaterZ = Level;
				EnsureWater(Bench, R);
				const FWaterMap& Water = R.Ripple->Water;
				Fall.Centre = Shape.Centre;
				// Evers' slide axis (gamma = 0) across the hull's shorter side: the boat broadside, its beam the thickness s and its length
				// the width b.
				const bool bBeamShorter = Shape.Beam <= Shape.Length;
				Fall.Axis = FVector2D(bBeamShorter ? Shape.Side : Shape.Along).GetSafeNormal();
				Fall.ThicknessCm = bBeamShorter ? Shape.Beam : Shape.Length;
				Fall.WidthCm = bBeamShorter ? Shape.Length : Shape.Beam;
				Fall.HeightCm = Shape.Height;
				// The depth the core runs under the hull, and the fastest wave its window holds there.
				const FLinearColor Under = Water.CoreAt(Fall.Centre);
				Fall.DepthCm = Under.G;
				Fall.bDepthKnown = Under.R > 0.5f && Under.G > 0.f;
				Fall.FastestCmS = CoreFastestSpeed(Fall.DepthCm, Layer->GetWindowCm());
				// How far its rings go before they meet a wall, a shore or the window's sponge (the map stops where the sponge starts).
				Fall.ClearCm = Water.ClearRadius(Fall.Centre, Water.Size * Water.Step);
				Fall.ReachCm = 0.5 * FMath::Sqrt(FMath::Square(static_cast<double>(Shape.Length)) + FMath::Square(static_cast<double>(Shape.Beam)));
				const UFluidNinjaBuoyancyComponent* Leave[] = {Body};
				const TArray<FVector2D> Directions = ChooseDirections(Water, Fall.Centre, Fall.Axis, DropRadiiCm, OtherBoxes(Bench, Leave), 3);
				Fall.Log = FProbeLog();
				Fall.Radii.Reset();
				Fall.Gammas.Reset();
				for (const FVector2D& Direction : Directions)
				{
					const double Gamma = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FMath::Abs(Direction | Fall.Axis), 0.0, 1.0)));
					for (const double Radius : DropRadiiCm)
					{
						const FVector2D Point = Fall.Centre + Direction * Radius;
						if (Water.DepthAt(Point) >= OpenDepthCm)
						{
							Fall.Log.Points.Add(Point);
							Fall.Radii.Add(Radius);
							Fall.Gammas.Add(Gamma);
						}
					}
				}
				Layer->SetProbes(Fall.Log.Points);
				Layer->RequestStats(true);
			}, Log);
			// At rest before the lift: its static head at rest is read in the 0.6 s before it, and a body still bobbing is read at another
			// draft every frame. The fresh field's frames count, so a body already at rest goes on at once; one still moving is waited for
			// up to WeightSettleLimitSeconds, then dropped and judged anyway (its line says it was not settled).
			Made->Phases.Add({FString::Printf(TEXT("%s coming to rest"), *Planned.Name), WeightSettleLimitSeconds, 0.f, nullptr, nullptr, [Index, Log, JudgeRest](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
			{
				Log(Bench, R);
				FDropRun& Fall = R.Ripple->Drops[Index];
				const UWorld* World = Bench.GetWorld();
				if (World && Fall.SettledTime < 0.0 && JudgeRest(Fall, World->GetTimeSeconds()))
				{
					Fall.SettledTime = World->GetTimeSeconds();
					R.Phases[R.Phase].Seconds = 0.f;
				}
			}});
			// Dropped at the mass it settled at.
			Made->Phases.Add({FString::Printf(TEXT("%s dropped from 3 m"), *Planned.Name), 8.f, 0.f, [Index, DropBody](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
			{
				FDropRun& Fall = R.Ripple->Drops[Index];
				UFluidNinjaBuoyancyComponent* Body = Fall.Body.Get();
				UPrimitiveComponent* Primitive = Body ? Body->GetSimulatedPrimitive() : nullptr;
				const UWorld* World = Bench.GetWorld();
				if (!Primitive || !World)
				{
					return;
				}
				Fall.DroppedMassKg = Primitive->GetMass();
				DropBody(Bench, *Body);
				Fall.DropTime = World->GetTimeSeconds();
				Fall.LastBottom = Body->GetLowestPointZ();
				Fall.LastFallSpeed = 0.f;
				Fall.bImpacted = false;
			}, [Index](FFluidNinjaBuoyancyTestBench&, FRun& R)
			{
				FDropRun& Fall = R.Ripple->Drops[Index];
				const UFluidNinjaBuoyancyComponent* Body = Fall.Body.Get();
				const UPrimitiveComponent* Primitive = Body ? Body->GetSimulatedPrimitive() : nullptr;
				if (Primitive)
				{
					Fall.DisplacedLitres = Body->GetReadout().DisplacedLitres;
				}
			}, Log});
		}
		Made->Evaluate = [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			const double Density = UFluidNinjaBuoyancySettings::Get().WaterDensity * 1.0e-6;   // kg/cm³
			const UWorld* World = Bench.GetWorld();
			const double Gravity = World ? FMath::Max(-static_cast<double>(World->GetGravityZ()), 1.0) : 980.0;
			// The plan's ring-height check against Evers et al. (2019) is ripple-evers' (inside Table 3-3, before a reflection can arrive):
			// here the rings are read over the whole record, the pool's wall reflections and all, so they are reported beside the manual
			// only. On how many distance lines the drop was inside Table 3-3 at all, and why not on the others.
			int32 EversLines = 0;
			int32 EversInRange = 0;
			TArray<FString> EversNotJudged;
			for (const FDropRun& Fall : R.Ripple->Drops)
			{
				const FString Source = Fall.bImpulse ? TEXT("displacement and impulse") : TEXT("displacement only");
				if (Fall.DropTime < 0.0)
				{
					Bench.Report(R.Name, Fall.Name, 0, Source + TEXT(": not dropped (the body is gone)"));
					continue;
				}
				// The field before the drop and once the body has settled (its last second), and the most wave energy after it.
				double Before = 0.0;
				double BeforeEnergy = 0.0;
				int32 BeforeCount = 0;
				double After = 0.0;
				int32 AfterCount = 0;
				double Peak = 0.0;
				const double End = Fall.StatTimes.IsEmpty() ? 0.0 : Fall.StatTimes.Last();
				for (int32 Sample = 0; Sample < Fall.StatTimes.Num(); ++Sample)
				{
					const double Time = Fall.StatTimes[Sample];
					if (Time <= Fall.DropTime && Time >= Fall.DropTime - 0.6)
					{
						Before += Fall.Volumes[Sample];
						BeforeEnergy += Fall.Energies[Sample];
						++BeforeCount;
					}
					if (Time > Fall.DropTime && Time >= End - 1.0)
					{
						After += Fall.Volumes[Sample];
						++AfterCount;
					}
				}
				if (BeforeCount == 0 || AfterCount == 0)
				{
					Bench.Report(R.Name, Fall.Name, 0, Source + TEXT(": volume and energy not measured (no field statistics came back around the drop)"));
				}
				else
				{
					Before /= BeforeCount;
					BeforeEnergy /= BeforeCount;
					After /= AfterCount;
					for (int32 Sample = 0; Sample < Fall.StatTimes.Num(); ++Sample)
					{
						if (Fall.StatTimes[Sample] > Fall.DropTime)
						{
							Peak = FMath::Max(Peak, Fall.Energies[Sample] - BeforeEnergy);
						}
					}
					const double DropEnergy = Fall.DroppedMassKg * Gravity * DropMetres * 100.0 * 1.0e-4;   // kg·cm²/s² to J
					const double Share = DropEnergy > 0.0 ? Peak / DropEnergy : 0.0;
					// FluidNinja Waves export η + P and P, not ∂η/∂t: the energy read is the potential alone (about half a free wave's), so it is
					// reported, not judged.
					Bench.Report(R.Name, Fall.Name, 0,
						FString::Printf(TEXT("%.0f kg, %s: hit the water at %.2f m/s; wave energy (potential only, not judged: FluidNinja Waves export no surface speed) %.0f J, %.1f%% of m g h = %.0f J; depth %s"),
							Fall.DroppedMassKg, *Source, Fall.ImpactSpeed / 100.f, Peak, 100.0 * Share, DropEnergy,
							Fall.bDepthKnown ? *FString::Printf(TEXT("%.0f cm"), Fall.DepthCm) : TEXT("unknown")));

					// The plan's volume check, on the static head: the water the source says the hull pushes aside at rest, the window's
					// Σ P · cell area at rest (the 0.6 s before the lift, the body settled at the test's mass) less the same while it fell (it has
					// none in the air), from the same readback as the stats, against Archimedes: a body floating free at rest displaces its weight
					// in water, m / ρ (cm³, for a Buoyancy Multiplier of 1), whatever level the water stands at, and the source rasters the head
					// under the body's own water level every frame. Beside it, information only: the wave hull's triangles clipped at 1 cm on the
					// CPU at Fall.WaterZ, with that level. Fall.WaterZ is one frame's reading, taken as the waves were cleared, and every mm it is
					// off moves the clipped volume by the hull's waterplane area (the 80 cm crate: 0.64 L per mm, so 0.6 mm is the 2% of the
					// 20 kg crate's 20 L): on 2026-10-18 the 20 kg crate's static head held 20.03 L, its readings spread over 0.15 L, against
					// 17.85 L clipped so (the 2.18 L between them would be 3.4 mm of level on its waterplane; a different pose between the clip and
					// the raster would add to it). A body resting on or leaning against something displaces less than
					// its weight, and fails here. Also information: the physics hull's own reading.
					const auto MeanIn = [](const TArray<double>& Times, const TArray<double>& Values, double From, double To, int32& OutCount)
					{
						double Sum = 0.0;
						OutCount = 0;
						for (int32 Sample = 0; Sample < Times.Num() && Sample < Values.Num(); ++Sample)
						{
							if (Times[Sample] >= From && Times[Sample] <= To)
							{
								Sum += Values[Sample];
								++OutCount;
							}
						}
						return OutCount > 0 ? Sum / OutCount : 0.0;
					};
					const double Archimedes = Fall.DroppedMassKg / FMath::Max(Density, 1.0e-12);   // cm³
					int32 RestCount = 0;
					int32 AirCount = 0;
					int32 EndCount = 0;
					int32 HullCount = 0;
					const double HeadRest = MeanIn(Fall.SourceTimes, Fall.SourceCm3, Fall.DropTime - 0.6, Fall.DropTime, RestCount);
					const double HeadAir = Fall.bImpacted ? MeanIn(Fall.SourceTimes, Fall.SourceCm3, Fall.DropTime + 0.15, Fall.ImpactTime, AirCount) : 0.0;
					const double LastSource = Fall.SourceTimes.IsEmpty() ? 0.0 : Fall.SourceTimes.Last();
					const double HeadEnd = MeanIn(Fall.SourceTimes, Fall.SourceCm3, LastSource - 1.0, LastSource, EndCount);
					const double HullRest = MeanIn(Fall.DisplacedTimes, Fall.DisplacedCm3, Fall.DropTime - 0.6, Fall.DropTime, HullCount);
					if (RestCount == 0 || AirCount == 0 || Archimedes <= 0.0)
					{
						Bench.Report(R.Name, Fall.Name, 0, FString::Printf(TEXT("%s, static head: not measured (the window's static head did not come back both at rest and while the body fell: %d and %d readings)"),
							*Source, RestCount, AirCount));
					}
					else
					{
						const double Head = HeadRest - HeadAir;
						const double Miss = (Head - Archimedes) / Archimedes;
						// Information: the wave hull's volume clipped at Fall.WaterZ over the same 0.6 s, and how far the static head is from it.
						int32 HullClippedCount = 0;
						const double HullClipped = MeanIn(Fall.HullTimes, Fall.HullCm3, Fall.DropTime - 0.6, Fall.DropTime, HullClippedCount);
						const FString ClippedText = HullClippedCount > 0 && HullClipped > 0.0
							? FString::Printf(TEXT("%.2f L (the static head %+.2f%% from it)"), HullClipped / 1000.0, 100.0 * (Head - HullClipped) / HullClipped)
							: FString(TEXT("no volume (no frame of it was clipped in the 0.6 s before the lift, or none of it was under that level)"));
						// How much the window's static head wandered within each of the two windows (the body's own bobbing: every other body is
						// lifted with its Waves off), beside the miss.
						const auto Spread = [&Fall](double From, double To)
						{
							double Least = TNumericLimits<double>::Max();
							double Most = -TNumericLimits<double>::Max();
							for (int32 Sample = 0; Sample < Fall.SourceTimes.Num() && Sample < Fall.SourceCm3.Num(); ++Sample)
							{
								if (Fall.SourceTimes[Sample] >= From && Fall.SourceTimes[Sample] <= To)
								{
									Least = FMath::Min(Least, Fall.SourceCm3[Sample]);
									Most = FMath::Max(Most, Fall.SourceCm3[Sample]);
								}
							}
							return Most >= Least ? Most - Least : 0.0;
						};
						// Whether it was at rest when it was read (the coming-to-rest phase before the lift), and how long that took from when it
						// was let go on its water. Judged either way, so a body that never comes to rest still shows a regression in its miss.
						FString RestText;
						if (!Fall.bSettleJudged)
						{
							RestText = TEXT("its rest was not judged (no frame of it was followed before the lift)");
						}
						else
						{
							const FString FastestText = Fall.SettleFastestCmS < 0.0 ? FString(TEXT("it was still held (no physics to read)"))
								: FString::Printf(TEXT("it moved up or down at most %.2f cm/s (limit %.1f)"), Fall.SettleFastestCmS, WeightSettledCmS);
							const FString SpreadText = FString::Printf(TEXT("its static head's %d readings spread over %.2f L (limit %.2f L, %.0f%% of the %.2f L its wave hull had under the water; at least %d readings)"),
								Fall.SettleReadings, Fall.SettleSpreadCm3 / 1000.0, WeightSettledSpread * Fall.SettleHullCm3 / 1000.0, 100.0 * WeightSettledSpread,
								Fall.SettleHullCm3 / 1000.0, WeightSettledReadings);
							const FString FloatedText = Fall.FloatingTime >= 0.0
								? FString::Printf(TEXT("let go on its water %.1f s after it was put back"), Fall.FloatingTime - Fall.PlacedTime)
								: FString(TEXT("never seen let go on its water"));
							// The settle time is when both conditions began to hold without a break (looked back from the judgement), counted
							// from when it was let go on its water. The check itself only runs once a second of the fresh field's static-head
							// readings exists, so it is judged later than that; and the static head is followed only from the fresh field on,
							// so a steadiness dated to its first reading may have begun earlier.
							const bool bFloated = Fall.FloatingTime >= 0.0;
							const double Base = bFloated ? Fall.FloatingTime : Fall.PlacedTime;
							const TCHAR* BaseName = bFloated ? TEXT("let go on its water") : TEXT("put back");
							const double FirstHead = Fall.SourceTimes.IsEmpty() ? Base : Fall.SourceTimes[0];
							const FString RunText = FString::Printf(TEXT("slower than %.1f cm/s from %.1f s and its static head within the limit from %.1f s after it was %s, the static head followed only from %.1f s"),
								WeightSettledCmS, Fall.StillSince - Base, Fall.SteadySince - Base, BaseName, FirstHead - Base);
							RestText = Fall.SettledTime >= 0.0
								? FString::Printf(TEXT("settled %.1f s after it was %s (%s; %s), judged at %.1f s once a whole second of it had been watched: over its last %.0f s %s and %s"),
									FMath::Max(Fall.StillSince, Fall.SteadySince) - Base, BaseName, *FloatedText, *RunText, Fall.SettledTime - Base, WeightSettledSeconds,
									*FastestText, *SpreadText)
								: FString::Printf(TEXT("not settled, read while it still moved: not at rest %.0f s after the fresh field's 2 s (the limit; %s; %s): over its last %.0f s %s and %s"),
									WeightSettleLimitSeconds, *FloatedText, *RunText, WeightSettledSeconds, *FastestText, *SpreadText);
						}
						Bench.Report(R.Name, Fall.Name, FMath::Abs(Miss) <= SourceVolumeTolerance ? 1 : -1,
							FString::Printf(TEXT("%s, static head: %s; at rest the static head under it held %.2f L (the window's sum of P times the cell area at rest, %d readings spread over %.2f L, less while it fell, %d readings spread over %.2f L) against its weight in water, m / rho = %.2f L for %.2f kg (what a body floating free at rest displaces, whatever level the water stands at): %+.2f%% (pass: within %.0f%%). Information: its wave hull had %s under the water clipped at %.2f cm (one frame's reading of the level, taken as the waves were cleared; its triangles clipped at 1 cm; every mm that level is off moves this by the hull's waterplane area), and its physics hull read %.2f L at rest; in its last second after the drop the static head held %.2f L"),
								*Source, *RestText, Head / 1000.0, RestCount, Spread(Fall.DropTime - 0.6, Fall.DropTime) / 1000.0, AirCount, Spread(Fall.DropTime + 0.15, Fall.ImpactTime) / 1000.0,
								Archimedes / 1000.0, Fall.DroppedMassKg, 100.0 * Miss, 100.0 * SourceVolumeTolerance, *ClippedText, Fall.WaterZ,
								HullCount > 0 ? HullRest / 1000.0 : 0.0, EndCount > 0 ? (HeadEnd - HeadAir) / 1000.0 : 0.0));
					}
					// The field's own volume, the sum of η = B − A times the cell area. The lift erases the hollow the body sat in: η = −P there
					// becomes 0, so the field gains m / ρ, while s = η + P stays 0 there, so the core's mean relax (it acts on the mean of s) takes
					// none of it off during the fall. As the body comes back in, its static head raises the mean of s by m / ρ (Archimedes' rise of
					// the level, which a walled window would keep), and the core's mean relax at r takes that rise off as e^(−r t), t from the
					// impact, so the gain heads back to 0, the field's volume at rest. The gain expected is therefore (m / ρ) e^(−r t) (m / ρ
					// throughout at r = 0, or with no impact), read early, before the fastest wave the window holds reaches the nearest wall, shore
					// or sponge, and over the last second (the expectation averaged over the same readings). r is the core's effective rate as the
					// waves' status line gives it for this window (CoreMeanRelaxOf; the bench's focus is still on the drop's simulation here).
					// Information: the expectation takes the head as whole from the impact (a body still plunging or bobbing presses more or less),
					// and the core's operator is not conservative at walls, nor are its wall band and breaking.
					double CoreRelax = -1.0;
					if (const IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves())
					{
						const double Cell = Layer->GetCellCm();
						CoreRelax = CoreMeanRelaxOf(Layer->GetStatusLine(), FMath::RoundToInt32(Layer->GetWindowCm() / FMath::Max(Cell, 1.0e-3)), Cell);
					}
					const bool bExpected = CoreRelax >= 0.0;
					const auto ExpectedGain = [&Fall, Archimedes, CoreRelax](double Time)
					{
						return Fall.bImpacted ? Archimedes * FMath::Exp(-FMath::Max(CoreRelax, 0.0) * FMath::Max(Time - Fall.ImpactTime, 0.0)) : Archimedes;
					};
					double ExpectedAfter = 0.0;
					int32 ExpectedCount = 0;
					for (const double Time : Fall.StatTimes)
					{
						if (Time > Fall.DropTime && Time >= End - 1.0)   // the readings After averages
						{
							ExpectedAfter += ExpectedGain(Time);
							++ExpectedCount;
						}
					}
					ExpectedAfter = ExpectedCount > 0 ? ExpectedAfter / ExpectedCount : 0.0;
					const double FrontSeconds = Fall.FastestCmS > 0.0 ? FMath::Max(Fall.ClearCm - Fall.ReachCm, 0.0) / Fall.FastestCmS : 0.0;
					int32 Early = INDEX_NONE;
					for (int32 Sample = 0; Sample < Fall.StatTimes.Num(); ++Sample)
					{
						if (Fall.bImpacted && Fall.StatTimes[Sample] > Fall.ImpactTime && Fall.StatTimes[Sample] <= Fall.ImpactTime + FrontSeconds)
						{
							Early = Sample;   // the last reading inside the window
						}
					}
					const FString EarlyText = Early == INDEX_NONE
						? FString::Printf(TEXT("no reading came back before its fastest waves (%.1f m/s at the core's %.0f cm) reached the nearest wall, shore or sponge %.1f m from its edge (%.2f s)"),
							Fall.FastestCmS / 100.0, Fall.DepthCm, FMath::Max(Fall.ClearCm - Fall.ReachCm, 0.0) / 100.0, FrontSeconds)
						: FString::Printf(TEXT("%+.2f L %.2f s after the impact%s, before its fastest waves (%.1f m/s at the core's %.0f cm) reached the nearest wall, shore or sponge %.1f m from its edge (%.2f s)"),
							(Fall.Volumes[Early] - Before) / 1000.0, Fall.StatTimes[Early] - Fall.ImpactTime,
							bExpected ? *FString::Printf(TEXT(" (expected %+.2f L)"), ExpectedGain(Fall.StatTimes[Early]) / 1000.0) : TEXT(""),
							Fall.FastestCmS / 100.0, Fall.DepthCm, FMath::Max(Fall.ClearCm - Fall.ReachCm, 0.0) / 100.0, FrontSeconds);
					const FString AgainstText = bExpected
						? FString::Printf(TEXT("against (m / rho) e^(-r t) from the impact, m / rho = %.2f L and r = %.2f /s the core's mean relax"), Archimedes / 1000.0, CoreRelax)
						: FString::Printf(TEXT("beside m / rho = %.2f L, with no expectation: the waves' status line gives no core mean relax for this window"), Archimedes / 1000.0);
					Bench.Report(R.Name, Fall.Name, 0, FString::Printf(TEXT("%s, field volume: the field's own volume rose %+.2f L in its last second%s and %s, %s (information: the lift's erase fills the body's hollow; its static head coming back in raises the mean of eta + P by m / rho, Archimedes' rise, which the core's mean relax takes off at r, so the field heads back to its volume at rest; the expectation takes the head as whole from the impact, and the core's masked operator at walls, its wall band and breaking do not keep the volume exactly)"),
						*Source, (After - Before) / 1000.0, bExpected ? *FString::Printf(TEXT(" (expected %+.2f L)"), ExpectedAfter / 1000.0) : TEXT(""), *EarlyText, *AgainstText));
				}

				// The rings, by distance: per direction the first crest and trough (and the largest), beside Evers et al. (2019), read as the
				// image slide an open-water drop is (FluidNinjaEvers3D::ImageSlide: half the hull down a reflecting wall at its centre), as ripple-evers
				// judges it.
				FluidNinjaEvers3D::FParams Prm;
				const bool bEvers = Fall.bDepthKnown && Fall.bImpacted && Fall.WidthCm > 0.f && Fall.ThicknessCm > 0.f;
				if (bEvers)
				{
					FluidNinjaEvers3D::FInput In;
					In.SlideVelocity = Fall.ImpactSpeed;
					In.SlideThickness = Fall.ThicknessCm;
					In.SlideWidth = Fall.WidthCm;
					In.SlideMass = Fall.DroppedMassKg;
					In.SlideVolume = static_cast<double>(Fall.ThicknessCm) * Fall.WidthCm * Fall.HeightCm;   // the hull's box: V and D checks only
					In.ImpactAngleDeg = 90.0;
					In.WaterDepth = Fall.DepthCm;
					In.WaterDensity = Density;
					In.Gravity = Gravity;
					Prm = FluidNinjaEvers3D::ComputeParams(FluidNinjaEvers3D::ImageSlide(In));
				}
				// The limits that kept this drop's lines outside Table 3-3 (each probe's, the drop's own among them).
				uint32 NotJudgedFlags = 0;
				bool bAnyNotJudged = false;
				for (const double Radius : DropRadiiCm)
				{
					FString Line;
					bool bAllInRange = bEvers;
					bool bAllWithin = true;
					uint32 LineFlags = 0;
					int32 Probes = 0;
					for (int32 Probe = 0; Probe < Fall.Radii.Num(); ++Probe)
					{
						if (!FMath::IsNearlyEqual(Fall.Radii[Probe], Radius))
						{
							continue;
						}
						// The probe's record after the drop, less its level before.
						double Level = 0.0;
						int32 LevelCount = 0;
						TArray<double> Z;
						for (int32 Reading = 0; Reading < Fall.Log.Times.Num(); ++Reading)
						{
							const double Time = Fall.Log.Times[Reading];
							const double Height = Fall.Log.Heights[Reading].IsValidIndex(Probe) ? Fall.Log.Heights[Reading][Probe] : 0.0;
							if (Time <= Fall.DropTime && Time >= Fall.DropTime - 1.0)
							{
								Level += Height;
								++LevelCount;
							}
							else if (Time > Fall.DropTime)
							{
								Z.Add(Height);
							}
						}
						const double Rest = LevelCount > 0 ? Level / LevelCount : 0.0;
						for (double& Value : Z)
						{
							Value -= Rest;
						}
						const FLobes Lobes = MeasureLobes(Z);
						++Probes;
						Line += FString::Printf(TEXT("%sgamma %.0f: crest %.2f trough %.2f H %.2f (largest %.2f/%.2f)"), Line.IsEmpty() ? TEXT("") : TEXT("; "), Fall.Gammas[Probe],
							Lobes.FirstCrest, Lobes.FirstTrough, Lobes.FirstCrest + Lobes.FirstTrough, Lobes.MaxCrest, Lobes.MaxTrough);
						if (!bEvers)
						{
							continue;
						}
						const FluidNinjaEvers3D::FWave Wave = FluidNinjaEvers3D::Predict(Prm, Fall.WidthCm, Fall.DepthCm, Gravity, Radius, Fall.Gammas[Probe]);
						const uint32 Flags = FluidNinjaEvers3D::CheckRanges(Prm, 90.0, Radius / Fall.DepthCm, Fall.Gammas[Probe], Wave.bInsideImpactZone);
						bAllInRange &= Flags == FluidNinjaEvers3D::Range_None;
						LineFlags |= Flags;
						if (Wave.bInsideImpactZone)
						{
							Line += FString::Printf(TEXT(" | Evers: inside the impact zone (r0 %.0f cm), no prediction"), Wave.R0);
							continue;
						}
						Line += FString::Printf(TEXT(" | Evers (image slide) a_c1 %.2f (%.2f-%.2f) a_t1 %.2f (%.2f-%.2f) H1 %.2f a_c2 %.2f"), Wave.AC1,
							Wave.AC1 * (1.0 - FluidNinjaEvers3D::CrestBelow), Wave.AC1 * (1.0 + FluidNinjaEvers3D::CrestAbove), Wave.AT1, Wave.AT1 * (1.0 - FluidNinjaEvers3D::TroughBelow),
							Wave.AT1 * (1.0 + FluidNinjaEvers3D::TroughAbove), Wave.H1, Wave.AC2);
						bAllWithin &= Lobes.FirstCrest >= Wave.AC1 * (1.0 - FluidNinjaEvers3D::CrestBelow) && Lobes.FirstCrest <= Wave.AC1 * (1.0 + FluidNinjaEvers3D::CrestAbove)
							&& Lobes.FirstTrough >= Wave.AT1 * (1.0 - FluidNinjaEvers3D::TroughBelow) && Lobes.FirstTrough <= Wave.AT1 * (1.0 + FluidNinjaEvers3D::TroughAbove);
						if (Flags != FluidNinjaEvers3D::Range_None)
						{
							Line += TEXT(" [out of range: ") + FluidNinjaEvers3D::RangeNames(Flags) + TEXT("]");
						}
					}
					if (Probes == 0)
					{
						Bench.Report(R.Name, FString::Printf(TEXT("%s r %.0f m"), *Fall.Name, Radius / 100.0), 0, Source + TEXT(": no probe over open water at this distance"));
						continue;
					}
					// Information only, inside Table 3-3 or not: these crests and troughs are read over the whole record, so a wall's
					// reflection can be in them (ripple-evers judges Evers, reading each only before a reflection can arrive). Outside the
					// table the scatter may be larger too, so that is an extrapolation.
					const FString Note = !bEvers ? FString(TEXT(" (no Evers prediction: the depth or the impact speed is unknown)"))
						: bAllInRange ? FString::Printf(TEXT(" (inside Table 3-3 as its image slide; first crests and troughs %s the scatter of Table 4-5; information only: read over the whole record, reflections and all, so ripple-evers judges this)"),
							bAllWithin ? TEXT("within") : TEXT("NOT all within"))
						: FString(TEXT(" (extrapolated beyond Table 3-3: information only)"));
					Bench.Report(R.Name, FString::Printf(TEXT("%s r %.0f m"), *Fall.Name, Radius / 100.0), 0,
						FString::Printf(TEXT("%s, cm: %s%s"), *Source, *Line, *Note));
					++EversLines;
					if (bEvers && bAllInRange)
					{
						++EversInRange;
					}
					else
					{
						bAnyNotJudged = true;
						NotJudgedFlags |= LineFlags;
					}
				}
				if (bEvers)
				{
					// Table 3-3's limits on the drop itself (checked at a distance and angle inside the table, so only the slide's and the
					// water's own show): a drop outside any of them has no ring line inside the table at all.
					const uint32 DropFlags = FluidNinjaEvers3D::CheckRanges(Prm, 90.0, 1.0, 0.0, false);
					const FString Limits = DropFlags == FluidNinjaEvers3D::Range_None ? FString(TEXT("inside every limit of the drop itself"))
						: TEXT("outside ") + FluidNinjaEvers3D::RangeNames(DropFlags);
					Bench.Report(R.Name, Fall.Name, 0, FString::Printf(TEXT("%s: Evers inputs h %.0f cm, V_s %.0f cm/s, s %.0f cm, b %.0f cm, m %.0f kg, read as its image slide (s/2, m/2 and the box's Vol/2, the same b, down a wall at its centre): F %.2f, S %.2f, M %.3f, B %.2f, V %.3f, D %.2f, P %.3f; Table 3-3: %s (information only; ripple-evers judges Evers)"),
						*Source, Fall.DepthCm, Fall.ImpactSpeed, Fall.ThicknessCm, Fall.WidthCm, Fall.DroppedMassKg, Prm.F, Prm.S, Prm.M, Prm.B, Prm.V, Prm.D, Prm.P, *Limits));
				}
				if (bAnyNotJudged)
				{
					EversNotJudged.AddUnique(!bEvers ? Fall.Name + TEXT(": no prediction (the depth or the impact speed is unknown)")
						: Fall.Name + TEXT(": outside ") + FluidNinjaEvers3D::RangeNames(NotJudgedFlags));
				}
			}
			// The plan's Evers condition in one line: it is judged by ripple-evers, not here (these rings are read over the whole record,
			// reflections and all), and on how many distance lines these drops were inside Table 3-3 at all, and for each drop that was not,
			// why (the per-distance INFO lines must not stand in for the check).
			if (EversLines > 0)
			{
				const FString Why = FString::Join(EversNotJudged, TEXT("; "));
				Bench.Report(R.Name, TEXT("Evers (2019)"), 0, EversInRange > 0
					? FString::Printf(TEXT("NOT JUDGED here: the plan's pass condition, ring heights within the scatter of Evers et al. (2019), is judged by ripple-evers, which reads each crest and trough before a reflection can arrive; these rings are read over the whole record, the pool's reflections and all. %d of %d distance lines were inside Table 3-3 as image slides (reported only)%s%s"),
						EversInRange, EversLines, Why.IsEmpty() ? TEXT("") : TEXT("; outside Table 3-3: "), *Why)
					: FString::Printf(TEXT("NOT JUDGED here: the plan's pass condition, ring heights within the scatter of Evers et al. (2019), is judged by ripple-evers, inside the equations' tested range (Table 3-3) and before a reflection can arrive; none of these %d distance lines lies inside the table either, so their heights are reported only. Per drop: %s"),
						EversLines, *Why));
			}
			else if (!R.Ripple->Drops.IsEmpty())
			{
				Bench.Report(R.Name, TEXT("Evers (2019)"), 0, TEXT("NOT JUDGED here: the plan's pass condition, ring heights within the scatter of Evers et al. (2019), is judged by ripple-evers; these drops had no rings read over open water to report beside it (no drop, or no probe over open water)"));
			}
		};
	}
	else if (Name == TEXT("ripple-rest"))
	{
		// The row boat at rest on the calmed live pool at each κ in turn (the Wave Feel Scale setting, written for the run): 0, feeling no waves, reported;
		// then 1, feeling the others' waves with its own taken out, judged (the self-coupling fix must keep it from building waves).
		UFluidNinjaBuoyancyComponent* RestBoat = FindTestBody(TEXT("Boat"));
		for (const float Lid : RestCouplings)
		{
			FRestRun& Rest = Made->Ripple->Rests.AddDefaulted_GetRef();
			Rest.Boat = RestBoat;
			Rest.Coupling = Lid;
		}
		Made->Phases.Add({TEXT("calming the pool"), 0.1f, 0.f, [Begin, Abort, LivePool, LiftOthers](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			if (!Begin(Bench, R))
			{
				return;
			}
			if (R.Ripple->Rests.IsEmpty() || !R.Ripple->Rests[0].Boat.IsValid())
			{
				Abort(Bench, R, TEXT("no row boat among the floating bodies"));
				return;
			}
			R.bResetAfter = true;
			R.Phases[R.Phase].Seconds = LivePool(Bench);
			// The boat alone: the waves at its probes are its own (the others' bobbing is not the self-coupling's).
			const UFluidNinjaBuoyancyComponent* Boat = R.Ripple->Rests[0].Boat.Get();
			LiftOthers(Bench, R, [Boat](const UFluidNinjaBuoyancyComponent& Body) { return &Body == Boat; });
		}, nullptr});
		for (int32 Index = 0; Index < Made->Ripple->Rests.Num(); ++Index)
		{
			const float Lid = Made->Ripple->Rests[Index].Coupling;
			Made->Phases.Add({TEXT("row boat put back"), 4.f, 0.f, [Index](FFluidNinjaBuoyancyTestBench&, FRun& R)
			{
				if (UFluidNinjaBuoyancyComponent* Boat = R.Ripple->Rests[Index].Boat.Get())
				{
					Boat->RestartAt(Boat->GetStartTransform());
				}
			}, nullptr});
			// The field starts flat with this run's coupling.
			AddClear(*FString::Printf(TEXT("fresh waves, κ %.0f"), Lid), [Index, Focus, EnsureWater, NoWater, Abort](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
			{
				FRestRun& Rest = R.Ripple->Rests[Index];
				SetWaveFeel(Rest.Coupling);   // κ: how much of the others' waves the boat feels (its own taken out)
				UFluidNinjaBuoyancyComponent* Boat = Rest.Boat.Get();
				IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
				FHullShape Shape;
				if (!Boat || !Layer || !MeasureHull(*Boat, Shape))
				{
					Abort(Bench, R, TEXT("the row boat could not be found"));
					return;
				}
				Focus(Bench, R, Shape.Centre);
				if (!EnsureWater(Bench, R))
				{
					Abort(Bench, R, TEXT("no water to read around the row boat: ") + NoWater(Bench));
					return;
				}
				// Eight probes a metre off its hull, over open water.
				Rest.RingCm = 0.5 * FMath::Max(Shape.Length, Shape.Beam) + 100.0;
				Rest.Log = FProbeLog();
				for (int32 Turn = 0; Turn < 8; ++Turn)
				{
					const FVector2D Point = Shape.Centre + FVector2D(1.0, 0.0).GetRotated(Turn * 45.0) * Rest.RingCm;
					if (R.Ripple->Water.DepthAt(Point) >= OpenDepthCm)
					{
						Rest.Log.Points.Add(Point);
					}
				}
				if (Rest.Log.Points.IsEmpty())
				{
					Abort(Bench, R, TEXT("no open water around the row boat"));
					return;
				}
				SetWaveSources(R.bWasDisplacement, R.bWasImpulse);   // the sources as the run found them
				Layer->SetProbes(Rest.Log.Points);
			}, nullptr);
			Made->Phases.Add({FString::Printf(TEXT("row boat at rest, κ %.0f"), Lid), 60.f, 0.f, nullptr, nullptr, [Index](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
			{
				FRestRun& Rest = R.Ripple->Rests[Index];
				const IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
				const UWorld* World = Bench.GetWorld();
				const UFluidNinjaBuoyancyComponent* Boat = Rest.Boat.Get();
				const UPrimitiveComponent* Primitive = Boat ? Boat->GetSimulatedPrimitive() : nullptr;
				if (Layer)
				{
					Rest.Log.Take(*Layer);
				}
				if (World && Primitive)
				{
					Rest.ZTimes.Add(World->GetTimeSeconds());
					Rest.Z.Add(Primitive->GetComponentLocation().Z);
				}
			}});
		}
		Made->Evaluate = [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			for (int32 Index = 0; Index < R.Ripple->Rests.Num(); ++Index)
			{
				const FRestRun& Rest = R.Ripple->Rests[Index];
				// The κ 1 run (the default: the boat feels the others' waves with its own taken out) is the judged one, as it is the one that
				// tests FluidNinja Waves' self-coupling fix; κ 0 (it feels none, so nothing can build up through it) is reported beside it.
				const bool bJudged = Rest.Coupling >= 0.5f;
				const FString Who = FString::Printf(TEXT("%s, κ %.0f"), Rest.Boat.IsValid() ? *Bench.BodyName(Rest.Boat.Get()) : TEXT("row boat"), Rest.Coupling);
				if (Rest.Log.Times.Num() < 10 || Rest.Z.Num() < 10)
				{
					Bench.Report(R.Name, Who, 0, FString::Printf(TEXT("not measured: %d probe readings, %d heights"), Rest.Log.Times.Num(), Rest.Z.Num()));
					continue;
				}
				const double First = Rest.Log.Times[0];
				const double Last = Rest.Log.Times.Last();
				double Sum = 0.0;
				int32 Count = 0;
				double Early = 0.0;
				int32 EarlyCount = 0;
				double Late = 0.0;
				int32 LateCount = 0;
				double Largest = 0.0;
				TArray<double> PerProbe;
				PerProbe.Init(0.0, Rest.Log.Points.Num());
				for (int32 Reading = 0; Reading < Rest.Log.Times.Num(); ++Reading)
				{
					const double Time = Rest.Log.Times[Reading];
					for (int32 Probe = 0; Probe < Rest.Log.Heights[Reading].Num() && Probe < PerProbe.Num(); ++Probe)
					{
						const double Squared = FMath::Square(static_cast<double>(Rest.Log.Heights[Reading][Probe]));
						Sum += Squared;
						++Count;
						PerProbe[Probe] += Squared;
						Largest = FMath::Max(Largest, FMath::Abs(static_cast<double>(Rest.Log.Heights[Reading][Probe])));
						if (Time < First + 10.0)
						{
							Early += Squared;
							++EarlyCount;
						}
						if (Time > Last - 10.0)
						{
							Late += Squared;
							++LateCount;
						}
					}
				}
				double Worst = 0.0;
				for (const double ProbeSum : PerProbe)
				{
					Worst = FMath::Max(Worst, FMath::Sqrt(ProbeSum / Rest.Log.Times.Num()));
				}
				const double Rms = Count > 0 ? FMath::Sqrt(Sum / Count) : 0.0;
				const double EarlyRms = EarlyCount > 0 ? FMath::Sqrt(Early / EarlyCount) : 0.0;
				const double LateRms = LateCount > 0 ? FMath::Sqrt(Late / LateCount) : 0.0;
				// How far it floats from where it began (the last 5 s against the first 5).
				double StartZ = 0.0;
				int32 StartCount = 0;
				double EndZ = 0.0;
				int32 EndCount = 0;
				for (int32 Sample = 0; Sample < Rest.Z.Num(); ++Sample)
				{
					if (Rest.ZTimes[Sample] < Rest.ZTimes[0] + 5.0)
					{
						StartZ += Rest.Z[Sample];
						++StartCount;
					}
					if (Rest.ZTimes[Sample] > Rest.ZTimes.Last() - 5.0)
					{
						EndZ += Rest.Z[Sample];
						++EndCount;
					}
				}
				const double Drift = (EndCount > 0 ? EndZ / EndCount : 0.0) - (StartCount > 0 ? StartZ / StartCount : 0.0);
				const FString Text = FString::Printf(TEXT("ripple RMS %.3f cm at %d probes %.0f cm from its centre over %.0f s (worst probe %.3f; first 10 s %.3f, last 10 s %.3f), largest |z| %.3f cm; floated %+.2f cm (last 5 s against the first)"),
					Rms, Rest.Log.Points.Num(), Rest.RingCm, Last - First, Worst, EarlyRms, LateRms, Largest, Drift);
				if (bJudged)
				{
					// A hull at rest on still water makes no waves: whatever its restart left must die away, not grow. So the last 10 s may be no
					// louder than the first 10 s (or a 0.2 mm floor), must have come down under 1 mm, and the boat must not float up or down.
					const bool bPass = LateRms <= FMath::Max(EarlyRms, 0.02) && LateRms < 0.1 && FMath::Abs(Drift) < 0.5;
					Bench.Report(R.Name, Who, bPass ? 1 : -1, Text + TEXT(" (pass: no build-up, the last 10 s no louder than the first 10 s or 0.02 cm, and under 0.1 cm; drift under 0.5 cm)"));
				}
				else
				{
					// Built up: louder at the end than at the start (twice, and past a millimetre), or past the judged run's limit.
					const bool bBuilt = LateRms > 2.0 * FMath::Max(EarlyRms, 0.01);
					Bench.Report(R.Name, Who, 0, Text + FString::Printf(TEXT("; %s (reported, not judged: at κ 0 the boat feels no waves, so none can build up through it)"),
						bBuilt ? TEXT("it built up") : TEXT("it did not build up within the run")));
				}
			}
		};
	}
	else if (Name == TEXT("ripple-coupling"))
	{
		Made->Ripple->Coupling.Boat = FindTestBody(TEXT("Boat"));
		Made->Ripple->Coupling.Ball = FindTestBody(TEXT("Ball"));
		Made->Phases.Add({TEXT("calming the pool"), 0.1f, 0.f, [Begin, Abort, LivePool, LiftOthers](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			if (!Begin(Bench, R))
			{
				return;
			}
			const UFluidNinjaBuoyancyComponent* Boat = R.Ripple->Coupling.Boat.Get();
			const UFluidNinjaBuoyancyComponent* Ball = R.Ripple->Coupling.Ball.Get();
			if (!Boat || !Ball)
			{
				Abort(Bench, R, TEXT("it needs a row boat and a beach ball among the floating bodies"));
				return;
			}
			R.bResetAfter = true;
			// Every other body out of the way: the waves they make and scatter would reach the ball too.
			LiftOthers(Bench, R, [Boat, Ball](const UFluidNinjaBuoyancyComponent& Body) { return &Body == Boat || &Body == Ball; });
			R.Phases[R.Phase].Seconds = LivePool(Bench);
		}, nullptr});
		Made->Phases.Add({TEXT("row boat put back"), 4.f, 0.f, [](FFluidNinjaBuoyancyTestBench&, FRun& R)
		{
			if (UFluidNinjaBuoyancyComponent* Boat = R.Ripple->Coupling.Boat.Get())
			{
				Boat->RestartAt(Boat->GetStartTransform());
			}
		}, nullptr});
		// The ball 3 m from the boat's centre at a fixed oblique heading, 45 degrees from the hull's side axis, over open water. The shared
		// choice (ChooseDirections, which ripple-weight keeps) ranks headings by their distance to the other bodies, which LiftOthers has
		// moved only in height, from wherever the test before left them: suite and standalone runs could place the ball at different
		// headings, and the ring that reaches it changes about threefold from end-on to broadside. Here the lifted bodies (physics off) are
		// left out and the heading comes first: of the sixteen headings every 22.5 degrees, over open water, the nearest to 45 degrees from
		// the side axis, then the furthest from a body still floating, then the furthest from a wall or shore (in the water map's steps, so a
		// hull settling a millimetre elsewhere does not flip it), then the first in turn. A wall never ranks above the heading: that could
		// pick broadside along a near wall.
		Made->Phases.Add({TEXT("beach ball 3 m away"), 4.f, 0.f, [Focus, EnsureWater, NoWater, Abort](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			FCouplingRun& Couple = R.Ripple->Coupling;
			UFluidNinjaBuoyancyComponent* Boat = Couple.Boat.Get();
			UFluidNinjaBuoyancyComponent* Ball = Couple.Ball.Get();
			const UPrimitiveComponent* BallPrimitive = Ball ? Ball->GetSimulatedPrimitive() : nullptr;
			FHullShape Shape;
			if (!Boat || !BallPrimitive || !MeasureHull(*Boat, Shape))
			{
				Abort(Bench, R, TEXT("the row boat or the ball could not be found"));
				return;
			}
			Focus(Bench, R, Shape.Centre);
			if (!EnsureWater(Bench, R))
			{
				Abort(Bench, R, TEXT("no water to read around the row boat: ") + NoWater(Bench));
				return;
			}
			const FWaterMap& Water = R.Ripple->Water;
			// The other bodies still floating (physics on); those LiftOthers lifted are out of the water and out of the choice.
			TArray<FBox2D> Floating;
			FString FloatingText;
			int32 Lifted = 0;
			if (const UFluidNinjaBuoyancySubsystem* Hydro = Bench.GetHydro())
			{
				for (const UFluidNinjaBuoyancyComponent* Other : Hydro->GetBodies())
				{
					const UPrimitiveComponent* Primitive = Other->GetSimulatedPrimitive();
					if (!Primitive || Other == Boat || Other == Ball)
					{
						continue;
					}
					if (!Primitive->IsSimulatingPhysics())
					{
						++Lifted;
						continue;
					}
					const FBox Box = Primitive->Bounds.GetBox();
					Floating.Add(FBox2D(FVector2D(Box.Min), FVector2D(Box.Max)));
					FloatingText += FString::Printf(TEXT("%s%s at (%.0f, %.0f)"), FloatingText.IsEmpty() ? TEXT("") : TEXT(", "), *Bench.BodyName(Other), Box.GetCenter().X, Box.GetCenter().Y);
				}
			}
			constexpr double BallDistanceCm = 300.0;
			const FVector2D Side = FVector2D(Shape.Side).GetSafeNormal();
			// Clear overhead: the ball is set down at its present height and then lowered onto its water from 3 m above (SettleOnWater's
			// sweep), so a heading under a jetty, a beam or any static shape over the water would leave it resting dry on that shape (the
			// water map sees only FluidNinja's own obstacles: a 45 degree heading in the demo pool put it on a prop 44 cm above its water).
			const UWorld* SweepWorld = Bench.GetWorld();
			const double BallZ = BallPrimitive->GetComponentLocation().Z;
			const FCollisionShape BallSphere = FCollisionShape::MakeSphere(static_cast<float>(BallPrimitive->Bounds.SphereRadius));
			FCollisionQueryParams SweepParams(TEXT("FluidNinjaBuoyancyCouplingBall"), false, Ball->GetOwner());
			SweepParams.AddIgnoredActor(Boat->GetOwner());
			// FluidNinja's own actors are left out: a NinjaLive's collision box encloses its whole water (the sweep starts inside it).
			if (SweepWorld)
			{
				for (TActorIterator<AActor> It(SweepWorld); It; ++It)
				{
					if (NinjaLiveComponent(**It))
					{
						SweepParams.AddIgnoredActor(*It);
					}
				}
			}
			int32 Covered = 0;
			FString CoveredBy;
			int32 BestTurn = INDEX_NONE;
			int32 BestKey[3] = {0, 0, 0};
			for (int32 Turn = 0; Turn < 16; ++Turn)
			{
				const FVector2D Direction = Side.GetRotated(Turn * 22.5);
				const FVector2D Point = Shape.Centre + Direction * BallDistanceCm;
				if (Water.DepthAt(Point) < OpenDepthCm || Water.ClearRadius(Point, 100.0) < 50.0)
				{
					continue;   // not over open water (ChooseDirections' test)
				}
				// Covered only where the sweep stops well above the ball's rest pose: its end sphere reaches into the water, and the surface's
				// own mesh (or anything at the waterline) stops it within a few cm of the rest pose; the prop that caught it stopped it 44 cm up.
				// A sweep that starts inside something (a volume enclosing the pool) says nothing about what is over the water: not counted.
				FHitResult Overhead;
				if (SweepWorld && SweepWorld->SweepSingleByObjectType(Overhead, FVector(Point, BallZ + 300.0), FVector(Point, BallZ), FQuat::Identity,
					FCollisionObjectQueryParams(ECC_WorldStatic), BallSphere, SweepParams)
					&& !Overhead.bStartPenetrating && Overhead.Location.Z > BallZ + 15.0)
				{
					if (CoveredBy.IsEmpty())
					{
						CoveredBy = FString::Printf(TEXT("%s, %.0f cm above the ball's rest"), *GetNameSafe(Overhead.GetActor()), Overhead.Location.Z - BallZ);
					}
					++Covered;
					continue;   // something static over the water there
				}
				const double Gamma = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FMath::Abs(Direction | Side), 0.0, 1.0)));
				double Nearest = 1000.0;
				for (const FBox2D& Box : Floating)
				{
					Nearest = FMath::Min(Nearest, FMath::Sqrt(Box.ComputeSquaredDistanceToPoint(Point)));
				}
				// Smaller is better in every part: off 45 degrees (tenths), nearness to a floating body and to a wall (in map steps).
				const int32 Key[3] = {FMath::RoundToInt32(10.0 * FMath::Abs(Gamma - 45.0)), -FMath::FloorToInt32(Nearest / Water.Step),
					-FMath::FloorToInt32(Water.ClearRadius(Point, 1000.0) / Water.Step)};
				const bool bBetter = BestTurn == INDEX_NONE || Key[0] < BestKey[0] || (Key[0] == BestKey[0] && (Key[1] < BestKey[1] || (Key[1] == BestKey[1] && Key[2] < BestKey[2])));
				if (bBetter)
				{
					BestTurn = Turn;
					BestKey[0] = Key[0];
					BestKey[1] = Key[1];
					BestKey[2] = Key[2];
				}
			}
			if (BestTurn == INDEX_NONE)
			{
				Abort(Bench, R, FString::Printf(TEXT("no open water 3 m from the row boat (%d headings over open water covered from above%s)"), Covered,
					CoveredBy.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("; the first by %s"), *CoveredBy)));
				return;
			}
			const FVector2D Chosen = Side.GetRotated(BestTurn * 22.5);
			Couple.BallPoint = Shape.Centre + Chosen * BallDistanceCm;
			Couple.BoatCentre = Shape.Centre;
			Couple.bBoatCentre = true;
			Couple.GammaDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FMath::Abs(Chosen | Side), 0.0, 1.0)));
			Couple.WallCm = Water.ClearRadius(Couple.BallPoint, 1000.0);
			Couple.HeadingText = BestKey[0] == 0 ? FString(TEXT("the fixed oblique")) : FString(TEXT("no oblique over open water: the open heading nearest one"));
			UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy test %s: ball at (%.0f, %.0f), %.0f cm from the row boat's centre (%.0f, %.0f), %.0f deg from its side axis (0 broadside, 90 end-on; %s, %.1f deg turned from the side axis), %.0f cm from the nearest wall or shore (at most 1000 read); %d headings over open water left out, covered from above%s; other bodies still floating: %s; %d lifted out of the water (left out of the choice)"),
				*R.Name, Couple.BallPoint.X, Couple.BallPoint.Y, BallDistanceCm, Shape.Centre.X, Shape.Centre.Y, Couple.GammaDeg, *Couple.HeadingText, BestTurn * 22.5, Couple.WallCm,
				Covered, CoveredBy.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (the first by %s)"), *CoveredBy), FloatingText.IsEmpty() ? TEXT("none") : *FloatingText, Lifted);
			Ball->RestartAt(FTransform(BallPrimitive->GetComponentQuat(), FVector(Couple.BallPoint, BallPrimitive->GetComponentLocation().Z)));
		}, nullptr});
		auto Follow = [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			FCouplingRun& Couple = R.Ripple->Coupling;
			const IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
			const UWorld* World = Bench.GetWorld();
			const UFluidNinjaBuoyancyComponent* Ball = Couple.Ball.Get();
			const UPrimitiveComponent* BallPrimitive = Ball ? Ball->GetSimulatedPrimitive() : nullptr;
			if (Layer && !Couple.Log.Points.IsEmpty())
			{
				Couple.Log.Take(*Layer);
			}
			if (World && BallPrimitive)
			{
				Couple.BallTimes.Add(World->GetTimeSeconds());
				Couple.BallZ.Add(BallPrimitive->GetComponentLocation().Z);
				Couple.BallXY.Add(FVector2D(BallPrimitive->GetComponentLocation()));
				Couple.BallState.Add(FString::Printf(TEXT("%s; rigid body %s, rising %.1f cm/s"), *Ball->GetStatus(), BallPrimitive->RigidBodyIsAwake() ? TEXT("awake") : TEXT("asleep"),
					BallPrimitive->GetPhysicsLinearVelocity().Z));
			}
		};
		// The probe where the ball floats now, read as the ball's height is followed. The boat's static head alone makes the ring: what is
		// measured is one body's waves moving another, not the impulse source (ripple-weight reports what that adds).
		AddClear(TEXT("fresh waves, displacement only"), [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			FCouplingRun& Couple = R.Ripple->Coupling;
			IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
			const UFluidNinjaBuoyancyComponent* Ball = Couple.Ball.Get();
			const UPrimitiveComponent* BallPrimitive = Ball ? Ball->GetSimulatedPrimitive() : nullptr;
			if (Layer && BallPrimitive)
			{
				Couple.Log.Points = {FVector2D(BallPrimitive->GetComponentLocation())};
				SetWaveSources(true, false);
				Layer->SetProbes(Couple.Log.Points);
			}
		}, Follow);
		// The ball at rest before the drop: still bobbing from being put in place, it would add its own motion to the crest's. At most 10 s;
		// on once its vertical speed has stayed under SettledBallCmS for a second.
		Made->Phases.Add({TEXT("the ball settling"), 10.f, 0.f, nullptr, nullptr, [Follow](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			Follow(Bench, R);
			FCouplingRun& Couple = R.Ripple->Coupling;
			const UFluidNinjaBuoyancyComponent* Ball = Couple.Ball.Get();
			const UPrimitiveComponent* BallPrimitive = Ball ? Ball->GetSimulatedPrimitive() : nullptr;
			const UWorld* World = Bench.GetWorld();
			if (!BallPrimitive || !World)
			{
				return;
			}
			const double Now = World->GetTimeSeconds();
			if (Couple.StillSince < 0.0 || FMath::Abs(BallPrimitive->GetPhysicsLinearVelocity().Z) > SettledBallCmS)
			{
				Couple.StillSince = Now;
			}
			else if (Now - Couple.StillSince >= 1.0)
			{
				R.Phases[R.Phase].Seconds = 0.f;
			}
		}});
		Made->Phases.Add({TEXT("row boat dropped from 3 m"), 7.f, 0.f, [DropBody](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			FCouplingRun& Couple = R.Ripple->Coupling;
			UFluidNinjaBuoyancyComponent* Boat = Couple.Boat.Get();
			const UWorld* World = Bench.GetWorld();
			if (Boat && World)
			{
				DropBody(Bench, *Boat);
				Couple.DropTime = World->GetTimeSeconds();
			}
		}, nullptr, Follow});
		// The ring that reaches the ball: the same drop again with the ball out of the water, read by the same probe. The ball feels the field
		// less its own waves (B - B_self), while a probe at a light body reads its own waves too, so the ring without it is the reference.
		Made->Phases.Add({TEXT("row boat put back, the ball out of the water"), 8.f, 0.f, [LiftOthers](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			FCouplingRun& Couple = R.Ripple->Coupling;
			if (UFluidNinjaBuoyancyComponent* Boat = Couple.Boat.Get())
			{
				Boat->RestartAt(Boat->GetStartTransform());
			}
			const UFluidNinjaBuoyancyComponent* Ball = Couple.Ball.Get();
			LiftOthers(Bench, R, [Ball](const UFluidNinjaBuoyancyComponent& Body) { return &Body != Ball; });
		}, nullptr});
		auto FollowIncident = [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			FCouplingRun& Couple = R.Ripple->Coupling;
			if (const IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves(); Layer && !Couple.Incident.Points.IsEmpty())
			{
				Couple.Incident.Take(*Layer);
			}
		};
		AddClear(TEXT("fresh waves, displacement only, the ball away"), [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			FCouplingRun& Couple = R.Ripple->Coupling;
			IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
			if (Layer && !Couple.Log.Points.IsEmpty())
			{
				Couple.Incident.Points = Couple.Log.Points;
				SetWaveSources(true, false);
				Layer->SetProbes(Couple.Incident.Points);
			}
		}, FollowIncident);
		Made->Phases.Add({TEXT("row boat dropped again"), 7.f, 0.f, [DropBody](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			FCouplingRun& Couple = R.Ripple->Coupling;
			UFluidNinjaBuoyancyComponent* Boat = Couple.Boat.Get();
			const UWorld* World = Bench.GetWorld();
			if (Boat && World)
			{
				DropBody(Bench, *Boat);
				Couple.IncidentDropTime = World->GetTimeSeconds();
			}
		}, nullptr, FollowIncident});
		Made->Evaluate = [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			const FCouplingRun& Couple = R.Ripple->Coupling;
			const FString Who = Couple.Ball.IsValid() ? Bench.BodyName(Couple.Ball.Get()) : FString(TEXT("ball"));
			if (Couple.DropTime < 0.0 || Couple.IncidentDropTime < 0.0 || Couple.Incident.Times.IsEmpty() || Couple.BallZ.IsEmpty() || !Couple.bBoatCentre)
			{
				Bench.Report(R.Name, Who, 0, TEXT("not measured: no drops, probe readings or ball heights"));
				return;
			}
			// A reading's largest rise from From to To seconds after its drop, against its level in the second before the drop, and when.
			auto Rise = [](const TArray<double>& Times, TFunctionRef<double(int32)> Value, double Drop, double From, double To, double& OutAfter)
			{
				double Level = 0.0;
				int32 Count = 0;
				for (int32 Index = 0; Index < Times.Num(); ++Index)
				{
					if (Times[Index] <= Drop && Times[Index] >= Drop - 1.0)
					{
						Level += Value(Index);
						++Count;
					}
				}
				Level = Count > 0 ? Level / Count : 0.0;
				// -1: nothing in that time.
				double Peak = 0.0;
				OutAfter = -1.0;
				for (int32 Index = 0; Index < Times.Num(); ++Index)
				{
					const double After = Times[Index] - Drop;
					if (After > From && After <= To && (OutAfter < 0.0 || Value(Index) - Level > Peak))
					{
						Peak = Value(Index) - Level;
						OutAfter = After;
					}
				}
				return Peak;
			};
			const auto IncidentAt = [&Couple](int32 Index) { return static_cast<double>(Couple.Incident.Heights[Index].IsEmpty() ? 0.f : Couple.Incident.Heights[Index][0]); };
			const auto WithBallAt = [&Couple](int32 Index) { return static_cast<double>(Couple.Log.Heights[Index].IsEmpty() ? 0.f : Couple.Log.Heights[Index][0]); };
			const auto BallAt = [&Couple](int32 Index) { return Couple.BallZ[Index]; };
			constexpr double Forever = 1.0e9;
			const TArray<double>& Times = Couple.Incident.Times;
			// The whole record's largest rises, information only: a later crest (the pool's reflections, another body stirring) can be
			// larger than the direct ring, so neither is judged, and the scale the direct ring is picked against is measured only until its
			// long waves have arrived (below). The ring's must still reach 3 mm somewhere in the record.
			double WholeAfter = 0.0;
			const double WholeRing = Rise(Times, IncidentAt, Couple.IncidentDropTime, 0.0, Forever, WholeAfter);
			if (WholeAfter < 0.0)
			{
				Bench.Report(R.Name, Who, 0, TEXT("not measured: no probe reading after the drop with the ball away"));
				return;
			}
			double AnyAfter = 0.0;
			const double AnyLift = Rise(Couple.BallTimes, BallAt, Couple.DropTime, 0.0, Forever, AnyAfter);
			const FString WholeText = FString::Printf(TEXT("over the whole record the ring's largest rise was %.2f cm (%.1f s after the drop) and the ball's %.2f cm (%.1f s after its drop)"),
				WholeRing, WholeAfter, AnyLift, AnyAfter);
			if (WholeRing < 0.3)
			{
				// A ring that never reaches the ball is what this test is there to catch, so it fails rather than going unjudged (under 3 mm
				// the ball's own bobbing would swamp the comparison anyway).
				Bench.Report(R.Name, Who, -1, TEXT("failed precondition, the ring is under 3 mm at the ball (a 3 m drop 3 m away must send one there): ") + WholeText);
				return;
			}
			const double Depth = R.Ripple->Water.IsValid() ? R.Ripple->Water.CoreAt(Couple.BallPoint).G : 0.0;
			if (Depth <= 0.0)
			{
				Bench.Report(R.Name, Who, 0, TEXT("not measured: the depth the core runs at the ball is not known, so neither when its direct ring can arrive nor how long its waves are; ") + WholeText);
				return;
			}
			// The probe's level in the second before the drop (every rise is against it), and how long a crest at a reading lasts: twice
			// the time between the zero crossings around it, against that level (0 where the record reaches no crossing on one side).
			double Level = 0.0;
			int32 LevelCount = 0;
			for (int32 Index = 0; Index < Times.Num(); ++Index)
			{
				if (Times[Index] <= Couple.IncidentDropTime && Times[Index] >= Couple.IncidentDropTime - 1.0)
				{
					Level += IncidentAt(Index);
					++LevelCount;
				}
			}
			Level = LevelCount > 0 ? Level / LevelCount : 0.0;
			const auto CrestPeriod = [&Times, &IncidentAt, Level](int32 Peak)
			{
				if (Peak == INDEX_NONE)
				{
					return 0.0;
				}
				int32 Up = Peak;
				int32 Down = Peak;
				while (Up > 0 && IncidentAt(Up - 1) > Level)
				{
					--Up;
				}
				while (Down + 1 < Times.Num() && IncidentAt(Down + 1) > Level)
				{
					++Down;
				}
				return Up > 0 && Down + 1 < Times.Num() ? 2.0 * (Times[Down + 1] - Times[Up - 1]) : 0.0;
			};
			// The direct ring: the boat's own first waves at the ball's place, not what the pool sends back later or what another body
			// stirs. Its first crest is the first local maximum, after the earliest a wave can arrive and no later than the latest its long
			// waves can, that rises above RingPickShare of the largest rise in that time. The earliest arrival is the boat's centre's
			// distance at √(g h), the long-wave limit of the core's dispersion (no wave is faster), counted from the lift. Its hull's edge is
			// nearer the ball than its centre, by under 2 m, which a wave at √(g h) (6.3 m/s at 4 m) crosses in well under the 0.77 s the
			// 3 m fall takes before the hull reaches the water, so the bound stays early. The latest is the fall, then the centre's distance
			// at the group speed of a RingShortestWaveCm wave (every longer wave of the ring is in by then): the scale is measured only up
			// to it, as a later, larger crest (the suite's 16.7 cm at 5.9 s) would put 30 % of the whole record above the direct crest itself.
			// The hull's nearer edge only makes the ring come sooner, so this bound stays late.
			const double Distance = FVector2D::Distance(Couple.BallPoint, Couple.BoatCentre);
			const double LongWave = FMath::Sqrt(CoreGravity * Depth);
			const double Earliest = Distance / LongWave;
			const UWorld* World = Bench.GetWorld();
			const double Gravity = World ? FMath::Max(-static_cast<double>(World->GetGravityZ()), 1.0) : 980.0;
			const double FallSeconds = FMath::Sqrt(2.0 * DropMetres * 100.0 / Gravity);
			const double ShortestGroup = CoreGroupVelocity(2.0 * UE_DOUBLE_PI / RingShortestWaveCm, Depth);
			const double Latest = FallSeconds + Distance / FMath::Max(ShortestGroup, 1.0e-3);
			double ScaleAfter = 0.0;
			const double RingScale = Rise(Times, IncidentAt, Couple.IncidentDropTime, 0.0, Latest, ScaleAfter);
			if (ScaleAfter < 0.0)
			{
				Bench.Report(R.Name, Who, 0, FString::Printf(TEXT("not measured: no probe reading with the ball away by the latest the direct ring's long waves can arrive (%.2f s after the drop); %s"), Latest, *WholeText));
				return;
			}
			int32 FirstPeak = INDEX_NONE;
			for (int32 Index = 1; Index + 1 < Times.Num() && FirstPeak == INDEX_NONE; ++Index)
			{
				const double After = Times[Index] - Couple.IncidentDropTime;
				const double Value = IncidentAt(Index) - Level;
				if (After > Earliest && After <= Latest && Value > RingPickShare * RingScale
					&& Value >= IncidentAt(Index - 1) - Level && Value > IncidentAt(Index + 1) - Level)
				{
					FirstPeak = Index;   // equal readings (a readback repeated) count from the last of them
				}
			}
			const FString PickText = FString::Printf(TEXT("its first crest above %.0f%% of the largest rise up to the latest the direct ring's long waves can arrive (%.2f cm, so above %.2f cm), after the earliest a wave can arrive (%.2f s after the drop: %.2f m from the boat's centre at sqrt(g h) %.2f m/s) and no later than that latest (%.2f s after the drop: the %.2f s fall, then the group of a %.2f m wave at %.2f m/s)"),
				100.0 * RingPickShare, RingScale, RingPickShare * RingScale, Earliest, Distance / 100.0, LongWave / 100.0, Latest, FallSeconds,
				RingShortestWaveCm / 100.0, ShortestGroup / 100.0);
			const double FirstPeriod = CrestPeriod(FirstPeak);
			if (FirstPeak == INDEX_NONE || FirstPeriod <= 0.0)
			{
				Bench.Report(R.Name, Who, 0, FString::Printf(TEXT("not measured: no direct ring at the ball: %s %s; %s"), *PickText,
					FirstPeak == INDEX_NONE ? TEXT("was not found") : TEXT("had no zero crossing on one side within the record, so no period"), *WholeText));
				return;
			}
			// The ring's window after the drop with the ball away: half that crest's period either side of it, and no later than the latest
			// its long waves can arrive (Latest, the pick's own bound), so the ring read is the first crest picked, a local maximum inside
			// that bound. The old window ran on to one and a half periods after it, uncapped: the suite's 5.9 s crest came back into it and
			// was compared with the ball's 4.7 s one, and capping that wider window alone could leave the ring's largest on a crest still
			// rising at the cap (a flank). It is set on the crest measured, not at r / c_g of its wavelength: one crest's length is a poor
			// clock for a dispersing ring (ripple-coupling standalone, 2026-10-18: a 1.07 s, 1.78 m crest at 3.0 s after the lift puts r / c_g
			// at 3.6 s, and a window there misses that very crest), so r / c_g is printed beside it, information.
			const double FirstAfter = Times[FirstPeak] - Couple.IncidentDropTime;
			const double From = FirstAfter - 0.5 * FirstPeriod;
			const double To = FMath::Min(FirstAfter + 0.5 * FirstPeriod, Latest);
			double RingAfter = 0.0;
			const double Ring = Rise(Times, IncidentAt, Couple.IncidentDropTime, From, To, RingAfter);
			// That first crest's wavelength at the core's depth, the group speed it travels at, and when its group would reach the ball from
			// the impact (the impact itself about a 3 m fall after the lift, FallSeconds above), information.
			const double FirstK = CoreWaveNumber(2.0 * UE_DOUBLE_PI / FirstPeriod, Depth);
			const double FirstGroup = CoreGroupVelocity(FirstK, Depth);
			// How long the judged crest is at the ball, as a wavelength at the core's depth there, beside the ball's size. A floating body
			// follows a wave much longer than itself (its heave's own frequency lies far above a wave's); a wave not much longer than it
			// tilts it instead.
			FString Length;
			{
				const double RingTime = Couple.IncidentDropTime + RingAfter;
				const double Period = CrestPeriod(Times.IndexOfByPredicate([RingTime](double Time) { return Time >= RingTime; }));
				const UPrimitiveComponent* BallPrimitive = Couple.Ball.IsValid() ? Couple.Ball->GetSimulatedPrimitive() : nullptr;
				if (Period > 0.0 && BallPrimitive)
				{
					Length = FString::Printf(TEXT("; the crest at the ball lasts about %.2f s, a %.2f m wave at the core's %.0f cm, against the ball's %.2f m"),
						Period, 2.0 * UE_DOUBLE_PI / CoreWaveNumber(2.0 * UE_DOUBLE_PI / Period, Depth) / 100.0, Depth, 2.0 * BallPrimitive->Bounds.SphereRadius / 100.0);
				}
			}
			// The ball's rise on that same crest: half the first crest's period either side of when the ring rose most, after its own drop
			// (its heave lags the water a little). Each one's largest over the whole window could be different crests (ring 5.9 s, ball
			// 4.7 s in the suite).
			const double LiftFrom = RingAfter - 0.5 * FirstPeriod;
			const double LiftTo = RingAfter + 0.5 * FirstPeriod;
			double LiftAfter = 0.0;
			const double Lift = Rise(Couple.BallTimes, BallAt, Couple.DropTime, LiftFrom, LiftTo, LiftAfter);
			if (LiftAfter < 0.0)
			{
				Bench.Report(R.Name, Who, 0, FString::Printf(TEXT("not measured: no ball height on the direct ring's crest, %.2f to %.2f s after its drop; %s"), LiftFrom, LiftTo, *WholeText));
				return;
			}
			double WithAfter = -1.0;
			const double WithBall = Couple.Log.Times.IsEmpty() ? 0.0 : Rise(Couple.Log.Times, WithBallAt, Couple.DropTime, LiftFrom, LiftTo, WithAfter);
			// How far the waves pushed the ball off the probe point (where it floated at the clear; the reference ring is read there): the
			// most after its drop, and at its judged rise.
			const FVector2D ProbePoint = Couple.Log.Points.IsEmpty() ? Couple.BallPoint : Couple.Log.Points[0];
			double MostDrift = 0.0;
			double DriftAtLift = -1.0;
			for (int32 Index = 0; Index < Couple.BallXY.Num() && Index < Couple.BallTimes.Num(); ++Index)
			{
				const double After = Couple.BallTimes[Index] - Couple.DropTime;
				if (After < 0.0)
				{
					continue;
				}
				const double Off = FVector2D::Distance(Couple.BallXY[Index], ProbePoint);
				MostDrift = FMath::Max(MostDrift, Off);
				DriftAtLift = DriftAtLift < 0.0 && After >= LiftAfter ? Off : DriftAtLift;
			}
			const FString Text = FString::Printf(TEXT("with the ball away the direct ring rose %.2f cm at its place (%.1f s after the drop); on it the ball rose %.2f cm (%.1f s after its drop), %.0f%% of it (pass: 80%% to 125%%: a body much smaller than the wave rides it)%s. Both read on the direct ring only: the ring %.2f to %.2f s after its drop (%s, at %.2f s, half its period (%.2f s) either side of it, and no later than that latest), the ball on the same crest, %.2f to %.2f s after its drop (half that period either side of the ring's rise). The ball %.0f deg from the hull's side axis (0 broadside, 90 end-on; %s), %.0f cm from the nearest wall or shore; it drifted at most %.1f cm off the probe point after its drop (%.1f cm at its judged rise). Information: that first crest is a %.2f m wave at the core's %.0f cm, whose group (%.2f m/s) would reach the ball %.2f s after the impact (r / c_g, the impact about %.2f s after the lift); %s; with the ball in the water the field at its place rose %.2f cm on the direct ring (its own waves included)"),
				Ring, RingAfter, Lift, LiftAfter, Ring > 0.0 ? 100.0 * Lift / Ring : 0.0, *Length, From, To, *PickText, FirstAfter, FirstPeriod, LiftFrom, LiftTo,
				Couple.GammaDeg, *Couple.HeadingText, Couple.WallCm, MostDrift, FMath::Max(DriftAtLift, 0.0),
				2.0 * UE_DOUBLE_PI / FirstK / 100.0, Depth, FirstGroup / 100.0, Distance / FMath::Max(FirstGroup, 1.0e-3), FallSeconds, *WholeText, WithAfter >= 0.0 ? WithBall : 0.0);
			if (Ring < 0.3)
			{
				// As above: under 3 mm the ball's own bobbing would swamp the comparison.
				Bench.Report(R.Name, Who, -1, TEXT("failed precondition, the direct ring is under 3 mm at the ball (a 3 m drop 3 m away must send one there): ") + Text);
				return;
			}
			Bench.Report(R.Name, Who, Lift >= 0.8 * Ring && Lift <= 1.25 * Ring ? 1 : -1, Text);
			// The ball's state just after the drop, at its largest rise over the whole record and at its judged rise on the direct ring
			// (information).
			const auto StateAt = [&Couple](double After)
			{
				const int32 Index = Couple.BallTimes.IndexOfByPredicate([&Couple, After](double Time) { return Time >= Couple.DropTime + After; });
				return Couple.BallState.IsValidIndex(Index) ? FString::Printf(TEXT("%.1f s: %s"), Couple.BallTimes[Index] - Couple.DropTime, *Couple.BallState[Index]) : FString(TEXT("-"));
			};
			Bench.Report(R.Name, Who, 0, TEXT("the ball after the drop: ") + StateAt(0.0) + TEXT(" | at its largest rise over the whole record: ") + StateAt(AnyAfter) + TEXT(" | at its judged rise on the direct ring: ") + StateAt(LiftAfter));
		};
	}
	else if (Name == TEXT("ripple-kelvin"))
	{
		Made->Ripple->Tow.Boat = FindTestBody(TEXT("Boat"));
		// The boat in the longest straight run of open water of any simulation running waves, every other body out of the way (lifted,
		// still, its Waves off).
		Made->Phases.Add({TEXT("placing the row boat"), 4.f, 0.f, [Begin, Abort, Controlled, Focus, EnsureWater, LiftOthers](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			if (!Begin(Bench, R))
			{
				return;
			}
			FTowRun& Tow = R.Ripple->Tow;
			UFluidNinjaBuoyancyComponent* Boat = Tow.Boat.Get();
			const UFluidNinjaBuoyancySubsystem* Hydro = Bench.GetHydro();
			FHullShape Shape;
			if (!Boat || !Hydro || !MeasureHull(*Boat, Shape))
			{
				Abort(Bench, R, TEXT("no row boat among the floating bodies"));
				return;
			}
			R.bResetAfter = true;
			Controlled(Bench);
			Bench.SetSceneryCollision(false);
			// Every other body out of the way, the river's floaters too (the run may be in the river).
			LiftOthers(Bench, R, [Boat](const UFluidNinjaBuoyancyComponent& Body) { return &Body == Boat; });
			// The longest run with 2 m of open water either side, in any simulation running waves, through its most open spot or through
			// the boat's start, in 24 headings.
			IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
			TArray<FBox2D> Windows;
			if (Layer)
			{
				Layer->GetBenchWindows(Windows);
			}
			double Best = 0.0;
			FVector2D From = FVector2D::ZeroVector;
			FVector2D Heading = FVector2D(1.0, 0.0);
			for (const FBox2D& Window : Windows)
			{
				Focus(Bench, R, Window.GetCenter());
				if (!EnsureWater(Bench, R))
				{
					continue;
				}
				const FWaterMap& Water = R.Ripple->Water;
				FVector2D Open = Shape.Centre;
				double OpenCm = 0.0;
				Water.MostOpen(Water.Centre(), 1500.0, Open, OpenCm);
				for (const FVector2D& Pivot : {Open, Shape.Centre})
				{
					if (!Water.IsOpen(Pivot, 200.0))
					{
						continue;
					}
					for (int32 Turn = 0; Turn < 24; ++Turn)
					{
						const FVector2D Way = FVector2D(1.0, 0.0).GetRotated(Turn * 15.0);
						double Ahead = 0.0;
						double Behind = 0.0;
						while (Ahead < 4000.0 && Water.IsOpen(Pivot + Way * (Ahead + 50.0), 200.0))
						{
							Ahead += 50.0;
						}
						while (Behind < 4000.0 && Water.IsOpen(Pivot - Way * (Behind + 50.0), 200.0))
						{
							Behind += 50.0;
						}
						if (Ahead + Behind > Best)
						{
							Best = Ahead + Behind;
							From = Pivot - Way * Behind;
							Heading = Way;
						}
					}
				}
			}
			if (Best <= 0.0)
			{
				Abort(Bench, R, TEXT("no open water with 2 m clear either side in any simulation running waves (their Wave Aux)"));
				return;
			}
			// The bench reads the chosen run's simulation from now on.
			Focus(Bench, R, From + Heading * (0.5 * Best));
			if (!EnsureWater(Bench, R))
			{
				Abort(Bench, R, TEXT("the chosen run's Wave Aux could not be read again"));
				return;
			}
			const FWaterMap& Water = R.Ripple->Water;
			const double Available = Best - FMath::Max(Shape.Length, Shape.Beam) - 100.0;
			Tow.Seconds = FMath::Min(TowLongestSeconds, Available / TowSpeedCmS - WakeReadSeconds);
			if (Tow.Seconds < TowShortestSeconds)
			{
				Abort(Bench, R, FString::Printf(TEXT("no straight run of open water long enough: %.1f m found, %.1f m needed for %.0f s at %.2f m/s and %.0f s of reading"),
					Best / 100.0, ((TowShortestSeconds + WakeReadSeconds) * TowSpeedCmS + FMath::Max(Shape.Length, Shape.Beam) + 100.0) / 100.0, TowShortestSeconds,
					TowSpeedCmS / 100.0, WakeReadSeconds));
				return;
			}
			Tow.RunCm = Best;
			Tow.Direction = FVector(Heading, 0.0);
			Tow.Side = FVector(-Heading.Y, Heading.X, 0.0);
			// The core along the run: the depth it runs there (the theory's), its cell, damping and hyperviscosity.
			double DepthSum = 0.0;
			int32 DepthCount = 0;
			Tow.ShallowestCm = TNumericLimits<double>::Max();
			Tow.DeepestCm = 0.0;
			for (double Along = 0.0; Along <= Best; Along += 50.0)
			{
				const double Depth = Water.CoreAt(From + Heading * Along).G;
				DepthSum += Depth;
				++DepthCount;
				Tow.ShallowestCm = FMath::Min(Tow.ShallowestCm, Depth);
				Tow.DeepestCm = FMath::Max(Tow.DeepestCm, Depth);
			}
			Tow.DepthCm = DepthCount > 0 ? DepthSum / DepthCount : 0.0;
			const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
			Tow.CellCm = Layer->GetCellCm();
			Tow.Damping = FMath::Max(Settings.WaveDamping, 0.f);
			Tow.Hyper = FMath::Clamp(Settings.WaveHyperviscosity, 0.f, 50.f);
			// Its tow phase lasts as long as the water allows.
			R.Phases[R.Phase + 3].Seconds = static_cast<float>(Tow.Seconds);
			const FVector2D Start = From + Heading * (0.5 * FMath::Max(Shape.Length, Shape.Beam) + 50.0);
			const UPrimitiveComponent* Primitive = Boat->GetSimulatedPrimitive();
			const float Yaw = static_cast<float>(FMath::RadiansToDegrees(FMath::Atan2(Heading.Y, Heading.X)));
			Boat->RestartAt(FTransform(FQuat(FVector::ZAxisVector, FMath::DegreesToRadians(Yaw)) * Boat->RestAttitude.Quaternion(),
				FVector(Start, Primitive->GetComponentLocation().Z)));
		}, nullptr});
		// Its static head alone: the theory is the wake of the hull's displacement, P moving with it; the impulse source would add the drag
		// and slam impulses of the tow, which a moving pressure patch has none of.
		AddClear(TEXT("fresh waves, displacement only"), [](FFluidNinjaBuoyancyTestBench& Bench, FRun&)
		{
			if (IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves())
			{
				SetWaveSources(true, false);
				SetWaveCurrentCoupling(0.f);   // still water's wake: not carried by FluidNinja's current
				Layer->SetProbes(TArray<FVector2D>());
			}
		}, nullptr);
		// Towed: its velocity along the run every frame (its heave and pitch its own), its heading held; where its centre is each frame
		// (the waves see the same pose that frame).
		auto Towing = [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			FTowRun& Tow = R.Ripple->Tow;
			UFluidNinjaBuoyancyComponent* Boat = Tow.Boat.Get();
			UPrimitiveComponent* Primitive = Boat ? Boat->GetSimulatedPrimitive() : nullptr;
			const UWorld* World = Bench.GetWorld();
			if (!Primitive || !World || !Primitive->IsSimulatingPhysics())
			{
				return;
			}
			const double Now = World->GetTimeSeconds();
			if (Tow.TowStart < 0.0)
			{
				FHullShape Shape;
				if (MeasureHull(*Boat, Shape))
				{
					Tow.CentreLocal = Shape.CentreLocal;
					Tow.LengthCm = Shape.Length;
					Tow.BeamCm = Shape.Beam;
				}
				Tow.TowStart = Now;
			}
			const FVector Velocity = Primitive->GetPhysicsLinearVelocity();
			Primitive->SetPhysicsLinearVelocity(FVector(Tow.Direction.X * TowSpeedCmS, Tow.Direction.Y * TowSpeedCmS, Velocity.Z));
			FVector Spin = Primitive->GetPhysicsAngularVelocityInRadians();
			Spin.Z = 0.0;
			Primitive->SetPhysicsAngularVelocityInRadians(Spin);
			Tow.PoseTimes.Add(Now);
			Tow.PoseCentres.Add(FVector2D(Primitive->GetComponentLocation() + Primitive->GetComponentQuat().RotateVector(Tow.CentreLocal)));
		};
		Made->Phases.Add({TEXT("towed at 1.75 m/s"), static_cast<float>(TowLongestSeconds), 0.f, nullptr, nullptr, Towing});
		// Snapshots of both arcs behind it as it goes, each a little further out (over one transverse wavelength, 2π U²/g, so every point
		// of the arcs sees a crest pass); the hull's footprint as it goes, for the theory.
		Made->Phases.Add({TEXT("reading the wake"), 3.f, 0.f, [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			FTowRun& Tow = R.Ripple->Tow;
			UFluidNinjaBuoyancyComponent* Boat = Tow.Boat.Get();
			const UPrimitiveComponent* Primitive = Boat ? Boat->GetSimulatedPrimitive() : nullptr;
			const UFluidNinjaBuoyancySubsystem* Hydro = Bench.GetHydro();
			FBenchHull Hull;
			if (!Primitive || !Hydro || !GetBenchHull(*Boat, Hull))
			{
				return;
			}
			const FTransform Frame(Primitive->GetComponentQuat(), Primitive->GetComponentLocation());
			const FVector Centre = Primitive->GetComponentLocation() + Primitive->GetComponentQuat().RotateVector(Tow.CentreLocal);
			const double Reach = 0.5 * FMath::Max(Tow.LengthCm, Tow.BeamCm) + 3.0 * WakeGridCm;
			Tow.FootFirstAlong = -FMath::CeilToInt32(Reach / WakeGridCm);
			Tow.FootFirstAcross = Tow.FootFirstAlong;
			Tow.FootAlong = 2 * -Tow.FootFirstAlong + 1;
			Tow.FootAcross = Tow.FootAlong;
			HullFootprint(Hull, Frame, Hydro->GetControlledWaterHeight(), Centre, Tow.Direction, Tow.Side, WakeGridCm, Tow.FootFirstAlong, Tow.FootFirstAcross,
				Tow.FootAlong, Tow.FootAcross, Tow.Footprint);
		}, nullptr, [Towing](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			Towing(Bench, R);
			FTowRun& Tow = R.Ripple->Tow;
			IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
			const UWorld* World = Bench.GetWorld();
			if (!Layer || !World || Tow.PoseCentres.IsEmpty())
			{
				return;
			}
			if (Tow.bPending)
			{
				double Time = 0.0;
				TArray<float> Heights;
				if (Layer->GetProbeResults(Time, Heights) && Heights.Num() == Tow.PendingPoints.Num())
				{
					FTowRun::FSnapshot& Shot = Tow.Snapshots.AddDefaulted_GetRef();
					Shot.Time = Time;
					Shot.Offset = Tow.PendingOffset;
					Shot.Points = Tow.PendingPoints;
					Shot.Heights = MoveTemp(Heights);
					Tow.bPending = false;
				}
			}
			if (Tow.bPending)
			{
				return;
			}
			if (Tow.Snapshots.Num() >= WakeSnapshots)
			{
				R.Phases[R.Phase].Seconds = 0.f;   // all read: on to the theory
				return;
			}
			const double Gravity = FMath::Max(-static_cast<double>(World->GetGravityZ()), 1.0);
			const double Wavelength = 2.0 * UE_DOUBLE_PI * TowSpeedCmS * TowSpeedCmS / Gravity;
			Tow.PendingOffset = (Tow.NextOffset - 0.5 * (WakeSnapshots - 1)) * Wavelength / WakeSnapshots;
			++Tow.NextOffset;
			const FVector2D Centre = Tow.PoseCentres.Last();
			const FVector2D Ahead(Tow.Direction);
			const FVector2D Across(Tow.Side);
			Tow.PendingPoints.Reset();
			for (const double Arc : WakeArcsCm)
			{
				for (int32 Degrees = -WakeArcDegrees; Degrees <= WakeArcDegrees; ++Degrees)
				{
					const double Angle = FMath::DegreesToRadians(static_cast<double>(Degrees));
					const double Radius = Arc + Tow.PendingOffset;
					Tow.PendingPoints.Add(Centre - Ahead * (Radius * FMath::Cos(Angle)) + Across * (Radius * FMath::Sin(Angle)));
				}
			}
			Layer->SetProbes(Tow.PendingPoints);
			Tow.bPending = true;
		}});
		Made->Evaluate = [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			const FTowRun& Tow = R.Ripple->Tow;
			const FString Who = Tow.Boat.IsValid() ? Bench.BodyName(Tow.Boat.Get()) : FString(TEXT("row boat"));
			const UWorld* World = Bench.GetWorld();
			if (Tow.Snapshots.IsEmpty() || Tow.Footprint.IsEmpty() || Tow.TowStart < 0.0 || Tow.PoseTimes.Num() < 2 || !World)
			{
				Bench.Report(R.Name, Who, 0, FString::Printf(TEXT("not measured: %d snapshots of the wake, %d poses"), Tow.Snapshots.Num(), Tow.PoseTimes.Num()));
				return;
			}
			if (Tow.DepthCm <= 0.0)
			{
				Bench.Report(R.Name, Who, 0, TEXT("not measured: the core's depth along the run is unknown (no Wave Aux), so the theory has no dispersion"));
				return;
			}
			const double Gravity = FMath::Max(-static_cast<double>(World->GetGravityZ()), 1.0);
			const double Speed = ((Tow.PoseCentres.Last() - Tow.PoseCentres[0]) | FVector2D(Tow.Direction)) / FMath::Max(Tow.PoseTimes.Last() - Tow.PoseTimes[0], 1.0e-3);
			const FVector2D Ahead(Tow.Direction);
			const FVector2D Across(Tow.Side);
			// The theory's grid, along the run: the hull starts 30 m behind the grid's centre (it goes about 17 m), its footprint as it went.
			const int32 N = WakeGridCells;
			const double Cell = WakeGridCm;
			const int32 StartCells = -3000 / static_cast<int32>(Cell);
			TArray<FBenchComplex> Spectrum;
			Spectrum.SetNumZeroed(N * N);
			for (int32 J = 0; J < Tow.FootAcross; ++J)
			{
				for (int32 I = 0; I < Tow.FootAlong; ++I)
				{
					const int32 GridI = N / 2 + StartCells + Tow.FootFirstAlong + I;
					const int32 GridJ = N / 2 + Tow.FootFirstAcross + J;
					if (GridI >= 0 && GridJ >= 0 && GridI < N && GridJ < N)
					{
						Spectrum[GridJ * N + GridI].Re = Tow.Footprint[J * Tow.FootAlong + I];
					}
				}
			}
			Fft2D(Spectrum, N, false);
			// The core's own dispersion and damping at the run's depth (the static head is rastered as it is: only impulses are smoothed).
			TArray<double> Omega;
			TArray<double> Gamma;
			Omega.SetNumUninitialized(N * N);
			Gamma.SetNumUninitialized(N * N);
			const double Unit = 2.0 * UE_DOUBLE_PI / (N * Cell);
			for (int32 J = 0; J < N; ++J)
			{
				for (int32 I = 0; I < N; ++I)
				{
					const double Kx = (I < N / 2 ? I : I - N) * Unit;
					const double Ky = (J < N / 2 ? J : J - N) * Unit;
					const double K = FMath::Sqrt(Kx * Kx + Ky * Ky);
					Omega[J * N + I] = CoreOmega(K, Tow.DepthCm);
					Gamma[J * N + I] = CoreDamping(K, Tow.Damping, Tow.Hyper, Tow.CellCm);
				}
			}
			// Per arc: measured against the theory at every point of every snapshot, and the envelope (largest |z|) per degree. A snapshot's
			// readings show the core some ticks before the frame they are stamped with (MaxLagTicks): the theory and the boat's pose are taken
			// that much earlier, and the lag that fits best is kept.
			const int32 Bins = 2 * WakeArcDegrees + 1;
			const int32 Arcs = UE_ARRAY_COUNT(WakeArcsCm);
			const double TickSeconds = 1.0 / (Bench.GetWaves() ? Bench.GetWaves()->GetStepsPerSecond() : 60.0);
			TArray<double> ErrorSum;
			TArray<double> NormSum;
			TArray<double> CrossSum;
			TArray<double> GotSum;
			TArray<int32> Points;
			TArray<double> Measured;
			TArray<double> Theory;
			int32 Lag = 0;
			double BestError = TNumericLimits<double>::Max();
			TArray<FBenchComplex> Work;
			TArray<double> Field;
			for (int32 TryLag = 0; TryLag <= MaxLagTicks; ++TryLag)
			{
				TArray<double> TryError;
				TArray<double> TryNorm;
				TArray<double> TryCross;
				TArray<double> TryGot;
				TArray<int32> TryPoints;
				TArray<double> TryMeasured;
				TArray<double> TryTheory;
				TryError.Init(0.0, Arcs);
				TryNorm.Init(0.0, Arcs);
				TryCross.Init(0.0, Arcs);
				TryGot.Init(0.0, Arcs);
				TryPoints.Init(0, Arcs);
				TryMeasured.Init(0.0, Arcs * Bins);
				TryTheory.Init(0.0, Arcs * Bins);
				for (const FTowRun::FSnapshot& Shot : Tow.Snapshots)
				{
					const double Shown = Shot.Time - TryLag * TickSeconds;
					const double Seconds = Shown - Tow.TowStart;
					TowedWake(Spectrum, Omega, Gamma, N, Cell, Speed, Seconds, Work, Field);
					// The boat's centre then (the probes stay where they were set while it moves on).
					int32 Pose = Tow.PoseTimes.Num() - 1;
					for (int32 Index = 0; Index < Tow.PoseTimes.Num(); ++Index)
					{
						if (Tow.PoseTimes[Index] >= Shown - 1.0e-6)
						{
							Pose = Index;
							break;
						}
					}
					const FVector2D Centre = Tow.PoseCentres[Pose];
					for (int32 Index = 0; Index < Shot.Points.Num() && Index < Shot.Heights.Num(); ++Index)
					{
						const FVector2D Offset = Shot.Points[Index] - Centre;
						const double OnAhead = Offset | Ahead;
						const double OnAcross = Offset | Across;
						const double X = (StartCells * Cell + Speed * Seconds + OnAhead) / Cell + N / 2;
						const double Y = OnAcross / Cell + N / 2;
						const double Expected = SampleGrid(Field, N, X, Y);
						const double Got = Shot.Heights[Index];
						const int32 Arc = FMath::Min(Index / Bins, Arcs - 1);
						TryError[Arc] += FMath::Square(Got - Expected);
						TryNorm[Arc] += FMath::Square(Expected);
						TryCross[Arc] += Got * Expected;
						TryGot[Arc] += Got * Got;
						++TryPoints[Arc];
						const int32 Bin = FMath::Clamp(FMath::RoundToInt32(FMath::RadiansToDegrees(FMath::Atan2(OnAcross, -OnAhead))), -WakeArcDegrees, WakeArcDegrees) + WakeArcDegrees;
						TryMeasured[Arc * Bins + Bin] = FMath::Max(TryMeasured[Arc * Bins + Bin], FMath::Abs(Got));
						TryTheory[Arc * Bins + Bin] = FMath::Max(TryTheory[Arc * Bins + Bin], FMath::Abs(Expected));
					}
				}
				double Total = 0.0;
				for (int32 Arc = 0; Arc < Arcs; ++Arc)
				{
					Total += TryNorm[Arc] > 0.0 ? TryError[Arc] / TryNorm[Arc] : 0.0;
				}
				if (Total < BestError)
				{
					BestError = Total;
					Lag = TryLag;
					ErrorSum = MoveTemp(TryError);
					NormSum = MoveTemp(TryNorm);
					CrossSum = MoveTemp(TryCross);
					GotSum = MoveTemp(TryGot);
					Points = MoveTemp(TryPoints);
					Measured = MoveTemp(TryMeasured);
					Theory = MoveTemp(TryTheory);
				}
			}
			const double LengthFroude = Tow.LengthCm > 0.f ? Speed / FMath::Sqrt(Gravity * Tow.LengthCm) : 0.0;
			const double DepthFroude = Speed / FMath::Sqrt(Gravity * Tow.DepthCm);
			Bench.Report(R.Name, Who, 0, FString::Printf(TEXT("towed %.2f m/s for %.1f s (length Froude %.2f, depth Froude %.2f at %.0f cm), its static head alone, %d snapshots of the arcs. Judged against the exact linear wake of this hull towed this way (the core's own equation, B_tt + Gamma B_t + omega^2 B = P_tt + Gamma P_t with the hull's static head P moving from rest, solved in closed form at each snapshot's time): each arc's normalised RMS at most %.0f%%, and each side's cusp line (through the caustics on the %.0f and %.0f m arcs, where the envelope, outward of its peak, falls to %.2f of it, the Airy function's share on a caustic; the measured envelope read on the lobe of the theory's peak; a line only where it opens away from the track) within %.0f deg of the exact wake's. Kelvin's %.2f deg is the far wake's, reported beside them: %.0f and %.0f m behind a %.1f m hull the bow and stern wakes still overlap and the start's transient is still building them, so it is there to be read only where the exact wake's own line lies within %.0f deg of it"),
				Speed / 100.0, Tow.Seconds, LengthFroude, DepthFroude, Tow.DepthCm, Tow.Snapshots.Num(), 100.0 * WakeRmsLimit, WakeArcsCm[0] / 100.0, WakeArcsCm[Arcs - 1] / 100.0,
				CausticShare, KelvinToleranceDeg, KelvinAngleDeg, WakeArcsCm[0] / 100.0, WakeArcsCm[Arcs - 1] / 100.0, Tow.LengthCm / 100.f, KelvinToleranceDeg));
			// The theory's dispersion and damping (information), and the readings' lag that fitted best.
			Bench.Report(R.Name, Who, 0, FString::Printf(TEXT("the exact wake's dispersion is the core's own, omega^2 = g (k + (sigma/rho g) k^3) tanh(k d), at the run's %.0f cm (the core runs %.0f-%.0f cm along it), with its damping %.3f/s and hyperviscosity %.2f/s on %.2f cm cells; the readings fitted best %d tick%s behind the frames they are stamped with"),
				Tow.DepthCm, Tow.ShallowestCm, Tow.DeepestCm, Tow.Damping, Tow.Hyper, Tow.CellCm, Lag, Lag == 1 ? TEXT("") : TEXT("s")));
			// Per arc, side (port, starboard) and envelope (measured, theory): the caustic's angle off the track, -1 where there is none.
			TArray<double> Caustics;
			Caustics.Init(-1.0, Arcs * 2 * 2);
			auto AngleText = [](double Value) { return Value >= 0.0 ? FString::Printf(TEXT("%.1f"), Value) : FString(TEXT("none")); };
			for (int32 Arc = 0; Arc < Arcs; ++Arc)
			{
				// Each side's envelope peak (degrees off the track), measured and in theory, and how alike the envelopes are.
				int32 Peaks[2][2] = {{0, 0}, {0, 0}};
				double Dot = 0.0;
				double MeasuredNorm = 0.0;
				double TheoryNorm = 0.0;
				for (int32 Bin = 0; Bin < Bins; ++Bin)
				{
					const double M = Measured[Arc * Bins + Bin];
					const double T = Theory[Arc * Bins + Bin];
					Dot += M * T;
					MeasuredNorm += M * M;
					TheoryNorm += T * T;
					const int32 Side = Bin >= WakeArcDegrees ? 1 : 0;
					const int32 Degrees = FMath::Abs(Bin - WakeArcDegrees);
					if (Degrees == 0)
					{
						continue;
					}
					if (M > Measured[Arc * Bins + WakeArcDegrees + (Side ? Peaks[Side][0] : -Peaks[Side][0])])
					{
						Peaks[Side][0] = Degrees;
					}
					if (T > Theory[Arc * Bins + WakeArcDegrees + (Side ? Peaks[Side][1] : -Peaks[Side][1])])
					{
						Peaks[Side][1] = Degrees;
					}
				}
				// Each side's caustic, in theory and measured (the envelope from the track outward, one value per degree): the theory's on
				// its largest lobe, the measured one on the lobe the theory's peak lies in, so the two are read on the same wave system.
				TArray<double> SideEnvelope;
				SideEnvelope.SetNumZeroed(WakeArcDegrees + 1);
				for (int32 Hand = 0; Hand < 2; ++Hand)
				{
					int32 TheoryPeak = INDEX_NONE;
					for (int32 Which = 1; Which >= 0; --Which)
					{
						const TArray<double>& Envelopes = Which == 0 ? Measured : Theory;
						for (int32 Off = 0; Off <= WakeArcDegrees; ++Off)
						{
							SideEnvelope[Off] = Envelopes[Arc * Bins + WakeArcDegrees + (Hand ? Off : -Off)];
						}
						const int32 Top = Which == 1 ? EnvelopePeak(SideEnvelope) : EnvelopePeak(SideEnvelope, TheoryPeak);
						TheoryPeak = Which == 1 ? Top : TheoryPeak;
						double Edge = 0.0;
						if (CausticAngle(SideEnvelope, Top, Edge))
						{
							Caustics[(Arc * 2 + Hand) * 2 + Which] = Edge;
						}
					}
				}
				// Judged: the arc against the exact linear wake at the same snapshots, every point of each (no signal in the theory on the
				// arc leaves nothing to judge it by).
				const double Rms = NormSum[Arc] > 0.0 ? FMath::Sqrt(ErrorSum[Arc] / NormSum[Arc]) : 0.0;
				const double Alike = MeasuredNorm > 0.0 && TheoryNorm > 0.0 ? Dot / FMath::Sqrt(MeasuredNorm * TheoryNorm) : 0.0;
				// What of a miss is size and what is shape (information): the measured wake's least-squares multiple of the theory's, and their
				// correlation (1: the same shape and phase at every point).
				const FString RmsText = NormSum[Arc] > 0.0
					? FString::Printf(TEXT("normalised RMS %.1f%% over %d points (pass: %.0f%%); the measured wake is %.2f times the theory's at best fit, correlation %.2f"), 100.0 * Rms,
						Points[Arc], 100.0 * WakeRmsLimit, CrossSum[Arc] / NormSum[Arc], GotSum[Arc] > 0.0 ? CrossSum[Arc] / FMath::Sqrt(GotSum[Arc] * NormSum[Arc]) : 0.0)
					: FString(TEXT("no theory signal on this arc, so no RMS"));
				Bench.Report(R.Name, FString::Printf(TEXT("%s r %.0f cm"), *Who, WakeArcsCm[Arc]), NormSum[Arc] > 0.0 ? (Rms <= WakeRmsLimit ? 1 : -1) : 0,
					FString::Printf(TEXT("against the exact linear wake: %s; port / starboard: envelope peaks %d / %d deg off the track (theory %d / %d), caustics %s / %s deg (theory %s / %s); envelopes alike %.2f"),
						*RmsText, Peaks[0][0], Peaks[1][0], Peaks[0][1], Peaks[1][1], *AngleText(Caustics[(Arc * 2 + 0) * 2 + 0]), *AngleText(Caustics[(Arc * 2 + 1) * 2 + 0]),
						*AngleText(Caustics[(Arc * 2 + 0) * 2 + 1]), *AngleText(Caustics[(Arc * 2 + 1) * 2 + 1]), Alike));
			}
			// Judged per side: the cusp line through the nearest and furthest arcs' caustics against the same line through the exact linear
			// wake. Close behind a hull of this length, after a tow this short, the start's transient is still building the near wake, so even
			// in the exact wake the largest lobes on the two arcs need not be of one wave system (then there is no line to compare with), nor
			// their line at Kelvin's angle, which each side reports beside it (information) with why it can or cannot be read here.
			const TCHAR* const Hands[] = {TEXT("port"), TEXT("starboard")};
			const int32 Last = Arcs - 1;
			for (int32 Hand = 0; Hand < 2; ++Hand)
			{
				const double NearSeen = Caustics[(0 * 2 + Hand) * 2 + 0];
				const double FarSeen = Caustics[(Last * 2 + Hand) * 2 + 0];
				const double NearTheory = Caustics[(0 * 2 + Hand) * 2 + 1];
				const double FarTheory = Caustics[(Last * 2 + Hand) * 2 + 1];
				const double SeenLine = Last > 0 && NearSeen >= 0.0 && FarSeen >= 0.0 ? CuspLineAngle(WakeArcsCm[0], NearSeen, WakeArcsCm[Last], FarSeen) : -1.0;
				const double TheoryLine = Last > 0 && NearTheory >= 0.0 && FarTheory >= 0.0 ? CuspLineAngle(WakeArcsCm[0], NearTheory, WakeArcsCm[Last], FarTheory) : -1.0;
				const bool bSeen = IsCuspLine(SeenLine);
				const bool bTheory = IsCuspLine(TheoryLine);
				const FString Label = FString::Printf(TEXT("%s %s"), *Who, Hands[Hand]);
				const FString Criterion = FString::Printf(TEXT("pass: within %.0f deg of the exact linear wake's line"), KelvinToleranceDeg);
				const FString SeenText = FString::Printf(TEXT("caustics %s deg (%.0f m) and %s deg (%.0f m)"), *AngleText(NearSeen), WakeArcsCm[0] / 100.0, *AngleText(FarSeen),
					WakeArcsCm[Last] / 100.0);
				const FString TheorySeenText = FString::Printf(TEXT("caustics %s and %s deg"), *AngleText(NearTheory), *AngleText(FarTheory));
				if (!bTheory)
				{
					// Neither having one is no measurement: the exact wake itself has no single line on these arcs at this tow length.
					Bench.Report(R.Name, Label, 0, FString::Printf(TEXT("not measured: in the exact linear wake itself the largest lobe on the %.0f m arc and the one on the %.0f m arc are of different wave systems (the bow's on one, the stern's on the other) at this tow length, so their caustics make no line to compare with (%s; measured %s%s)"),
						WakeArcsCm[0] / 100.0, WakeArcsCm[Last] / 100.0, *TheorySeenText, *SeenText, bSeen ? *FString::Printf(TEXT(", a line at %.1f deg"), SeenLine) : TEXT("")));
				}
				else if (!bSeen)
				{
					// No single line in the measured wake where the exact one has one is the solver's failing.
					Bench.Report(R.Name, Label, -1, FString::Printf(TEXT("no cusp line in the measured wake (%s: no fall to %.2f of the lobe's peak within %d deg, or the two belong to different lobes), where the exact linear wake's line lies at %.1f deg (%s; %s)"),
						*SeenText, CausticShare, WakeArcDegrees, TheoryLine, *TheorySeenText, *Criterion));
				}
				else
				{
					Bench.Report(R.Name, Label, FMath::Abs(SeenLine - TheoryLine) <= KelvinToleranceDeg ? 1 : -1,
						FString::Printf(TEXT("cusp line %.1f deg off the track, through the %s; the exact linear wake of this hull gives %.1f deg (%s): %+.1f deg (%s)"),
							SeenLine, *SeenText, TheoryLine, *TheorySeenText, SeenLine - TheoryLine, *Criterion));
				}
				// Kelvin's angle (information): there to be read on these arcs only where the exact wake's own line lies near it.
				const bool bKelvinHere = bTheory && FMath::Abs(TheoryLine - KelvinAngleDeg) <= KelvinToleranceDeg;
				const FString SeenKelvin = bSeen ? FString::Printf(TEXT("the measured line, %.1f deg, is %+.1f deg from it"), SeenLine, SeenLine - KelvinAngleDeg)
					: FString(TEXT("the measured wake has no line"));
				const FString Why = FString::Printf(TEXT("%.0f and %.0f m behind a %.1f m hull at length Froude %.2f the bow and stern wakes still overlap and the start's transient is still building them (Kelvin's angle is the far wake's, behind a point)"),
					WakeArcsCm[0] / 100.0, WakeArcsCm[Last] / 100.0, Tow.LengthCm / 100.f, LengthFroude);
				Bench.Report(R.Name, Label, 0, bKelvinHere
					? FString::Printf(TEXT("Kelvin's %.2f deg (information): measurable at this tow length, as the exact linear wake's own line, %.1f deg, lies within %.0f deg of it; %s"),
						KelvinAngleDeg, TheoryLine, KelvinToleranceDeg, *SeenKelvin)
					: bTheory
						? FString::Printf(TEXT("Kelvin's %.2f deg (information): not measurable at this tow length; the exact linear wake of this hull towed %.1f s has its line at %.1f deg on these arcs (%+.1f deg from Kelvin's), as %s; %s"),
							KelvinAngleDeg, Tow.Seconds, TheoryLine, TheoryLine - KelvinAngleDeg, *Why, *SeenKelvin)
						: FString::Printf(TEXT("Kelvin's %.2f deg (information): not measurable at this tow length; the exact linear wake of this hull towed %.1f s has no single line on these arcs (its largest lobes are of different wave systems), as %s; %s"),
							KelvinAngleDeg, Tow.Seconds, *Why, *SeenKelvin));
			}
		};
	}
	else if (Name == TEXT("ripple-evers"))
	{
		// Evers et al. (2019)'s 3D impulse-wave equations against the solver, inside their tested range: a rigid block made for the still
		// water found (its size, mass and drop follow from the traced depth h, so every limit of Table 3-3 holds), dropped into the calmed
		// live pool with the other bodies out of the way, displacement alone and then with the impulse. Its first crest, first trough and
		// first wave height at r/h 1.5, 2 and 3, on the gamma = 0 axis and at gamma = 45 deg, are read only before a reflection can arrive.
		// It is judged as the image slide an open-water drop is (FluidNinjaEvers3D::ImageSlide: half the block down a reflecting wall at its
		// centre); the whole block read as one slide is reported beside it. Judged: displacement and impulse, the shipped default (Wave
		// Impulse on) and the physical case; the displacement-only drop (the static head alone) is reported with the same numbers,
		// information only.
		// The block is destroyed after each drop; the bodies are put back after the test.
		Made->Phases.Add({TEXT("calming the pool"), 0.1f, 0.f, [Begin, LivePool](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			if (!Begin(Bench, R))
			{
				return;
			}
			R.bResetAfter = true;
			R.Phases[R.Phase].Seconds = LivePool(Bench);
		}, nullptr});
		// The bodies were put back after the last test and settle for a moment: until one is afloat and knows its water (10 s at most).
		Made->Phases.Add({TEXT("waiting for the bodies to float"), 10.f, 0.f, nullptr, nullptr, [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			const UFluidNinjaBuoyancySubsystem* Hydro = Bench.GetHydro();
			for (const UFluidNinjaBuoyancyComponent* Body : Hydro ? Hydro->GetBodies() : TArray<UFluidNinjaBuoyancyComponent*>())
			{
				FFluidNinjaBuoyancyBodyDebug Debug;
				if (Body->GetReadout().bInWater && !Body->IsAwaitingWater() && Hydro->GetBodyDebug(Body, Debug) && Debug.bHasWaterZ && !Debug.bHeld)
				{
					R.Phases[R.Phase].Seconds = 0.f;
					return;
				}
			}
		}});
		Made->Phases.Add({TEXT("planning the block"), 1.f, 0.f, [Abort, Focus, EnsureWater, LiftOthers](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			FEversPlan& Plan = R.Ripple->Evers;
			UFluidNinjaBuoyancySubsystem* Hydro = Bench.GetHydro();
			const UWorld* World = Bench.GetWorld();
			IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
			if (!Hydro || !World || !Layer)
			{
				Abort(Bench, R, TEXT("no waves"));
				return;
			}
			// Each simulation's most open water, with its water level (where the bodies afloat in it float) and the floor under that spot:
			// Evers' h is one depth, so the floor must lie about as deep as the core runs the waves there (EversLevelBed). The roomiest such
			// spot is taken (the bench reads that window from now on).
			TArray<FBox2D> Windows;
			Layer->GetBenchWindows(Windows);
			FVector2D Spot = FVector2D::ZeroVector;
			double Open = -1.0;
			float Level = 0.f;
			TArray<FString> Refused;
			for (const FBox2D& Window : Windows)
			{
				Focus(Bench, R, Window.GetCenter());
				FVector2D Try;
				double TryOpen = 0.0;
				if (!EnsureWater(Bench, R) || !R.Ripple->Water.MostOpen(R.Ripple->Water.Centre(), 1500.0, Try, TryOpen))
				{
					continue;
				}
				double LevelSum = 0.0;
				int32 LevelCount = 0;
				for (const UFluidNinjaBuoyancyComponent* Body : Hydro->GetBodies())
				{
					FFluidNinjaBuoyancyBodyDebug Debug;
					const UPrimitiveComponent* Primitive = Body->GetSimulatedPrimitive();
					if (Primitive && Window.IsInside(FVector2D(Primitive->GetComponentLocation())) && Body->GetReadout().bInWater && Hydro->GetBodyDebug(Body, Debug) && Debug.bHasWaterZ)
					{
						LevelSum += Debug.WaterZ;
						++LevelCount;
					}
				}
				if (LevelCount == 0)
				{
					Refused.Add(FString::Printf(TEXT("(%.0f, %.0f): no body afloat there knows its water"), Try.X, Try.Y));
					continue;
				}
				const float TryLevel = static_cast<float>(LevelSum / LevelCount);
				const double Depth = R.Ripple->Water.CoreAt(Try).G;
				const float Floor = Bench.TraceBedDepth(Try, TryLevel);
				if (Floor <= 0.f || FMath::Abs(Floor - Depth) > EversLevelBed * Depth)
				{
					Refused.Add(FString::Printf(TEXT("(%.0f, %.0f): the core runs %.0f cm deep, the floor lies %s"), Try.X, Try.Y, Depth,
						Floor > 0.f ? *FString::Printf(TEXT("%.0f cm down"), Floor) : TEXT("nowhere below")));
					continue;
				}
				if (TryOpen > Open)
				{
					Spot = Try;
					Open = TryOpen;
					Level = TryLevel;
				}
			}
			if (Open < 0.0)
			{
				Abort(Bench, R, TEXT("no still water with one depth found: ") + (Refused.IsEmpty() ? FString(TEXT("no simulation shows open water")) : FString::Join(Refused, TEXT("; "))));
				return;
			}
			Focus(Bench, R, Spot);
			if (!EnsureWater(Bench, R))
			{
				Abort(Bench, R, TEXT("the chosen water's Wave Aux could not be read again"));
				return;
			}
			const FWaterMap& Water = R.Ripple->Water;
			// Every other body out of the way, as ripple-kelvin does.
			LiftOthers(Bench, R, [](const UFluidNinjaBuoyancyComponent&) { return false; });
			// h is the depth the core runs the waves at there.
			const double Depth = Water.CoreAt(Spot).G;
			if (Depth < EversShallowestCm)
			{
				Abort(Bench, R, FString::Printf(TEXT("no spot fits Table 3-3: the most open water, at (%.0f, %.0f), runs %.0f cm deep in the core; the block needs at least %.0f cm (each of its image slides, 0.29 h thick, then spans at least 17 cells)"),
					Spot.X, Spot.Y, Depth, EversShallowestCm));
				return;
			}
			const double Gravity = FMath::Max(-static_cast<double>(World->GetGravityZ()), 1.0);
			const double Density = UFluidNinjaBuoyancySettings::Get().WaterDensity * 1.0e-6;   // kg/cm³
			Plan.Centre = Spot;
			Plan.WaterZ = Level;
			Plan.DepthCm = Depth;
			Plan.ClearCm = Open;
			Plan.ThicknessCm = EversThicknessShare * Depth;
			Plan.WidthCm = EversWidthShare * Depth;
			Plan.HeightCm = EversVolumeShare * Depth * Depth / Plan.ThicknessCm;
			Plan.MassKg = EversDensity * Density * Plan.ThicknessCm * Plan.WidthCm * Plan.HeightCm;
			Plan.DropCm = EversDropShare * Depth;
			Plan.ReachCm = 0.5 * FMath::Sqrt(FMath::Square(Plan.ThicknessCm) + FMath::Square(Plan.WidthCm));
			if (!Water.IsOpen(Spot, Plan.ReachCm + 50.0))
			{
				Abort(Bench, R, FString::Printf(TEXT("no spot fits Table 3-3: the most open water, at (%.0f, %.0f), has %.1f m of open water around it, and the %.1f x %.1f m block needs %.1f m"),
					Spot.X, Spot.Y, Open / 100.0, Plan.ThicknessCm / 100.0, Plan.WidthCm / 100.0, (Plan.ReachCm + 50.0) / 100.0));
				return;
			}
			// The manual's prediction for the planned drop (its bottom let go DropCm above the water meets it at √(2 g DropCm)), read as the
			// image slide the drop is (half the block down a reflecting wall at its centre, as it is judged): the impact radius, and the first
			// crest's celerity and period, for where the probes can be and how long they can read before a reflection.
			FluidNinjaEvers3D::FInput In;
			In.SlideVelocity = FMath::Sqrt(2.0 * Gravity * Plan.DropCm);
			In.SlideThickness = Plan.ThicknessCm;
			In.SlideWidth = Plan.WidthCm;
			In.SlideMass = Plan.MassKg;
			In.SlideVolume = Plan.ThicknessCm * Plan.WidthCm * Plan.HeightCm;
			In.ImpactAngleDeg = 90.0;
			In.WaterDepth = Depth;
			In.WaterDensity = Density;
			In.Gravity = Gravity;
			const FluidNinjaEvers3D::FParams Prm = FluidNinjaEvers3D::ComputeParams(FluidNinjaEvers3D::ImageSlide(In));
			const uint32 DropFlags = FluidNinjaEvers3D::CheckRanges(Prm, 90.0, EversRadiusShares[0], 0.0, false);
			if (DropFlags != FluidNinjaEvers3D::Range_None)
			{
				Abort(Bench, R, FString::Printf(TEXT("no spot fits Table 3-3: the block planned for h %.0f cm, read as its image slide (s/2, m/2, Vol/2), is outside %s"), Depth, *FluidNinjaEvers3D::RangeNames(DropFlags)));
				return;
			}
			// The core's own fastest wave, its longest at that depth (CoreFastestSpeed): a reflection is never taken to come back slower than
			// that, however slow the manual's crest is.
			Plan.FastestCmS = CoreFastestSpeed(Depth, Bench.GetWaves()->GetWindowCm());
			// Every cell that is not open water (walls, shores, rocks, and the map's edge, where the sponge starts): a reflection's shortest way
			// to a probe runs from the block's edge to one of them and on to the probe, less one map cell (a wall's face may lie that much
			// nearer than the centre of the first cell the trace found it in).
			TArray<FVector2D> Walls;
			for (int32 Y = -1; Y <= Water.Size; ++Y)
			{
				for (int32 X = -1; X <= Water.Size; ++X)
				{
					const bool bInside = X >= 0 && Y >= 0 && X < Water.Size && Y < Water.Size;
					if (!bInside || Water.Depth[Y * Water.Size + X] < OpenDepthCm)
					{
						Walls.Add(Water.Min + FVector2D(X * Water.Step, Y * Water.Step));
					}
				}
			}
			auto ReflectPath = [&Walls, &Plan, &Water](const FVector2D& Point)
			{
				double Shortest = TNumericLimits<double>::Max();
				for (const FVector2D& Wall : Walls)
				{
					Shortest = FMath::Min(Shortest, FMath::Max(FVector2D::Distance(Plan.Centre, Wall) - Plan.ReachCm, 0.0) + FVector2D::Distance(Wall, Point));
				}
				return FMath::Max(Shortest - Water.Step, 0.0);
			};
			// The heading (every 15 deg) whose probes can read the most before a reflection: troughs first (they come later), then crests,
			// then probes at all. A crest has passed a probe a sixth of a period after it (the record back to half of it), a trough two thirds.
			struct FHeading
			{
				double Yaw = 0.0;
				int32 Crests = 0;
				int32 Troughs = 0;
				double LongestSeconds = 0.0;
				TArray<FVector2D> Points;
				TArray<double> Radii;
				TArray<double> Gammas;
				TArray<double> Depths;
				TArray<double> ReflectCm;
				TArray<FString> LeftOut;
			};
			FHeading Chosen;
			bool bChosen = false;
			for (int32 Turn = 0; Turn < 12; ++Turn)
			{
				FHeading Try;
				Try.Yaw = Turn * 15.0;
				const FVector2D Axis = FVector2D(1.0, 0.0).GetRotated(Try.Yaw);
				const FVector2D Ways[] = {Axis, -Axis, Axis.GetRotated(45.0), Axis.GetRotated(-45.0), (-Axis).GetRotated(45.0), (-Axis).GetRotated(-45.0)};
				const double WayGammas[] = {0.0, 0.0, 45.0, 45.0, 45.0, 45.0};
				for (int32 Way = 0; Way < static_cast<int32>(UE_ARRAY_COUNT(Ways)); ++Way)
				{
					for (const double Share : EversRadiusShares)
					{
						const double Radius = Share * Depth;
						const FString Where = FString::Printf(TEXT("gamma %.0f r/h %.1f"), WayGammas[Way], Share);
						const FluidNinjaEvers3D::FWave Wave = FluidNinjaEvers3D::Predict(Prm, Plan.WidthCm, Depth, Gravity, Radius, WayGammas[Way]);
						if (Wave.bInsideImpactZone)
						{
							Try.LeftOut.AddUnique(Where + FString::Printf(TEXT(" inside the impact zone (r0 %.0f cm)"), Wave.R0));
							continue;
						}
						// Open water all the way out from the block's edge, over a level bed.
						const FVector2D Point = Spot + Ways[Way] * Radius;
						bool bOpen = true;
						for (double Along = Plan.ReachCm; Along <= Radius && bOpen; Along += 50.0)
						{
							bOpen = Water.DepthAt(Spot + Ways[Way] * Along) >= OpenDepthCm;
						}
						const double Bed = Water.DepthAt(Point);
						if (!bOpen || Bed < OpenDepthCm)
						{
							Try.LeftOut.Add(Where + TEXT(" not over open water"));
							continue;
						}
						if (FMath::Abs(Bed - Depth) > EversLevelBed * Depth)
						{
							Try.LeftOut.Add(Where + FString::Printf(TEXT(" where the core runs %.0f cm deep (not level with h)"), Bed));
							continue;
						}
						const double Reflect = ReflectPath(Point);
						const double Until = Reflect / FMath::Max(Plan.FastestCmS, Wave.CC1 * (1.0 + EversCelerityAbove));
						const double Due = Radius / Wave.CC1;
						Try.Crests += Until >= Due + Wave.T1 / 6.0 ? 1 : 0;
						Try.Troughs += Until >= Due + 2.0 * Wave.T1 / 3.0 ? 1 : 0;
						Try.LongestSeconds = FMath::Max(Try.LongestSeconds, Until);
						Try.Points.Add(Point);
						Try.Radii.Add(Radius);
						Try.Gammas.Add(WayGammas[Way]);
						Try.Depths.Add(Bed);
						Try.ReflectCm.Add(Reflect);
					}
				}
				const bool bBetter = !bChosen || Try.Troughs > Chosen.Troughs || (Try.Troughs == Chosen.Troughs
					&& (Try.Crests > Chosen.Crests || (Try.Crests == Chosen.Crests && Try.Points.Num() > Chosen.Points.Num())));
				if (bBetter)
				{
					Chosen = MoveTemp(Try);
					bChosen = true;
				}
			}
			const FluidNinjaEvers3D::FWave Nearest = FluidNinjaEvers3D::Predict(Prm, Plan.WidthCm, Depth, Gravity, EversRadiusShares[0] * Depth, 0.0);
			if (Chosen.Points.IsEmpty())
			{
				Abort(Bench, R, FString::Printf(TEXT("no spot fits Table 3-3: around the most open water, at (%.0f, %.0f), %.0f cm deep, no probe at r/h 1.5, 2 or 3 lies over open water of that depth outside the impact zone, in any heading (%s)"),
					Spot.X, Spot.Y, Depth, *FString::Join(Chosen.LeftOut, TEXT("; "))));
				return;
			}
			if (Chosen.Crests == 0)
			{
				Abort(Bench, R, FString::Printf(TEXT("no room: with the block at the most open water, at (%.0f, %.0f), %.0f cm deep with %.1f m of open water around it, a reflection can reach every probe before the first crest has passed it, in every heading (the first wave's period T1 is about %.1f s and its crest moves at %.1f m/s, so the crest %.1f m out passes about %.1f s after the impact; a reflection is taken to come back at %.1f m/s, the faster of the core's own fastest wave, its longest at %.0f cm, and that crest's celerity with its +10%% scatter), so the manual's predictions cannot be read here"),
					Spot.X, Spot.Y, Depth, Open / 100.0, Nearest.T1, Nearest.CC1 / 100.0, EversRadiusShares[0] * Depth / 100.0, EversRadiusShares[0] * Depth / FMath::Max(Nearest.CC1, 1.0) + Nearest.T1 / 6.0,
					FMath::Max(Plan.FastestCmS, Nearest.CC1 * (1.0 + EversCelerityAbove)) / 100.0, Depth));
				return;
			}
			Plan.YawDegrees = Chosen.Yaw;
			Plan.Axis = FVector2D(1.0, 0.0).GetRotated(Chosen.Yaw);
			Plan.Points = MoveTemp(Chosen.Points);
			Plan.Radii = MoveTemp(Chosen.Radii);
			Plan.Gammas = MoveTemp(Chosen.Gammas);
			Plan.Depths = MoveTemp(Chosen.Depths);
			Plan.ReflectCm = MoveTemp(Chosen.ReflectCm);
			Plan.LeftOut = MoveTemp(Chosen.LeftOut);
			// Its fall, then until the last probe's reflection can arrive (and the readbacks' few frames).
			Plan.RecordSeconds = FMath::Clamp(FMath::Sqrt(2.0 * Plan.DropCm / Gravity) + Chosen.LongestSeconds + 0.5, 3.0, 10.0);
			Plan.bPlanned = true;
		}, nullptr});
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			FDropRun& Planned = Made->Ripple->Drops.AddDefaulted_GetRef();
			Planned.bImpulse = Pass == 1;
			Planned.Name = TEXT("block");
			const int32 Index = Pass;
			// Every frame from the fresh field on: the probes, the frame's length and the solver's bands (from the drop on), and the block
			// until it meets the water.
			auto Follow = [Index, FollowFall](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
			{
				FDropRun& Fall = R.Ripple->Drops[Index];
				const IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
				const UWorld* World = Bench.GetWorld();
				if (!Layer || !World)
				{
					return;
				}
				Fall.Log.Take(*Layer);
				if (Fall.DropTime >= 0.0)
				{
					Fall.LongestFrame = FMath::Max(Fall.LongestFrame, Bench.FrameSeconds);
				}
				FollowFall(Fall, *World);
			};
			AddClear(Planned.bImpulse ? TEXT("fresh ripples, displacement and impulse") : TEXT("fresh ripples, displacement only"), [Index](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
			{
				FDropRun& Fall = R.Ripple->Drops[Index];
				const FEversPlan& Plan = R.Ripple->Evers;
				IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
				if (!Layer || !Plan.bPlanned)
				{
					return;
				}
				SetWaveSources(true, Fall.bImpulse);
				SetWaveCurrentCoupling(0.f);   // still water's waves: not carried by FluidNinja's current
				Fall.Log = FProbeLog();
				Fall.Log.Points = Plan.Points;
				Fall.Radii = Plan.Radii;
				Fall.Gammas = Plan.Gammas;
				Fall.WaterZ = Plan.WaterZ;
				Fall.DepthCm = static_cast<float>(Plan.DepthCm);
				Fall.bDepthKnown = true;
				Fall.FastestCmS = Plan.FastestCmS;
				Layer->SetProbes(Fall.Log.Points);
			}, Follow);
			// The block made and let go: an engine cube scaled to it, floated by FluidNinja Buoyancy like any body.
			Made->Phases.Add({TEXT("block dropped"), 8.f, 0.f, [Index, Abort](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
			{
				FDropRun& Fall = R.Ripple->Drops[Index];
				const FEversPlan& Plan = R.Ripple->Evers;
				UWorld* World = Bench.GetWorld();
				if (!Plan.bPlanned || !World)
				{
					return;
				}
				UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
				if (!Cube)
				{
					Abort(Bench, R, TEXT("the engine's cube (/Engine/BasicShapes/Cube) did not load"));
					return;
				}
				// The cube is 100 cm on a side about its centre, so its scale is the block's size in metres: its x (the thickness s) along the
				// gamma = 0 axis, its bottom DropCm above the water.
				const FTransform Placed(FRotator(0.0, Plan.YawDegrees, 0.0), FVector(Plan.Centre, Plan.WaterZ + Plan.DropCm + 0.5 * Plan.HeightCm),
					FVector(Plan.ThicknessCm, Plan.WidthCm, Plan.HeightCm) / 100.0);
				AStaticMeshActor* Block = World->SpawnActorDeferred<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Placed, nullptr, nullptr,
					ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
				UStaticMeshComponent* Mesh = Block ? Block->GetStaticMeshComponent() : nullptr;
				if (!Mesh)
				{
					Abort(Bench, R, TEXT("the block could not be made"));
					return;
				}
				Mesh->SetMobility(EComponentMobility::Movable);
				Mesh->SetStaticMesh(Cube);
				// Collision from the cube's simple box; its mass from the one density the manual tested (Body Mass keeps it). Setting the
				// profile also turns the static mesh actor's default collision off (UStaticMeshComponent::SetCollisionProfileName clears
				// bUseDefaultCollision), so registering it does not put the cube's own BlockAll back.
				Mesh->SetCollisionProfileName(UCollisionProfile::PhysicsActor_ProfileName);
				Mesh->BodyInstance.SetMassOverride(static_cast<float>(Plan.MassKg), true);
				Block->FinishSpawning(Placed);
				R.Spawned.Add(Block);
				Fall.Spawned = Block;
#if WITH_EDITOR
				Block->SetActorLabel(TEXT("FluidNinja Buoyancy test block"));
#endif
				// Registered after the actor began play, so the component's own play begins as it registers: its hull from the box, its
				// physics on, FluidNinja Buoyancy floating it; let go at once (no settling), making ripples.
				UFluidNinjaBuoyancyComponent* Floating = NewObject<UFluidNinjaBuoyancyComponent>(Block, TEXT("FluidNinjaBuoyancyTestBlockBuoyancy"));
				Floating->HullModel = EFluidNinjaHullModel::Triangles;
				Floating->MassMode = EFluidNinjaBuoyancyMass::BodyMass;
				Floating->bSettleOnStart = false;
				Floating->bWaves = true;
				Block->AddInstanceComponent(Floating);
				Floating->RegisterComponent();
				FBenchHull BlockHull;
				if (!Floating->HasBegunPlay() || !GetBenchHull(*Floating, BlockHull))
				{
					Abort(Bench, R, TEXT("FluidNinja Buoyancy could not float the block (no hull from its collision; the log has its warning)"));
					return;
				}
				Fall.Body = Floating;
				Fall.DroppedMassKg = Mesh->GetMass();
				Fall.DropTime = World->GetTimeSeconds();
				Fall.LastBottom = Floating->GetLowestPointZ();
				Fall.LastFallSpeed = 0.f;
				Fall.bImpacted = false;
				R.Phases[R.Phase].Seconds = static_cast<float>(Plan.RecordSeconds);
			}, [Index](FFluidNinjaBuoyancyTestBench&, FRun& R)
			{
				FDropRun& Fall = R.Ripple->Drops[Index];
				if (const UFluidNinjaBuoyancyComponent* Body = Fall.Body.Get())
				{
					Fall.DisplacedLitres = Body->GetReadout().DisplacedLitres;
				}
				if (AActor* Block = Fall.Spawned.Get())
				{
					Block->Destroy();
				}
			}, Follow});
			if (Pass == 0)
			{
				// The pool's own water stills again before the second block (the first one's wakes stirred it).
				Made->Phases.Add({TEXT("the pool settling"), 4.f, 0.f, nullptr, nullptr});
			}
		}
		Made->Evaluate = [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			const FEversPlan& Plan = R.Ripple->Evers;
			if (!Plan.bPlanned)
			{
				return;   // the planning said why
			}
			const UWorld* World = Bench.GetWorld();
			const double Gravity = World ? FMath::Max(-static_cast<double>(World->GetGravityZ()), 1.0) : 980.0;
			const double Density = UFluidNinjaBuoyancySettings::Get().WaterDensity * 1.0e-6;   // kg/cm³
			const double StepRate = Bench.GetWaves() ? Bench.GetWaves()->GetStepsPerSecond() : 60.0;
			FString Shares;
			for (const double Share : EversRadiusShares)
			{
				Shares += FString::Printf(TEXT("%s%.1f"), Shares.IsEmpty() ? TEXT("") : TEXT(", "), Share);
			}
			const FString LeftOut = Plan.LeftOut.IsEmpty() ? FString() : TEXT("; left out: ") + FString::Join(Plan.LeftOut, TEXT(", "));
			Bench.Report(R.Name, TEXT("block"), 0, FString::Printf(TEXT("a rigid block %.0f x %.0f x %.0f cm (s along gamma = 0, b across it, its height), %.0f kg (D %.3f, the one density the manual's tests used), its bottom let go %.0f cm above still water %.0f cm deep (h, the depth the core runs at (%.0f, %.0f), %.1f m of open water around; its axis at %.0f deg); %d probes at r/h %s on the gamma = 0 axis and at gamma = 45 deg, r from its centre%s. Judged: displacement and impulse, the shipped default (Wave Impulse on) and the physical case; the displacement-only drop (the static head alone) is reported with the same numbers, information only. Judged per probe as the image slide an open-water drop is (half the block, s/2, m/2 and Vol/2 with the same b, down a reflecting wall at its centre: Evers' 3D case is a slide entering at a straight shore, at alpha 90 deg a vertical wall, and the drop is mirror-symmetric about that plane) against Evers et al. (2019) Eqs. 3.29-3.30 (p. 33) within the scatter of Table 4-5 (p. 74): a_c1 -45%% / +25%%, a_t1 -25%% / +40%% (the trough after the first crest), and H1 = a_c1 + a_t1 within the sum of the two (the scatters of combined equations add, p. 73), each read only from the readings before a reflection can arrive (the shortest way to the probe by a wall, shore or the window's sponge, at the faster of the core's own fastest wave, its longest at that depth (%.1f m/s), and the manual's first crest celerity c_c1, Eq. 3.32, with its +10%% scatter); the whole block read as one slide is reported beside it, information only. Per probe also, information: when its first crest and trough came after the impact, the first wave's period (first crest to the next, else twice first crest to first trough) and its length at the depth the core runs there (the core's dispersion), beside Evers' T1 and L1"),
				Plan.ThicknessCm, Plan.WidthCm, Plan.HeightCm, Plan.MassKg, EversDensity, Plan.DropCm, Plan.DepthCm, Plan.Centre.X, Plan.Centre.Y, Plan.ClearCm / 100.0,
				Plan.YawDegrees, Plan.Points.Num(), *Shares, *LeftOut, Plan.FastestCmS / 100.0));
			// The judged drop's measures (the one with the impulse), and the displacement-only drop's apart (information).
			int32 Judged = 0;
			int32 Within = 0;
			int32 Unread = 0;
			int32 Extrapolated = 0;
			int32 InfoRead = 0;
			int32 InfoWithin = 0;
			for (const FDropRun& Fall : R.Ripple->Drops)
			{
				const FString Source = Fall.bImpulse ? TEXT("displacement and impulse") : TEXT("displacement only");
				if (Fall.DropTime < 0.0)
				{
					Bench.Report(R.Name, TEXT("block"), 0, Source + TEXT(": not dropped (the block was not made)"));
					continue;
				}
				if (!Fall.bImpacted)
				{
					Bench.Report(R.Name, TEXT("block"), 0, Source + TEXT(": not measured, the block was not seen to reach the water"));
					continue;
				}
				// At most four steps a frame: a longer frame drops time (the waves slow for it), which no comparison may see.
				if (Fall.LongestFrame >= 4.0 / StepRate)
				{
					Bench.Report(R.Name, TEXT("block"), 0, FString::Printf(TEXT("%s: not measured, a frame took %.0f ms while the rings were read; the solver runs at most 4 steps (%.0f ms) a frame and dropped time (an editor in the background runs at 3 fps)"),
						*Source, Fall.LongestFrame * 1000.f, 4000.0 / StepRate));
					continue;
				}
				// The reflections' cut-off is the core's fastest wave at the drop's depth.
				if (Fall.FastestCmS <= 0.0)
				{
					Bench.Report(R.Name, TEXT("block"), 0, Source + TEXT(": not measured, the core's depth was not known at the drop, so when a reflection can arrive is unknown"));
					continue;
				}
				// The drop as it happened: the speed it met the water at, and its mass. Judged as the image slide it is: an open-water drop is
				// two slides down a reflecting wall at the block's centre, each of half its thickness, mass and volume (FluidNinjaEvers3D::ImageSlide).
				// Beside it, the direct reading of the whole block as one slide, information only (it predicts the waves of a block twice as
				// thick and heavy).
				FluidNinjaEvers3D::FInput In;
				In.SlideVelocity = Fall.ImpactSpeed;
				In.SlideThickness = Plan.ThicknessCm;
				In.SlideWidth = Plan.WidthCm;
				In.SlideMass = Fall.DroppedMassKg;
				In.SlideVolume = Plan.ThicknessCm * Plan.WidthCm * Plan.HeightCm;
				In.ImpactAngleDeg = 90.0;
				In.WaterDepth = Plan.DepthCm;
				In.WaterDensity = Density;
				In.Gravity = Gravity;
				const FluidNinjaEvers3D::FParams Prm = FluidNinjaEvers3D::ComputeParams(FluidNinjaEvers3D::ImageSlide(In));
				const uint32 DropFlags = FluidNinjaEvers3D::CheckRanges(Prm, 90.0, EversRadiusShares[0], 0.0, false);
				const FluidNinjaEvers3D::FParams Direct = FluidNinjaEvers3D::ComputeParams(In);
				const uint32 DirectFlags = FluidNinjaEvers3D::CheckRanges(Direct, 90.0, EversRadiusShares[0], 0.0, false);
				// It must weigh what the plan made it: Table 3-3's density is one tested value (1.338), and the range check accepts any
				// within 0.59-1.72, so a mass override that did not hold would have its lines judged at a density nobody tested.
				const bool bMassHeld = FMath::Abs(Fall.DroppedMassKg - Plan.MassKg) <= 0.01 * Plan.MassKg;
				if (!bMassHeld)
				{
					Bench.Report(R.Name, TEXT("block"), 0, FString::Printf(TEXT("%s: its lines are reported, not judged: the block weighed %.0f kg, not the planned %.0f kg, so its density (D %.3f) is not the 1.338 the manual tested"),
						*Source, Fall.DroppedMassKg, Plan.MassKg, Prm.D));
				}
				Bench.Report(R.Name, TEXT("block"), 0, FString::Printf(TEXT("%s: it met the water at %.2f m/s, %.0f kg; as its image slide (s/2 %.0f cm, m/2 %.0f kg, b %.0f cm, down a wall at its centre): F %.2f, S %.2f, M %.3f, B %.2f, V %.3f, D %.3f, alpha 90 deg, P %.3f; Table 3-3: %s (its relative granulate density, 2.429, is a granular slide's, which a rigid block has none of). The direct reading (the whole block as one slide), information only: S %.2f, M %.3f, V %.3f, P %.3f, %s"),
					*Source, Fall.ImpactSpeed / 100.f, Fall.DroppedMassKg, 0.5 * Plan.ThicknessCm, 0.5 * Fall.DroppedMassKg, Plan.WidthCm, Prm.F, Prm.S, Prm.M, Prm.B, Prm.V, Prm.D, Prm.P,
					DropFlags == FluidNinjaEvers3D::Range_None ? TEXT("inside every limit") : *(TEXT("outside ") + FluidNinjaEvers3D::RangeNames(DropFlags) + TEXT(", so its lines are reported, not judged")),
					Direct.S, Direct.M, Direct.V, Direct.P, DirectFlags == FluidNinjaEvers3D::Range_None ? TEXT("inside every limit") : *(TEXT("outside ") + FluidNinjaEvers3D::RangeNames(DirectFlags))));
				for (int32 Probe = 0; Probe < Fall.Radii.Num() && Probe < Plan.ReflectCm.Num(); ++Probe)
				{
					const double Radius = Fall.Radii[Probe];
					const double Gamma = Fall.Gammas[Probe];
					const FString Label = FString::Printf(TEXT("block r %.1f m gamma %.0f, %s"), Radius / 100.0, Gamma, Fall.bImpulse ? TEXT("both") : TEXT("displacement"));
					// The probe's record after the drop, less its level in the second before it.
					double Level = 0.0;
					int32 LevelCount = 0;
					TArray<double> Times;
					TArray<double> Z;
					for (int32 Reading = 0; Reading < Fall.Log.Times.Num(); ++Reading)
					{
						const double Time = Fall.Log.Times[Reading];
						const double Height = Fall.Log.Heights[Reading].IsValidIndex(Probe) ? Fall.Log.Heights[Reading][Probe] : 0.0;
						if (Time <= Fall.DropTime && Time >= Fall.DropTime - 1.0)
						{
							Level += Height;
							++LevelCount;
						}
						else if (Time > Fall.DropTime)
						{
							Times.Add(Time);
							Z.Add(Height);
						}
					}
					const double Rest = LevelCount > 0 ? Level / LevelCount : 0.0;
					for (double& Value : Z)
					{
						Value -= Rest;
					}
					const FluidNinjaEvers3D::FWave Wave = FluidNinjaEvers3D::Predict(Prm, Plan.WidthCm, Plan.DepthCm, Gravity, Radius, Gamma);
					const uint32 Flags = FluidNinjaEvers3D::CheckRanges(Prm, 90.0, Radius / Plan.DepthCm, Gamma, Wave.bInsideImpactZone);
					if (Wave.bInsideImpactZone)
					{
						Bench.Report(R.Name, Label, 0, FString::Printf(TEXT("%s: inside the impact zone at the drop's own inputs (r0 %.0f cm), no prediction"), *Source, Wave.R0));
						continue;
					}
					// Read until a reflection can arrive: its shortest way at the manual's celerity with its upper scatter, or at the solver's own
					// fastest wave where that is faster (in the bands it ran from this drop on).
					const double Fastest = FMath::Max(Fall.FastestCmS, Wave.CC1 * (1.0 + EversCelerityAbove));
					const double ReadSeconds = Plan.ReflectCm[Probe] / Fastest;
					const FWindowedLobes Lobes = MeasureLobesUntil(Times, Z, Fall.ImpactTime + ReadSeconds);
					const double CrestLow = Wave.AC1 * (1.0 - FluidNinjaEvers3D::CrestBelow);
					const double CrestHigh = Wave.AC1 * (1.0 + FluidNinjaEvers3D::CrestAbove);
					const double TroughLow = Wave.AT1 * (1.0 - FluidNinjaEvers3D::TroughBelow);
					const double TroughHigh = Wave.AT1 * (1.0 + FluidNinjaEvers3D::TroughAbove);
					const bool bHeight = Lobes.bCrest && Lobes.bTrough;
					const double Height1 = Lobes.FirstCrest + Lobes.FirstTrough;
					const bool bCrestIn = Lobes.FirstCrest >= CrestLow && Lobes.FirstCrest <= CrestHigh;
					const bool bTroughIn = Lobes.FirstTrough >= TroughLow && Lobes.FirstTrough <= TroughHigh;
					const bool bHeightIn = Height1 >= CrestLow + TroughLow && Height1 <= CrestHigh + TroughHigh;
					auto Measure = [](bool bRead, double Value, bool bIn)
					{
						return bRead ? FString::Printf(TEXT("%.2f (%s)"), Value, bIn ? TEXT("inside") : TEXT("OUTSIDE")) : FString(TEXT("not read (a reflection could arrive before it had passed)"));
					};
					const FluidNinjaEvers3D::FWave DirectWave = FluidNinjaEvers3D::Predict(Direct, Plan.WidthCm, Plan.DepthCm, Gravity, Radius, Gamma);
					const FString DirectText = DirectWave.bInsideImpactZone ? FString(TEXT("inside its impact zone"))
						: FString::Printf(TEXT("a_c1 %.2f, a_t1 %.2f, H1 %.2f"), DirectWave.AC1, DirectWave.AT1, DirectWave.H1);
					const int32 Read = (Lobes.bCrest ? 1 : 0) + (Lobes.bTrough ? 1 : 0) + (bHeight ? 1 : 0);
					const int32 Inside = (Lobes.bCrest && bCrestIn ? 1 : 0) + (Lobes.bTrough && bTroughIn ? 1 : 0) + (bHeight && bHeightIn ? 1 : 0);
					// Judged: displacement and impulse, the shipped default (Wave Impulse on) and the physical case; the displacement-only drop
					// (the static head alone) is counted apart and reported with the same numbers, information only.
					const bool bInTable = bMassHeld && Flags == FluidNinjaEvers3D::Range_None;
					const bool bJudge = Fall.bImpulse && bInTable && Read > 0;
					if (bJudge)
					{
						Judged += Read;
						Within += Inside;
					}
					else if (!Fall.bImpulse && bInTable)
					{
						InfoRead += Read;
						InfoWithin += Inside;
					}
					if (Fall.bImpulse)
					{
						Extrapolated += bInTable ? 0 : Read;
						Unread += 3 - Read;
					}
					const FString Note = Flags != FluidNinjaEvers3D::Range_None ? TEXT(" [outside Table 3-3: ") + FluidNinjaEvers3D::RangeNames(Flags) + TEXT("; reported, not judged]")
						: !bMassHeld ? FString(TEXT(" [not the planned mass, so not the tested density: reported, not judged]"))
						: Read == 0 ? FString(TEXT(" [nothing read before a reflection could arrive, so not judged]"))
						: !Fall.bImpulse ? FString(TEXT(" [the displacement-only drop (the static head alone): reported with the same numbers, information only; judged: displacement and impulse, the shipped default (Wave Impulse on) and the physical case]"))
						: FString();
					// When its first crest and trough came (after the impact), the first wave's period from them, and its length at the depth
					// the core runs under the probe (the core's own dispersion, CoreWaveNumber), beside Evers' first wave: a period much
					// shorter than T1 says the crest read is the impact's short near-field waves, not the long first wave the manual measured.
					const double ProbeDepth = Plan.Depths.IsValidIndex(Probe) && Plan.Depths[Probe] > 0.0 ? Plan.Depths[Probe] : Plan.DepthCm;
					double Period = 0.0;
					const TCHAR* PeriodFrom = TEXT("");
					if (Lobes.bCrest && Lobes.bNextCrest && Lobes.NextCrestTime > Lobes.CrestTime)
					{
						Period = Lobes.NextCrestTime - Lobes.CrestTime;
						PeriodFrom = TEXT("first crest to the next");
					}
					else if (Lobes.bCrest && Lobes.bTrough && Lobes.TroughTime > Lobes.CrestTime)
					{
						Period = 2.0 * (Lobes.TroughTime - Lobes.CrestTime);
						PeriodFrom = TEXT("twice first crest to first trough");
					}
					const FString CrestAt = Lobes.bCrest ? FString::Printf(TEXT("%.2f s"), Lobes.CrestTime - Fall.ImpactTime) : FString(TEXT("not read"));
					const FString TroughAt = Lobes.bTrough ? FString::Printf(TEXT("%.2f s"), Lobes.TroughTime - Fall.ImpactTime) : FString(TEXT("not read"));
					const FString WaveText = Period > 0.0
						? FString::Printf(TEXT("period %.2f s (%s), a %.2f m wave at the core's %.0f cm"), Period, PeriodFrom,
							2.0 * UE_DOUBLE_PI / CoreWaveNumber(2.0 * UE_DOUBLE_PI / Period, ProbeDepth) / 100.0, ProbeDepth)
						: FString(TEXT("period not read (no whole first crest and trough before a reflection could arrive)"));
					const FString Timing = FString::Printf(TEXT("measured, information: first crest %s and first trough %s after the impact, %s; Evers' first wave: T1 %.2f s, L1 %.1f m"),
						*CrestAt, *TroughAt, *WaveText, Wave.T1, Wave.L1 / 100.0);
					Bench.Report(R.Name, Label, bJudge ? (Inside == Read ? 1 : -1) : 0,
						FString::Printf(TEXT("%s, cm, r/h %.2f (r* %.0f cm): first crest %s against Evers (image slide) %.2f (%.2f-%.2f); first trough %s against %.2f (%.2f-%.2f); H1 %s against %.2f (%.2f-%.2f); read for %.2f s after the impact (a reflection's shortest way %.1f m at %.2f m/s; the crest due at %.2f s, r / c_c1, period T1 %.1f s); %s; the direct reading (the whole block as one slide) gives %s%s"),
							*Source, Radius / Plan.DepthCm, Wave.RStar, *Measure(Lobes.bCrest, Lobes.FirstCrest, bCrestIn), Wave.AC1, CrestLow, CrestHigh,
							*Measure(Lobes.bTrough, Lobes.FirstTrough, bTroughIn), Wave.AT1, TroughLow, TroughHigh, *Measure(bHeight, Height1, bHeightIn), Wave.H1,
							CrestLow + TroughLow, CrestHigh + TroughHigh, ReadSeconds, Plan.ReflectCm[Probe] / 100.0, Fastest / 100.0, Radius / Wave.CC1, Wave.T1, *Timing,
							*DirectText, *Note));
				}
			}
			// The validation in one line: how many of the judged drop's measures were judged, and how many were inside the manual's
			// scatter; the displacement-only drop's count beside it, information only.
			const FString InfoText = FString::Printf(TEXT("; information, not judged: the displacement-only drop (the static head alone) had %d of %d measures read inside Table 3-3 within the scatter"),
				InfoWithin, InfoRead);
			Bench.Report(R.Name, TEXT("Evers (2019)"), 0, Judged > 0
				? FString::Printf(TEXT("the drop with the impulse (displacement and impulse, the judged one): %d of %d measures read inside Table 3-3 lay within the scatter of Table 4-5 (%d not read before a reflection could arrive, %d read outside Table 3-3 or at another mass and reported only)%s"),
					Within, Judged, Unread, Extrapolated, *InfoText)
				: FString::Printf(TEXT("NOT JUDGED: no first crest, trough or wave height of the drop with the impulse (the judged one) was read inside Table 3-3 before a reflection could arrive (%d not read in time, %d read outside Table 3-3 or at another mass and reported only)%s"),
					Unread, Extrapolated, *InfoText));
		};
	}
	else if (Name == TEXT("ripple-advection"))
	{
		// A poke in the river's live current, its ring followed along a line of probes down the current: how far its centre drifts against
		// how far the local current (under the river floater it was poked beside) would carry it; then the same poke at the same spot with
		// the waves not carried (User.Wave.CurrentCoupling 0: still water to them, with the same depth and banks). Information only.
		Made->Phases.Add({TEXT("starting the river"), 0.1f, 0.f, [Begin, Abort](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			if (!Begin(Bench, R))
			{
				return;
			}
			if (!Bench.HasRiver())
			{
				Abort(Bench, R, TEXT("this level has no river (an actor with StartRiver / StopRiver and a SimulationTag, as the demo's river) to carry a ring"));
				return;
			}
			Bench.SetControlledWater(false);
			const float Zero = 0.f;
			Bench.SetTestWind(true, 0.f, &Zero);
			if (!Bench.IsRiverRunning())
			{
				// Started as the River menu's Start does; after the test it is stopped again and its floaters put back where they waited.
				if (UWorld* World = Bench.GetWorld())
				{
					for (TActorIterator<AActor> It(World); It; ++It)
					{
						UFluidNinjaBuoyancyComponent* Floater = It->ActorHasTag(RiverFloaterTag) ? It->FindComponentByClass<UFluidNinjaBuoyancyComponent>() : nullptr;
						if (const UPrimitiveComponent* Primitive = Floater ? Floater->GetSimulatedPrimitive() : nullptr)
						{
							R.RiverPlaced.Add({Floater, FTransform(Primitive->GetComponentQuat(), Primitive->GetComponentLocation())});
						}
					}
				}
				Bench.SetRiverRunning(true);
				R.bRiverStartedByTest = true;
				R.Phases[R.Phase].Seconds = static_cast<float>(AdvectSpinUpSeconds);
			}
		}, nullptr});
		AddClear(TEXT("fresh ripples, the poke alone (no hull or swimmer source)"), [](FFluidNinjaBuoyancyTestBench& Bench, FRun&)
		{
			SetWaveCurrentCoupling(1.f);
			SetWaveSources(false, false);
			if (IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves())
			{
				Layer->SetProbes(TArray<FVector2D>());
			}
		}, nullptr);
		// Every frame of a ring: the probes, the frame's length, and the current under the floater.
		auto Follow = [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			if (R.Ripple->Advects.IsEmpty())
			{
				return;
			}
			FAdvectRun& Ring = R.Ripple->Advects.Last();
			const IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
			const UWorld* World = Bench.GetWorld();
			if (Layer)
			{
				Ring.Log.Take(*Layer);
			}
			Ring.LongestFrame = FMath::Max(Ring.LongestFrame, Bench.FrameSeconds);
			if (const UFluidNinjaBuoyancyComponent* Floater = R.Ripple->AdvectPlan.Floater.Get(); Floater && World)
			{
				Ring.FlowTimes.Add(World->GetTimeSeconds());
				Ring.Flows.Add(Floater->GetReadout().FlowCmS);
			}
		};
		// The poke and its probes in one frame (readings stamped from this frame on are of these probes, and this frame's steps have the hump).
		auto PokeRing = [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R, bool bCarried)
		{
			const FAdvectPlan& Plan = R.Ripple->AdvectPlan;
			IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
			const UWorld* World = Bench.GetWorld();
			if (!Plan.bPlanned || !Layer || !World)
			{
				return;
			}
			FAdvectRun& Ring = R.Ripple->Advects.AddDefaulted_GetRef();
			Ring.bCarried = bCarried;
			Ring.Log.Points = Plan.Points;
			Layer->SetProbes(Ring.Log.Points);
			Layer->Poke(Plan.Spot, PokeHeightCm, PokeSigmaCm);
			// The hump goes in before this frame's steps, and those steps cover the frame just passed.
			Ring.PokeTime = World->GetTimeSeconds() - Bench.FrameSeconds;
			R.Phases[R.Phase].Seconds = static_cast<float>(AdvectSeconds + 0.5);   // and the readbacks' few frames
		};
		Made->Phases.Add({TEXT("poked in the current: following its ring"), 2.f, 0.f, [Abort, Focus, EnsureWater, NoWater, PokeRing](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
			const UFluidNinjaBuoyancySubsystem* Hydro = Bench.GetHydro();
			if (!Layer || !Hydro)
			{
				Abort(Bench, R, TEXT("no waves"));
				return;
			}
			// The river floater in the fastest water inside a simulation running waves (its sponge and the line's reach left out) marks the
			// local current; the poke goes in where it floats.
			const double Inset = FMath::Max(UFluidNinjaBuoyancySettings::Get().WaveSpongeCm, 0.f) + AdvectLineMarginCm;
			UFluidNinjaBuoyancyComponent* Marker = nullptr;
			double Fastest = 0.0;
			int32 Floaters = 0;
			for (UFluidNinjaBuoyancyComponent* Body : Hydro->GetBodies())
			{
				const AActor* Owner = Body->GetOwner();
				const UPrimitiveComponent* Primitive = Body->GetSimulatedPrimitive();
				FFluidNinjaBuoyancyBodyDebug Debug;
				if (!Owner || !Primitive || !Owner->ActorHasTag(RiverFloaterTag) || !Body->GetReadout().bInWater || !Hydro->GetBodyDebug(Body, Debug) || !Debug.bHasWaterZ)
				{
					continue;
				}
				++Floaters;
				const FVector2D Where(Primitive->GetComponentLocation());
				Layer->SetBenchFocus(Where);
				const FBox2D Inside(Layer->GetWindowOrigin() + FVector2D(Inset), Layer->GetWindowOrigin() + FVector2D(Layer->GetWindowCm() - Inset));
				const double Speed = Body->GetReadout().FlowCmS.Size();
				if (Layer->GetWindowCm() > 0.0 && Inside.IsInside(Where) && Speed > Fastest)
				{
					Marker = Body;
					Fastest = Speed;
				}
			}
			if (!Marker)
			{
				Abort(Bench, R, Floaters > 0 ? FString(TEXT("no river floater (tag FNB_RiverFloater) afloat inside a simulation running waves, clear of its sponge"))
					: FString(TEXT("no river floater (tag FNB_RiverFloater) afloat to read the current with")));
				return;
			}
			if (Fastest < AdvectSlowestCmS)
			{
				Abort(Bench, R, FString::Printf(TEXT("the current under the river floaters is at most %.0f cm/s, under the %.0f cm/s a ring's drift can be told from its spreading in"),
					Fastest, AdvectSlowestCmS));
				return;
			}
			Focus(Bench, R, FVector2D(Marker->GetSimulatedPrimitive()->GetComponentLocation()));
			if (!EnsureWater(Bench, R))
			{
				Abort(Bench, R, TEXT("no water to read in the river: ") + NoWater(Bench));
				return;
			}
			FAdvectPlan& Plan = R.Ripple->AdvectPlan;
			Plan.Floater = Marker;
			Plan.FloaterName = Bench.BodyName(Marker);
			Plan.Spot = SnapToCell(FVector2D(Marker->GetSimulatedPrimitive()->GetComponentLocation()), Layer->GetCellCm(), Layer->GetWindowOrigin());
			Plan.Direction = Marker->GetReadout().FlowCmS.GetSafeNormal();
			Plan.SpeedCmS = Fastest;
			Plan.ClearCm = R.Ripple->Water.ClearRadius(Plan.Spot, 1000.0);
			// The line, from AdvectLineMarginCm behind the poke to as far beyond where the current would take it: at most 256 probes, a cell
			// or more apart.
			const double Behind = AdvectLineMarginCm;
			const double Ahead = AdvectLineMarginCm + Fastest * AdvectSeconds;
			const int32 Count = FMath::Clamp(FMath::FloorToInt32((Behind + Ahead) / FMath::Max(Layer->GetCellCm(), 1.0)) + 1, 2, 256);
			const double Spacing = (Behind + Ahead) / (Count - 1);
			// The core's own drift down the line (its Wave Aux: cells per tick, times the cell and the ticks a second).
			const double CoreScale = Layer->GetCellCm() * Layer->GetStepsPerSecond();
			for (int32 Probe = 0; Probe < Count; ++Probe)
			{
				const double Along = -Behind + Probe * Spacing;
				const FVector2D Point = Plan.Spot + Plan.Direction * Along;
				const FLinearColor Core = R.Ripple->Water.CoreAt(Point);
				Plan.Points.Add(Point);
				Plan.Along.Add(Along);
				Plan.CoreAtPoke.Add((Core.B * Plan.Direction.X + Core.A * Plan.Direction.Y) * CoreScale);
			}
			Plan.bPlanned = true;
			PokeRing(Bench, R, true);
		}, [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			// The core's drift down the line again as the ring is let go (the current may still be changing).
			FAdvectPlan& Plan = R.Ripple->AdvectPlan;
			const IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves();
			FWaterMap Now;
			if (!Plan.bPlanned || !Layer || !Bench.BuildWaterMap(Now))
			{
				return;
			}
			const double CoreScale = Layer->GetCellCm() * Layer->GetStepsPerSecond();
			for (const FVector2D& Point : Plan.Points)
			{
				const FLinearColor Core = Now.CoreAt(Point);
				Plan.CoreAtEnd.Add((Core.B * Plan.Direction.X + Core.A * Plan.Direction.Y) * CoreScale);
			}
		}, Follow});
		AddClear(TEXT("fresh ripples, not carried (still water to them)"), [](FFluidNinjaBuoyancyTestBench& Bench, FRun&)
		{
			SetWaveCurrentCoupling(0.f);
			SetWaveSources(false, false);
			if (IFluidNinjaBuoyancyBenchWaves* Layer = Bench.GetWaves())
			{
				Layer->SetProbes(TArray<FVector2D>());
			}
		}, nullptr);
		Made->Phases.Add({TEXT("poked in still water: following its ring"), 2.f, 0.f, [PokeRing](FFluidNinjaBuoyancyTestBench& Bench, FRun& R) { PokeRing(Bench, R, false); }, nullptr, Follow});
		Made->Evaluate = [](FFluidNinjaBuoyancyTestBench& Bench, FRun& R)
		{
			const FAdvectPlan& Plan = R.Ripple->AdvectPlan;
			if (!Plan.bPlanned)
			{
				return;   // the planning said why
			}
			const double StepRate = Bench.GetWaves() ? Bench.GetWaves()->GetStepsPerSecond() : 60.0;
			const double Spacing = Plan.Along.Num() > 1 ? Plan.Along[1] - Plan.Along[0] : 0.0;
			Bench.Report(R.Name, TEXT("poke"), 0, FString::Printf(TEXT("a %.1f cm pressure head, sigma %.0f cm, pressing for one tick at (%.0f, %.0f) beside the river floater %s, where the current ran %.0f cm/s toward %.0f deg (%.1f m of open water around); its ring followed for %.1f s along %d probes %.1f cm apart down that current (%.0f cm behind the poke to %.0f cm past it), first in the current, then at the same spot with the waves not carried (User.Wave.CurrentCoupling 0: still water to them, with the same depth and banks). The ring's centre along the line is the centroid of z squared on it (a ring is symmetric about its centre along any line across it). Judged: the core carries its state (eta, v) with its own drift (Wave Prepare's advection, the drift its Wave Aux shows), so in the current the ring's centre must move with that drift (a ratio of 1, within %.0f%%); the current the floater feels, and the still-water ring, are information"),
				PokeHeightCm, PokeSigmaCm, Plan.Spot.X, Plan.Spot.Y, *Plan.FloaterName, Plan.SpeedCmS, FMath::RadiansToDegrees(FMath::Atan2(Plan.Direction.Y, Plan.Direction.X)),
				Plan.ClearCm / 100.0, AdvectSeconds, Plan.Points.Num(), Spacing, Plan.Along.IsEmpty() ? 0.0 : -Plan.Along[0], Plan.Along.IsEmpty() ? 0.0 : Plan.Along.Last(),
				100.0 * AdvectTolerance));
			// The core's own drift down the line at a place on it (cm/s): the probe nearest it, the mean of the two reads.
			const auto CoreDriftAt = [&Plan](double Along)
			{
				int32 Nearest = 0;
				for (int32 Probe = 1; Probe < Plan.Along.Num(); ++Probe)
				{
					Nearest = FMath::Abs(Plan.Along[Probe] - Along) < FMath::Abs(Plan.Along[Nearest] - Along) ? Probe : Nearest;
				}
				double Sum = 0.0;
				int32 Count = 0;
				for (const TArray<double>* Read : {&Plan.CoreAtPoke, &Plan.CoreAtEnd})
				{
					if (Read->IsValidIndex(Nearest))
					{
						Sum += (*Read)[Nearest];
						++Count;
					}
				}
				return Count > 0 ? Sum / Count : 0.0;
			};
			// How far the core's drift carries the ring's centre from the poke in Since seconds, stepped at its ticks. Along the line, the
			// ring's z squared sits where it crosses the line, at its radius either side of its centre (the main group's: the group velocity
			// at k = √2/σ at the core's depth there, times the time), so its centroid moves at the mean of the drift at those two places.
			const double TickSeconds = 1.0 / StepRate;
			const double RingSpeed = CoreGroupVelocity(FMath::Sqrt(2.0) / PokeSigmaCm, FMath::Max(static_cast<double>(R.Ripple->Water.CoreAt(Plan.Spot).G), 1.0));
			const auto CoreCarry = [&CoreDriftAt, TickSeconds, RingSpeed](double Since)
			{
				double Along = 0.0;
				for (double Time = 0.0; Time < Since; Time += TickSeconds)
				{
					const double Radius = RingSpeed * Time;
					Along += 0.5 * (CoreDriftAt(Along - Radius) + CoreDriftAt(Along + Radius)) * FMath::Min(TickSeconds, Since - Time);
				}
				return Along;
			};
			// How far the current under the floater would carry the ring along the line from the poke to a world time: each frame's reading
			// held over the time up to it (the core takes each frame's current), the first one back to the poke, the last one on.
			auto CarryUntil = [&Plan](const FAdvectRun& Ring, double Until)
			{
				double Carry = 0.0;
				double From = Ring.PokeTime;
				for (int32 Sample = 0; Sample < Ring.FlowTimes.Num() && Sample < Ring.Flows.Num() && From < Until; ++Sample)
				{
					const double To = FMath::Min(Ring.FlowTimes[Sample], Until);
					if (To > From)
					{
						Carry += (Ring.Flows[Sample] | Plan.Direction) * (To - From);
						From = To;
					}
				}
				if (Until > From && !Ring.Flows.IsEmpty())
				{
					Carry += (Ring.Flows.Last() | Plan.Direction) * (Until - From);
				}
				return Carry;
			};
			// Per ring, at each reading in its first AdvectSeconds: its centre along the line (from the poke), the current's carry by then, and
			// its z² summed along the line (its strength there).
			double CarriedEnergy = -1.0;
			double StillEnergy = -1.0;
			double CarryAtEnd = 0.0;
			for (const FAdvectRun& Ring : R.Ripple->Advects)
			{
				TArray<double> Elapsed;
				TArray<double> Drift;
				TArray<double> Carry;
				TArray<double> Core;
				double LateEnergy = 0.0;
				int32 LateCount = 0;
				for (int32 Reading = 0; Reading < Ring.Log.Times.Num(); ++Reading)
				{
					const double Since = Ring.Log.Times[Reading] - Ring.PokeTime;
					if (Since < 0.0 || Since > AdvectSeconds)
					{
						continue;
					}
					double Weight = 0.0;
					double Moment = 0.0;
					const TArray<float>& Heights = Ring.Log.Heights[Reading];
					for (int32 Probe = 0; Probe < Heights.Num() && Probe < Plan.Along.Num(); ++Probe)
					{
						const double Squared = FMath::Square(static_cast<double>(Heights[Probe]));
						Weight += Squared;
						Moment += Squared * Plan.Along[Probe];
					}
					if (Weight <= 1.0e-12)
					{
						continue;
					}
					Elapsed.Add(Since);
					Drift.Add(Moment / Weight);
					Carry.Add(Ring.bCarried ? CarryUntil(Ring, Ring.Log.Times[Reading]) : 0.0);
					Core.Add(Ring.bCarried ? CoreCarry(Since) : 0.0);
					if (Since >= AdvectSeconds - 0.3)
					{
						LateEnergy += Weight;
						++LateCount;
					}
				}
				const FString Who = Ring.bCarried ? TEXT("poke in the current") : TEXT("poke in still water");
				const FString Frames = FString::Printf(TEXT("%d readings, longest frame %.0f ms%s"), Elapsed.Num(), Ring.LongestFrame * 1000.f,
					Ring.LongestFrame >= 4.0 / StepRate ? TEXT(" (a frame of more than four of the core's ticks)") : TEXT(""));
				if (Elapsed.Num() < 5)
				{
					Bench.Report(R.Name, Who, 0, FString::Printf(TEXT("not measured: %s within %.1f s of the poke"), *Frames, AdvectSeconds));
					continue;
				}
				const double MeanLateEnergy = LateCount > 0 ? LateEnergy / LateCount : -1.0;
				if (Ring.bCarried)
				{
					CarriedEnergy = MeanLateEnergy;
				}
				else
				{
					StillEnergy = MeanLateEnergy;
				}
				if (Ring.bCarried)
				{
					// The ratios: the least-squares slope of drift against carry over the readings from 0.2 s on (the ring first forms around its
					// centre), against the core's own drift (judged) and against the current under the floater (information).
					const auto Slope = [&Elapsed, &Drift](const TArray<double>& Against, int32& OutFitted)
					{
						double Numerator = 0.0;
						double Denominator = 0.0;
						OutFitted = 0;
						for (int32 Index = 0; Index < Elapsed.Num(); ++Index)
						{
							if (Elapsed[Index] >= 0.2)
							{
								Numerator += Drift[Index] * Against[Index];
								Denominator += Against[Index] * Against[Index];
								++OutFitted;
							}
						}
						return Denominator > 0.0 ? Numerator / Denominator : 0.0;
					};
					int32 Fitted = 0;
					const double CoreRatio = Slope(Core, Fitted);
					const double FloaterRatio = Slope(Carry, Fitted);
					CarryAtEnd = Carry.Last();
					const bool bDrifts = FMath::Abs(Core.Last()) > 1.0e-6;
					Bench.Report(R.Name, Who, bDrifts && Fitted >= 5 ? (FMath::Abs(CoreRatio - 1.0) <= AdvectTolerance ? 1 : -1) : 0,
						FString::Printf(TEXT("after %.2f s its centre had drifted %+.1f cm down the line, against the %+.1f cm the core's own drift (%.0f cm/s at the poke, from its Wave Aux) carries it: ratio %.2f (least squares over the %d readings from 0.2 s on; pass: 1 within %.0f%%)%s; %s"),
							Elapsed.Last(), Drift.Last(), Core.Last(), CoreDriftAt(0.0), CoreRatio, Fitted, 100.0 * AdvectTolerance,
							bDrifts ? TEXT("") : TEXT(" (not judged: the core shows no drift there)"), *Frames));
					Bench.Report(R.Name, Who, 0, FString::Printf(TEXT("against the current the floater feels (information): %+.1f cm in that time (%.0f cm/s along the line on average), ratio %.2f; the core's own drift at the poke is %.2f of it"),
						Carry.Last(), Carry.Last() / FMath::Max(Elapsed.Last(), 1.0e-3), FloaterRatio, FMath::Abs(Carry.Last()) > 1.0e-6 ? Core.Last() / Carry.Last() : 0.0));
				}
				else
				{
					Bench.Report(R.Name, Who, 0, FString::Printf(TEXT("after %.2f s its centre had drifted %+.1f cm down the line%s; %s"), Elapsed.Last(), Drift.Last(),
						FMath::Abs(CarryAtEnd) > 1.0e-6 ? *FString::Printf(TEXT(" (%.0f%% of the current run's carry)"), 100.0 * Drift.Last() / CarryAtEnd) : TEXT(""), *Frames));
				}
			}
			// The ring's strength on the line, carried against still water, over the last 0.3 s of each (the advection's interpolation
			// smooths a carried ring; in a non-uniform current it is stretched as well).
			if (CarriedEnergy > 0.0 && StillEnergy > 0.0)
			{
				Bench.Report(R.Name, TEXT("poke"), 0, FString::Printf(TEXT("the ring's z squared summed along the line over the last 0.3 s: %.0f%% of still water's when carried (information: the advection's loss, with any stretching by the current)"),
					100.0 * CarriedEnergy / StillEnergy));
			}
		};
	}
	else
	{
		Report(Name, TEXT("-"), 0, TEXT("no such ripple test (ripple-dispersion, ripple-fps, ripple-kelvin, ripple-weight, ripple-rest, ripple-coupling, ripple-evers, ripple-advection; ripples runs all eight)"));
		return nullptr;
	}
	return Made;
}

/* ---- Every frame ---- */

void FFluidNinjaBuoyancyTestBench::Tick(float DeltaSeconds)
{
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (!Hydro)
	{
		return;
	}
	FrameSeconds = DeltaSeconds;
	AttachPanel();
	if (!Selected.IsValid())
	{
		Select(0);
	}
	// The still water under the afloat bodies while it is live, for controlled water to be set at when every body is held for its water
	// (SetControlledWater). Under controlled water the bodies know only its height, which is not kept. Only while a test runs or the panel
	// may be up (its Ctrl+Shift+C sets controlled water), so a level in Play without the bench pays nothing for it.
	if (!Hydro->IsWaterControlled() && WantsStatusEveryFrame())
	{
		float StillWater = 0.f;
		if (BodiesWaterHeight(StillWater))
		{
			LastStillWaterZ = StillWater;
			bLastStillWater = true;
		}
	}
	// A level with river floaters starts with its river stopped, as the plugin demo's does: they wait at the head until Start. Once
	// FluidNinja is surely running (0.5 s), and again at 2 s, as it may apply its preset after Begin Play.
	if (RiverHold < 2)
	{
		const UWorld* World = GetWorld();
		const double Now = World ? World->GetTimeSeconds() : 0.0;
		if (RiverHold == 0 && CountRiverFloaters() == 0)
		{
			RiverHold = 2;
		}
		else if (Now > (RiverHold == 0 ? 0.5 : 2.0))
		{
			SetRiverRunning(false);
			++RiverHold;
		}
	}
	if (Current.IsValid())
	{
		// The tracks are made when the run starts; bodies known before it (walkaway reads who was afloat).
		if (Current->Phase < 0 && Current->Name == TEXT("walkaway"))
		{
			for (FTrack& Track : Current->Tracks)
			{
				Track.bWasInWater = Track.Body.IsValid() && Track.Body->GetReadout().bInWater;
			}
		}
		if (const UWorld* World = GetWorld())
		{
			for (FTrack& Track : Current->Tracks)
			{
				if (Track.Body.IsValid() && Track.Body->GetReadout().bInWater)
				{
					Track.LastInWater = World->GetTimeSeconds();
				}
			}
		}
		TickRun();
	}
	if (Widget.IsValid() && bVisible && bLabels && CVarTestHUD.GetValueOnGameThread() > 0)
	{
		DrawLabels();
	}
}

void FFluidNinjaBuoyancyTestBench::DrawLabels() const
{
	UWorld* World = GetWorld();
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (!World || !Hydro)
	{
		return;
	}
	for (const UFluidNinjaBuoyancyComponent* Body : Hydro->GetBodies())
	{
		const UPrimitiveComponent* Primitive = Body->GetSimulatedPrimitive();
		if (!Primitive)
		{
			continue;
		}
		const FBoxSphereBounds Bounds = Primitive->Bounds;
		const bool bSelected = Body == Selected.Get();
		FFluidNinjaBuoyancyBodyDebug Debug;
		Hydro->GetBodyDebug(Body, Debug);
		float DraftNow = 0.f;
		const FString State = Debug.bHeld ? TEXT("held") : Debug.bDormant ? TEXT("asleep") : Body->IsAwaitingWater() ? TEXT("settling") : Body->GetReadout().bInWater ? TEXT("afloat") : TEXT("dry");
		const FString Label = FString::Printf(TEXT("%s%s\n%s  %s  tilt %.0f"), bSelected ? TEXT("> ") : TEXT(""), *BodyName(Body), *State,
			Draft(Body, DraftNow) ? *FString::Printf(TEXT("draft %.0f/%.0f cm"), DraftNow, Body->GetRestingDraft()) : TEXT("no water"), Tilt(Body));
		DrawDebugString(World, Bounds.Origin + FVector(0.0, 0.0, Bounds.BoxExtent.Z + 60.0), Label, nullptr,
			bSelected ? FColor(255, 220, 90) : FColor(170, 220, 255), 0.f, true, bSelected ? 1.1f : 0.9f);
		if (bSelected)
		{
			DrawDebugBox(World, Bounds.Origin, Bounds.BoxExtent + FVector(5.0), FColor(255, 220, 90), false, -1.f, 0, 2.f);
		}
	}
}

/* ---- Panel ---- */

FString FFluidNinjaBuoyancyTestBench::BodyLine(const UFluidNinjaBuoyancyComponent* Body, bool bIsSelected) const
{
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	const UPrimitiveComponent* Primitive = Body ? Body->GetSimulatedPrimitive() : nullptr;
	if (!Hydro || !Primitive)
	{
		return FString();
	}
	FFluidNinjaBuoyancyBodyDebug Debug;
	Hydro->GetBodyDebug(Body, Debug);
	const FFluidNinjaBuoyancyReadout Read = Body->GetReadout();
	const FString State = Debug.bHeld ? TEXT("held") : Debug.bDormant ? TEXT("asleep") : Body->IsAwaitingWater() ? TEXT("settling") : Read.bInWater ? TEXT("afloat") : TEXT("dry");
	float DraftNow = 0.f;
	const FString DraftText = Draft(Body, DraftNow) ? FString::Printf(TEXT("%5.1f/%-5.1f"), DraftNow, Body->GetRestingDraft()) : FString(TEXT("  -  /  -  "));
	const float Speed = Primitive->IsSimulatingPhysics() ? static_cast<float>(Primitive->GetPhysicsLinearVelocity().Size2D()) : 0.f;
	return FString::Printf(TEXT("%s %-18s %s L%d %-8s %s %5.1f %5.2f %5.2f %5.0f"), bIsSelected ? TEXT(">") : TEXT(" "), *BodyName(Body).Left(18), ModelName(Body->HullModel),
		Debug.Level, *State, *DraftText, Tilt(Body), Speed / 100.f, Read.FlowCmS.Size() / 100.f, Read.WindForceN.Size2D());
}

FText FFluidNinjaBuoyancyTestBench::Readout() const
{
	const double Now = FPlatformTime::Seconds();
	if (Now - CachedAt < 0.1)
	{
		return CachedText;
	}
	CachedAt = Now;
	const UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (!Hydro)
	{
		CachedText = FText::GetEmpty();
		return CachedText;
	}
	FString Text = TEXT("FLUIDNINJA BUOYANCY TEST BENCH\n");
	if (Hydro->IsWaterControlled())
	{
		Text += FString::Printf(TEXT("Water    CONTROLLED: flat at %.0f cm, still (no current, waves or wakes)%s\n"), Hydro->GetControlledWaterHeight(),
			IsSceneryCollision() ? TEXT("") : TEXT(", free water (scenery ignored)"));
	}
	else
	{
		Text += FString::Printf(TEXT("Water    live river: %d of %d surface pieces drawn, %s\n"), Hydro->GetCapturedPrimitives(), Hydro->GetSurfacePrimitives(),
			Hydro->IsGathering() ? *FString::Printf(TEXT("GPU gather %d bodies, %d points"), Hydro->GetGatherBodies(), Hydro->GetGatherPoints()) : TEXT("per-body copies"));
	}
	Text += Hydro->DescribeStirApproach() + TEXT("\n");
	FString Calibration = Hydro->GetCalibrationReport();
	if (Calibration.Len() > 150)
	{
		Calibration = Calibration.Left(147) + TEXT("...");
	}
	Text += FString::Printf(TEXT("Current  %.0f cm/s per buffer unit (multiplier %.2f)%s%s\n"), Hydro->GetFlowScale(), UFluidNinjaBuoyancySettings::Get().FlowScaleMultiplier,
		Calibration.IsEmpty() ? TEXT("") : TEXT("\n         "), *Calibration);
	const FFluidNinjaBuoyancyWind& Wind = Hydro->GetWind();
	Text += FString::Printf(TEXT("Wind     %.1f m/s toward %.0f deg, %s\n"), Wind.SpeedMs, Wind.DirectionDegrees,
		Wind.bOverride ? TEXT("TEST WIND") : TEXT("project setting"));
	float GameMs = 0.f;
	float PatchesMs = 0.f;
	float PreTickMs = 0.f;
	float PhysicsMs = 0.f;
	Hydro->GetCosts(GameMs, PatchesMs, PreTickMs, PhysicsMs);
	Text += FString::Printf(TEXT("Cost     game %.2f ms (patches %.2f), pre-tick %.2f, physics %.2f ms/frame, wakes %d, debug view %s\n"), GameMs, PatchesMs, PreTickMs, PhysicsMs,
		Hydro->GetWakePoints(), UFluidNinjaBuoyancySettings::IsDebugView() ? TEXT("on") : TEXT("off"));
	if (const IFluidNinjaBuoyancyBenchWaves* Ripples = GetWaves())
	{
		FString Line = Ripples->GetStatusLine();
		Line.RemoveFromStart(TEXT("FluidNinja Buoyancy waves: "));
		Text += TEXT("Waves    ") + FluidNinjaBuoyancyBenchRipples::WrapStatus(Line, 110, TEXT("         ")) + TEXT("\n");
	}
	else
	{
		Text += TEXT("Waves    none in this world (FluidNinja Buoyancy makes them at Begin Play)\n");
	}
	Text += TEXT("\n  body               mdl lod state    draft/rest  tilt  m/s  water windN\n");
	for (const UFluidNinjaBuoyancyComponent* Body : Hydro->GetBodies())
	{
		Text += BodyLine(Body, Body == Selected.Get()) + TEXT("\n");
	}
	if (const UFluidNinjaBuoyancyComponent* Body = Selected.Get())
	{
		FFluidNinjaBuoyancyBodyDebug Debug;
		Hydro->GetBodyDebug(Body, Debug);
		const FFluidNinjaBuoyancyReadout& R = Body->GetReadout();
		Text += FString::Printf(TEXT("\n%s: %.0f kg, %.0f L under water, %.2f m2 wet, force (%.0f, %.0f, %.0f) N, wind (%.0f, %.0f) N, slam %.0f W\n"), *BodyName(Body), R.MassKg,
			R.DisplacedLitres, R.WettedAreaM2, R.ForceN.X, R.ForceN.Y, R.ForceN.Z, R.WindForceN.X, R.WindForceN.Y, R.SlamWatts);
		Text += FString::Printf(TEXT("  water %s, %d points %.1f cm apart, %.0f ms old, far field %s; stability roll %.0f cm, pitch %.0f cm%s\n"),
			Debug.bInCapture ? TEXT("read") : Debug.bFlat ? TEXT("published (flat)") : TEXT("unknown"), Debug.PatchPoints, Debug.PatchSpacing, Debug.PatchAge * 1000.f,
			Debug.bFarFieldLearned ? *FString::Printf(TEXT("%+.0f cm"), Debug.FarFieldCorrection) : TEXT("not learned"),
			Body->GetRollStability(), Body->GetPitchStability(),
			Body->bPropulsion ? *FString::Printf(TEXT("; throttle %.1f rudder %.1f"), Body->Throttle, Body->Rudder) : TEXT(""));
	}
	if (const FRun* Running = Current.Get(); Running && Running->Phase >= 0)
	{
		const UWorld* World = GetWorld();
		const double Left = World ? Running->Phases[Running->Phase].Seconds - (World->GetTimeSeconds() - Running->PhaseStarted) : 0.0;
		Text += FString::Printf(TEXT("\nRUNNING  %s: %s (%d/%d), %.0f s left%s\n"), *Running->Name, *Running->Phases[Running->Phase].Label, Running->Phase + 1, Running->Phases.Num(),
			FMath::Max(0.0, Left), Queue.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", then %s"), *FString::Join(Queue, TEXT(", "))));
	}
	if (!Results.IsEmpty())
	{
		Text += TEXT("\nResults (newest first)\n");
		for (int32 Index = Results.Num() - 1, Shown = 0; Index >= 0 && Shown < 12; --Index, ++Shown)
		{
			const FResult& Result = Results[Index];
			FString Line = FString::Printf(TEXT("%s %-8s %-16s %s"), Result.Verdict > 0 ? TEXT("PASS") : Result.Verdict < 0 ? TEXT("FAIL") : TEXT("info"), *Result.Test,
				*Result.Body.Left(16), *Result.Text);
			Text += (Line.Len() > 150 ? Line.Left(147) + TEXT("...") : Line) + TEXT("\n");
		}
	}
	Text.RemoveFromEnd(TEXT("\n"));
	CachedText = FText::FromString(Text);
	return CachedText;
}

TSharedRef<SWidget> FFluidNinjaBuoyancyTestBench::Build()
{
	const TWeakPtr<FFluidNinjaBuoyancyTestBench> Weak = AsShared();
	const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Regular", 9);
	const FSlateFontInfo Mono = FCoreStyle::GetDefaultFontStyle("Mono", 9);

	auto Button = [Weak, Font](const FString& Label, TFunction<void(FFluidNinjaBuoyancyTestBench&)> Action, TFunction<bool(const FFluidNinjaBuoyancyTestBench&)> IsActive = nullptr,
		bool bKeepMenuOpen = false) -> TSharedRef<SWidget>
	{
		return SNew(SBox).Padding(FMargin(0.f, 0.f, 3.f, 3.f))
		[
			SNew(SButton)
			.ContentPadding(FMargin(6.f, 1.f))
			.ButtonColorAndOpacity_Lambda([Weak, IsActive]()
			{
				const TSharedPtr<FFluidNinjaBuoyancyTestBench> Pinned = Weak.Pin();
				return FSlateColor(Pinned && IsActive && IsActive(*Pinned) ? FLinearColor(0.15f, 0.5f, 0.95f) : FLinearColor(0.22f, 0.22f, 0.22f));
			})
			.OnClicked_Lambda([Weak, Action, bKeepMenuOpen]()
			{
				if (const TSharedPtr<FFluidNinjaBuoyancyTestBench> Pinned = Weak.Pin())
				{
					Action(*Pinned);
				}
				if (!bKeepMenuOpen && FSlateApplication::IsInitialized())
				{
					FSlateApplication::Get().DismissAllMenus();
				}
				return FReply::Handled();
			})
			[
				SNew(STextBlock).Text(FText::FromString(Label)).Font(Font).ColorAndOpacity(FLinearColor::White)
			]
		];
	};
	auto Dropdown = [Weak, Font](const FString& Label, TFunction<FString(const FFluidNinjaBuoyancyTestBench&)> Value, const TArray<TSharedRef<SWidget>>& Items) -> TSharedRef<SWidget>
	{
		TSharedRef<SWrapBox> Menu = SNew(SWrapBox).PreferredSize(420.f);
		for (const TSharedRef<SWidget>& Item : Items)
		{
			Menu->AddSlot()[Item];
		}
		return SNew(SBox).Padding(FMargin(0.f, 0.f, 4.f, 4.f))
		[
			SNew(SComboButton)
			.ButtonColorAndOpacity(FLinearColor(0.22f, 0.22f, 0.22f))
			.ForegroundColor(FLinearColor::White)
			.ContentPadding(FMargin(6.f, 1.f))
			.ButtonContent()
			[
				SNew(STextBlock).Font(Font).ColorAndOpacity(FLinearColor::White)
				.Text_Lambda([Weak, Label, Value]()
				{
					const TSharedPtr<FFluidNinjaBuoyancyTestBench> Pinned = Weak.Pin();
					return FText::FromString(Pinned && Value ? Label + TEXT(": ") + Value(*Pinned) : Label);
				})
			]
			.MenuContent()
			[
				SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
				.BorderBackgroundColor(FLinearColor(0.02f, 0.02f, 0.02f, 0.92f))
				.Padding(FMargin(6.f, 6.f, 3.f, 3.f))
				[
					Menu
				]
			]
		];
	};
	auto Hydro = [](const FFluidNinjaBuoyancyTestBench& Bench) { return Bench.Subsystem.Get(); };
	auto TestWind = [](float Speed)
	{
		return [Speed](FFluidNinjaBuoyancyTestBench& Bench) { Bench.SetTestWind(true, Speed); };
	};
	auto TestWindIs = [Hydro](float Speed)
	{
		return [Hydro, Speed](const FFluidNinjaBuoyancyTestBench& Bench) { const UFluidNinjaBuoyancySubsystem* H = Hydro(Bench); return H && H->GetWind().bOverride && FMath::IsNearlyEqual(H->GetWind().SpeedMs, Speed, 0.05f); };
	};
	auto Test = [](const TCHAR* Name) { return [Name = FString(Name)](FFluidNinjaBuoyancyTestBench& Bench) { Bench.Run(Name); }; };

	TArray<TSharedRef<SWidget>> WaterButtons = {
		Button(TEXT("Live river"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.SetControlledWater(false); }, [Hydro](const FFluidNinjaBuoyancyTestBench& Bench) { const UFluidNinjaBuoyancySubsystem* H = Hydro(Bench); return H && !H->IsWaterControlled(); }),
		Button(TEXT("Controlled (flat, still)"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.SetControlledWater(true); }, [Hydro](const FFluidNinjaBuoyancyTestBench& Bench) { const UFluidNinjaBuoyancySubsystem* H = Hydro(Bench); return H && H->IsWaterControlled(); }),
		Button(TEXT("Calibrate current (at the body)"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.Command({TEXT("calibrate")}); }),
		Button(TEXT("Gather on/off"), [](FFluidNinjaBuoyancyTestBench&) { SetSwitch(TEXT("fnb.Buoyancy.Gather"), UFluidNinjaBuoyancySettings::Get().GatherEnabled() ? 0 : 1); }, [](const FFluidNinjaBuoyancyTestBench&) { return UFluidNinjaBuoyancySettings::Get().GatherEnabled(); }, true),
		Button(TEXT("Wakes on/off"), [](FFluidNinjaBuoyancyTestBench&) { SetSwitch(TEXT("fnb.Buoyancy.Wake"), UFluidNinjaBuoyancySettings::Get().WakesEnabled() ? 0 : 1); }, [](const FFluidNinjaBuoyancyTestBench&) { return UFluidNinjaBuoyancySettings::Get().WakesEnabled(); }, true),
		Button(TEXT("Debug view"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.ToggleDebugView(); }, [](const FFluidNinjaBuoyancyTestBench&) { return UFluidNinjaBuoyancySettings::IsDebugView(); }, true),
		Button(TEXT("Free water (ignore scenery)"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.SetSceneryCollision(!Bench.IsSceneryCollision()); },
			[](const FFluidNinjaBuoyancyTestBench& Bench) { return !Bench.IsSceneryCollision(); }, true) };
	TArray<TSharedRef<SWidget>> WindButtons = {
		Button(TEXT("Project wind"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.SetTestWind(false); }, [Hydro](const FFluidNinjaBuoyancyTestBench& Bench) { const UFluidNinjaBuoyancySubsystem* H = Hydro(Bench); return H && !H->GetWind().bOverride; }),
		Button(TEXT("Calm"), TestWind(0.f), TestWindIs(0.f)),
		Button(TEXT("5 m/s"), TestWind(5.f), TestWindIs(5.f)),
		Button(TEXT("10 m/s"), TestWind(10.f), TestWindIs(10.f)),
		Button(TEXT("20 m/s"), TestWind(20.f), TestWindIs(20.f)),
		Button(TEXT("30 m/s"), TestWind(30.f), TestWindIs(30.f)) };
	TArray<TSharedRef<SWidget>> BodyButtons = {
		Button(TEXT("< Previous"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.Select(-1); }, nullptr, true),
		Button(TEXT("Next >"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.Select(1); }, nullptr, true),
		Button(TEXT("Reset all"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.ResetBodies(true); }),
		Button(TEXT("Reset this"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.ResetBodies(false); }),
		Button(TEXT("Bring here"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.BringHere(); }),
		Button(TEXT("Push 300 N.s"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.Push(300.f); }, nullptr, true),
		Button(TEXT("Heel 30"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.Capsize(30.f); }, nullptr, true),
		Button(TEXT("Capsize 120"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.Capsize(120.f); }, nullptr, true),
		Button(TEXT("Drop 3 m"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.Drop(3.f); }, nullptr, true),
		Button(TEXT("Labels"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.bLabels = !Bench.bLabels; }, [](const FFluidNinjaBuoyancyTestBench& Bench) { return Bench.bLabels; }, true) };
	TArray<TSharedRef<SWidget>> BoatButtons = {
		Button(TEXT("Astern"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.SetBoat(-0.5f, 0.f); }),
		Button(TEXT("Stop"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.SetBoat(0.f, 0.f); }),
		Button(TEXT("Half ahead"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.SetBoat(0.5f, 0.f); }),
		Button(TEXT("Full ahead"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.SetBoat(1.f, 0.f); }),
		Button(TEXT("Port"), [](FFluidNinjaBuoyancyTestBench& Bench) { if (const UFluidNinjaBuoyancyComponent* B = Bench.GetSelected()) { Bench.SetBoat(B->Throttle, -1.f); } }, nullptr, true),
		Button(TEXT("Rudder centre"), [](FFluidNinjaBuoyancyTestBench& Bench) { if (const UFluidNinjaBuoyancyComponent* B = Bench.GetSelected()) { Bench.SetBoat(B->Throttle, 0.f); } }, nullptr, true),
		Button(TEXT("Starboard"), [](FFluidNinjaBuoyancyTestBench& Bench) { if (const UFluidNinjaBuoyancyComponent* B = Bench.GetSelected()) { Bench.SetBoat(B->Throttle, 1.f); } }, nullptr, true) };
	auto Stir = [](EFluidNinjaBuoyancyStirApproach Approach)
	{
		return [Approach](FFluidNinjaBuoyancyTestBench& Bench) { if (UFluidNinjaBuoyancySubsystem* H = Bench.Subsystem.Get()) { H->SetStirApproach(Approach); Bench.CachedAt = -1.0; } };
	};
	auto StirIs = [Hydro](EFluidNinjaBuoyancyStirApproach Approach)
	{
		return [Hydro, Approach](const FFluidNinjaBuoyancyTestBench& Bench) { const UFluidNinjaBuoyancySubsystem* H = Hydro(Bench); return H && H->IsStirApproachSet() && H->GetStirApproach() == Approach; };
	};
	TArray<TSharedRef<SWidget>> StirButtons = {
		Button(TEXT("Wakes"), Stir(EFluidNinjaBuoyancyStirApproach::Wakes), StirIs(EFluidNinjaBuoyancyStirApproach::Wakes)),
		Button(TEXT("Tracking by tag"), Stir(EFluidNinjaBuoyancyStirApproach::TrackingByTag), StirIs(EFluidNinjaBuoyancyStirApproach::TrackingByTag)),
		Button(TEXT("Tracking by object type"), Stir(EFluidNinjaBuoyancyStirApproach::TrackingByType), StirIs(EFluidNinjaBuoyancyStirApproach::TrackingByType)),
		Button(TEXT("Wakes + tracking (tag)"), Stir(EFluidNinjaBuoyancyStirApproach::WakesAndTracking), StirIs(EFluidNinjaBuoyancyStirApproach::WakesAndTracking)) };
	TArray<TSharedRef<SWidget>> PoolButtons = {
		Button(TEXT("Calm (no current, flat)"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.SetPoolCalm(true); }, [](const FFluidNinjaBuoyancyTestBench& Bench) { return Bench.IsPoolCalm(); }),
		Button(TEXT("Live (its own current)"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.SetPoolCalm(false); }, [](const FFluidNinjaBuoyancyTestBench& Bench) { return !Bench.IsPoolCalm(); }) };
	TArray<TSharedRef<SWidget>> RiverButtons = {
		Button(TEXT("Start river"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.SetRiverRunning(true); }, [](const FFluidNinjaBuoyancyTestBench& Bench) { return Bench.IsRiverRunning(); }),
		Button(TEXT("Stop river"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.SetRiverRunning(false); }, [](const FFluidNinjaBuoyancyTestBench& Bench) { return Bench.HasRiver() && !Bench.IsRiverRunning(); }),
		Button(TEXT("Reset (floaters back to the head)"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.ResetRiver(); }) };
	TArray<TSharedRef<SWidget>> TestButtons = {
		Button(TEXT("Settle"), Test(TEXT("settle"))), Button(TEXT("Capsize"), Test(TEXT("capsize"))), Button(TEXT("Drop"), Test(TEXT("drop"))),
		Button(TEXT("Wind"), Test(TEXT("wind"))), Button(TEXT("Throttle"), Test(TEXT("throttle"))), Button(TEXT("Gather A/B"), Test(TEXT("gather"))),
		Button(TEXT("Walk away"), Test(TEXT("walkaway"))), Button(TEXT("Run all"), Test(TEXT("all"))),
		Button(TEXT("Stop"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.Stop(); }),
		Button(TEXT("Clear results"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.Results.Reset(); Bench.CachedAt = -1.0; }) };
	TArray<TSharedRef<SWidget>> RippleButtons = {
		Button(TEXT("Poke 2 cm (3 m ahead)"), [](FFluidNinjaBuoyancyTestBench& Bench) { UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("FluidNinja Buoyancy test: %s"), *Bench.PokeAhead(2.f, 10.f)); }, nullptr, true),
		Button(TEXT("Dispersion"), Test(TEXT("ripple-dispersion"))), Button(TEXT("30/60 fps"), Test(TEXT("ripple-fps"))),
		Button(TEXT("Kelvin wake"), Test(TEXT("ripple-kelvin"))), Button(TEXT("Weight (drops)"), Test(TEXT("ripple-weight"))),
		Button(TEXT("Rest"), Test(TEXT("ripple-rest"))), Button(TEXT("Coupling"), Test(TEXT("ripple-coupling"))),
		Button(TEXT("Evers block"), Test(TEXT("ripple-evers"))), Button(TEXT("Advection (river)"), Test(TEXT("ripple-advection"))),
		Button(TEXT("Run ripples (7 min)"), Test(TEXT("ripples"))),
		Button(TEXT("Stop"), [](FFluidNinjaBuoyancyTestBench& Bench) { Bench.Stop(); }) };

	TSharedRef<SWrapBox> Controls = SNew(SWrapBox).UseAllottedSize(true);
	auto AddControl = [&Controls](const TSharedRef<SWidget>& Item) { Controls->AddSlot()[Item]; };
	AddControl(Dropdown(TEXT("Water"), [Hydro](const FFluidNinjaBuoyancyTestBench& Bench) { const UFluidNinjaBuoyancySubsystem* H = Hydro(Bench); return FString(H && H->IsWaterControlled() ? TEXT("controlled") : TEXT("live")); }, WaterButtons));
	AddControl(Dropdown(TEXT("Wind"), [Hydro](const FFluidNinjaBuoyancyTestBench& Bench)
	{
		const UFluidNinjaBuoyancySubsystem* H = Hydro(Bench);
		return H ? FString::Printf(TEXT("%s %.0f m/s"), H->GetWind().bOverride ? TEXT("test") : TEXT("project"), H->GetWind().SpeedMs) : FString();
	}, WindButtons));
	AddControl(Dropdown(TEXT("Stir"), [Hydro](const FFluidNinjaBuoyancyTestBench& Bench)
	{
		const UFluidNinjaBuoyancySubsystem* H = Hydro(Bench);
		return H ? FString(H->IsStirApproachSet() ? UFluidNinjaBuoyancySubsystem::StirApproachName(H->GetStirApproach()) : TEXT("level settings")) : FString();
	}, StirButtons));
	AddControl(Dropdown(TEXT("Pool"), [](const FFluidNinjaBuoyancyTestBench& Bench) { return FString(Bench.IsPoolCalm() ? TEXT("calm") : TEXT("live")); }, PoolButtons));
	AddControl(Dropdown(TEXT("River"), [](const FFluidNinjaBuoyancyTestBench& Bench)
	{
		return FString(!Bench.HasRiver() ? TEXT("none") : Bench.IsRiverRunning() ? TEXT("running") : TEXT("stopped"));
	}, RiverButtons));
	AddControl(Dropdown(TEXT("Body"), [](const FFluidNinjaBuoyancyTestBench& Bench) { return Bench.BodyName(Bench.GetSelected()); }, BodyButtons));
	AddControl(Dropdown(TEXT("Boat"), [](const FFluidNinjaBuoyancyTestBench& Bench)
	{
		const UFluidNinjaBuoyancyComponent* B = Bench.GetSelected();
		return B && B->bPropulsion ? FString::Printf(TEXT("%.1f / %.1f"), B->Throttle, B->Rudder) : FString(TEXT("-"));
	}, BoatButtons));
	AddControl(Dropdown(TEXT("Tests"), [](const FFluidNinjaBuoyancyTestBench& Bench) { return Bench.Current.IsValid() ? Bench.Current->Name : FString(TEXT("idle")); }, TestButtons));
	AddControl(Dropdown(TEXT("Waves"), [](const FFluidNinjaBuoyancyTestBench& Bench)
	{
		const IFluidNinjaBuoyancyBenchWaves* Ripples = Bench.GetWaves();
		return FString(Ripples && Ripples->IsActive() ? TEXT("on") : TEXT("off"));
	}, RippleButtons));

	return SNew(SBox)
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Top)
		.Padding(FMargin(14.f, 14.f, 14.f, 14.f))
		.Visibility(EVisibility::SelfHitTestInvisible)
		[
			SNew(SBorder)
			.Visibility_Lambda([Weak]()
			{
				const TSharedPtr<FFluidNinjaBuoyancyTestBench> Pinned = Weak.Pin();
				return Pinned && Pinned->bVisible && CVarTestHUD.GetValueOnGameThread() > 0 ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FLinearColor(0.f, 0.02f, 0.05f, 0.66f))
			.Padding(FMargin(10.f, 8.f))
			[
				SNew(SBox).MaxDesiredWidth(780.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Text_Lambda([Weak]() { const TSharedPtr<FFluidNinjaBuoyancyTestBench> Pinned = Weak.Pin(); return Pinned ? Pinned->Readout() : FText::GetEmpty(); })
						.Font(Mono)
						.ColorAndOpacity(FLinearColor::White)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 6.f, 0.f, 0.f))
					[
						Controls
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 2.f, 0.f, 0.f))
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Shift+F1 frees the mouse. Ctrl+Shift: B hide, R reset, PgUp/PgDn select, T run all, W wind, C water, A stir, D debug, K heel, J drop. Console: fnb.Test help")))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
						.ColorAndOpacity(FLinearColor(0.65f, 0.65f, 0.65f))
						.AutoWrapText(true)
					]
				]
			]
		];
}

/* ---- Console ---- */

FString FFluidNinjaBuoyancyTestBench::Command(const TArray<FString>& Args)
{
	const FString Verb = Args.IsEmpty() ? FString(TEXT("help")) : Args[0].ToLower();
	auto Arg = [&Args](int32 Index) { return Args.IsValidIndex(Index) ? Args[Index] : FString(); };
	auto Number = [&Args](int32 Index, float Default) { return Args.IsValidIndex(Index) && Args[Index].IsNumeric() ? FCString::Atof(*Args[Index]) : Default; };
	UFluidNinjaBuoyancySubsystem* Hydro = Subsystem.Get();
	if (Verb == TEXT("run"))
	{
		Run(Arg(1).IsEmpty() ? FString(TEXT("all")) : Arg(1));
		return Current.IsValid() ? FString::Printf(TEXT("running %s"), *Current->Name) : FString(TEXT("nothing to run"));
	}
	if (Verb == TEXT("stop"))
	{
		Stop();
		return TEXT("stopped");
	}
	if (Verb == TEXT("reset"))
	{
		const bool bAll = !Arg(1).Equals(TEXT("selected"), ESearchCase::IgnoreCase);
		ResetBodies(bAll);
		return bAll ? TEXT("every body put back") : TEXT("selected body put back");
	}
	if (Verb == TEXT("select"))
	{
		const FString Which = Arg(1);
		if (Which.IsEmpty() || Which.Equals(TEXT("next"), ESearchCase::IgnoreCase)) { Select(1); }
		else if (Which.Equals(TEXT("prev"), ESearchCase::IgnoreCase)) { Select(-1); }
		else if (!SelectByName(Which)) { return FString::Printf(TEXT("no body named like %s"), *Which); }
		return FString::Printf(TEXT("selected %s"), *BodyName(GetSelected()));
	}
	if (Verb == TEXT("pool"))
	{
		const FString Mode = Arg(1).ToLower();
		if (Mode == TEXT("calm") || Mode == TEXT("live"))
		{
			SetPoolCalm(Mode == TEXT("calm"));
		}
		else if (!Mode.IsEmpty())
		{
			return TEXT("fnb.Test pool <calm|live>");
		}
		return IsPoolCalm() ? TEXT("pool calm (FluidNinja turbulence off)") : TEXT("pool live (FluidNinja's own turbulence)");
	}
	if (Verb == TEXT("river"))
	{
		const FString Mode = Arg(1).ToLower();
		if (Mode == TEXT("start") || Mode == TEXT("stop"))
		{
			SetRiverRunning(Mode == TEXT("start"));
		}
		else if (Mode == TEXT("reset"))
		{
			ResetRiver();
		}
		else if (!Mode.IsEmpty())
		{
			return TEXT("fnb.Test river <start|stop|reset>");
		}
		return !HasRiver() ? FString(TEXT("no river (a BP_SwimRiver with a Ninja River)"))
			: FString::Printf(TEXT("river %s, %d river floaters"), IsRiverRunning() ? TEXT("running") : TEXT("stopped"), CountRiverFloaters());
	}
	if (Verb == TEXT("water"))
	{
		const FString Mode = Arg(1).ToLower();
		const bool bOn = Mode == TEXT("toggle") ? !(Hydro && Hydro->IsWaterControlled()) : Mode == TEXT("controlled") || Mode == TEXT("flat");
		if (bOn && Args.IsValidIndex(2) && Args[2].IsNumeric())
		{
			const float Height = FCString::Atof(*Args[2]);
			SetControlledWater(true, &Height);
		}
		else
		{
			SetControlledWater(bOn);
		}
		return Hydro && Hydro->IsWaterControlled() ? FString::Printf(TEXT("controlled water at %.0f"), Hydro->GetControlledWaterHeight()) : FString(TEXT("live river"));
	}
	if (Verb == TEXT("wind"))
	{
		const FString Mode = Arg(1).ToLower();
		if (Mode.IsEmpty() || Mode == TEXT("project"))
		{
			SetTestWind(false);
		}
		else
		{
			const float Speed = Mode == TEXT("calm") ? 0.f : Number(1, 0.f);
			if (Args.IsValidIndex(2) && Args[2].IsNumeric())
			{
				const float Degrees = FCString::Atof(*Args[2]);
				SetTestWind(true, Speed, &Degrees);
			}
			else
			{
				SetTestWind(true, Speed);
			}
		}
		return Hydro ? FString::Printf(TEXT("wind %.1f m/s toward %.0f deg (%s)"), Hydro->GetWind().SpeedMs, Hydro->GetWind().DirectionDegrees, Hydro->GetWind().bOverride ? TEXT("test") : TEXT("project")) : FString();
	}
	if (Verb == TEXT("push")) { Push(Number(1, 300.f)); return TEXT("pushed"); }
	if (Verb == TEXT("capsize") || Verb == TEXT("heel")) { Capsize(Number(1, 30.f)); return TEXT("heeled"); }
	if (Verb == TEXT("drop")) { Drop(Number(1, 3.f)); return TEXT("dropped"); }
	if (Verb == TEXT("here")) { BringHere(); return TEXT("brought in front of the view"); }
	if (Verb == TEXT("poke")) { return PokeAhead(Number(1, 2.f), Number(2, 10.f)); }
	if (Verb == TEXT("boat")) { SetBoat(Number(1, 0.f), Number(2, 0.f)); return TEXT("throttle and rudder set"); }
	if (Verb == TEXT("debug")) { ToggleDebugView(); return UFluidNinjaBuoyancySettings::IsDebugView() ? TEXT("debug view on") : TEXT("debug view off"); }
	if (Verb == TEXT("scenery")) { SetSceneryCollision(!Arg(1).Equals(TEXT("off"), ESearchCase::IgnoreCase)); return IsSceneryCollision() ? TEXT("bodies meet the scenery") : TEXT("free water: bodies ignore the scenery"); }
	if (Verb == TEXT("calibrate"))
	{
		// fnb.Test calibrate [apply] [view]: at the selected body (in its water), or around the view.
		const bool bApply = Args.ContainsByPredicate([](const FString& A) { return A.Equals(TEXT("apply"), ESearchCase::IgnoreCase); });
		const bool bView = Args.ContainsByPredicate([](const FString& A) { return A.Equals(TEXT("view"), ESearchCase::IgnoreCase); });
		const UPrimitiveComponent* Primitive = GetSelected() ? GetSelected()->GetSimulatedPrimitive() : nullptr;
		if (Hydro && Primitive && !bView)
		{
			Hydro->CalibrateFlowAt(Primitive->Bounds.Origin, bApply);
			return FString::Printf(TEXT("calibrating at %s"), *BodyName(GetSelected()));
		}
		if (Hydro)
		{
			Hydro->CalibrateFlow(bApply);
		}
		return TEXT("calibrating around the view");
	}
	if (Verb == TEXT("hud")) { bVisible = !bVisible; return bVisible ? TEXT("panel shown") : TEXT("panel hidden"); }
	if (Verb == TEXT("stir"))
	{
		if (!Hydro)
		{
			return TEXT("no FluidNinja Buoyancy");
		}
		const FString Which = Arg(1).ToLower();
		if (Which.IsEmpty() || Which == TEXT("report"))
		{
			return Hydro->DescribeStirApproach();
		}
		const TMap<FString, EFluidNinjaBuoyancyStirApproach> Approaches = {{TEXT("wakes"), EFluidNinjaBuoyancyStirApproach::Wakes},
			{TEXT("tag"), EFluidNinjaBuoyancyStirApproach::TrackingByTag}, {TEXT("type"), EFluidNinjaBuoyancyStirApproach::TrackingByType}, {TEXT("both"), EFluidNinjaBuoyancyStirApproach::WakesAndTracking}};
		if (const EFluidNinjaBuoyancyStirApproach* Approach = Approaches.Find(Which))
		{
			Hydro->SetStirApproach(*Approach);
			CachedAt = -1.0;
			return FString::Printf(TEXT("stir approach: %s"), UFluidNinjaBuoyancySubsystem::StirApproachName(*Approach));
		}
		return TEXT("fnb.Test stir <wakes|tag|type|both|report>");
	}
	if (Verb == TEXT("results"))
	{
		FString Out;
		for (const FResult& Result : Results)
		{
			Out += FString::Printf(TEXT("%s %s %s: %s\n"), Result.Verdict > 0 ? TEXT("PASS") : Result.Verdict < 0 ? TEXT("FAIL") : TEXT("INFO"), *Result.Test, *Result.Body, *Result.Text);
		}
		return Out.IsEmpty() ? FString(TEXT("no results yet")) : Out;
	}
	return TEXT("fnb.Test run <settle|capsize|drop|wind|throttle|gather|walkaway|all|ripple-dispersion|ripple-fps|ripple-kelvin|ripple-weight|ripple-rest|ripple-coupling|ripple-evers|ripple-advection|ripples> | ")
		TEXT("stop | reset [selected] | select <next|prev|name> | ")
		TEXT("water <live|controlled [height]|toggle> | wind <project|calm|m/s [toward degrees]> | push [N.s] | heel [deg] | drop [m] | here | ")
		TEXT("boat <throttle> [rudder] | scenery <on|off> | debug | calibrate [apply] [view] | stir <wakes|tag|type|both|report> | pool <calm|live> | ")
		TEXT("river <start|stop|reset> | poke [height cm = 2] [sigma cm = 10] | hud | results");
}

#if !UE_BUILD_SHIPPING
namespace
{
	FFluidNinjaBuoyancyTestBench* FindBench(UWorld* World)
	{
		auto From = [](UWorld* Candidate) -> FFluidNinjaBuoyancyTestBench*
		{
			const UFluidNinjaBuoyancySubsystem* Hydro = Candidate ? Candidate->GetSubsystem<UFluidNinjaBuoyancySubsystem>() : nullptr;
			// The factory this module binds makes only FFluidNinjaBuoyancyTestBench, so the subsystem's bench is one.
			return Hydro ? static_cast<FFluidNinjaBuoyancyTestBench*>(Hydro->GetBench()) : nullptr;
		};
		if (FFluidNinjaBuoyancyTestBench* Bench = From(World))
		{
			return Bench;
		}
		// Typed into the editor's console: the world in Play.
		if (!GEngine)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE || Context.WorldType == EWorldType::Game)
			{
				if (FFluidNinjaBuoyancyTestBench* Bench = From(Context.World()))
				{
					return Bench;
				}
			}
		}
		return nullptr;
	}

	FAutoConsoleCommandWithWorldAndArgs GFluidNinjaBuoyancyTestCommand(
		TEXT("fnb.Test"),
		TEXT("FluidNinja Buoyancy test bench (in Play, non-shipping builds): fnb.Test <verb> [arguments]. fnb.Test help (or no verb) lists the verbs: run <test> (all by ")
		TEXT("default), stop, results, reset, select, water, wind, push, heel, drop, capsize, here, boat, pool, river, stir, scenery, hud, debug, calibrate, poke."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			FFluidNinjaBuoyancyTestBench* Bench = FindBench(World);
			const FString Out = Bench ? Bench->Command(Args) : FString(TEXT("fnb.Test: run it in Play (no FluidNinja Buoyancy test bench in this world)"));
			UE_LOG(LogFluidNinjaBuoyancy, Display, TEXT("fnb.Test: %s"), *Out);
		}));
}
#endif
