# Water birds

A bird that plunge-dives, lands on the water, floats and paddles, dips its head and takes off again is moved by its animation
(root motion, Mover, a montage), not by physics, so the floating-body machinery never sees it. The **FluidNinja Water Bird**
component (`UFluidNinjaWaterBirdComponent`) gives such a body its water contacts: a few spheres on its bones, followed every frame,
hand the water the momentum they carry in and out of it. What the water does with that is what real footage shows: clean
concentric rings spreading for metres, fine ripples of a few centimetres close to the bird, a small droplet splash where it goes
in fast, and no white patches. It works for any animated pawn, not only birds: a dog wading, an otter, a creature on Mover.

1. [What it does](#what-it-does)
2. [Setting it up](#setting-it-up)
3. [Parts](#parts)
4. [The momentum model](#the-momentum-model)
5. [Entries, exits and splashes](#entries-exits-and-splashes)
6. [The ring layer](#the-ring-layer)
7. [Floating the bird](#floating-the-bird)
8. [Tuning](#tuning)
9. [Diagnostics](#diagnostics)
10. [Known limitations](#known-limitations)

---

## What it does

Every frame while its contacts are on, for each part:
- its centre in the world, from the animated bone (the bone's transform times the part's offset from it, taken once from the
  mesh's reference pose);
- its velocity, from its last position;
- the water under it, from **Get Water At Location** (the still water plus the waves; the still water alone while the waves'
  readback is more than 0.5 s old), and its depth under the surface.

From these it works out how much of the water's vertical momentum the part carries ([The momentum model](#the-momentum-model)) and
hands every change of it to the water through **Add Water Contact** on the FluidNinja Buoyancy subsystem, which turns one contact
into:
- an impulse into FluidNinja Waves' grid (the long waves, the ones the floating bodies feel), as a Gaussian whose σ is at least
  two wave cells;
- a ring in the surface material's ring layer (the short ripples the grid is too coarse for, [The ring layer](#the-ring-layer));
- a splash, when the part crossed the surface fast;
- a little foam, for the body's entry;
- for a dive (the body going in fast), the **plunge splash**: one bigger splash facing the bird's heading
  ([The plunge splash](#the-plunge-splash)).

The component moves nothing. The movement that floats the bird reads **Get Float Body Above Water Cm** and **Get Heave Frequency Hz**
to hold the body on its waterline ([Floating the bird](#floating-the-bird)). It also presses no standing hollow: a resting bird's
displacement lies under its own body and below the grid's resolution, so only its motion (bobbing, paddling, dipping) makes rings.

**What it needs.** FluidNinja Buoyancy on, and water the subsystem knows at the bird (a floating body's patch or the far field: a
water volume, or the project's own). The grid part needs FluidNinja Waves (the modified core) with **Wave Impulse** on; the ring
part needs **Wave Rings** on and a surface material with the ring step (`WaveRingTable`, `WaveRingLUT`); the splash needs a Splash
System (the component's or the project's); the foam needs foam v2. Without one of them that part of the contact does nothing, and
the rest still works.

---

## Setting it up

1. **Add the component** to the pawn: Add Component → **FluidNinja Water Bird** in its Blueprint, or in C++ (the pawn's module
   lists `FluidNinjaBuoyancy` in its Build.cs dependencies):

   ```cpp
   WaterBird = CreateDefaultSubobject<UFluidNinjaWaterBirdComponent>(TEXT("WaterBird"));

   FFluidNinjaWaterBirdPart Body;
   Body.Name = TEXT("Body");
   Body.Bone = TEXT("spine_01");
   Body.ReferencePoint = FVector(-2.6, 0.0, 28.7);   // the body's centre in the reference pose, component space, cm
   Body.RadiusCm = 13.f;
   Body.AddedMassKg = 2.5f;
   Body.bWake = true;
   Body.WakeEdgeRadiusCm = 5.f;                      // the breast at the water line, not the 13 cm sphere
   WaterBird->Parts.Add(Body);
   // ... Head, Beak, FootL, FootR the same way
   WaterBird->MassKg = 3.5f;
   ```

2. **Mesh**: leave it empty for the owner's first skeletal mesh (found at Begin Play), or set the one whose bones carry the parts.
   The component ticks after physics and after that mesh's own tick, so it reads each frame's finished pose.
3. **Parts**: one sphere per body part that touches the water ([Parts](#parts)). A bird the size of a brown pelican (3.5 kg):

   | Name | Bone (your rig's) | Radius Cm | Added Mass Kg | Splash Scale | Wake | Wake Edge Radius Cm |
   |---|---|---|---|---|---|---|
   | Body | a spine bone, the point at the body's centre | 13 | 2.5 | 1 | on | 5 |
   | Head | the head bone | 5 | 0.3 | 1 | off | 0 |
   | Beak | the beak's tip bone | 2.5 | 0.08 | 1 | off | 0 |
   | FootL / FootR | the toe bones, the point under the webbing | 5 | 0.12 | 1 | on | 0 |
   | WingL / WingR (optional) | an outer-wing bone, the point among the primaries | 7 | 0.3 | 0 | off | 0 |

   The wings also get **Air Reach Cm** 30 and **Air Area Cm2** 2500 (the [downwash](#the-downwash)): a beat that comes down near the
   water ripples it whether or not the feathers touch.

4. **Body Part** names the body (default `Body`), and **Mass Kg** is the whole bird's mass at scale 1: together they give how high it
   floats and how fast it bobs.
5. **Switch the contacts on near water**: **Set Contacts Enabled** (true) from the movement or the AI when the bird comes within a few
   metres of the water or starts a water move, and false when it flies off. Each part samples the water every frame while they run, so
   a flock flying high should have them off. **Contacts Enabled** in the Details panel is the state at Begin Play (default off).
   Switching them on starts every part over: a bird switched on while already in the water makes no entry splash.
6. **Splash System**: a small droplet spray suits a bird. Empty uses the project's Splash System (Project Settings → FluidNinja
   Buoyancy → Effects). A path that fails to load gives no splashes at all, with a warning in the log, rather than someone else's.
7. **Plunge Splash System** (optional), for a bird that dives: the big splash its body throws going in, with **Plunge Min Speed Cm
   S** between its landings and its dives ([The plunge splash](#the-plunge-splash)).

**Finding a reference point.** Reference Point is the part's centre in the mesh's component space with the mesh in its reference
pose (cm, Unreal's axes as imported): read it from the modelling package's rest pose (converted to Unreal's axes and
centimetres), or from the skeleton editor with the preview in the reference pose. The component turns it into an offset from the
bone, so the point need not lie on the bone. Check it in Play: draw a debug sphere of Radius Cm × the actor's scale at **Get Part
Location** (Part) every frame; it must sit on the body part through the whole animation.

---

## Parts

| Field | Meaning |
|---|---|
| **Name** | The part's name: Body Part picks the body by it, Get Part Location takes it, the diagnostics list the parts by it. |
| **Bone** | The bone it rides on. None: the mesh component itself. A bone the mesh lacks: the part is left out (logged once). |
| **Reference Point** | Its centre in the reference pose, component space, cm. |
| **Radius Cm** | The sphere's radius at actor scale 1 (it scales with the actor), 0.5-200 cm. It is the width of the water the part disturbs (each contact's footprint, which also picks the ring band) and the band over which it goes from dry to wet. |
| **Added Mass Kg** | The water moving with the part (kg at scale 1; it scales by the scale's cube): how hard the part pushes the water. A sphere's is half the water it displaces fully under, 0.5 × 0.001 × 4/3 π r³: 0.26 kg at 5 cm, 4.6 kg at 13 cm. A flat foot or a spread wing pushes more than its volume says; a streamlined beak less. |
| **Splash Scale** | Art multiplier on this part's splashes (0: rings but no spray, as for a foot paddling under the body). |
| **Wake** | While wet, its motion along the water also pushes a wake at its bow (the body and the paddling feet). |
| **Air Reach Cm** | The [downwash](#the-downwash): how high above the water (from the sphere's bottom, cm at scale 1) a part beating down still disturbs it through the air. About the wing's width (a pelican's: 30). 0: none. |
| **Air Area Cm2** | The wing area that drives the air down (cm² at scale 1): the push grows with it and its ripples spread over a patch that size. Use the whole wing's area (a brown pelican's: about 2500). |
| **Wake Edge Radius Cm** | The width of the edge that presses the wake in (cm at scale 1): a body's breast at the water line, far narrower than its sphere. The wake's rings take their band from it, so a 13 cm body sphere's wake comes out about ten times flatter than a 5 cm edge's (and a full ring table drops it first). 0: the part's Radius Cm. |

The parts are spheres because a sphere's wet fraction and footprint follow from one number. A wing dragged along the water is a
sphere of the wing's width placed at its middle, with the added mass of the water the wing moves. A wing whose beats bring its
tips down onto the water (a bird stretching or beating its wings while floating) rings the water under it with a part on the
outer wing: a radius of 7.5 cm or less keeps its rings crisp, and Splash Scale 0 gives rings without spray.

---

## The momentum model

The water near a moving part moves with it: its **added mass** m_a. While the part is in the water the water carries vertical
momentum with it, and every change of that momentum is an impulse the part gives the water:

- **Wet fraction** f = smoothstep(−r, r, depth), r = Radius Cm × scale: 0 while the sphere is clear of the water, 1 once its top is
  under, smooth in between.
- **Carried momentum** P = m_a · f · (−v_z), m_a = Added Mass Kg × scale³, positive pressing down (kg·cm/s).
- **Handed over** each frame: ΔP = P − P_previous, added to the part's pending impulse, faded by the part's depth: e^(−d / (Depth
  Fade Radii × r)), d how far the part's top is under the surface. A submerged source's short waves reach the surface as e^(−k z),
  so a diving body slowing down 20-30 cm under the water stirs the surface little; at the surface a push rings fully. **Depth Fade
  Radii** 0.5 (0: no fade).

So going in fast presses the water down (P jumps from 0 to m_a v as the sphere goes under), slowing under the surface or turning up
pulls it back, and coming out releases the rest. Without the depth fade a whole dive's impulses add up to zero: in linear
(potential-flow) theory the water's momentum returns to it when the body stops, and what stays behind is the waves made along the
way, at the right times and places. With it, what happens deep down reaches the surface weaker, as it does. A body bobbing on its waterline hands over a small push and pull each cycle, and rings gently.

A frame without a speed (the first after the contacts switch on or the part finds water again, a jump of more than 150 cm, a frame
longer than 0.2 s) only takes the new P and hands nothing over, so a bird teleported or switched on in the water makes no false
impulse.

**Wake.** A wet part with **Wake** on pays drag as it moves along the water, 0.5 ρ Cd (π r² f) |v_h|² over the frame (kg·cm/s,
Cd = **Wake Drag Coefficient**), faded by depth as above, pressed into the water at its bow, the part's centre plus its direction of
travel times r. A pelican-sized body floating (f about 0.38) and paddling at 55 cm/s sends about 18 kg·cm/s every 0.15 s. The
wake's footprint is the part's **Wake Edge Radius Cm** when set (the narrow front that really pushes the water), else its radius.

**The downwash.** A part with **Air Reach Cm** above 0 that is clear of the water (its sphere's bottom h above the surface) and
moving down drives air onto the water under it: the momentum flux of the air it pushes, ρ_air A v_z² over the frame (ρ_air
1.2 × 10⁻⁶ kg/cm³, A = **Air Area Cm2** × scale²), times (1 − h / reach)², so nothing at the reach and all of it at the
surface. It is sent at most every **Ring Interval Seconds**, once 1.5 kg·cm/s has gathered, as three fine ripples (4 cm wide)
at random points of a patch of area A under the part: a gust's patch of short ripples, no spray. A pelican's wing beating down
at 4 m/s right over the water presses about a tenth of what slapping it would; air being 800 times lighter than water, it stays
a light touch. Once the part touches the water the momentum model above takes over. `air_pushes` in Get Diagnostics counts the
patches.

**Sending.** What gathered is sent as one contact at the part's (x, y) on the surface at most every **Ring Interval Seconds**, once
it reaches 0.5 kg·cm/s; the wake at most every **Wake Interval Seconds** (0.15: rings 8 cm apart at 55 cm/s, one wake), at the bow.
Entries and exits are sent at once, with what gathered so far and the splash. Each contact's footprint is the part's radius
(0.5-200 cm), a wake contact's its Wake Edge Radius Cm when set.

---

## Entries, exits and splashes

- **An entry** is a moving part's wet fraction rising from 0 (its sphere touching the surface). **An exit** is a wet part coming 1 cm
  clear of the water: the margin keeps a part resting at the water line on small waves from flickering in and out.
- **The splash** of an entry is Splash Scale × the part's Splash Scale × the project's splash strength of the slam's power,
  m_a v³ / (2 r) (in watts: SI units; |v| the part's speed): the momentum m_a v stopped over the part's own size at speed v, times v.
  The project's scale makes that (power / Splash Reference Watts)^(1/3), capped at Splash Strength Ceiling. Only when the part
  moves at least **Min Splash Speed Cm S**. An exit throws 0.35 of the splash an entry would at its speed.
- Splashes under strength 0.2 are not drawn, and the splashes share the floating bodies' budget, `fnb.Effects.SplashBudgetPerFrame`.
- **Foam**: only the Body Part's entry, **Entry Foam Rate** fresh foam per second over 1.5 × its radius for 0.25 s (aged foam at a
  fifth of it). The default, 0.04, is a trace of white; the rings are what shows.

What the defaults give a pelican-sized bird (Splash Scale 0.6, Splash Reference Watts 70): the beak (0.08 kg, 2.5 cm) going in at
13 m/s, 3.5 kW, a splash of 2.2; the body (2.5 kg, 13 cm) at 8 m/s, 4.9 kW, 2.5. For a finer spray lower Splash Scale, or a part's
own Splash Scale.

### The plunge splash

A diving bird throws far more than droplets: sheets of water rise either side of its path and break into spray at their rims. The
parts' splashes are spawned facing the world, scaled by each part's slam, and come from every part, a foot's pat included; the
plunge splash is one effect for the whole dive:

- **When**: the Body Part's entry at **Plunge Min Speed Cm S** or faster (world speed, default 600 cm/s), once per entry, with
  **Plunge Splash System** set and loaded. Set the threshold between the bird's gentle entries (landing, settling) and its dives:
  `entry_speed_cm_s` in the diagnostics lists each part's last entry speed.
- **Where**: on the surface at the body's (x, y).
- **Which way**: the system's Z is the water's normal and its +X the owner's forward laid on the water (the body's motion along the
  water when the owner points straight down), so a system that builds its sheets along ±Y puts them either side of the bird. The
  contact's **Splash Direction** carries the heading (Add Water Contact).
- **How big**: **Plunge Splash Scale** (default 1, the system at its own size) times the owner's scale, not scaled by the speed:
  a dive's splash is an art piece, and its size is the system's, for a bird at actor scale 1.
- **Per bird or actor**: **Plunge Splash Parameters** sets the plunge splash system's float user parameters on every plunge splash
  this component throws (by name, with or without `User.`; the parts' own splashes are unchanged), so one system serves birds and other bodies of different sizes. A crown splash
  built with, say, `CrownDiameterCm` and `CrownHeightCm` parameters gets each actor's own diameter and height. The values are in the
  system's units at actor scale 1 (Plunge Splash Scale and the owner's scale still apply on top). Names the system lacks change
  nothing; empty uses the system's own values. A pooled splash starts from the system's own values every time.
- **The parts' own splashes** are held back around a dive, while a Plunge Splash System is set: the plunge splash is the dive's
  spray, and the parts' droplet bursts scattered over it read as clutter.
  - **Ahead of the body:** a part going in while the body, still above the water, comes down at Plunge Min Speed Cm S or faster
    (the beak and head of a diving bird).
  - **After the plunge:** every part, entries and exits, for **Plunge Quiet Seconds** (default 0.6 s): the feet going in behind the
    body, the surfacing.

  Landings, paddling, dips and take-offs splash as before. Speed alone cannot tell these apart: a pelican's feet go in at
  570-590 cm/s behind a diving body and at up to 575 cm/s running on the water to take off.
- It is sent before the body's own contact, so a busy frame's splash budget (`fnb.Effects.SplashBudgetPerFrame`) spends itself on
  it first. It carries no impulse or foam: the body's own contact still makes the rings.

---

## The ring layer

FluidNinja Waves runs on a grid (for example 11.7 cm cells over a 120 m simulation), which carries no wave shorter than about four
cells, half a metre. The rings that make a bird look right are far finer: 2-30 cm ripples close to it, spreading out for metres. They
are drawn by the surface material, per pixel, from a small table of each simulation's recent contacts and a precomputed exact linear
(Cauchy-Poisson) response of the water to a surface impulse, with gravity, surface tension and viscosity. The grid takes the same
impulse's long part, so the grid and the rings together are the whole impulse, counted once.

Each contact's footprint picks its ring band: source width σ = r / 2, the band (2.5, 6 or 14 cm) nearest in log. A ring lives 8 s;
each simulation holds at most `fnb.Waves.RingsMax` (64 by default, up to 128) and drops the one that matters least when full.
Settings: **Wave Rings**, **Wave Ring Strength** (the rings' slopes; 1 is the linear response), **Wave Ring Max Slope** (0.25: the
steepest a ring draws; a plunge's linear response would break, so the strong contacts are held to it while gentle ones keep their
size), **Wave Ring Viscosity Cm2 S** (how soon short ripples die: 0.01 clean water, 0.03-0.1 natural water with surface films);
`fnb.Waves.Rings 0` turns the layer off for an A/B. How it works, the response table and the material:
[FluidNinja Waves § Rings](FluidNinjaWaves.md#rings).

**The ring budget.** Every contact with an impulse takes a slot for 8 s. A paddling bird sends about 7 wake contacts a second for
each part with Wake on (Wake Interval Seconds 0.15) and its bob a few more; when the table is full the smallest, oldest rings go
first, so a dive's rings live their full time. For several birds on one simulation raise **Wake Interval Seconds** or
`fnb.Waves.RingsMax`.

---

## Floating the bird

The component reports what the movement needs to float the bird; it moves nothing itself.

- **Get Float Body Above Water Cm**: where the body part's centre floats above the still water line, world cm at the owner's current
  scale: the body sphere's radius R less its draft h, the depth at which the sphere's cap displaces the bird's mass
  (π h² (3R − h) / 3 = Mass Kg / 0.001 kg/cm³). It grows linearly with the scale (divide by the scale for the value at scale 1).
  Negative: the centre floats under the water line; −R: the sphere cannot hold the mass up. 0 without a body part. A 13 cm body
  holding 3.5 kg: 2.1 cm.
- **Get Heave Frequency Hz**: the natural frequency of the bob, √(ρ g A / (m + m_a)) / 2π, A = π h (2R − h) the waterplane at the
  floating draft, m the bird's mass and m_a the body part's added mass, both scaled by the scale's cube. 0 without a body part or
  when it sinks. The 13 cm body with 3.5 kg and 2.5 kg added: 1.5 Hz.
- **Heave Damping Ratio** (default 0.35) is held next to it for the movement's spring.

A floating movement then holds the body part's height on the waterline with a damped spring, per bird:

```
e   = (Get Part Location (Body).Z − surface Z) − Get Float Body Above Water Cm
ω   = 2π · Get Heave Frequency Hz
a   = −ω² e − 2 ζ ω v_z            (ζ = Heave Damping Ratio)
v_z = v_z + a · dt                 (clamped to a sensible speed, then commanded)
```

Starting v_z from the bird's own vertical speed on landing or surfacing makes it bob. If the float animation puts the body part some
height above the root, the root sits that height less Get Float Body Above Water Cm below the water line.

---

## Tuning

| You see | Change |
|---|---|
| Splashes too big or too many | **Splash Scale**, a part's **Splash Scale**, **Min Splash Speed Cm S** |
| A dive throws no plunge splash, or a landing throws one | **Plunge Min Speed Cm S** against `entry_speed_cm_s` |
| The plunge splash too big or small | **Plunge Splash Parameters** (the system's size parameters, per actor), **Plunge Splash Scale** |
| Droplet bursts on top of a dive's plunge splash | **Plunge Quiet Seconds** up; `splashes_held_back` counts what was held back |
| Rings too weak or too strong overall | the parts' **Added Mass Kg**; the project's **Wave Ring Strength** (rings only) or **Wave Impulse Scale** (grid and rings) |
| The ring table full, rings cut short | **Wake Interval Seconds** or **Ring Interval Seconds** up, or `fnb.Waves.RingsMax` |
| A dive's rings too steep or too faint | the project's **Wave Ring Max Slope** |
| Rings from under the water too strong or weak | **Depth Fade Radii** |
| Rings coarse, in steps | **Ring Interval Seconds** down |
| Wing beats over the water leave no ripples, or too many | the wing parts' **Air Area Cm2** (strength) and **Air Reach Cm** (how high a beat still reaches the water); `air_pushes` counts them |
| The swimming wake (V) too strong or weak | **Wake Drag Coefficient**; **Wake** off on a part |
| A swimming bird leaves no visible wake | the body's **Wake Edge Radius Cm** (about 5 cm for a pelican): a wide body sphere's wake is flat and the first a full ring table drops; feet deep under the body fade by depth, as they should |
| Short ripples die too soon or linger | the project's **Wave Ring Viscosity Cm2 S** |
| White patches | **Entry Foam Rate** down (0: none) |
| The bird bobs too long or too stiffly | **Heave Damping Ratio** (the movement's spring) |
| The bird floats too high or too low | **Mass Kg**, the body part's **Radius Cm**, its Reference Point |

---

## Diagnostics

**Get Diagnostics** returns a JSON object for QA:

| Key | Meaning |
|---|---|
| `water_contacts` | The contacts run. |
| `water_entries`, `water_exits` | Entries and exits of moving parts since Begin Play (also Get Entries / Get Exits). |
| `contacts` | Contacts sent to the subsystem (Get Contacts). |
| `splashes` | Contacts sent with a splash of strength 0.2 or more (Get Splashes); the budget may leave some undrawn. |
| `plunges` | Plunge splashes sent (Get Plunges). |
| `splashes_held_back` | Parts' splashes held back around a dive (ahead of the diving body, or within Plunge Quiet Seconds after a plunge). |
| `water_readback_fallbacks` | Samples where the waves' readback was over 0.5 s old and the still water stood in. |
| `impulse_abs_sum_kg_cm_s` | The sum of every sent impulse's size. |
| `part_names` | The parts, in order; the arrays below follow it. |
| `depth_cm`, `peak_depth_cm` | Each part's depth now and its deepest since Reset Peak Depths (−10000: not measured). |
| `wet_fraction` | Each part's wet fraction now. |
| `entry_speed_cm_s` | Each part's speed at its last entry while moving (0: none yet): what Plunge Min Speed Cm S is set against. |
| `float_body_above_water_cm`, `heave_frequency_hz` | The two floating functions' values. |

In C++, **GetDepths** and **GetPeakDepths** return the arrays; call **Reset Peak Depths** before a move to read how deep it went.

**When nothing happens:**
- `water_contacts` false: nobody switched the contacts on.
- A part's depth stays −10000: no water is known under it (no patch, no water volume), or its bone is missing (a warning at Begin
  Play names it).
- Entries counted but no rings: Wave Rings or `fnb.Waves.Rings` off, no active wave simulation at the bird, or a surface material
  without the ring step. No grid waves either: Waves, Wave Impulse or the modified core missing.
- Splashes counted but none seen: the splash budget spent that frame, or a splash system too small to read at that strength.
- No splashes counted: the parts cross slower than Min Splash Speed Cm S or throw under strength 0.2, or the component's Splash System
  failed to load (a warning names it).
- No plunges counted on a dive: the body's `entry_speed_cm_s` is under Plunge Min Speed Cm S, Plunge Splash Scale is 0, or the
  Plunge Splash System is empty or failed to load (a warning names it).
- `water_readback_fallbacks` climbing every frame: the waves' readback is stale (a hitch, a simulation switched off); the contacts
  run on the still water.
- Nothing sent at a very low frame rate: a frame longer than 0.2 s gives no speed (it is taken for a hitch), so under about 5 fps
  no part hands anything over. An unfocused editor throttles Play to a few frames a second: turn background throttling off for
  checks in Play.
- Parts that stop following the animation off screen: a mesh whose Visibility Based Anim Tick Option ticks the pose only when
  rendered freezes its bones while unseen (the parts then move with the actor alone). Use Always Tick Pose for a bird whose water
  contacts matter off screen.

---

## Known limitations

- Only vertical motion hands momentum over, plus the drag of parts with Wake; the sideways sweep of a part without Wake (a wing
  skimming the water) makes nothing.
- Parts are spheres; crossings are judged once a frame (a beak at 13 m/s moves 22 cm a frame at 60 fps; the wet fraction follows
  the depth, so no crossing is missed, but its timing is the frame's).
- No standing hollow: a resting bird presses nothing; only its motion rings.
- The wake is pressed into the waves as an impulse; it does not stir FluidNinja's dye or current (no point-input wake).
- The component moves nothing: buoyancy, the bob and the drift with the current are the movement's.
- Pending impulses wait for Ring Interval Seconds, so a ring starts up to that long after its push.
- One scale: the actor's X scale stands for a uniformly scaled bird.
