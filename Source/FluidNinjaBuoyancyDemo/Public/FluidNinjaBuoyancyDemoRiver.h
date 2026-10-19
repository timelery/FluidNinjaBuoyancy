#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "FluidNinjaBuoyancyDemoLantern.h"
#include "FluidNinjaBuoyancyDemoNight.h"
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
 * The lantern night (StartLanternNight) turns the level's lights off and sets a group of lit paper lanterns on the slow river.
 *
 * An on-screen panel offers Start, Reset and Lantern night buttons: Tab brings the mouse cursor out to click them, and a click goes back
 * to mouse look. Keys: Enter starts or stops the river, R resets it (and ends a lantern night), L starts or ends the lantern night, Tab
 * shows or hides the mouse cursor. Console: fnb.Demo.River start | stop | reset | lanterns | day.
 */
UCLASS(meta = (DisplayName = "FluidNinja Buoyancy Demo River"))
class FLUIDNINJABUOYANCYDEMO_API AFluidNinjaBuoyancyDemoRiver : public AActor
{
	GENERATED_BODY()

public:
	AFluidNinjaBuoyancyDemoRiver();

	/** The actor tag (Actor > Tags) on the river's NinjaLive actor: the first actor with it that has a NinjaLive component is the river's
	 * simulation. Default FNB_RiverSimulation. With no such actor, Start and Reset only log a warning. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "River")
	FName SimulationTag = TEXT("FNB_RiverSimulation");

	/** The actor tag on the bodies that ride the river: actors with it and a FluidNinja Buoyancy component are moored while the river is
	 * still and go back to their start on Reset. Default FNB_RiverFloater. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "River")
	FName FloaterTag = TEXT("FNB_RiverFloater");

	/** FluidNinja's direct velocity while the river runs, written into the NinjaLive component's DirectVelocityX and DirectVelocityY (world
	 * X and Y, in FluidNinja's own units, not cm/s): larger runs the river faster, the sign sets its way. Default (0.5, 0). Read when the
	 * river starts; FluidNinja takes it up at its next parameter update. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "River")
	FVector2D FlowVelocity = FVector2D(0.5, 0.0);

	/** While the river is still, each awake floater is held to where it started by a soft, critically damped horizontal spring of this
	 * period, in seconds: shorter holds it more firmly, longer lets it wander more. Default 6; at least 0, and 0 does not hold them. Not while
	 * the river runs (except on the lantern night, which keeps them at the head), nor on the test bench's controlled water. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "River", meta = (ClampMin = "0", Units = "s"))
	float MooringPeriod = 6.f;

	/** Show the on-screen Start / Reset panel (Tab brings the mouse cursor out to click it). Read at Begin Play. Default on; off leaves the
	 * keys (Enter, R) and the fnb.Demo.River console command. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "River")
	bool bShowPanel = true;

	/** The lantern night's lanterns: how many, how tightly they are grouped, how far they keep from the floaters, and which lantern Blueprint
	 * rides each floating base (none: the plugin's FluidNinja water lantern). Read each time the lantern night starts. Default 20 lanterns
	 * in a 4.6 x 2.6 m group, 55 cm apart. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lantern Night")
	FFluidNinjaLanternGroup Lanterns;

	/** FluidNinja's direct velocity on the lantern night (DirectVelocityX/Y, world X and Y, FluidNinja's own units like Flow Velocity): the
	 * river's slow setting, so the lanterns drift rather than race. Larger runs it faster, the sign sets its way; its direction is also the
	 * way the lantern group is laid out downstream. Default (0.16, 0), about 0.6 m/s on the demo river against Flow Velocity's 1.9 m/s;
	 * (0, 0) leaves the river still. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lantern Night")
	FVector2D LanternFlowVelocity = FVector2D(0.16, 0.0);

	/** How dark the lantern night is: the moon's faint ambient (lux), the level's sky lights' share and the fixed exposure. Read when the
	 * lantern night starts; fnb.Demo.Night changes a running night live. Default moon 0.5 lux, sky lights off, exposure bias -3. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lantern Night")
	FFluidNinjaNightLook NightLook;

	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Demo")
	void StartRiver();

	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Demo")
	void StopRiver();

	/** Stops the current and puts every floater back where it started. */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Demo")
	void ResetRiver();

	UFUNCTION(BlueprintPure, Category = "FluidNinja|Demo")
	bool IsRunning() const { return bRunning; }

	/**
	 * The lantern night: the floaters go back to the head of the river and stay moored there, the level's lights go off (a faint moon
	 * and a fixed exposure instead: Night Look), Lanterns are set on the water just downstream of the floaters in a loose group, and
	 * the river runs at its slow setting (Lantern Flow Velocity), so the lit lanterns drift down it. Again: a fresh group.
	 */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Demo")
	void StartLanternNight();

	/** Ends the lantern night: the lanterns go, the lights come back on and the river stops. */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Demo")
	void EndLanternNight();

	UFUNCTION(BlueprintPure, Category = "FluidNinja|Demo")
	bool IsLanternNight() const { return bLanternNight; }

	/**
	 * How the floaters stir the river, measured: every floater gets this Interaction, the river resets, runs for 10 s, and the log gets
	 * each floater's route (wakes, or FluidNinja's own tracking), the water under it and how far it went, against the reference
	 * numbers; then the river resets and the floaters get their own Interaction back. Console: fnb.Demo.RiverTest [wakes | tracking].
	 */
	UFUNCTION(BlueprintCallable, Category = "FluidNinja|Demo")
	void RunInteractionTest(bool bFluidNinjaTracking);

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	/** The simulation running (its direct velocity and turbulence) or still (no velocity, no turbulence). */
	void ApplyFlow(bool bRun);
	void Moor(float DeltaSeconds);
	/** Every floater back where it started; returns how many. */
	int32 PutFloatersBack();
	void RemoveLanterns();
	UActorComponent* FindSimulation() const;
	void ShowCursor(bool bShow);
	void AddPanel();
	void RemovePanel();
	void TickTest();

	bool bRunning = false;
	/** The direct velocity while running: Flow Velocity, or Lantern Flow Velocity on the lantern night. */
	FVector2D ActiveFlow = FVector2D::ZeroVector;
	bool bLanternNight = false;
	TArray<TWeakObjectPtr<AActor>> LanternActors;
	TWeakObjectPtr<AActor> Night;
	/** The simulation's own turbulence (velocity noise, bitmap velocity input), given back when the river runs. */
	TMap<FName, double> Turbulence;
	bool bCursor = false;
	bool bStillAgain = false;
	TSharedPtr<SWidget> Panel;
	// The interaction test: its stage (-1 idle), when the stage began, where the floaters started and the Interaction each had.
	int32 TestStage = -1;
	double TestMark = 0.0;
	bool bTestTracking = false;
	TMap<TWeakObjectPtr<UFluidNinjaBuoyancyComponent>, FVector> TestStart;
	TMap<TWeakObjectPtr<UFluidNinjaBuoyancyComponent>, uint8> TestInteractionBefore;
};
