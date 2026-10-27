#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FluidNinjaWaterBirdComponent.generated.h"

class UFluidNinjaBuoyancySubsystem;
class UNiagaraSystem;
class USkeletalMesh;
class USkeletalMeshComponent;
struct FFluidNinjaWaterSample;

/** One part of a water bird whose crossings of the surface and motion in the water push the water (FluidNinja Water Bird): a sphere riding
 * on a bone, placed by its centre in the mesh's reference pose. A bird usually has five: its body, head, beak and two feet. */
USTRUCT(BlueprintType)
struct FLUIDNINJABUOYANCY_API FFluidNinjaWaterBirdPart
{
	GENERATED_BODY()

	/** The part's name, unique on the component: the Body Part picks the body by it, Get Part Location takes it, and the diagnostics list
	 * the parts by it (Body, Head, Beak, FootL, FootR, say). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird")
	FName Name;

	/** The bone the part rides on, in the mesh's skeleton: the part keeps its place relative to this bone, so it follows the animation. None:
	 * the mesh component itself (a part that does not move with any bone). A bone the mesh does not have: the part is left out (logged once
	 * when the mesh is bound). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird")
	FName Bone;

	/** The part's centre in the mesh's reference pose, in the mesh's component space (cm, as the mesh was imported; +X is usually forward):
	 * read it off the reference pose, for example as the position of a socket placed on the bone in the skeleton editor, or from the
	 * modelling package. It is turned into an offset from the bone once (the bone's reference transform), so the point need not lie on the
	 * bone. Any point. Default the component's origin. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird", meta = (Units = "cm"))
	FVector ReferencePoint = FVector::ZeroVector;

	/** The part's sphere radius at actor scale 1 (cm; it scales with the actor): how wide the water it disturbs is (each contact's footprint)
	 * and the band over which it goes from dry to wet (its wet fraction rises from 0 as the sphere's bottom touches the surface to 1 when its
	 * top goes under). Larger: wider, smoother rings and a gentler entry. 0.5-200. Default 5. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird", meta = (ClampMin = "0.5", ClampMax = "200", Units = "cm"))
	float RadiusCm = 5.f;

	/** The water moving with the part when it moves in the water (kg, at actor scale 1; it scales by the scale's cube): its momentum is what
	 * the part hands the water. A sphere's is half the water it displaces when fully under, 0.5 x 0.001 kg/cm3 x 4/3 pi r3 (0.26 kg at 5 cm,
	 * 4.6 kg at 13 cm); a flat foot or a spread wing pushes more than its volume says. Larger: stronger rings and splashes from this part.
	 * 0-100. Default 0.1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird", meta = (ClampMin = "0", ClampMax = "100", Units = "kg"))
	float AddedMassKg = 0.1f;

	/** Art multiplier on this part's splashes, on top of the component's Splash Scale: 0 for a part that should make rings but no spray (a
	 * foot paddling under the body), above 1 for one that throws more than its momentum says. 0-10. Default 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird", meta = (ClampMin = "0", ClampMax = "10"))
	float SplashScale = 1.f;

	/** While the part is wet, its motion along the water also pushes a wake: the drag it pays (Wake Drag Coefficient) pressed into the water
	 * at its bow, so a swimming bird leaves a V of rings. On for the body and the paddling feet; off (the default) for the head and beak,
	 * whose dips make rings by their up and down motion alone. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird")
	bool bWake = false;

	/** The width of the edge that presses this part's wake into the water (cm, at actor scale 1; it scales with the actor): a body's breast at
	 * the water line is far narrower than its sphere, and a narrower source throws shorter, steeper ripples. A ring's band follows the
	 * contact's width, so a 13 cm body's wake rings come out about ten times flatter than a 5 cm edge's, and they are the first a full ring
	 * table drops (fnb.Waves.RingsMax). The drag pressed in still comes from the part's wet sphere, at its bow. 0: the part's Radius Cm (one
	 * wide source). 0-200. Default 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird", meta = (ClampMin = "0", ClampMax = "200", Units = "cm", EditCondition = "bWake"))
	float WakeEdgeRadiusCm = 0.f;

	/** The downwash: a part beating down towards the water (a wing) disturbs it through the air before, or without, touching it. While the
	 * part's sphere is clear of the water and moving down with its bottom within this height of the surface (cm, at actor scale 1; it scales
	 * with the actor), the air it drives down presses on the water under it: air density x Air Area Cm2 x its speed down squared, over the
	 * time, fading to nothing at this height as (1 - height / reach)^2. It shows as a patch of fine ripples, no spray, about a tenth of what
	 * the same beat would hand the water by hitting it. About the wing's width: a pelican's wing, 30. 0 (the default): no downwash.
	 * 0-200. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird", meta = (ClampMin = "0", ClampMax = "200", Units = "cm"))
	float AirReachCm = 0.f;

	/** The wing area that drives the air down in the downwash (cm2, at actor scale 1; it scales with the scale's square): the push grows with
	 * it, and its ripples are spread over a patch of the same area under the part. The whole wing beats the air down, so use the wing's
	 * area even when the part sits on its outer half: a brown pelican's wing is about 2500 (3.5 kg on about 0.58 m2 of wings). Larger: a
	 * stronger, wider patch. 0-20000. Default 1000. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird", meta = (ClampMin = "0", ClampMax = "20000", EditCondition = "AirReachCm > 0"))
	float AirAreaCm2 = 1000.f;

	/** Art multiplier on the downwash's push, and so on its ripples. A gust shows on water as a roughening of millimetre ripples, finer than
	 * the ring layer draws (its finest rings are 2.5 cm), so the downwash's real push, drawn as those rings, comes out a slope of a few
	 * hundredths: invisible. Raise it until the patch under a beating wing reads (the drawn slope is capped by the project's Wave Ring Max
	 * Slope). 1: the push as the air gives it. 0-100. Default 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird", meta = (ClampMin = "0", ClampMax = "100", EditCondition = "AirReachCm > 0"))
	float AirStrength = 1.f;
};

/**
 * The water contacts of a body moved by animation rather than physics (Add Component → FluidNinja Water Bird): a bird that plunge-dives,
 * lands on, swims on, dips its head in and takes off from FluidNinja water, or any pawn moved by root motion or Mover. Each part (a sphere on
 * a bone) is followed every frame while contacts are on; the water it carries with it, its added mass times its wet fraction times its
 * speed down, is handed to the water as it changes, so going in presses the water down, coming up pulls it, and a bobbing body rings. Each
 * hand-over is one AddWaterContact on the FluidNinja Buoyancy subsystem: an impulse into FluidNinja Waves' grid, a fine ring in the surface
 * material's ring layer, and, where a part goes in or comes out fast, a small splash (and, for the body's entry, a touch of foam; for a dive,
 * the plunge splash).
 *
 * The component moves nothing: the movement that floats the bird reads Get Float Body Above Water Cm and Get Heave Frequency Hz (with Heave
 * Damping Ratio) to hold the body on its waterline. Contacts are off until Set Contacts Enabled (or Contacts Enabled in the Details panel):
 * turn them on near water, as each part samples the water every frame while they run. See Docs/WaterBirds.md.
 */
UCLASS(ClassGroup = "FluidNinja", meta = (BlueprintSpawnableComponent, DisplayName = "FluidNinja Water Bird"))
class FLUIDNINJABUOYANCY_API UFluidNinjaWaterBirdComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFluidNinjaWaterBirdComponent();

	/** The parts that touch the water: a sphere on a bone each (Name, Bone, Reference Point, Radius Cm, Added Mass Kg, Splash Scale, Wake,
	 * Wake Edge Radius Cm, Air Reach Cm, Air Area Cm2, Air Strength).
	 * Read when the mesh is bound (Begin Play, a new Mesh or mesh asset, a change in their number) and when contacts are switched on, so set
	 * them before then. Empty: the component does nothing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird")
	TArray<FFluidNinjaWaterBirdPart> Parts;

	/** How near the camera must be for the downwash (the parts' Air Reach Cm) to run (cm, straight-line from the player's camera to the
	 * owner): its fine ripples cannot be seen from further away, and each costs a slot in the simulation's ring table and a little GPU time
	 * per water pixel near it, so past this distance the parts neither gather nor send it (contacts, splashes and wakes go on). A 4 cm ripple
	 * is about a pixel at 20 m on a 1080p screen. 0: at any distance. 0-100000. Default 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird", meta = (ClampMin = "0", ClampMax = "100000", Units = "cm"))
	float AirViewDistanceCm = 0.f;

	/** The part that is the bird's body (by Name): its sphere floats the bird (Get Float Body Above Water Cm, Get Heave Frequency Hz) and only
	 * its entries drop foam (Entry Foam Rate). None, or a name no part has: no foam, and both functions return 0. Default Body. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird")
	FName BodyPart = TEXT("Body");

	/** The whole bird's mass at actor scale 1 (kg; it scales by the scale's cube). Only how high the body floats and how fast it bobs depend on
	 * it (the body sphere holding this mass up, Archimedes); the contacts use the parts' added masses. A brown pelican 3.5 (2-5), a mallard
	 * 1.1, a mute swan 10. 0.01-1000. Default 3.5. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird", meta = (ClampMin = "0.01", ClampMax = "1000", Units = "kg"))
	float MassKg = 3.5f;

	/** The splash spawned where a part goes in or comes out faster than Min Splash Speed Cm S, scaled by how hard (Splash Scale, the part's,
	 * the project's splash scale); any Niagara system works, a small droplet spray suits a bird. Loaded at Begin Play. Empty: the project's
	 * Splash System (Project Settings → FluidNinja Buoyancy → Effects). Set but failing to load: no splashes at all (logged once). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird")
	TSoftObjectPtr<UNiagaraSystem> SplashSystem;

	/** Multiplier on every splash this bird makes, on the project's splash scale (a part going in with power P = added mass x speed3 / (2 x
	 * radius) throws a splash of strength (P / Splash Reference Watts)^(1/3), capped at Splash Strength Ceiling; a part coming out 0.35 of
	 * that). Splashes under strength 0.2 are not drawn. Lower: smaller sprays; 0: no splashes, rings only. 0-10. Default 0.6, which keeps a
	 * pelican's dive to a small droplet splash. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird", meta = (ClampMin = "0", ClampMax = "10"))
	float SplashScale = 0.6f;

	/** A part crossing the surface slower than this makes no splash, only its rings (cm/s): a bird settling, paddling or dipping its head
	 * gently stays clean. Lower: splashes from gentler moves too. 0-2000. Default 60. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird", meta = (ClampMin = "0", ClampMax = "2000", Units = "cm/s"))
	float MinSplashSpeedCmS = 60.f;

	/** The plunge splash: the big splash a dive throws up, spawned once when the Body Part goes into the water at Plunge Min Speed Cm S or
	 * faster, on top of the parts' own small splashes (Splash System). It faces the bird's heading: the system's +X is the owner's forward
	 * laid on the water (the body's velocity when the owner points along the water's normal) and its Z the water's normal, so a system with
	 * sheets of water either side of the bird's path builds them along ±Y. Spawned at Plunge Splash Scale times the owner's scale (not
	 * scaled by the speed), ahead of
	 * the parts' own splashes in that frame's splash budget. The plunge splash is the dive's spray, so while it is set the parts' own
	 * splashes are held back around a dive: a part going in while the body, still above the water, comes down at Plunge Min Speed Cm S or
	 * faster behind it (a beak, a head), and every part for Plunge Quiet Seconds after the plunge (the feet following the body, the
	 * surfacing). Loaded at Begin Play. Empty (the default): no plunge splash, and every part splashes as usual. Set but failing to load:
	 * the same (logged once). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird")
	TSoftObjectPtr<UNiagaraSystem> PlungeSplashSystem;

	/** The body's speed going in (cm/s, world) at and above which it throws the plunge splash; the body coming down this fast while still
	 * above the water also holds back the splashes of parts going in ahead of it. Set it between the bird's landings and its dives
	 * (GetDiagnostics lists each part's last entry speed, entry_speed_cm_s). Lower: gentler entries plunge too. 0-5000. Default 600. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird", meta = (ClampMin = "0", ClampMax = "5000", Units = "cm/s"))
	float PlungeMinSpeedCmS = 600.f;

	/** The scale the plunge splash is spawned at, times the owner's scale: 1 is the system at its own size for a bird at actor scale 1, 2
	 * twice as large. 0: no plunge splash. 0-10. Default 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird", meta = (ClampMin = "0", ClampMax = "10"))
	float PlungeSplashScale = 1.f;

	/** How long after a plunge splash the parts' own splashes are held back (s): the feet going in behind the body and the bird surfacing
	 * would scatter droplet bursts over the plunge's spray. Longer: a cleaner dive; 0: only the parts going in ahead of the body are held
	 * back. 0-3. Default 0.6 (a pelican's feet follow its body in 0.07 s and it surfaces 0.4-0.5 s after the plunge). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird", meta = (ClampMin = "0", ClampMax = "3", Units = "s"))
	float PlungeQuietSeconds = 0.6f;

	/** Values for the plunge splash system's float user parameters, by name (as in its User Parameters, with or without "User."), set
	 * on every plunge splash this bird throws: the size of its splash per bird or per placed actor, from one splash system (a crown
	 * splash's diameter and height, say). They are in the system's own units, at actor scale 1: the splash is still spawned at Plunge
	 * Splash Scale times the owner's scale. Names the system does not have change nothing. Empty (the default): the system's own values. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird")
	TMap<FName, float> PlungeSplashParameters;

	/** Fresh foam per second the body part's entry drops over 1.5 x its radius for 0.25 s (aged foam at a fifth of it), on the foam's scale
	 * where about 1-5 whitens a churned patch in under a second (Add Water Foam). Only the Body Part's entries, and only with foam v2. 0: no
	 * foam. 0-5. Default 0.04: a trace of white where the body went in, no white patch. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird", meta = (ClampMin = "0", ClampMax = "5"))
	float EntryFoamRate = 0.04f;

	/** The drag coefficient of parts with Wake on, for the push their motion along the water gives it: 0.5 x water density x this x the wet
	 * part of the sphere's cross-section x speed2, pressed in at the part's bow every Ring Interval Seconds. Higher: stronger bow waves and
	 * wake. 0: no wakes. 0-2. Default 0.4 (a sphere in the water). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird", meta = (ClampMin = "0", ClampMax = "2"))
	float WakeDragCoefficient = 0.4f;

	/** The shortest time between two contacts a part sends while it moves in the water (s): what it hands the water is summed in between and
	 * sent once at least 0.5 kg·cm/s has gathered. Entries and exits are sent at once. Lower: more, smaller rings (each takes a slot in the
	 * simulation's ring table, fnb.Waves.RingsMax); higher: fewer, coarser ones. 0-1. Default 0.05. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird", meta = (ClampMin = "0", ClampMax = "1", Units = "s"))
	float RingIntervalSeconds = 0.05f;

	/** The shortest time between two wake contacts of a part with Wake (s): the drag it pays is summed in between and pressed in ahead of it.
	 * A swimming bird's wake is the train of these rings, so at 0.15 s and 55 cm/s they come 8 cm apart, close enough to read as one wake
	 * while a paddling bird takes few of the simulation's ring slots. Lower: a smoother wake, more slots; higher: separate rings. 0.02-1.
	 * Default 0.15. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird", meta = (ClampMin = "0.02", ClampMax = "1", Units = "s"))
	float WakeIntervalSeconds = 0.15f;

	/** How fast a part's push fades with its depth: what a part hands the water while its top is Depth below the surface reaches the surface
	 * as e^(-Depth / (this x its radius)), as a submerged source's short waves do (e^(-k z)). So a diving body slowing down 20-30 cm under the
	 * water stirs the surface little, while the same push at the surface rings fully. 0 turns the fade off (every push rings at full
	 * strength from any depth). 0-4. Default 0.5. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird", meta = (ClampMin = "0", ClampMax = "4"))
	float DepthFadeRadii = 0.5f;

	/** How quickly the bird's bob on its waterline dies out, as a damping ratio, for the movement that floats it (this component moves
	 * nothing; it holds the value next to Get Heave Frequency Hz): 0 bobs for ever, 1 settles without overshoot, above 1 creeps back. Floating
	 * birds 0.2-0.5. 0-2. Default 0.35. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird", meta = (ClampMin = "0", ClampMax = "2"))
	float HeaveDampingRatio = 0.35f;

	/** The skeletal mesh whose bones carry the parts. Empty: the owner's first skeletal mesh, found at Begin Play. A new mesh (or mesh asset)
	 * is bound on the next frame: the parts' offsets are rebuilt and this component then ticks after it, once its animation is done. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water Bird")
	TObjectPtr<USkeletalMeshComponent> Mesh;

	/** Contacts run (the parts sample the water every frame and push it). Off (the default): nothing runs; switch it on near water, from the
	 * movement or the brain, with Set Contacts Enabled. Set here, it is the state at Begin Play. */
	UPROPERTY(EditAnywhere, Category = "Water Bird")
	bool bContactsEnabled = false;

	/** Turns the contacts on or off. Switching them on starts every part over (no speed for its first frame, so a bird switched on while
	 * already in the water makes no entry splash) and rebuilds the parts' offsets from Parts. */
	UFUNCTION(BlueprintCallable, Category = "Water Bird")
	void SetContactsEnabled(bool bEnabled);

	/** The contacts run now. */
	UFUNCTION(BlueprintPure, Category = "Water Bird")
	bool AreContactsEnabled() const { return bContactsEnabled; }

	/** A part's centre in the world now (by Name), from its bone; the owner's location when the part, its bone or the mesh is unknown. */
	UFUNCTION(BlueprintPure, Category = "Water Bird")
	FVector GetPartLocation(FName Part) const;

	/** How far above the still water line the body part's centre floats when the bird rests on the water (cm, world, at the owner's current
	 * scale; it grows linearly with the scale, so divide by the scale for the value at scale 1): the body sphere's radius less its draft,
	 * the depth at which the sphere displaces Mass Kg of water. Negative: the centre floats under the water line; minus the radius: the
	 * sphere cannot hold the mass up and sinks. 0 without a body part. A 13 cm body holding 3.5 kg: 2.1 cm. */
	UFUNCTION(BlueprintPure, Category = "Water Bird")
	float GetFloatBodyAboveWaterCm() const;

	/** The natural frequency of the bird's bob on its waterline (Hz): sqrt(water density x g x waterplane area / (Mass Kg + the body part's
	 * Added Mass Kg, both scaled by the scale's cube)) / 2 pi, the waterplane being the body sphere's circle at the floating draft. 0 without a
	 * body part, or when the body sinks. A 13 cm body holding 3.5 kg with 2.5 kg added: 1.5 Hz. */
	UFUNCTION(BlueprintPure, Category = "Water Bird")
	float GetHeaveFrequencyHz() const;

	/** For QA: a JSON object with the counters (water_contacts on/off, water_entries, water_exits, contacts, splashes, plunges,
	 * splashes_held_back, air_pushes, air_impulse_sum_kg_cm_s, water_readback_fallbacks, impulse_abs_sum_kg_cm_s), the parts' names and, per part in the same order, its depth now, its deepest depth
	 * since Reset Peak Depths (cm; -10000 when not measured), its wet fraction and its speed at its last entry (cm/s; 0: none yet), then the
	 * floating body's height above the water and the heave frequency. */
	UFUNCTION(BlueprintPure, Category = "Water Bird")
	FString GetDiagnostics() const;

	/** Starts every part's deepest depth over (-10000: not measured). */
	UFUNCTION(BlueprintCallable, Category = "Water Bird")
	void ResetPeakDepths();

	/** Parts that went into the water (wet fraction from 0 to above it) while moving, since Begin Play. */
	UFUNCTION(BlueprintPure, Category = "Water Bird")
	int32 GetEntries() const { return Entries; }

	/** Parts that came out of the water (1 cm clear of it) while moving, since Begin Play. */
	UFUNCTION(BlueprintPure, Category = "Water Bird")
	int32 GetExits() const { return Exits; }

	/** Contacts sent to the subsystem (Add Water Contact), since Begin Play. */
	UFUNCTION(BlueprintPure, Category = "Water Bird")
	int32 GetContacts() const { return Contacts; }

	/** Contacts sent with a splash (strength 0.2 or more), since Begin Play (the subsystem's splash budget, fnb.Effects.SplashBudgetPerFrame,
	 * may still leave some undrawn). */
	UFUNCTION(BlueprintPure, Category = "Water Bird")
	int32 GetSplashes() const { return Splashes; }

	/** Plunge splashes sent (the Body Part going in at Plunge Min Speed Cm S or faster, with a Plunge Splash System), since Begin Play. */
	UFUNCTION(BlueprintPure, Category = "Water Bird")
	int32 GetPlunges() const { return Plunges; }

	/** Each part's depth under the surface on the last frame (cm, negative above it; -10000 when not measured), in Parts' order. */
	const TArray<float>& GetDepths() const { return Depths; }

	/** Each part's deepest depth since Reset Peak Depths (cm; -10000 when not measured), in Parts' order. */
	const TArray<float>& GetPeakDepths() const { return PeakDepths; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;

#if WITH_EDITOR
	/** Contacts Enabled changed in the Details panel while playing goes through Set Contacts Enabled. */
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	/** One part's state between frames. */
	struct FPartState
	{
		FVector Previous = FVector::ZeroVector;
		/** Where the part's wake is pushed in: its last direction along the water. */
		FVector WakeDirection = FVector::ZeroVector;
		/** The wet fraction, 0 to 1. */
		float Wet = 0.f;
		/** The water's vertical momentum carried with the part (kg·cm/s, down positive). */
		float Momentum = 0.f;
		/** Momentum handed to the water and not sent yet (kg·cm/s, down positive), and the wake's. */
		float Pending = 0.f;
		float PendingWake = 0.f;
		/** The downwash's push gathered and not sent yet (kg·cm/s, down positive). */
		float PendingAir = 0.f;
		float SinceSent = 0.f;
		float SinceWakeSent = 0.f;
		float SinceAirSent = 0.f;
		bool bInitialized = false;
		/** Momentum holds a measured value (the frame before had a speed): only then is its change handed over. */
		bool bHasMomentum = false;
		bool bWet = false;
	};

	/** The mesh to use: Mesh, else the owner's first skeletal mesh. */
	USkeletalMeshComponent* FindMesh() const;
	/** Binds InMesh when it, its asset or the number of parts changed since the last bind; returns the mesh when it has an asset. */
	USkeletalMeshComponent* ResolveMesh();
	/** Rebuilds the parts' bones and offsets from InMesh's reference pose, makes this component tick after it, starts the parts over. */
	void Bind(USkeletalMeshComponent* InMesh);
	void LoadSplash();
	void LoadPlunge();
	void UpdateContacts(float DeltaSeconds);
	/** The water at Location: GetWaterAtLocation, with the still water standing in when the waves' readback is over 0.5 s old. */
	bool SampleWater(const FVector& Location, FFluidNinjaWaterSample& OutWater);
	void Send(UFluidNinjaBuoyancySubsystem& Subsystem, const FVector& Where, float SurfaceZ, const FVector& Normal, float RadiusCm, float ImpulseDown,
		float SplashStrength, float FoamRate);
	/** The plunge splash at Where on the water, facing the owner's heading (Velocity's when the owner points along the water's normal). */
	void SendPlunge(UFluidNinjaBuoyancySubsystem& Subsystem, const FVector& Where, float SurfaceZ, const FVector& Normal, float RadiusCm,
		const FVector& Velocity);
	float OwnerScale() const;
	const FFluidNinjaWaterBirdPart* FindBodyPart() const;

	UPROPERTY(Transient)
	TObjectPtr<UNiagaraSystem> LoadedSplash;
	FSoftObjectPath LoadedSplashPath;
	/** Splash System is set but did not load: no splashes. */
	bool bSplashMissing = false;
	UPROPERTY(Transient)
	TObjectPtr<UNiagaraSystem> LoadedPlunge;
	FSoftObjectPath LoadedPlungePath;
	/** Seconds since the last plunge splash (large: none yet), and whether the body was last seen diving in (dry, coming down at plunge
	 * speed): the parts' own splashes are held back around a dive. */
	float SincePlunge = 1.0e6f;
	bool bBodyDiving = false;

	TWeakObjectPtr<USkeletalMeshComponent> BoundMesh;
	TWeakObjectPtr<const USkeletalMesh> BoundAsset;
	/** Per part, in Parts' order: its bone in the bound mesh (INDEX_NONE: the component itself), whether it was found, its offset from it. */
	TArray<int32> PartBones;
	TArray<bool> PartFound;
	TArray<FVector> PartOffsets;
	TArray<FPartState> States;
	TArray<float> Depths;
	TArray<float> PeakDepths;
	/** Each part's speed at its last entry while moving (cm/s; 0: none yet), in Parts' order (GetDiagnostics). */
	TArray<float> EntrySpeeds;

	int32 Entries = 0;
	int32 Exits = 0;
	int32 Contacts = 0;
	int32 Splashes = 0;
	int32 Plunges = 0;
	/** Splashes the parts would have thrown, held back around a dive (Plunge Quiet Seconds, the body diving in behind them). */
	int32 HeldBack = 0;
	/** Downwash patches sent (Air Reach Cm), since Begin Play. */
	int32 AirPushes = 0;
	/** What the downwash patches pressed in, summed (kg·cm/s, Air Strength included). */
	double AirImpulseSum = 0.0;
	int32 ReadbackFallbacks = 0;
	double ImpulseAbsSum = 0.0;
};
