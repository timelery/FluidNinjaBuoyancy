#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "FluidNinjaBuoyancyDemoRiver.generated.h"

class SWidget;
class UFluidNinjaBuoyancyComponent;

/**
 * The demo river's switch. The river's FluidNinja simulation (the actor tagged Simulation Tag) is held still at Begin Play: no direct
 * velocity, and its velocity noise and bitmap inputs off, so its water calms. The floating bodies tagged Floater Tag are moored
 * softly where they start. Start sets the direct velocity (Flow Velocity, in FluidNinja's units), gives back the turbulence and
 * lets the floaters go: they drift down the river from its head. Reset stops the current and puts every one of them back.
 * FluidNinja takes the new values up at its next parameter update (ParamUpdateFrequency, 1 s by default).
 *
 * An on-screen panel offers Start and Reset buttons: Tab brings the mouse cursor out to click them, and a click goes back to mouse
 * look. Keys: Enter starts or stops the river, R resets it, Tab shows or hides the mouse cursor. Console: fnb.Demo.River start | stop | reset.
 */
UCLASS(meta = (DisplayName = "FluidNinja Buoyancy Demo River"))
class FLUIDNINJABUOYANCYDEMO_API AFluidNinjaBuoyancyDemoRiver : public AActor
{
	GENERATED_BODY()

public:
	AFluidNinjaBuoyancyDemoRiver();

	/** The tag on the river's NinjaLive actor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "River")
	FName SimulationTag = TEXT("FNB_RiverSimulation");

	/** The tag on the bodies that ride the river (they go back to their start on Reset). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "River")
	FName FloaterTag = TEXT("FNB_RiverFloater");

	/** FluidNinja's direct velocity while the river runs (DirectVelocityX/Y, world space). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "River")
	FVector2D FlowVelocity = FVector2D(0.5, 0.0);

	/** While the river is still, each floater is held to where it started by a soft spring of this period (0: not held). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "River", meta = (ClampMin = "0", Units = "s"))
	float MooringPeriod = 6.f;

	/** Show the Start / Reset panel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "River")
	bool bShowPanel = true;

	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Demo")
	void StartRiver();

	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Demo")
	void StopRiver();

	/** Stops the current and puts every floater back where it started. */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Demo")
	void ResetRiver();

	UFUNCTION(BlueprintPure, Category = "FluidNinja|Demo")
	bool IsRunning() const { return bRunning; }

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	/** The simulation running (its direct velocity and turbulence) or still (no velocity, no turbulence). */
	void ApplyFlow(bool bRun);
	void Moor(float DeltaSeconds);
	UActorComponent* FindSimulation() const;
	void ShowCursor(bool bShow);
	void AddPanel();
	void RemovePanel();

	bool bRunning = false;
	/** The simulation's own turbulence (velocity noise, bitmap velocity input), given back when the river runs. */
	TMap<FName, double> Turbulence;
	bool bCursor = false;
	bool bStillAgain = false;
	TSharedPtr<SWidget> Panel;
};
