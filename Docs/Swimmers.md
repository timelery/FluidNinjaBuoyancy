# Swimmers

Swimmers feel the water, and the water feels them. This page covers the swimmer systems:
the one water query swim code floats on, the provider interface that
lets any movement system drive a swimmer, skeleton profiles as data assets, the entry package for dives, wading pokes, and
horizontal stroke forces. The older swimmer features — body-part contacts, displacement spheres, wakes and the current ring —
are described in [FluidNinjaWaves.md § Swimmers](FluidNinjaWaves.md#swimmers) and still work exactly as documented there; this
page says what was added around them.

1. [The water at a point](#the-water-at-a-point)
2. [The swimmer provider](#the-swimmer-provider)
3. [The FluidNinja Swimmer component](#the-fluidninja-swimmer-component)
4. [Skeleton profiles](#skeleton-profiles)
5. [The entry package](#the-entry-package)
6. [Wading pokes](#wading-pokes)
7. [Strokes](#strokes)
8. [Live water for registered swimmers](#live-water-for-registered-swimmers)
9. [Settings](#settings)
10. [Verifying in PIE](#verifying-in-pie)
11. [Known limitations](#known-limitations)

---

## The water at a point

**Get Water At Location** (`UFluidNinjaBuoyancyWaterLibrary::GetWaterAtLocation`, or `UFluidNinjaBuoyancySubsystem::SampleWater`
in C++) is the one query a swim mode needs. It returns `FFluidNinjaWaterSample`:

| Field | Meaning |
|---|---|
| **bWater** | There is water here. False: every other field is 0. |
| **SurfaceZ** | The surface a swimmer floats on: the still water plus the waves, world cm. |
| **StillZ** | The still water the waves ride on (FluidNinja's own surface as drawn, or the far field). |
| **Normal** | The surface's normal from the waves' slope (straight up without waves). |
| **Current** | FluidNinja's current around the point (the ring that leaves a swimmer's own wake out), cm/s, Z 0. |
| **WaveHeight** | The free wave height η here, cm (0 without the modified core or outside a wave simulation). |
| **Depth** | How far the point is under SurfaceZ (negative above it). |
| **AgeSeconds** | How old the GPU readback the current and the waves came from is (two or three frames). |
| **bFromPatch** | The still water came from a floating body's patch (the Surface Mirror and the gather); false: the flat far field. |
| **bHasCurrent / bHasWaves** | The reading has arrived (the first calls at a new place return the still water alone). |

Where the numbers come from:
- **The still water** is what the floating bodies feel: the nearest body's patch when the point lies in one (FluidNinja's surface
  as the Surface Mirror drew it, carried forward by its rate of change), else the far field (a water volume's top, or whatever
  the project's derived subsystem returns from `FarFieldTop`).
- **The waves** are read from the wave simulation's pressure buffer at the point, as the free height η = B − A: the drawn wave
  height less every body's static head, so a swimmer rides a boat's wake and the pokes of a wave maker without sinking into
  anyone's standing hollow. The read shares the current probe's readback (one small square per swimmer per frame).
- **The current** is `GetCurrentAtLocation`'s ring mean around the point.

Call it every frame for a moving swimmer: each call also asks for a fresh reading there. Calls within 25 cm in the same frame
share one reading, so a swimmer's locomotion, its wake and its contacts all cost one probe.

**The drift.** A swim mode should not take `Current` raw: the reading is a few frames old and FluidNinja's eddies wobble it.
**Filter Drift Current** (`FilterDriftCurrent(Previous, Measured, bMeasured, DeltaSeconds)`) low-passes it over **0.5 s** and
dead-bands it below **10 cm/s** (a still pool reads a few cm/s of noise; the swimmer must not creep). Registered swimmers get
this filtering for free (`GetSwimmerWater`, below): the swim mode floats on `SurfaceZ` and drifts with the filtered current.

---

## The swimmer provider

`IFluidNinjaSwimmerProvider` is how anything that moves a character tells the plugin how the body stands each frame. One
function matters:

```
bool GetSwimmerPose(FFluidNinjaSwimmerPose& OutPose)
```

`FFluidNinjaSwimmerPose` carries the body (location, velocity, facing, capsule), its state (swimming / wading / airborne), its
head, chest and hips as spheres, its parts (the bones that splash and press rings, from the profile), and what only the
locomotion knows: the submerged fraction, the stroke phase, and the stroke's push on the water (`StrokeImpulse`). Two optional
functions return the swimmer's own wake tuning (`GetSwimmerWakeTuning`) and its profile (`GetSwimmerProfile`).

What the plugin reads from the pose: the location, the head, chest and hips spheres, the parts, the mass, `StrokeImpulse`,
(for the player's pawn) swimming / airborne, and (for a registered swimmer) the velocity and the water Z:
- **Velocity** drives a registered swimmer's wake (its speed through the water, and its plunge going in). Zero: the mesh's own
  motion frame to frame stands in, as for a mesh registered without a provider;
- **bHasWaterZ / WaterZ** give a registered swimmer that is not on the water query its still water (its parts, dive, spheres,
  strokes and wake), in place of its registration's Water Z;
- the swimmer's **wake tuning** (`GetSwimmerWakeTuning`, Override on) replaces the project's wake settings for its wake and its
  strokes' wake points.

Reserved — carried for a project's own code, not read by the plugin: facing, the capsule's half height and radius, wading, the
submerged fraction, the stroke phase, and on each part its kind and density (the profile's per-part buoyancy scale is not on the
pose at all).

Who supplies the pose:
- **Nothing** (the default): the plugin builds it from the mesh's bones through the profile fitting it, and from a Character
  Movement component when the owner has one (`UFluidNinjaSwimmerProviderLibrary::BuildDefaultPose`). This is exactly the old
  behaviour: the demo's Manny swims unchanged.
- **The FluidNinja Swimmer component** (below): the default implementation plus strokes, the entry event and the live water.
- **Your own class**: implement the interface on the pawn, a component, or in Blueprint, and register the mesh with it. A
  project on another movement system (Mover, say) implements `GetSwimmerPose` once; contacts, displacement, strokes, the dive
  and the live water then run on it unchanged, with no subsystem overrides. A registered swimmer's
  wake runs on the same pose: its hips and head spheres say whether it is in the water and at the surface (any rig's bones, through
  its profile), and its velocity is the pose's.

`UFluidNinjaSwimmerProviderLibrary` has the lookups: `FindSwimmerProvider(Actor)` (the actor, else its first component that
implements the interface), `FindSwimmerMesh(Actor, Profile)`, `BuildDefaultPose(...)`.

---

## The FluidNinja Swimmer component

**Add Component → FluidNinja Swimmer** (`UFluidNinjaSwimmerComponent`) makes the owner a swimmer with no code:

- registers the owner's mesh at Begin Play (the first skeletal mesh fitting the profile) and unregisters at End Play;
- is itself the swimmer's provider: pose from the mesh and profile, swimming/falling from Character Movement — a Mover or
  custom pawn overrides `IsSwimmingNow()` / `IsAirborneNow()` in a small subclass;
- **Water From Query** (default on): the swimmer's water is sampled at its hips every frame — `GetWater` returns the sample and
  the filtered drift current, the one source of truth for the swim locomotion. Off: **Still Water Z** stands, as a plain
  `RegisterSwimmer`'s Water Z does;
- **Wake Tuning** (Override on): this swimmer's own wake strength, gain, brush size, minimum speed and current ring, in place of
  the project's Swimmers settings, for its wake and its strokes' wake points (read every frame; values outside their ranges are
  held to them);
- **Displacement**, **Part Contacts**, **Whole Body**, **Water From Query** and **Still Water Z** take effect at once, also while
  playing: a Blueprint write goes through their setters (`SetDisplacement` and the others) and a Details-panel change in PIE
  through `PostEditChangeProperty`, both updating the registration in place;
- **On Entry** (Blueprint-assignable): the owner dived — bind the entry package to it (see below);
- `AddStrokeImpulse` / `SetStrokePhase`: the animation feeds strokes in (see [Strokes](#strokes));
- `GetSwimmerStats`: what the body parts did to the water.

A mesh registered through plain `RegisterSwimmer(Mesh, Source)` keeps working as before (no provider, Water Z fixed).

---

## Skeleton profiles

`UFluidNinjaSwimmerProfile` (a data asset) replaces the hard-coded mannequin bone table: which bones are the head, chest and
hips (the spheres), which bones are the parts that meet the water (each with radius, added mass, contact area, kick and
whole-body flags, density, buoyancy scale), the entry/exit/dive thresholds, and the splash / impulse / sound / stroke scales.

The plugin ships three presets (`UFluidNinjaSwimmerProfile::GetPreset`):

| Preset | Skeleton | Notes |
|---|---|---|
| **Manny** | UE5's mannequins (SK_Mannequin: Manny, Quinn) | The numbers are the original table, unchanged: the demo behaves identically. |
| **MetaHuman** | The MetaHuman body (`metahuman_base_skel`) | The MetaHuman body skeleton uses Manny's bone names (verified against the UE 5.8 engine asset), so this is Manny's profile under its own name. TODO: check the chest bone's height on a tall MetaHuman in the editor. |
| **Quadruped** | A four-legged creature | An example: pelvis/spine/neck/head and four kicking feet (`hand_l/r` as fore feet, as many animal rigs name them). Copy it into a data asset and rename the bones for the rig. |

Without a profile, a swimmer gets the preset whose bones its mesh has (`FindPresetFor`: a mesh with `lowerarm_r` is a
mannequin/MetaHuman; else the quadruped). To make a creature swim: create a Data Asset of class FluidNinjaSwimmerProfile, pick
a Preset, press **Apply Preset**, rename the bones and tune the radii/masses, and set it on the FluidNinja Swimmer component
(or pass it to the extended `Register`).

---

## The entry package

A **dive** is the swimmer's chest going under at or above the profile's **Dive Min Cm S** (150 cm/s). It fires once per entry:

- `FFluidNinjaBuoyancySwimmers::OnEntry` (native, per world) and `OnAnyEntry()` (static) broadcast
  `(Mesh, Location, SpeedCmS, MassShareKg)` — the mass share is the body's volume under the surface at that moment, at the
  human body's density, capped at the body's mass;
- the FluidNinja Swimmer component relays it to its Blueprint-assignable **On Entry** `(Location, SpeedCmS, MassShareKg,
  SoundScale)` — bind the effects there: cavity, crown, bubble column, entry sound. The profile's **Sound Scale** rides along;
- the subsystem fires the project's entry splash from it (the hull entry splash, sized by the dive's entry power — the
  subsystem-side handler is part of the same change set).

The parts' own entries (hands, head, feet) still make their splashes and rings exactly as before; the dive event is the one
whole-body moment the big effects hang on.

---

## Wading pokes

**Poke At** (`UFluidNinjaBuoyancyWaterLibrary::PokeAt(Location, RadiusCm, StrengthNs)`) presses the water down at a point with
an impulse in newton-seconds over a disc — the footstep hook. A wading footstep is about **0.6 N·s over 8 cm** (a foot's added
water, 0.6 kg, stamped in at 1 m/s); a paddle blade or a thrown stone scales up from there. It routes into FluidNinja Waves'
impulse path at the next frame (scaled by Swimmer Impulse Scale and Wave Impulse Scale), so each step rings outward and
reflects like every other wave. Negative strength lifts the water. Nothing happens without the modified core; the footstep's
own splash and sound stay the footstep system's (a footstep system calls PokeAt per water step and keeps its own foley).

---

## Strokes

`FFluidNinjaSwimmerPose::StrokeImpulse` is the push a stroke gives the water in a frame, kg·cm/s, world: the momentum the arms
and legs hand the water — the body's thrust reversed. Feed it from the animation (hand speed on the pull), the locomotion's
acceleration, or a curve; on the component, call **Add Stroke Impulse** as the stroke pulls (calls in a frame add up).

What the plugin makes of it, scaled by **Swimmer Stroke Scale**:
- the **horizontal** part becomes a wake point behind the swimmer in FluidNinja's point input: the written speed is the impulse
  over the profile's **Stroke Water Kg** (the water a stroke sweeps, ~2 kg for a hand and forearm). FluidNinja ignores points
  under about 40 cm/s (see [FluidNinjaWaves.md § Wakes and the current](FluidNinjaWaves.md#wakes-and-the-current)) — a 2 kg
  stroke needs about 0.8 kg·m/s to show, which is a hard pull; points under the cut-off are not written at all, so idle strokes
  cost nothing and paint no density;
- the **vertical** part becomes a small impulse on the waves where the stroke pressed (a downward pull lifts the swimmer and
  presses the water: rings).

The stroke phase (`SetStrokePhase`, 0–1) rides on the pose for anything that wants to time effects to the stroke cycle.

---

## Live water for registered swimmers

A swimmer registered with **Water From Query** has its water sampled at its hips once per frame, and everything reads that one
sample: the parts' contacts and the dive judge entry against **SurfaceZ** (the surface with waves — a wave crest reaching a
hanging hand makes a contact), the displacement spheres and the wakes stand in **StillZ** (against the waves they would chase
their own hollows), and the swim locomotion reads the same sample back:

- Blueprint: **Get Swimmer Water** (`UFluidNinjaBuoyancyWaterLibrary::GetSwimmerWater(Mesh)`) → the sample + the filtered
  drift current; or the component's **Get Water** / **Get Drift Current**.
- C++: `FFluidNinjaBuoyancySwimmers::GetWater(Mesh, Sample, Drift)`.

So the swim mode floats on the same surface the splashes and rings are judged against: one source of truth, no second query.

---

## Settings

Everything in [FluidNinjaWaves.md § Swimmers](FluidNinjaWaves.md#swimmers) still applies. Added under **Project Settings →
FluidNinja Buoyancy → Swimmers**:

| Setting | Default | Meaning |
|---|---|---|
| Swimmer Stroke Scale | 1 | Scales the strokes' wake points and wave impulses (0: strokes do nothing). Editable whatever Swimmer Contacts says: registered swimmers' strokes run without it. |

Swimmer Splash Scale and Swimmer Impulse Scale are also editable with Swimmer Contacts off: a registered swimmer's dive splash
and **Poke At** use them either way.

Per swimmer, the profile's scales (Splash, Impulse, Sound, Stroke Water Kg) multiply or replace the project values as documented
above, and a provider's Wake Tuning with Override on (the component's **Wake Tuning**) replaces the project's Swimmer Wake
Strength, Gain, Size, Min Speed and Current Ring Inner / Outer for that swimmer's wake and strokes.

---

## Verifying in PIE

**On the demo map (FNB_DemoPool, Manny):**
1. **Swim**: Play, walk into the pool and swim. As before: stroke splashes and rings from the hands and feet, the lying body's
   displacement rings, a wake only against a fast current. `fnb.Buoyancy 2` shows the debug view; `stat FluidNinjaBuoyancy`
   shows the counters. Behaviour must match the pre-change demo (same bone table through the Manny preset).
2. **Dive**: jump from the pool edge. The log prints `swimmer ... dived at (x, y), v cm/s, m kg under` once per entry, and the
   entry splash fires at the chest's entry point. `GetSwimmerStats` on the pawn's mesh counts the parts' burst.
3. **Wade**: walk in the shallows; wading wake points at the feet as before. From a test Blueprint, call **Poke At** at a foot
   (0.6 N·s, 8 cm) each step and watch the step rings spread (Waves on, modified core).
4. **Water query**: from a test Blueprint on the pawn, call **Get Water At Location** at the pawn every frame and print
   SurfaceZ − StillZ while a crate bobs nearby: the wave height follows the crate's rings. Add a **FluidNinja Swimmer**
   component to the demo character and check **Get Water** returns the same, with the drift current settling to ~0 in the pool
   and to the river's speed in the lane.
5. **Strokes**: call **Add Stroke Impulse** (e.g. 2 kg × 150 cm/s backward = 300 kg·cm/s per pull) on the component while
   swimming; `fnb.Buoyancy.Wake` counters and the dye show a point per pull; in still water nothing under the 40 cm/s cut-off
   is written.

---

## Known limitations

- The wave height and current are GPU readbacks: two or three frames old, and absent for the first frames at a new place. The
  drift filter and the swim mode's own springs are expected to absorb this.
- The waves part of the sample needs the modified core (FluidNinja Waves) and a wave simulation under the point; elsewhere the
  sample is the still water alone (`bHasWaves` false).
- The sample reads the full wave field B − A, which includes the swimmer's own displacement (the player's lying body, which holds
  Swimmer Volume Litres, or a registered swimmer's spheres). A swimmer floating on SurfaceZ therefore sits a little low in its own hollow; the swim mode's float spring
  hides it. Subtracting a swimmer's own field the way bodies do (B − B_self) would need a self field per swimmer and is not
  built.
- One sample per swimmer per frame, at the hips: a whale-sized creature wanting per-part water needs its own queries.
- Dives are judged on the chest sphere alone; a flat belly-flop and a clean dive fire the same event (the speed and mass share
  tell them apart).
- The unregistered player's pawn's wake always runs on the project's wake settings and its pawn's velocity; register its mesh
  (the FluidNinja Swimmer component) for a wake of its own.
- A registered swimmer does not wade: its wake is judged from its hips sphere, and the pose's `bWading` is reserved (the
  unregistered player's pawn wades, its wake at each foot).
- Reserved pose fields (not read by the plugin): Facing, HalfHeightCm, RadiusCm, bWading, SubmergedFraction, StrokePhase, and each
  part's Kind and DensityKgM3; the profile's per-part BuoyancyScale is not carried on the pose.
