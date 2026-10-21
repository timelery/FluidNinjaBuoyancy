#include "FluidNinjaBuoyancySwimmerBody.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFluidNinjaSwimmerBodyVolumeTest, "FluidNinjaBuoyancy.Swimmer.PawnBodyVolume",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFluidNinjaSwimmerBodyVolumeTest::RunTest(const FString& Parameters)
{
	// LF's real 30 cm radius / 172 cm tall humanoid, fully afloat: conserve the configured 50 litres without concentrating it in spheres.
	const FFluidNinjaSwimmerBodyShape Swimming = FFluidNinjaSwimmerBodyShape::Make(30.f, 86.f, -60.0, 0.0, 50.f);
	TestTrue(TEXT("Full submerged body keeps the authored 50 litres"), FMath::IsNearlyEqual(Swimming.DisplacedLitres, 50.f, 0.001f));
	TestTrue(TEXT("Freestyle body is shallow"), Swimming.HalfThickness < 8.0);
	// Integrate independent horizontal strips of the projected elliptical footprint, including the full vertical thickness.
	double IntegratedCm3 = 0.0;
	constexpr int32 Steps = 10000;
	const double Dx = 2.0 * Swimming.HalfLength / Steps;
	for (int32 Index = 0; Index < Steps; ++Index)
	{
		const double X = -Swimming.HalfLength + (Index + 0.5) * Dx;
		const double SectionScale = 1.0 - FMath::Square(X / Swimming.HalfLength);
		IntegratedCm3 += UE_DOUBLE_PI * Swimming.HalfWidth * Swimming.HalfThickness * SectionScale * Dx;
	}
	TestTrue(TEXT("Tapered source geometry conserves volume"), FMath::IsNearlyEqual(IntegratedCm3 / 1000.0, 50.0, 0.001));
	const FFluidNinjaSwimmerBodyShape Waist = FFluidNinjaSwimmerBodyShape::Make(30.f, 86.f, 0.0, 0.0, 50.f);
	const FFluidNinjaSwimmerBodyShape MidFade = FFluidNinjaSwimmerBodyShape::Make(30.f, 86.f, -21.5, 0.0, 50.f);
	const FFluidNinjaSwimmerBodyShape Chest = FFluidNinjaSwimmerBodyShape::Make(30.f, 86.f, -43.0, 0.0, 50.f);
	TestEqual(TEXT("Wading at waist leaves body pressure off"), Waist.WetScale, 0.f);
	TestEqual(TEXT("Wading fade remains smooth at its midpoint"), MidFade.WetScale, 0.5f);
	TestEqual(TEXT("Chest-deep body reaches full pressure"), Chest.WetScale, 1.f);
	const FFluidNinjaSwimmerBodyShape DisabledVolume = FFluidNinjaSwimmerBodyShape::Make(30.f, 86.f, -60.0, 0.0, 0.f);
	TestEqual(TEXT("Zero authored volume stays finite and empty"), DisabledVolume.DisplacedLitres, 0.f);
	return true;
}
#endif
