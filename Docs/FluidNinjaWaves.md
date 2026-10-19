# FluidNinja Waves

Floating bodies and swimmers make real water waves in FluidNinja's own simulation:
- A body's weight presses a hollow into the water. When it moves or is dropped, that hollow spreads out as rings, sized by how much water the body pushes aside.
- A body that sinks pushes its whole volume aside as it plunges, felt at the surface less the deeper it is, instead of a hollow capped at the surface.
- Slams, strokes and impacts push their impulse into the water.
- The rings spread with the right speed for their length and the water's depth: long waves outrun short ones, and waves slow down and steepen in shallow water.
- Rings reflect off walls, or are absorbed by an open edge or a pool's gutter, are carried along by FluidNinja's current, and break when they get too steep.
- FluidNinja's surface material draws the waves, and every other floating body rides them.

This page covers:
1. [What you need](#what-you-need)
2. [How it works](#how-it-works)
3. [Setting it up](#setting-it-up)
4. [Settings](#settings)
5. [Foam](#foam)
6. [Splashes](#splashes)
7. [Swimmers](#swimmers)
8. [Blueprint and C++ functions](#blueprint-and-c-functions)
9. [The interface with the core](#the-interface-with-the-core)
10. [Re-applying after a FluidNinja update](#re-applying-after-a-fluidninja-update)
11. [Testing](#testing)
12. [Troubleshooting](#troubleshooting)
13. [Known limitations](#known-limitations)

---

## What you need

| | |
|---|---|
| The modified FluidNinja core | `FluidNinjaLive_2.0.0.56_Waves_RelativeTracking_UE5.8_core-9adbcbf6.zip` (core `9adbcbf6`, interface version 5): NinjaLiveCore with the wave stages, five wave modules, the 1 × 1 target `RT_WaveAux_Unbound`, and the SURFACE material function that draws the waves. It also has the relative-tracking change, so it replaces the relative-tracking zip. Its `README.txt` has the install steps and each file's SHA-256. These files are not in this repository; the toolchain that makes and checks them is ([Tools/CorePatch](../Tools/CorePatch/README.md)). A core is named by its sha8 and the plugin build that requires it is tagged `core-<sha8>`: [VERSIONS.md](../Tools/CorePatch/VERSIONS.md). This plugin build needs a core with `User.Wave.InterfaceVersion` 2 or more, 3 for the [wave grid scale-down](#the-wave-grid), 4 for the [foam](#foam) and 5 for the [closed-window ⟨v⟩ pin](#the-mean-level) (see [Interface version](#interface-version)); `apply_fluidninja_waves.py verify` says whether the installed one qualifies. |
| This plugin | Waves are on by default: **Project Settings → Game → FluidNinja Buoyancy → Waves → Waves**. |

Without the plugin driving it, the modified core behaves exactly as stock FluidNinja:
- its wave stages are off;
- its surface function adds 0;
- it writes nothing to the log.

With a stock core, the plugin logs one error per simulation ("stock NinjaLiveCore: no User.WaveSimEnabled") and that simulation makes no waves. Floating still works.

---

## How it works

The work is split between the core and the plugin.

| Piece | Where | What it does |
|---|---|---|
| **Wave grid** | Core and plugin | The cells the waves run on: FluidNinja's pressure buffer's cells over the simulation's **scale-down** S (1, 2 or 4; `User.Wave.ScaleDown`, interface version 3), cells S pressure cells wide. At 1 they are the pressure buffer's own cells. At 2 or 4 a tick's Wave Prepare, Wave Step and Wave Aux cost about 1 / S² of what they do at 1, and each body's own field spans S times the water for the same cost, at the price of the waves shorter than a few wave cells. S comes from the simulation's own override (its Wave Sim component or Wave Sims entry), else `fnb.Waves.ScaleDown`, which the quality tiers set. See [The wave grid](#the-wave-grid). |
| **Wave source** | Plugin, every frame | For each simulation, one RGBA32F texture the size of the wave grid plus a header row (`User.WaveSourceTexture`). For every floating body and swimmer it holds:<br>• the **static head** P: how deep the hull sits below the wave-free surface, in cm of water. A column whose top is under the water lets its head go — the surface over a sunken part is flat, so a sinking ball or a diver presses no pit — and P is capped at half the hull's width at the water line (never below 1.2 × its resting draft while it floats): a pit with walls past 45° is more than the linear waves can carry, and sprang back as a glassy spike. A hull whose top goes under lets its head go over the hollow's own collapse time, not in one tick (see [A hull going under](#a-hull-going-under));<br>• for a body that **sinks** (its resting draft below 0), a **deep volume head** instead: its whole submerged volume carried up to the surface by the finite-depth kernel, added to P, with no ripple impulses and no own field (see [A body that sinks](#a-body-that-sinks));<br>• the frame's **impulses** y: the slam and drag that stop the hull, pushed back into the water equal and opposite (the Cauchy–Poisson impulse problem). Each fades with its depth as e^(−depth ÷ the body's mean half-extent) — a pressure that deep barely moves the surface — so a body sinking away stops pumping the water over it;<br>• **erase** flags for a body that was teleported or reset;<br>• the pressing hull's **speed through the water** (its horizontal velocity less the current under it; interface version 4), for the [foam](#foam)'s waterline shear. |
| **Wave Prepare, Wave Step** | Core, every FluidNinja tick | A linear wave field (η, v) on the wave grid's cells. It runs at FluidNinja's fixed 60 ticks a second.<br>• Its gravity–capillary dispersion follows the depth: ω² = (g·k + σ/ρ·k³)·tanh(k·d).<br>• It uses FluidNinja's own obstacle masks (the SDF collision and the height field): a wave cell is wet when more than half of the S × S pressure cells it covers are, and its depth is their mean (interface version 3; at S = 1 the cell itself).<br>• Its state is advected by FluidNinja's current, averaged over a few cells (a 3 × 3 binomial at two cells' spacing, `_drift.hlsl`; dev-host core 97b976f6 on). A wave feels the current over its own size, not a grid-scale eddy, and advecting by a drift that changes sign from one cell to the next made small spots grow inside eddies.<br>• Waves steeper than Wave Breaking Slope break. Breaking is judged on the wave height η alone, never on the drawn surface η + P (interface version 2 on): a hull's static head and the impulses are sources, not waves. Judged on η + P, a moving hull's static-head edge broke on itself (the Kelvin wake came out at 0.56 of theory) and every impulse was damped in its own tick.<br>• Nothing in a linear wave field damps a uniform level, and under a current it drifted. The open-edge sponge relaxes η as well as v, and the window's mean of s = η + P over its wet cells (the core's own mip chain: WaveOp's top mip, `.x / .y`) is relaxed at **Wave Mean Relax** (interface version 2 on). That bounds the level at ⟨v⟩ / r, ⟨v⟩ being the uniform vertical velocity, which is not relaxed before interface version 5; from 5, Wave Step also pulls a walled window's ⟨v⟩ to 0 (`User.Wave.VMeanRelax`). See [The mean level](#the-mean-level). |
| **Wave Aux** | Core, every tick | For each wave cell, the obstacle mask, the depth and the current's drift, written into a target the plugin owns (`User.WaveAuxTarget`, at the wave grid's size). It also exports the foam F into the plugin's foam target (`User.WaveFoamTarget`, interface version 4). |
| **Foam** | Core, every tick (interface version 4) | A foam field F (0..1) per wave cell, a separate layer nothing feeds back into (η, v): accumulated from what breaks (the breaking decay × the water's speed), what a moving hull's waterline sheds (the static head's slope × the hull's speed through the water) and what hits (the impulses, pokes and swimmer parts), carried by the current as η is, fading at **Foam Decay**. See [Foam](#foam). |
| **Wave Export** | Core, every tick | FluidNinja's pressure buffer `RT_PressureDivergence` becomes RGBA16F. Its B is the wave height η + P that the surface draws; its A is the static head P. It keeps the pressure buffer's resolution whatever the wave grid's: at a scale-down above 1, η is upsampled bilinearly from the four wave cells around each texel, and P from the source texture's cells. |
| **Surface** | Core material function | `MF_NinjaOutput_BaseFunction_SURFACE` lifts the surface by B × `WaveHeightScale` and bends its normal by B's slope × `WaveNormalScale`. Both are exactly 0 while the waves are off. It also draws the foam (the water's colour lerped toward `WaveFoamColor`, the roughness raised) and, behind the off-by-default `WaveChoppy` switch, the Tessendorf choppiness. See [Foam](#foam). |
| **Own fields** | Plugin, GPU | A body must not feel its own waves through the few frames the readback lags: that acts as negative damping, and the body rocks itself harder and harder.<br>• Each body therefore has its own small field, a port of the core's step, fed only by that body's sources. It uses the core's mask, depth and drift from Wave Aux.<br>• A body feels B − B_self, which is everyone's waves but its own, scaled by κ (**Wave Feel Scale**).<br>• The drag of its own waves is put back synchronously as **radiation damping**: heave, roll and pitch, each the physical Haskind value at its natural frequency, tripled only near rest.<br>• Why the tripling: the core radiates a body's waves at every frequency the body moves at, while this damping is one constant. Bodies that make and feel waves share a collective mode near 1 Hz, where the core radiates 2–8 times the Haskind value at the bodies' own (higher) natural frequencies; at 1× everywhere eight resting floaters rocked each other up to a few cm, and at 3× everywhere a crate's bob stopped dead. So the physics step adds the other 2× on slow motion only: the mode starts from rest, while a drop or a jump moves far faster and keeps the physical value. `fnb.Waves.RadiationScale` scales the Haskind value for A/B (1: physical). |
| **Wall absorption** | Core | A closed, nearly lossless pool would keep every echo of a body's own waves. Within **Wave Wall Band** of a walled border, **Wave Wall Absorption** (0.4/s) takes energy out of the waves reaching it, as a pool's gutter does. A wall still returns about 80% of a 2 m wave's height. |

Everything runs on the GPU, apart from the bodies' and swimmers' sources, which are built on the game thread. The waves cost one wave step per tick per simulation, plus one small own field per wave body; bodies of the same size are batched in one dispatch. The foam adds one float per wave cell on the grid, one R16F target per simulation and a few lines to Wave Prepare, Wave Step and Wave Aux — well under a tenth of Wave Step's work.

### A hull going under

A rigid hull whose top goes under the water no longer drops its static head at once. Dropped in the one tick the top went under, a sinking 15 t block's 87 cm cap (40 % of its volume) vanished in 16.7 ms, and the hollow it had pressed sprang back as a spike. Instead the hull keeps pressing the head it went under with, and that head fades as e^(−t/τ). τ = √(w / g) is the time a hollow w wide takes to fill, where w is the waterline width the hull went under with, at least twice its cap and two wave cells: 174 cm gives 0.42 s. Below 1 % the head is dropped. While it fades, the hull is rastered where it is across but lifted back to the height its top had above the water when it went under, so every column keeps the head and the column fade it had then; rastered where it sinks, the two-cell column fade would take the head in a few frames. It keeps the cap it went under with and sheds no foam. A hull that comes back up through the surface takes its head back the same way, over the τ of its new waterline, so surfacing is not a step either. A reset starts a hull at its full head if it comes through the surface, and with none if it is under. Swimmers keep their own fade. A body coming down into the water needs no such ramp: its head is each column's depth below the surface, taken every frame, so it grows as the hull goes in. `fnb.Waves.SubmergeRelease 0` brings back the one-tick drop, for A/B.

With `fnb.Waves.PlungeSource` on (the default), a body that sinks takes the deep volume head instead ([A body that sinks](#a-body-that-sinks)), and the graded release is for floating hulls that go under for a while (a crate under a passing wave, a swamped boat). At `fnb.Waves.PlungeSource 0` it is for every hull, as before.

### A body that sinks

A body whose whole hull cannot carry its mass (its resting draft is below 0) never floats, and its static head used to be wrong in two ways. The head was capped at half its waterline width (87 cm for the bench's 15 t block, 40 % of its volume) and was let go once its top went under. Both put the water's response into short, steep waves around the hull, while a real plunge sends a long wave. With the head capped, the bench's block threw a 1.4 s packet first; Evers et al. (2019) give a first wave of 3.5 s.

With `fnb.Waves.PlungeSource 1` (the default), such a body presses a **deep volume head** from its first contact with the water. Floating bodies are not affected: a body whose resting draft is 0 or more keeps exactly the static head, the cap, the column fade, the graded release and the impulses, bit for bit. So does a body that floats only through its **Buoyancy Multiplier**. The resting draft is worked out at a multiplier of 1, so a body with a multiplier above 1 whose mass lies between what its whole hull's water weighs and the multiplier times that reads below 0. It floats all the same, and keeps the static head (its draft taken as 0), its impulses and its own field.

**The model.** In linear water of depth h, a volume source at depth z drives the free surface as η_tt + ω² η = ∂q/∂t · cosh(k (h − z)) / cosh(k h). The last factor is the share of the source's flux that reaches the surface over a flat bed. The core solves B_tt + Γ B_t + ω² B = P_tt, with B = η + P the drawn surface. So if P is the body's submerged volume filtered by that factor, B is exactly the surface the plunging body raises, in linear theory. The deep head is that P:
- the submerged volume taken as a uniform column over an equivalent rectangle. The rectangle and the column have the same plan second moments and the same vertical spread as the hull's part under the wave-free surface. They are worked out every frame from the hull's triangles clipped at the surface, and for an upright box they are the box itself;
- carried up to the surface on the GPU (`DeepHeadCS`): the bed's images, the first two exact for the rectangle and the rest as points, averaged over the last three terms;
- tapered as cos² over the outer fifth of 4 h (`fnb.Waves.PlungeTaperDepths`) and renormalised for what the taper takes off, so its volume is the submerged volume.

As the hull just comes in, this is the static head without the cap (each column's depth), so nothing steps as the deep head takes over. A body at rest presses a static P and radiates nothing: over a sunk block B goes to 0, as over a real one. Volume at depth z is filtered at wavelengths shorter than about 2π z, which the static head could not do.

**What else changes for a sinker:**
- **No ripple impulses.** The slam, drag and suction impulses stood in for the displacement that a lagging surface head missed in a fast entry. The deep head carries that displacement itself, so sending them too would count the entry twice. Its physics still feels every one of them. Both of the bench's drops, displacement only and displacement with impulse, are therefore the same for a sinker.
- **No own field** (Feel 0). It feels FluidNinja's own surface, the slam and the drag, but not the waves.
- **No graded release.** The head is held while the body moves and through its whole wave event.
- **It fades slowly.** After 15 s at rest (a block on the bed), and when the body goes away, its head fades with a 10 s time constant until it presses under 0.05 cm. Released faster, it would send waves of its own: over 0.42 s it fell into the long-wave band, and over 4 s the bench's troughs still came out 4–8 % high. A body that goes to sleep or is held keeps its head as it was. A removed body's water is not erased: the head fades where it was. Erasing all the water (the bench's fresh ripples, a restart) drops the fading heads in the same frame.
- **One decision per plunge.** The body takes the deep head or the static head on the first frame any of its hull is in its water, from whether it sinks then (its resting draft and its Buoyancy Multiplier). The decision is kept until it is teleported or removed, so a change of mass mid-plunge cannot flip it. Its head is not started over when the water it is measured against changes (the Surface Mirror finding or losing it), only when it is put somewhere.

**Cost.** One dispatch per simulation with a sinker in it: a thread per wave cell within 4 h of each sinker (410² cells for h = 300 cm on 5.86 cm cells), about 170 transcendental operations each. The per-cell sum is kept on the GPU and reused while every record stays the same, so a sunk body lying still costs no dispatch. A simulation with no sinker does no extra work, and its P is bit for bit what it was. Memory: one 32-bit value per wave cell, for each simulation while it has a deep head.

**Measured** on the numpy port of Wave Prepare and Wave Step, with the bench's `ripple-evers` drop and the deep head computed exactly as the shader computes it. All 54 measures (first crest, first trough and H1 at 18 probes) fell inside Evers' scatter, against 28 of 54 with the impulses. The tightest margin is the 4.5 m γ 45° trough: 15.45 cm against a ceiling of 16.13 cm. The first waves are 2.1–2.8 s long, against today's 1.4 s packet, but still shorter than Evers' 3.3–3.5 s. In this linear core at r / h = 1.5–3, the leading period is set by the dispersion and the source's width filtered by depth, not by how long the entry takes. It has not yet been measured on the GPU: see [Testing](#testing).

### The wave grid

The wave stages used to run on exactly the pressure buffer's cells (2048² on a large river), so a tick's wave step cost about what the fluid did. From interface version 3 the grid is the pressure buffer's cells over a per-simulation integer **scale-down** S = 1, 2 or 4 (`User.Wave.ScaleDown`): a 2048² buffer runs its waves on 1024² cells at 2 and 512² at 4, each wave cell S pressure cells wide.

| | At S |
|---|---|
| Wave Prepare, Wave Step, Wave Aux | About 1 / S² of their cost at 1: 1 / S² of the cells, each cell's work about the same (the mask and the depth read S × S pressure cells, which is S² small samples on 1 / S² of the cells). |
| Wave Export | Unchanged: it still writes every pressure texel, η upsampled bilinearly from the wave grid, P from the source texture. The surface material and the gather read `RT_PressureDivergence` as before. |
| The source texture, Wave Aux, the bodies' own fields, the impulse splats, the probes' theory | At the wave grid's cells. A 512-cell own field spans S times the water at the same cost, so a big hull that was too big for its field on fine cells gets one. |
| What is lost | Waves shorter than about four wave cells: the hyperviscosity damps the grid's finest wavelength at the same rate whatever the cell, so at S = 4 on 2.5 cm pressure cells the cell-sized ripples of 10 cm go. Crest arrivals of the waves the grid carries (a metre and up) stay within a few percent; the drawn surface is piecewise bilinear between wave cells, which shows as a faint grid in the normal at S = 4 up close. |

Where S comes from, in this order: the simulation's own override (its [Wave Sim component](#wave-sim-component)'s **Scale Down Override** or its Wave Sims entry's, above 0), else the console variable `fnb.Waves.ScaleDown` (a scalability variable: the quality tiers set it). A core before interface version 3 has no `User.Wave.ScaleDown` and runs at 1; the plugin says so once per simulation when more was asked, and the status line shows `scale-down 1, 4 asked: interface 2 core`. At S = 1 a version 3 core does exactly what a version 2 core did.

### The mean level

A linear wave field has two modes that nothing in it damps: a uniform level, and a uniform vertical velocity ⟨v⟩. The operator L does not see either. A uniform level does not decay at all, and ⟨v⟩ decays only at Wave Damping plus the wall band, about 0.06/s on an 800² pool. Three rates hold them:

| Rate | Console variable | What it does |
|---|---|---|
| The core's mean level | `fnb.Waves.MeanRelax`, sent as `User.Wave.MeanRelax` | Each tick Wave Step takes Dt · r · ⟨s⟩ off η in every wet cell, where ⟨s⟩ is the window's mean of s = η + P over its wet cells. Under a hull s is about 0, so a body's hollow adds no bias, and Archimedes' rise of the level is relaxed too. This bounds the level at ⟨v⟩ / r.<br>• Automatic by default: 30 /s, or 5 /s where the core pins ⟨v⟩ (below).<br>• The plugin clamps it to 0.5 ÷ the longest step the core integrates: 30 /s at a fixed 60 ticks a second, 4 /s on a core without a fixed tick, which can step Niagara's 0.125 s. As an explicit term it stays monotone there. |
| The own fields' mean level | `fnb.Waves.SelfMeanRelax` | The same term in each body's own field: 0.5 /s, at most 5. Their absorbing band dominates their uniform mode anyway (in the port, B − B_self stayed within ±0.3 cm at 0, 0.5 and 5 /s). A slam's P_dyn is inside the pinned mean, and on a small tile a strong pin would turn each impulse into a uniform kick. |
| The core's ⟨v⟩ (interface version 5) | `fnb.Waves.VMeanRelax`, sent as `User.Wave.VMeanRelax` | Pulls a walled window's ⟨v⟩ to 0 at 30 /s. The core clamps the rate to 1 ÷ its step. In a walled window, the sum of v over the wet cells is the rate of change of the sum of η, and volume conservation holds that at 0, so the pin restores a true constraint. Open windows (a river with its sponge) are left alone, because their sum of v has real fluxes through the edge, and so are the own fields. |

**Why the core's rate is 30 /s, not the 0.5 /s it was.**
- In a walled window under a current, breaking at the floating hulls' hollow edges damps the refilling v on one side only. That pumps ⟨v⟩, and nothing restored it.
- The pin on the mean of s then parked the level at ⟨v⟩ / r, a level that kept sinking (see [Known limitations](#known-limitations)).
- At 30 /s the level holds.
- This is a mitigation: ⟨v⟩ itself still grows.
- An interface-5 core's ⟨v⟩ pin removes that integrator, so the automatic s rate drops to 5 /s. That is gentle enough that a slam's P_dyn kicks the whole window by under about 0.1 cm for one tick (port).
- An open window (a river with a sponge edge) never sank: its sponge band relaxes η toward 0 at the window's edge, and the bench's river floaters held to a millimetre. There the automatic rate stays at 0.5 /s, because a strong pin only takes the level a sinking body raises out from under its own waves: the ripple-evers block, dropped in the demo river, read its troughs 0.25-0.3 cm deeper at 30 /s (its 11 m³ over the 60 m window is a 0.3 cm rise).

The status line gives the rates each running simulation uses, for example `mean relax core 30.00 /s, self 0.50 /s, no v pin (pre-5 core)`. On an interface-5 core it shows `v pin 30.0 /s`, `v pin off` (`fnb.Waves.VMeanRelax 0`) or `no v pin (open edge)`.

---

## Setting it up

1. Install the modified core (the zip's `README.txt`) and this plugin.
2. In **Project Settings → Game → FluidNinja Buoyancy → Waves**, leave **Waves** on.
3. Tell the plugin about each simulation whose water differs from the defaults: a **FluidNinja Wave Sim** component on its NinjaLive actor (below), or a **Wave Sims** entry in the project settings. By default a simulation is open water (**Wave Edge Open**), **Wave Depth** deep wherever FluidNinja's height field is off, and as deep as its height field says where that is on.
4. Floating bodies take part on their own: the component's **Waves** is on by default.
5. Press Play. A body you drop (test bench: `fnb.TestHUD 1`, select it, **Ctrl+Shift+J**) throws rings across the water, and the other floaters rock as the rings pass.

### Wave Sim component

A NinjaLive actor that carries a **FluidNinja Wave Sim** component (`UFluidNinjaWaveSimComponent`) is set up by it: the level carries the water's facts with the actor, and the settings travel with it when it is copied to another level. A Wave Sims entry naming the same actor is ignored (logged once).

| Field | Default | Meaning |
|---|---|---|
| Waves | on | This simulation makes waves. Off: it is no wave simulation at all (its floaters feel FluidNinja's own surface only). |
| Depth Cm | 200 | Its depth, cm. |
| Depth From Bed | off | On: the depth comes from FluidNinja's height field where it has one (water Z minus the bed), Depth Cm elsewhere. Off: Depth Cm everywhere. |
| Walled | off | A closed basin: its window's border is a wall (Edge Open off) that absorbs with Wall Absorption over Wall Band Cm. Off: open water with the sponge. |
| Wall Absorption, Wall Band Cm | 0.4 /s, 50 cm | The wall's absorption and the band it acts in, when Walled. |
| Scale Down Override | 0 | The wave grid's scale-down for this simulation, 1, 2 or 4; 0 takes `fnb.Waves.ScaleDown`. See [The wave grid](#the-wave-grid). |
| Importance | 1 | How much this simulation matters when `fnb.Waves.MaxActiveSims` keeps only the nearest running: its distance from the camera is divided by it. The player's pool at 2 counts as half as far away as a backdrop at 1. |

### Wave Sims

One entry per NinjaLive actor without a Wave Sim component, by its name or its label. Each override has its own tick box:

| Field | Meaning |
|---|---|
| NinjaLive | The NinjaLive actor's name or label. |
| Enabled | Off: this simulation runs no waves, and its floaters feel FluidNinja's own surface only. |
| Depth Cm | This simulation's depth everywhere, instead of Wave Depth and its height field (`User.Wave.DepthFromBed` 0). Use it for a pool or channel whose height field reads the water surface rather than the floor. |
| Edge Open | Its window's border: open water (on, waves leave through the sponge) or a wall that reflects them (off, a walled pool). |
| Wall Absorption | How strongly a walled border takes energy out of the waves, instead of Wave Wall Absorption. |
| Wall Band Cm | The band along a walled border its absorption acts in, instead of Wave Wall Band Cm. |
| Normal Scale | `WaveNormalScale` on its surfaces, instead of Wave Normal Scale Factor ÷ (0.005 × its size in cm). |
| Scale Down Override | The wave grid's scale-down for this simulation, 1, 2 or 4; 0 takes `fnb.Waves.ScaleDown`. |
| Importance | Divides its distance from the camera in the `fnb.Waves.MaxActiveSims` ranking (1: as it is). |

The plugin ships two entries, for the demo level:
- `Pool_FluidNinja`: a closed pool, 400 cm deep, walled.
- `River_FluidNinja`: a river lane, 300 cm deep.

Your own entries are saved to your project's `DefaultGame.ini`.

---

## Settings

**Project Settings → Game → FluidNinja Buoyancy → Waves.** Each User parameter named here is the core's; the plugin sets it every frame.

| Setting | Default | Meaning |
|---|---|---|
| Waves | on | Master switch. Off: no simulation runs waves (`User.WaveSimEnabled` false) and bodies float on FluidNinja's own surface. |
| Wave Depth Cm | 200 | Depth where FluidNinja's height field is off (`User.Wave.DepthCm`). |
| Wave Damping | 0.02 /s | Damping of every wave. At 0.02, rings cross a pool. |
| Wave Sponge Cm | 300 | The absorbing band at an open window edge. |
| Wave Edge Open | on | The window's border is open water. Off: a wall. Wave Sims can override it. |
| Wave Wall Absorption | 0.4 /s | Absorption within Wave Wall Band of a walled border. 0 returns everything, and a closed basin then slowly fills with a floater's own echoes. |
| Wave Wall Band Cm | 50 | Width of that band. |
| Wave Shore Depth Cm | 10 | Water shallower than this absorbs waves, more the shallower (a beach). |
| Wave Breaking Slope | 1.0 | Waves steeper than this (cell to cell) break, and so do crests higher than 0.6 of the depth: a net for cell-sized spikes, not a Stokes limit. 0.4 damped every vertical-sided hull's near wake (the Kelvin wake came out at half its height). |
| Wave Hyperviscosity | 0.5 | Damping of the shortest waves the grid carries, falling as k⁴ for longer ones. It keeps the cell-sized ripples from a hull's sharp edge from ringing. |
| Wave Current Coupling | 1 | How much FluidNinja's current carries the waves. |
| Wave Mean Relax | not a project setting | How fast the window's mean of η + P is pulled back to 0 (`User.Wave.MeanRelax`). The plugin drives it from the console variable `fnb.Waves.MeanRelax` (below): automatically 30 /s in a walled window, or 5 /s on an interface-5 core with the ⟨v⟩ pin, and 0.5 /s in an open one, clamped to 0.5 ÷ the core's step. The core asset's own default, 0.5 /s, applies only where the plugin does not drive the core. See [The mean level](#the-mean-level). |
| Wave Feel Scale | 1 | κ: how much of the waves bodies feel. 1 is physical. 0 floats them on FluidNinja's surface, while they still make waves. |
| Radiation Damping | on | The drag of each body's own waves, put back every physics step. |
| Wave Depth From Ground | off | The own fields take the depth under each body from a trace down to the ground. Off: Wave Depth, or the simulation's Depth Cm. A trace often meets a water mesh's collision at the surface and reads 1 cm, which is why it is off by default. |
| Wave Displacement | on | Source 1: each hull's static head. Its motion makes the impact rings, the bow waves and the Kelvin wake. |
| Wave Impulse | on | Source 2: the slam and drag impulses, each faded by its depth under the surface. |
| Wave Impulse Scale | 0.35 | Scales source 2. In a real water entry, much of the impulse goes into splash and turbulence rather than waves. |
| Wave Source Smooth Cm | 10 | The impulses' Gaussian width (σ), or 3 × the σ of the piece of hull each came from if that is wider, up to 16 wave cells. The water a slam drives down is taken up around the patch, not under it alone. |
| Swimmer Waves | on | The player's pawn pushes the water aside too, as a body lying along the water: a tapered ellipsoid turned to the pawn's facing, 0.95 of its capsule's half height long and 0.65 of its radius wide each way, as thick as **Swimmer Volume Litres** (76 L) makes it (76 L: semi-axes 85 × 23 cm, 9.4 cm thick each way). It presses only its part under the surface, and lets go as it goes well under. It comes in as the water rises from the pawn's waist to its chest, full from the chest up, so wading presses little. A pawn whose capsule has no collision makes none. |
| Max Wave Impulses | 32768 | Impulse pieces sent per frame. Past this, neighbours pressing the same way are merged, keeping the total; this is logged once. |
| Foam | on | The foam channel (interface version 4 core): whitecaps, wake trails and impact rings as a white layer on the surface. Off: the core accumulates none and the surfaces stay clean; the waves are identical either way. See [Foam](#foam). |
| Foam Decay | 0.7 /s | How fast the foam fades (`User.Wave.FoamDecay`): an e-fold in about 1.4 s. |
| Foam Breaking | 1 | Scale of the foam breaking waves make (`User.Wave.FoamBreaking`): the breaking decay × the water's speed. The whitecaps. |
| Foam Wake | 1 | Scale of the foam a moving hull's waterline sheds (`User.Wave.FoamWake`): the static head's edge × the hull's speed through the water. The wake trails and bow foam. |
| Foam Impact | 1 | Scale of the foam an impulse makes (`User.Wave.FoamImpact`): slams, Add Water Impulse, pokes, swimmer strokes. The impact rings. |
| Wave Height Scale Parameter, Wave Normal Scale Parameter | `WaveHeightScale`, `WaveNormalScale` | The surface material's scalars. |
| Wave Normal Scale Factor | 1 | A surface's `WaveNormalScale` = this ÷ (0.005 × the simulation's size in cm): 0.1 on a 20 m pool, 0.025 on an 80 m river. |
| Pressure Buffer Parameter | `PressureDivergenceBuffer` | The surface material's pressure-buffer texture: its B is the drawn wave height. |
| Wave Sims | the two demo entries | See [Wave Sims](#wave-sims). |

**On the component** (category **Waves**):

| Setting | Default | Meaning |
|---|---|---|
| Waves | on | The body makes waves and feels the others' waves. Off: it floats on FluidNinja's own surface and leaves the waves alone. |
| Wave Feel Scale | 1 | Its own κ, times the project's. |

**Console variables and commands** (`fnb.Waves.*`; the scalability ones are what a quality tier sets):

| Console | Default | Meaning |
|---|---|---|
| `fnb.Waves.ScaleDown` | 1 | Scalability. The wave grid's scale-down S (1, 2 or 4) for every simulation without an override of its own: the waves run on the pressure buffer's cells over S. See [The wave grid](#the-wave-grid). |
| `fnb.Waves.MaxActiveSims` | 0; the tiers set it: Low 1, Medium 2, High 3, Epic 4, Cinematic 0 | Scalability. A desktop project runs Epic unless it asks for another tier, so it gets waves on the 4 nearest simulations. At most this many simulations run waves: those whose waves are nearest the camera, each simulation's distance to its nearest wave source (a floating body on its surface, awake and with its Waves on, or a swimmer in its square) divided by its Importance, ranked once a second. A simulation with no wave source ranks behind every one that has one (at its square's distance plus its own size): ranked by the square alone, a simulation that follows the player (a river template, whose square always holds the camera) won every time, and on the demo's Low tier (1) the pool's floating bodies 15 m away never got waves. A running simulation keeps its place against a newcomer within 10 % of its rank, and no simulation changes state twice within a second, so one at the line does not flap. A simulation kept off is off on its core (`User.WaveSimEnabled` false: its stages stop), rasters no sources, and its bodies have no own fields and feel no waves (they still float on FluidNinja's own surface); when it comes near again it starts afresh, as a simulation does at Play. The status line names the ones kept off and their rank. 0: all of them run. |
| `fnb.Waves.SelfFieldDistanceCm` | 6000 | Bodies farther than this from the camera get no own field: they make waves and feel none. 0: every body gets one. |
| `fnb.Waves.RadiationScale` | 1 | Scale on every body's radiation damping (A/B). |
| `fnb.Waves.MeanRelax` | -1 | Wave Mean Relax (1/s), the core only (`User.Wave.MeanRelax`). Below 0, automatic, by the window: walled, 5 where the core pins its ⟨v⟩ (interface version 5 and `fnb.Waves.VMeanRelax` above 0), else 30; open (a sponge edge), 0.5, as it always was. A value of 0 or more is used as given (0 off). Either way it is clamped to 0.5 ÷ the core's longest step: 30 at a fixed 60 ticks a second, 4 on a core without a fixed tick. See [The mean level](#the-mean-level). |
| `fnb.Waves.SelfMeanRelax` | 0.5 | The same rate (1/s) in the bodies' own fields, at most 5 (and 0.5 ÷ tick). 0 off. |
| `fnb.Waves.VMeanRelax` | 30 | How fast (1/s) an interface-5 core pulls a walled window's mean vertical velocity to 0 (`User.Wave.VMeanRelax`). Open windows ignore it, and the core clamps it to 1 ÷ its step. Sent only to a core at interface version 5; an older one has no such parameter. While it runs, the automatic `fnb.Waves.MeanRelax` is 5 instead of 30. 0 off. |
| `fnb.Waves.SubmergeRelease` | 1 | 1: a rigid hull whose top goes under lets its static head go as e^(−t/τ), τ = √(waterline width ÷ g), and takes it back the same way when it surfaces. See [A hull going under](#a-hull-going-under). 0: the head goes in one tick, the old behaviour, for A/B. Swimmers are not affected. With `fnb.Waves.PlungeSource` on, it applies to floating hulls only. |
| `fnb.Waves.PlungeSource` | 1 | 1: a body that sinks (its resting draft below 0, and no Buoyancy Multiplier above 1 that floats it after all) presses a deep volume head from its first contact, sends no ripple impulses and has no own field. See [A body that sinks](#a-body-that-sinks). Floating bodies are not affected. 0: every body takes the capped static head with the graded release and the impulses, the old behaviour, for A/B. Switching it (or Wave Displacement) drops the deep heads at once: a step, for A/B only. |
| `fnb.Waves.PlungeTaperDepths` | 4 | How far a deep head reaches, in water depths from its centre (the last fifth a cos² taper, the volume renormalised). It is also the disc over which the core does not judge breaking while the head is there, and the size of the GPU's work. 2 failed the bench in the port. |
| `fnb.Waves.PlungeLag` | 0 | A first-order lag on the deep column's top and bottom, τ = c √(h / g), c this value. Off at 0. It is the fallback if a fast entry ever draws glass: in the port, 1 took the bench block's peak P from 93 to 60 cm and the steepest drawn slope beside it from 1.01 to 0.75. |
| `fnb.Waves.Foam` | 1 | Scalability. The foam channel's kill switch (with the Foam setting): 0 sets `User.Wave.Foam` 0, the core accumulates nothing and its Wave Aux writes 0 into the foam target, so the surfaces go clean within a tick. The waves are identical either way. |
| `fnb.Waves.Memory` | – | A command: lists the GPU memory the waves hold, one line per target (each simulation's source texture, Wave Aux target and foam target, the self-field atlas and pools, the bench's readbacks while they run) and the total in MB. The status line carries the same total. The per-frame transients (a simulation's static head and impulse textures, a batch's WaveOp slices) live in the render graph's pool and are not listed. |

---

## Foam

From interface version 4 the core carries a **foam field F** (0..1 per wave cell) beside (η, v): the honest answer to wake trails and impact rings, which FluidNinja's point input could not give (a point under 40 cm/s does nothing, and a faster one paints dye that speeds a river up — see [Wakes and the current](#wakes-and-the-current)). Nothing of the foam feeds back into the physics: with Foam off, or on a pre-4 core, the waves are bit for bit what they were.

**What feeds it**, each core tick (the scales are the Foam settings above):

| Source | What it is | What it draws |
|---|---|---|
| Breaking | The breaking decay Wave Step already computes (the slope and crest-height tests) × the water's vertical speed: the energy breaking takes out of a wave turns white. | Whitecaps on steep crests — the breaking rate exported, as render-side nonlinearity wants it. |
| Waterline shear | The static head's edge \|∇(P + P_dyn)\| (clamped to 1) × the hull's **speed through the water** at the cell (the source texture's A, written by the plugin: the hull's horizontal velocity less the current under it; swimmers use their ground speed). A resting hull has speed ~0 and sheds nothing. | A towed boat's wake trail and bow foam; a swimmer's trail. |
| Impacts | The frame's impulse head \|y\| (slams, drag, **Add Water Impulse**, pokes, swimmer parts), spread over the frame's ticks so the total is frame-rate independent. | Impact rings where something lands or strikes. |

F is **carried by the current** exactly as η is (the same back-trace, read bilinearly), **decays** at Foam Decay (0.7/s: a boat's trail fades in roughly 1.5 s), is clamped to 1, and is cleared by the erase flag with the body's waves.

**Where it goes.** Wave Aux writes F into a one-channel render target the plugin owns and binds at `User.WaveFoamTarget` (R16F at the wave grid's size; ~1.3 MB on an 800² grid at scale-down 1, listed by `fnb.Waves.Memory`). F is **not** written into `RT_PressureDivergence`: its four channels are taken (pressure, divergence, B = η + P, A = P), and a separate target lets the foam be dropped by tier without touching the export.

**How it is drawn.** The `material` step gives `MF_NinjaOutput_BaseFunction_SURFACE` (which every FluidNinja water material samples):

| Parameter | Default | Meaning |
|---|---|---|
| `WaveFoam` (static switch) | on | The foam in the base colour and roughness. Off: the foam sample is not even compiled. |
| `WaveFoamBuffer` (texture) | black | The simulation's foam target, sampled at the simulation UV like `PressureDivergenceBuffer`. The plugin binds it on the surfaces' dynamic materials (beside `WaveNormalScale`), once a second; unbound it is black, so the look is the vendor's. |
| `WaveFoamStrength` | 1 | Scale on F before it is clamped: FoamMask = saturate(F × WaveFoamStrength). |
| `WaveFoamColor` | white | The colour the water goes toward by FoamMask; the roughness has FoamMask added (saturated). At F = 0 both are exactly the vendor look. |
| `WaveChoppiness`, `WaveChoppy` (static switch) | 0, off | Render-side nonlinearity: WPO.xy += WaveChoppiness × ½(B two texels ahead − B two texels behind) × WaveHeightScale per sim axis (tangent → world) — the Tessendorf/Gerstner horizontal displacement toward the crests, from the ±2-texel low-passed slope (`RT_PressureDivergence` has no mips to sample instead). Off, the vertex shader does none of it; the four extra B reads exist only with the switch on. |

A project's own water material gets the same foam from the function's **FoamMask output** (0..1), to composite as it likes; `WaveFoamColor`/`WaveFoamStrength`/`WaveChoppiness` are ordinary material-instance parameters.

**FluidNinja's whitewater particles** (the surface-aligned particles, e.g. on a masked material instance) are a documented follow-up, not driven automatically: do **not** paint F into FluidNinja's density (painting density was the wake-law failure); instead, sample `WaveFoamBuffer` in the particle material's opacity at the particle's sim UV, exactly as the surface function samples it. The target is bound on the water surfaces' dynamic materials; a particle material needs the plugin to bind it there too, which is a one-line addition once a project asks for it.

---

## Splashes

Every splash in the project is on one scale, so splash sizes compare across bodies and heights. This covers the hull slams, the up-to-three plunge splashes beside them, the big slams' crowns, and the swimmers' strokes and dives.

- **Splash Reference Watts** (Effects, 70 W): a slam of this power throws a splash of strength 1, the splash system at its own size.
- Strength grows with the cube root of the power, so eight times the power makes twice the splash.
  - At 70 W, a 238 kg boat dropped 3 m (5.8 kW) throws a splash of 4.4.
  - A 175 kg crate dropped 3 m (35 kW) throws 8.
- A slam of strength 4 or more also throws a **crown**: four to eight splashes facing outward around where it went in, each 0.6 of the slam's strength — the water a big slam drives down leaves sideways.
- **Splash Strength Ceiling** (Effects, 12) is the largest any splash gets.

Per body (component, **Spray**):

| Setting | Default | Meaning |
|---|---|---|
| Splash Min Watts | 300 | The body's noise floor: weaker slams throw no splash, so a bobbing body stays quiet. 0: no slam splashes (bow spray still runs). |
| Max Splash Strength | 12 | The body's own cap, never past the project's ceiling. |
| Splash Scale | 1 | An art multiplier on every splash and spray from this body. |

---

## Swimmers

Swimmers make waves in two ways.

| Way | What it does | Settings |
|---|---|---|
| **Displacement** | The swimmer's body pushes the water aside: its bob, its strokes and jumping in make rings. | Waves → **Swimmer Waves**, **Swimmer Volume Litres** |
| **Body-part contacts** | Hands, forearms, feet and head splash where they go into the water and press a ring into it. Each is sized by the part's added mass times the speed it goes in at. Feet kicking just under the surface churn it. A diver's chest, hips and thighs take part too. | **Swimmers** category, below |

Who takes part:
- **The player's pawn**, while its Character Movement is swimming, or falling (jumping in), over a water volume: a Physics Volume with *Water Volume* ticked, whose top is the still water's height. Its skeletal mesh is the first with a `hand_r` bone. **Player Contacts** off leaves it out, for example when its own swim animation already spawns stroke splashes.
- **Any skeletal mesh registered with `RegisterSwimmer`**: an AI swimmer, a diver, or a pawn on another movement system (such as Mover). See below.

**Project Settings → Swimmers:**

| Setting | Default | Meaning |
|---|---|---|
| Swimmer Contacts | on | Body-part splashes and rings, for every swimmer. |
| Player Contacts | on | The player's pawn too. |
| Swimmer Entry Min Cm S | 60 | A part going in slower than this (straight down) makes nothing. |
| Swimmer Splash Scale | 1 | 1: the splash a slam of the same power would make. |
| Swimmer Impulse Scale | 1 | 1: the part's added mass × its entry speed. |
| Swimmer Cavity Seconds / Cm | 0.35 s / 30 cm | A part going in this soon after, and this near to, another part's entry follows it into the air pocket that part opened, at a third of the splash and half the impulse. So a pencil or head-first entry stays small, and a tuck does not. |
| Swimmer Kick Depth Cm | 25 | Feet this deep or shallower churn the surface. |
| Swimmer Wakes | on | Swimmers write their push into FluidNinja's point input, and a ring where they plunge in. See [Wakes and the current](#wakes-and-the-current) for what FluidNinja makes of it. |
| Swimmer Wake Strength | 0.35 | The wake's target: the current plus this share of the swimmer's speed through the water. |
| Swimmer Wake Gain | 0.2 | The share of what the water at the swimmer still lacks of the target that the wake writes each frame. At 0.2 the side wake stays under FluidNinja's 40 cm/s cut-off in still water. |
| Swimmer Wake Size Cm | 70 | The wake's brush size. It grows with speed, from × 0.6 to × 1.5. Wading uses two thirds of it at each foot, and a plunge 1.2 times it. |
| Swimmer Wake Min Speed Cm S | 15 | A swimmer slower than this through the water leaves no wake. |
| Swimmer Current Ring Inner / Outer Cm | 70 / 160 | The ring around a swimmer that its current is read on: by the wakes, and by **Get Current At Location** unless it is given its own radii. |

A registered swimmer whose provider returns a Wake Tuning with Override on (the FluidNinja Swimmer component's **Wake Tuning**)
uses its own Strength, Gain, Size, Min Speed and current ring in place of the five Swimmer Wake rows above; see
[Swimmers.md](Swimmers.md).

The splash system is the project default (Effects → **Splash System**).

### Wakes and the current

These two don't use the wave stages. They use only FluidNinja's point input (the Wake Data Channel) and its velocity buffer, so they work with Waves on or off.

**Swimmer wakes.** A swimmer's push through the water is written into FluidNinja's point input as moving brush points, as the floating bodies' wakes are.
- The target is the current plus **Swimmer Wake Strength** of the swimmer's speed through the water. Each frame the wake writes **Swimmer Wake Gain** of what the water there still lacks of that target.
- Wading, the push goes in at each foot.
- A swimmer going in faster than 2.5 m/s (straight down) pushes a ring of water outward, once (80 to 300 cm/s).

How FluidNinja takes the points (measured in the demo level with its NinjaLive settings):
- A point slower than about 40 cm/s does nothing at all.
- Above that, a point paints dye as well as velocity. In still water it drives a jet at about 2.3 times the written speed. In a driven current, such as a river, it speeds the current up along its flow, whatever direction is written. Points cannot brake the water.
- FluidNinja's puncture push (a NinjaLive's Brush Puncture) leaves a short backward dip behind a moving point.

What that means at the defaults:
- In still water and the pool, a swimmer's side wake stays under the cut-off: it leaves no measurable wake. Only the plunge ring reaches the water.
- In a river, a swimmer going upstream fast (about 5 m/s through the water) clears the cut-off with its braking write, and the river speeds up around it by 1 to 1.3 m/s.
- A higher gain makes a wake in still water. At 0.6, the water just behind a 3 m/s swimmer runs at about 47 cm/s (119 at most) and fades over 2.5 s. But a river then speeds up around a swimmer that only holds its place (about 2 to 3.3–3.9 m/s), and in the demo pool's swirl the water behind the swimmer runs backward. So the default stays at 0.2.
- The **visible** wake trail does not need the point input at all from interface version 4: the [foam](#foam) channel draws a swimmer's and a hull's trail without writing anything into FluidNinja's velocity or density, so it works at any speed and never speeds a river up. The point input stays for what moves actual water: the plunge rings.

Who makes wakes:
- **The player's pawn**, while its Character Movement swims over a water volume, or while it wades (at least 20 cm of water at its feet). Once the top of its capsule is more than 50 cm under the surface (diving), it leaves the surface alone. Character Movement floats a swimmer with its capsule's top at the water line.
- **Each registered swimmer**, while its hips (`pelvis`) are in its water and its head is no more than 50 cm under it. Its velocity is its mesh's own motion; its hips' fall counts for its plunge.
- When the player's pawn's mesh is registered, the pawn makes its wake as that registered swimmer only, so no body makes two.

**Swimmer Wakes** turns them off. So do `fnb.Buoyancy.Wake 0`, the test bench's controlled water, and a missing Wake Data Channel. FluidNinja reads the points only on a NinjaLive with `DataChannelPointReaderEnabled` on, as for the floating bodies' wakes.

Measured in the demo: a 3 m drop (7.7 m/s) pushed one ring, 8 points at 2.7 m/s outward.

**The current at a point.** **Get Current At Location** returns FluidNinja's current around a place, so a project's own swim code can carry its swimmers with the river (see the [functions](#blueprint-and-c-functions)). FluidNinja Buoyancy moves no swimmer itself: drifting, struggling against the flow and being swept away are the project's movement.
- The value is the mean flow on a ring around the place, by default **Swimmer Current Ring Inner / Outer Cm**. It is a ring because the swimmer's own wake lies under it: read there, the swimmer would feel the water it pushes, and be carried by it.
- It is read back from the GPU. The value is two or three frames old, and the call returns false for the first frames at a new place. Call it every frame for a moving swimmer.
- The wakes read the current the same way, for their target.
- It reads FluidNinja's velocity field wherever the simulation's square reaches, over the banks too: it does not check that there is water at Location. Call it while your swimmer is in the water.
- Measured in the demo river: at the eight floaters it agreed with each floater's own current to within 1 to 17 cm/s (6 typically), on currents of 0.7 to 3.5 m/s, in both directions.

---

## Blueprint and C++ functions

All are on **UFluidNinjaBuoyancySubsystem**, a world subsystem (in Blueprint: **Get FluidNinjaBuoyancySubsystem**).

| Function | What it does |
|---|---|
| **Add Water Impulse** (Location, Impulse Down, Area Cm2) | Presses the water down at Location with an impulse in kg·cm/s, Unreal's units (negative lifts it), spread over Area Cm2 seen from above, widened as the hulls' impulses are (at least Wave Source Smooth). It is applied at the next frame, scaled by Wave Impulse Scale like the hulls' impulses. Use it for a cannonball, an arrow or a foot stamp. |
| **Add Wave Poke** (World XY, Height Cm, Sigma Cm) | A Gaussian pressure head Height Cm high (negative: suction) and Sigma Cm wide, for one tick, at the next frame. A wave maker calls it every tick. |
| **Register Swimmer** (Mesh, Source) | Takes the mesh as a swimmer until **Unregister Swimmer** or until the mesh goes away. Registering it again changes its source. |
| **Unregister Swimmer** (Mesh) | Stops it. |
| **Get Swimmer Stats** (Mesh) → Stats | What the mesh's parts did to the water: entries, entries a minute, the last part, its entry speed, its impulse (N·s), its splash strength, and when it went in. This works for a registered swimmer or the player's mesh. |
| **Get Current At Location** (Location, Inner Radius Cm, Outer Radius Cm) → Current, Age Seconds, read | FluidNinja's current around Location, world cm/s (Z is 0), at most Max Flow Speed. It is the mean flow on a ring from Inner to Outer Radius Cm. The radii are advanced pins; left negative, they are the Swimmers ring (70 to 160 cm), and Inner 0 reads the whole disc. The value is read back from the GPU: each call asks for a fresh reading there and returns the newest one, Age Seconds old (two or three frames). It returns false for the first frames at a new place, outside every simulation the drawn water shows, and while the plugin is off. Calls a frame apart at nearby places share a reading, as do calls in the same frame within 25 cm of each other. Up to 32 places are read at once. See [Wakes and the current](#wakes-and-the-current). |

**Swimmer Source** (`FFluidNinjaSwimmerSource`):

| Field | Meaning |
|---|---|
| Water Z | The still water's height at the swimmer, world cm. |
| Displacement | Its head, chest and hips push the water aside. |
| Part Contacts | Its hands, forearms, feet and head splash and press rings. |
| Whole Body | Its chest, hips and thighs make contacts too (a diver going in). |

---

## The interface with the core

What the plugin and the modified NinjaLiveCore exchange. Use this if you drive the waves from your own code instead.

| User parameter | Type | Contents |
|---|---|---|
| `WaveSimEnabled` | bool | Waves on for this NinjaLive. Off (the default): every wave stage is off and the grids are 1 × 1. |
| `Wave.ScaleDown` | int32 | The wave grid's scale-down S, 1 to 4 (the plugin sends 1, 2 or 4): the grid is the pressure buffer's cells over S, by integer division (`max(RTW / S, 1)`), cells S pressure cells wide. Interface version 3 on; 1 is what every earlier core did. |
| `WaveSourceTexture` | Texture | RGBA32F, W × (H + 1), where W × H is the wave grid's size (the pressure buffer's over S).<br>• Texel (i, j) covers the world square from origin + (i, j) × cell to origin + (i + 1, j + 1) × cell. Its channels are R = static head P (cm of water), G = impulse y, B = erase (above 0.5 resets the cell), A = the pressing hull's speed through the water (cm/s, the fastest per cell; interface version 4 reads it for the waterline foam, earlier cores ignore it; a plugin that writes none means no waterline foam).<br>• Header texel (0, H) = (origin x, origin y, cell size cm, serial). The cell is the wave cell. |
| `WaveAuxTarget` | Render target | RGBA32F, W × H, the wave grid's size. Each tick the core writes (wet mask, depth cm, drift x, drift y in wave cells per tick) per wave cell, but only while the target has the wave grid's size. Its default, `RT_WaveAux_Unbound` (1 × 1), keeps an unbound NinjaLive quiet. |
| `WaveFoamTarget` | Render target | One channel (R16F does), W × H, the wave grid's size (interface version 4). Each tick the core's Wave Aux writes the foam F (0..1) per wave cell into it — 0 while `Wave.Foam` is 0 — but only while it has the wave grid's size and Wave Aux runs. Its default is the same 1 × 1 `RT_WaveAux_Unbound`: nothing bound, no foam. Wave Prepare also uses it as this tick's scratch (the carried foam for Wave Step), so read it as a texture, never write it from outside. |
| `Wave.DepthCm`, `Wave.DepthFromBed`, `Wave.Damping`, `Wave.SpongeCm`, `Wave.WallAbsorption`, `Wave.WallBandCm`, `Wave.EdgeOpen`, `Wave.ShoreDepthCm`, `Wave.BreakingSlope`, `Wave.Hyperviscosity`, `Wave.CurrentCoupling` | float | The settings above. |
| `Wave.MeanRelax` | float | Wave Mean Relax: how fast the window's mean of s = η + P over its wet cells is pulled back to 0 (1/s; 0 off). Interface version 2 on. From version 5 the core also clamps it to 0.5 ÷ its step; the plugin clamps it on every version (see [The mean level](#the-mean-level)). |
| `Wave.VMeanRelax` | float | How fast a walled window's mean vertical velocity ⟨v⟩ is pulled to 0 (1/s; 0 off). Open windows (`Wave.EdgeOpen` above 0.5) ignore it, and the core clamps it to 1 ÷ its step. The asset's default is 0, so a plugin that does not send it gets what interface version 4 did. Interface version 5 on. |
| `Wave.Foam`, `Wave.FoamDecay`, `Wave.FoamBreaking`, `Wave.FoamWake`, `Wave.FoamImpact` | float | The foam switch (above 0.5: accumulate) and its tuning ([Foam](#foam)). Interface version 4 on. |
| `Wave.InterfaceVersion` | int32 | The version of this interface, set by the apply script. The plugin requires 2 or more. See [Interface version](#interface-version). |
| `Wave.CoreBuild` | int32 | The date the apply script ran, YYYYMMDD. |

**Emitter outputs** (interface version 3 on; `Ninja_WaveParams` writes them each tick, for the stages and for anything else on the emitter): `Emitter.Wave.ScaleDown` (int32, S as clamped), `Emitter.Wave.Cells` (vec2: the wave grid's cells across and down) and `Emitter.Wave.CellCm` (float: the wave cell, cm, the window's extents over the cells).

**Output:** while the waves run, `RT_PressureDivergence` is RGBA16F holding (pressure, divergence, η + P, P), at the pressure buffer's own resolution whatever the wave grid's (η bilinear between wave cells, P bilinear between the source texture's cells). Without them it is the stock RG16F. The foam goes into `WaveFoamTarget` instead (above): the pressure buffer's four channels are taken, and a separate target lets a tier drop the foam without touching the export.

**Timing:**
- The source is read at every tick of the frame that follows its write.
- An impulse y is spread over that frame's ticks (`Engine.System.NumTimeSteps`), so the total pushed in is y whatever the frame rate.
- The plugin follows the core's tick count (Niagara's own) to step the own fields with exactly the same ticks.

### Interface version

Two user parameters say which core the plugin is talking to. No stage reads them; the plugin reads both on the game thread.

| Parameter | Meaning |
|---|---|
| `User.Wave.InterfaceVersion` | The version of this interface: the user parameters above, the source texture's layout and header, Wave Aux's channels, the export's channels, and what the step does with them. It moves when the plugin and the core must change together.<br>**2**: breaking judged on η alone, the sponge on η, the mean level relaxed at `Wave.MeanRelax`. The source-texture header protocol is unchanged from the cores before it.<br>**3**: the wave grid decoupled from the pressure buffer: `User.Wave.ScaleDown`, the source texture and Wave Aux at the wave grid's size, the mask and depth judged over each wave cell's block, the export upsampled. The header protocol is unchanged; at S = 1 a version 3 core does exactly what a version 2 core did.<br>**4**: the foam channel: the Foam attribute on the wave grid, `User.WaveFoamTarget` and `User.Wave.Foam*`, the source texture's A = the hull's speed through the water, and the SURFACE function's foam and choppiness parameters. Nothing in (η, v) changes: with the foam off a version 4 core does exactly what a version 3 core did, and the plugin runs on 2 and 3 as before, without foam (the status line says `no foam (pre-4 core)`).<br>**5**: the closed-window ⟨v⟩ pin and the in-step clamp of both mean-relax rates: `User.Wave.VMeanRelax` pulls a walled window's mean vertical velocity to 0, and Wave Step clamps it to 1 ÷ Dt and `Wave.MeanRelax` to 0.5 ÷ Dt, against the Dt it actually steps with, each only past a 0.01 % margin. At `Wave.VMeanRelax` 0 (the asset's default) and `Wave.MeanRelax` ≤ 0.5 ÷ Dt, (η, v) is bit for bit what a version 4 core makes: the margin lets the plugin's own clamp, 0.5 ÷ its tick computed on the CPU, pass through untouched instead of being swapped for the GPU's own rounded 0.5 ÷ Dt. The plugin runs on 2 to 4 as before, without the pin (the status line says `no v pin (pre-5 core)`), its core rate staying at 30 /s. See [The mean level](#the-mean-level). |
| `User.Wave.CoreBuild` | The date the apply script ran, YYYYMMDD, set by the script. It tells builds of the same interface apart; the status line and a bug report name it. |

The plugin requires `InterfaceVersion` ≥ 2. A core applied before interface version 2 has neither parameter: the plugin refuses it with a message naming the simulation and the core, and that simulation makes no waves. On a version 2 core this plugin runs, at scale-down 1, and says so once per simulation when a higher scale-down was asked; the scale-down needs version 3, the foam version 4 (on 2 and 3 it simply draws none), and the closed-window ⟨v⟩ pin version 5 (on 2 to 4 the level is held by the pin on the mean of s alone, at 30 /s). Re-apply the waves from the pass3 core with the current script ([below](#re-applying-after-a-fluidninja-update)); the script prints `interface version 5 core build YYYYMMDD` in its report, and its `verify` warns on an older core without failing it.

| Core (sha8) | `InterfaceVersion` | This plugin |
|---|---|---|
| 43cc1890, 97b976f6 (the zips before interface version 2) | no parameter | refused: re-apply |
| 1bed4654 (applied with the interface-2 script, build 20261017) | 2 | on this plugin build at scale-down 1, without foam |
| 9a11d0f1 (applied with the interface-3 script, build 20261017) | 3 | the scale-down; on this plugin build without foam |
| da2548bd (applied with the interface-4 script, build 20261018) | 4 | the foam channel; on this plugin build without the ⟨v⟩ pin, its core mean relax at 30 /s |
| `9adbcbf6` (core build 20261018) | 5 | the closed-window ⟨v⟩ pin; the full interface of this plugin build |

Every core, its base, build, the plugin commit that requires it and its zip: [Tools/CorePatch/VERSIONS.md](../Tools/CorePatch/VERSIONS.md). A core is named by its sha8 (the first 8 hex digits of the SHA-256 of `NinjaLiveCore.uasset`), the plugin commit that requires it is tagged `core-<sha8>`, and `apply_fluidninja_waves.py verify` prints the installed core's sha8 and tag on its last lines.

---

## Re-applying after a FluidNinja update

The toolchain is in the plugin, [Tools/CorePatch](../Tools/CorePatch/README.md): `apply_fluidninja_waves.py`, the module HLSL in `FluidNinjaWaves/`, and the Niagara / material editing helpers it calls (`FNBDevLibrary`, in the plugin's editor module `FluidNinjaBuoyancyEditor`). It runs from any project that has FluidNinja LIVE, the Python Editor Script Plugin, Editor Scripting Utilities and this plugin built; the core zips carry no copy of it any more.

In short:
1. Start from the **relative-tracking core** (the pass3 core: the NinjaLiveCore, PainterSourceData and Ninja_Painter of `FluidNinjaLive_2.0.0.56_RelativeTracking_UE5.8.zip`, core `3bee517c`), not the vendor one. The script consumes relative tracking but cannot create it: that change was made by hand in the Niagara editor ([TRACKING.md](../Tools/CorePatch/TRACKING.md) says what it is and what scripting it would take), so a vendor core yields waves *without* relative tracking. The `unapply` step puts the pass3 files and the vendor SURFACE function back and deletes `Core/Modules/Waves`, checking each file's hash.
2. With the editor closed, run (one process; the steps run in this order):
   ```
   UnrealEditor-Cmd.exe <project>.uproject -run=pythonscript -script="<project>/Plugins/FluidNinjaBuoyancy/Tools/CorePatch/apply_fluidninja_waves.py unapply --from=<RelativeTracking zip> --surface=<vendor SURFACE .uasset> core material verify" -unattended -nop4 -nosplash
   ```
3. Check `Saved/FluidNinjaWaves_apply.txt`: `RESULT PASS`, `core is pass3 (relative tracking)` (`core is stock` comes with a WARNING line and means no relative tracking), `interface version 5 core build YYYYMMDD`, the `VERIFY` lines, and the last lines `CORE sha8 <x> tag core-<x>`: the new core's name. Record it in [VERSIONS.md](../Tools/CorePatch/VERSIONS.md) and the script's `KNOWN_*` tables, tag the plugin commit `core-<x>`, and package the zip with `package_core.ps1`.

`verify` alone (no writes) says whether an installed core is one this plugin build runs on; it fails, with a non-zero exit code, on a stock core, a core without the waves, an interface version below 2, missing stages or modules, or a SURFACE function without the wave terms, and warns on an interface version below the script's own, 5 (a version 2 core has no scale-down, no foam and no ⟨v⟩ pin, a version 3 core no foam and no pin, a version 4 core no pin), or on a SURFACE function that carries the waves without the foam parameters (an earlier build's material edit: re-run `material`, which adds the foam and the choppiness and keeps the waves).

The `core` step refuses a core that already has the waves (the message names its interface version and build). It never changes Ninja_Painter, PainterSourceData, CopyGridToRenderTarget*, the NinjaLive Blueprints or the data channel, and it checks their hashes to make sure.

A new FluidNinja version has no pass3 core yet: relative tracking has to be redone on it first (`Docs/RelativeTrackingProposal.md`; the `inspect` step dumps the graphs of the current pass3 files to copy from), which the script cannot do.

---

## Testing

**Status.** The subsystem's **Get Status**, or the test bench panel (`fnb.TestHUD 1`), has a waves line for each simulation, for example:
```
FluidNinja Buoyancy waves: 14 bodies, 14 own fields (14 feel the waves), 726 impulse pieces, 2 writes, 24.5 MB of targets;
Pool_FluidNinja (component): core build 20261018, interface 4, 400 x 5.06 cm (scale-down 2 of 800 pressure cells), depth 400 cm, walled (absorbing 0.40/s over 50 cm) edge, normal scale 0.099, foam, 1 tick this frame ..., mean relax core 30.00 /s, self 0.50 /s, no v pin (pre-5 core)
```
`foam` means the foam channel is accumulating; `foam off` that the Foam setting or `fnb.Waves.Foam` is 0; `no foam (pre-4 core)` that the core predates it.
`mean relax core R /s, self S /s` are the rates the core's and the own fields' mean levels are relaxed at, after the automatic choice and the clamps; then `v pin V /s` on an interface-5 core with a walled window, `v pin off` at `fnb.Waves.VMeanRelax 0`, `no v pin (open edge)` in an open window, or `no v pin (pre-5 core)`. See [The mean level](#the-mean-level). The test bench reads the core's rate from here.
A simulation without waves says why: Wave Sims off (or its component's Waves off), a stock NinjaLiveCore, an older waves core, or `fnb.Waves.MaxActiveSims` keeping it off (with its rank and distance). `scale-down 1, 4 asked: interface 2 core` means the core predates the scale-down.

**Scale-down.** To compare the grids, run the bench tests at `fnb.Waves.ScaleDown 1`, 2 and 4 (the sims' own overrides at 0) in a fresh Play each: `ripple-dispersion` (crest arrivals within about 2 % of the 1 run at 2 and 4; the normalised RMS grows with the cell, the 4 m ring at S = 4 most), `ripple-kelvin` (the wake's height and cusp angles within the test's own tolerance at 2; at 4 the cells are about a tenth of the boat and the arcs smooth), `ripple-weight` (the static head's volume within 2 % at every S: Σ P × cell area is the hull's volume whatever the cell), `ripple-rest` and `ripple-coupling`. `stat gpu` shows the core's stages inside the NinjaLiveCore system's time (`stat Niagara`, or Insights' GPU track: the `Wave Step` dispatch); `stat FluidNinjaBuoyancy` shows the self cells, which fall as S² for the same bodies.

**By eye** (demo level):
1. `fnb.TestHUD 1`.
2. Select the boat and press **Ctrl+Shift+J** to drop it from 3 m. Rings run out across the pool, bounce off the walls, and rock the balls, the ring buoy, the crate and the mattress as they pass. The landing leaves a white patch (the foam's impact source) that fades in about a second and a half.
3. Swim across the pool: your strokes splash and leave rings, and a foam trail follows you.
4. Tow or row the boat: its waterline sheds a foam trail carried by the current, fading at Foam Decay. `fnb.Waves.Foam 0` removes every trace within a tick and changes nothing else; a pre-4 core draws the same water without foam.

**Rest.** Leave the demo pool alone for 4 minutes with the pool calm (`fnb.Test pool calm`, or the bench's **Pool → calm**). The floaters' bobbing should die away, from 4–6 cm in the first 20 s to under 0.15 cm. Measured in a standalone game at a fixed 30 fps and 60 fps, and free-running at 60 fps: the boat goes from 3.8 cm to 0.07 cm peak to peak.

**Open-water level.** To follow the mean level under a current, read the wave height in open water, away from the hulls, shores and walls. The 2026-10-18 runs read **Get Water At Location**'s wave height on a 5 × 5 grid over the demo pool every 0.5 s, during a standalone `walkaway`, with the pool's current running:

| `fnb.Waves.MeanRelax` | Open-water level |
|---|---|
| 0.5 (the old default) | −15 cm by 16 s, still falling; over the bench suite's minutes, from −40 to −67 cm (see [Known limitations](#known-limitations)) |
| 0.5, with Wave Breaking Slope 100 | no sink |
| 5 | −0.7 cm |
| 30 (the automatic rate in a walled window on a pre-5 core), 60 | about 0 |

**Read only the readings from before the away phase** (the first 16 s or so) and the ramp after the player is back. During `walkaway`'s 14 s "player 70 m away" phase those readings repeated bit for bit, from about 16 s to 30 s in each run: the readback is frozen while the player is away. A level fitted across that phase reads a plateau that is not there.

**Bench tests** (`fnb.Test run <name>`, or `ripples` for all of them in about 15 minutes). They measure FluidNinja Waves against the theory of the core's own equation, and report numbers.
- Each test reads the simulation it measures in.
- Each finds its water in the core's own Wave Aux (its wet mask and depth), as the waves see it.
- Each lifts the bodies it does not measure out of the way.
- Each switches off the sources it does not measure before it clears the waves. A body whose static head is switched off after the clear leaves its hollow behind, and the hollow springs back as waves.

The theory, from the core as it runs:
- **The field.** One linear field (η, v) per simulation, at FluidNinja's fixed 60 ticks a second: `v' = v − dt·g·L(η + P + P_dyn) − dt·Γ·v`, `η' = η + dt·v'`.
- **Dispersion.** ω² = g (k + (σ/ρg) k³) tanh(k d). The core uses its own g (981 cm/s²) and σ/ρg (72.8/981 cm²), at the depth its Wave Aux shows.
- **Damping of v.** Γ(k) = Wave Damping + Wave Hyperviscosity × (1 − B̂(k))², where B̂ is the 3 × 3 binomial's symbol. A wave's height decays at Γ/2.
- **Impulses.** An impulse y presses as P_dyn = y / (ticks × dt), spread over its frame's ticks. It kicks v by −g L(y) and lifts nothing: Cauchy–Poisson's impulse problem.
- **Static head.** P presses a hollow η = −P under a resting body. Σ P × cell area is the volume of its hull under the water.

| Test | What it measures | Pass |
|---|---|---|
| `ripple-dispersion` | A 4 cm pressure head, σ 10 cm, pressing for one tick (an impulse), on controlled water. It goes at the most open spot of the finest simulation with room for it. Its rings at 1, 2 and 4 m are read against the core's impulse response there. The same theory stepped as the core steps it (symplectic Euler) is reported beside it. | The largest crest's arrival within 5 %, and normalised RMS within 10 %. |
| `ripple-fps` | The same poke at 30 and at 60 fps: a fixed-step run's step, or `t.MaxFPS` otherwise. | The two runs' largest crests within 5 % in arrival and in height. |
| `ripple-kelvin` | The boat towed at 1.75 m/s on controlled water, its static head alone, along the longest straight open run of any simulation. Two arcs behind it are read against the exact linear wake of that hull (B_tt + Γ B_t + ω² B = P_tt + Γ P_t, solved in closed form), and so is each side's cusp line. The fit of size and shape is reported beside them. | Each arc's normalised RMS within 25 %; each cusp line within 2° of the exact wake's. |
| `ripple-weight` | The crate at 20, 80 and 320 kg, and the boat, each settled at that mass and dropped from 3 m, with every other body out of the way. Judged: the static head under it at rest, against its hull's volume under the water. The static head is the window's Σ P × cell area at rest, less the same while it falls; the hull's volume comes from its triangles clipped at 1 cm. Reported beside it: m / ρ, the ring heights against Evers et al. (2019), the field's volume (against (m / ρ) e^(−r t) from the impact, r being the core's mean relax: see Volume in a closed basin under [Known limitations](#known-limitations)) and its wave energy. | Within 2 %. |
| `ripple-rest` | The row boat alone at rest, at κ 0 (reported) and κ 1 (judged), 60 s each, read by eight probes 1 m off its hull. | No build-up: the last 10 s no louder than the first 10 s (or 0.02 cm) and under 0.1 cm; drift under 0.5 cm. |
| `ripple-coupling` | The boat, its static head alone, dropped from 3 m, with a beach ball 3 m away and every other body out of the way. The drop is made twice: once with the ball in the water, after it has settled (bobbing under 0.5 cm/s for a second), and once with it lifted out, which reads the ring that reaches its place. A probe at a light body also reads that body's own waves, so the second drop is the reference. Judged: the ball's rise on the ring's largest crest, against that crest. Reported: the crest's length against the ball's size, and the field at the ball's place with the ball in the water. | 80 % to 125 %: a body much smaller than the wave rides it. |
| `ripple-evers` | A block made for the open water with a level floor it finds, dropped in, against Evers et al. (2019)'s 3D impulse-wave equations inside Table 3-3. Each first crest, trough and wave height is read before a reflection can arrive. | Within the scatter of Table 4-5. |
| `ripple-advection` | A poke beside a river floater in the running river. Judged: its ring's drift down the current, against the core's own drift (its Wave Aux) along the ring's path. Reported: the current the floater feels, and the same poke not carried. | Drift ratio 1 within 15 %. |

Results go to the panel, the log and `Saved/FluidNinjaBuoyancy/TestResults/`. Run one group at a time per project: a second `ripples` running in parallel writes the same `latest.json`.

**Plunge source** ([A body that sinks](#a-body-that-sinks); still to be measured on the GPU):
1. `ripple-evers`, both drops. They should read the same, since a sinker sends no impulses. Watch the 4.5 m and 6 m γ 45° troughs first: they have the least headroom in the port (about 4 % and 7 %), and every variant tried failed there first. The log line "… sinks (resting draft −1.0 cm): it presses a deep volume head …" must say "whole from its first contact" ("fading in" means the head started at nothing and that drop is invalid), and gives the depth the head used, which must equal the bench's planned h (300 cm), and its footprint (174 × 300 cm, in either order). `GetSourceVolume` should read about the block's submerged volume while it plunges, 11.3 m³ once it is under. The block feels no waves now, so compare its bed time in the log with an earlier run.
2. `ripple-kelvin`, `ripple-weight` and `ripple-coupling`: floating bodies only, so they must read what they read before (the Kelvin wake 1.00× theory at Wave Breaking Slope 1.0).
3. The same with `fnb.Waves.PlungeSource 0`, for A/B: today's capped head, the graded release and the impulses.
4. Optionally, the impact speed ±10 % (the drop height), with the Evers bounds the bench recomputes for it, and spectra read with a windowed, mean-removed metric.

A frame of P in a window with floaters only should be bit for bit the same with and without the change: the deep pass is skipped there, and the assembly adds nothing.

**Results on the demo level** (standalone game at a fixed 60 fps and a fixed 30 fps, radiation damping at 3×, cores before interface version 2): 12 of 57 pass at 60 fps and 13 of 57 at 30 fps. Each failure is explained in [Known limitations](#known-limitations). The Kelvin, Evers and river-level rows are to be re-measured on an interface version 2 core.

| Test | 60 fps | 30 fps |
|---|---|---|
| `ripple-dispersion` | 1 m passes. 2 m and 4 m fail on RMS (17 %, 30 %), with arrivals within 0.1 %. | The same. |
| `ripple-fps` | Passes: the two runs agree within 1 %. | (one test across both rates) |
| `ripple-kelvin` | Fails: the wake is 0.56–0.58 of theory's height, correlation 0.89–0.93. With Wave Breaking Slope 2 all four pass: 1.01–1.02 of theory's height, correlation 0.99, RMS 12 %. | Fails: 0.55–0.56, correlation 0.88–0.92. |
| `ripple-weight` | 6 of 8 pass, within 1 %. The 20 kg crate reads +2.4 % and −2.9 %: in this run it did not float at its weight (see Known limitations). | 7 of 8 pass. The 20 kg crate reads +37 %. |
| `ripple-rest` | Passes: 0.007 → 0.006 cm RMS over 60 s. | Passes: 0.007 → 0.008 cm. |
| `ripple-coupling` | Passes: the ball rises 84 % of the incident crest (4.3 cm, a 1.6 m wave). | Fails: 76 % of a 5.0 cm crest (a 1.8 m wave). |
| `ripple-evers` | 34 of 108 measures inside the scatter. With the static head alone, the troughs and wave heights fall inside and the first crests about ⅓ of Evers'; with the impulse, everything is 2–3× too large. | 32 of 108, the same pattern. |
| `ripple-advection` | Passes on core 97b976f6 (drift averaging): 1.05 of the core's drift, 0.97 of the floater's current; the ring keeps 76 % of its still-water energy. (On core 43cc1890: 1.17 fail, 1.06 in an earlier run.) | Passes on core 97b976f6: 1.02, 0.91 of the floater's current, 63 %. (On 43cc1890: 1.03 and 1.02.) |

---

## Troubleshooting

| Symptom | Check |
|---|---|
| No waves; the status says "stock NinjaLiveCore: no User.WaveSimEnabled" | Install the modified core (the zip). |
| The status says "older FluidNinja Waves core: no User.WaveAuxTarget" | Your core predates Wave Aux. Install the current zip. |
| The status or log says the core has no `User.Wave.InterfaceVersion`, or one below 2 | Your core predates interface version 2 (breaking on η, the sponge on η, Wave Mean Relax). Install the current zip, or re-apply the waves from the pass3 core with the current script. |
| The status says `scale-down 1, 2 asked: interface 2 core`, or the log says a simulation's core has no `User.Wave.ScaleDown` | Your core predates interface version 3: it runs the waves at the pressure buffer's resolution. Re-apply the waves with the current script for the scale-down. |
| A simulation says `none (fnb.Waves.MaxActiveSims N: it ranks M, X m from the camera)` | The limit keeps it off: its nearest floating body or swimmer is not among the N nearest (X is that distance; a simulation with neither ranks behind all that have one). Raise the limit, or the simulation's Importance. |
| Waves look coarse or blocky, short ripples are gone | The scale-down is high for the water: 4 on a small pool with fine cells loses everything under about four wave cells. Set the simulation's Scale Down Override to 1 or 2 (a fact about that water; the tiers then leave it alone). |
| The log repeats "RenderTarget UserParam is required but invalid" | Your core predates the `RT_WaveAux_Unbound` default: every NinjaLive that nothing binds logs it every tick. Install the current zip. |
| Waves on one simulation only | Is the other in Wave Sims with Enabled off? Does its surface material use `MF_NinjaOutput_BaseFunction_SURFACE`? FluidNinja's own surface materials do. |
| Waves run too fast or too slow for the water | Its depth is wrong. A pool whose height field reads its water surface, not its floor, runs at the near-zero depth that gives: set Depth Cm in its Wave Sims entry. |
| Floaters in a closed pool keep bobbing for minutes | Is it walled (Edge Open off) with Wall Absorption above 0? A lossless basin keeps every echo. |
| Rings reflect at the edge of open water | Its Edge Open is off, or Wave Sponge Cm is 0. |
| The drawn waves look too sharp or too flat | Wave Normal Scale Factor, or the simulation's Normal Scale. |
| A glassy spike or column of water stands up at a body | Your plugin predates the wider impulse splats and the depth fades: a sinking body kept pressing the surface over it, and a deep narrow hull pressed a pit that sprang back as a spike. Update the plugin (the core is unchanged). |
| Splashes all the same size | They hit Splash Strength Ceiling or the body's Max Splash Strength. Raise Splash Reference Watts to bring them under it. |
| A swimmer makes no rings | Is its mesh registered (Register Swimmer), or is it the player's pawn with Character Movement swimming? Is Swimmer Contacts on? Is the part going in faster than Swimmer Entry Min? |
| No foam | The status line says why: `no foam (pre-4 core)` (re-apply the waves with the current script), or `foam off` (the Foam setting, or `fnb.Waves.Foam 0` from a quality tier). A surface whose material predates the interface-4 material edit draws none either: `verify` warns (`carries the waves without the foam`); re-run the `material` step. |
| Foam but no wake trail behind a moving hull | The trail needs the hull's speed through the water in the source texture's A: an older plugin build writes none. The whitecaps and impact rings still work. |

---

## Known limitations

| Limitation | Detail |
|---|---|
| Floaters rocking each other (fixed) | Several bodies that both make and feel waves share a collective mode near 1 Hz. With the radiation damping at 1× the Haskind value, it gained energy.<br>• The demo river's eight resting floaters plateaued at 2.5–5 cm peak to peak.<br>• At 30 fps it built up too, only later; the readback's lag just shifts the phase.<br>Fixed by tripling the radiation damping near rest (see Own fields). With it, 300 s rests decay to 0.00 cm (river) and 0.07 cm (pool) at fixed 30 and 60 fps, free-running and in editor Play.<br>The tripling fades out on motion, so a dropped or driven body keeps the physical Haskind value. |
| Waves carried by a current (`ripple-advection`) | On core 97b976f6 (drift averaging) a poked ring in the demo river drifts at 1.05 / 1.02 of the core's own drift at 60 / 30 fps. Against the current a floater feels it is 0.97 / 0.91. It keeps 76 % / 63 % of its still-water energy; Catmull-Rom smooths a carried ring.<br>On core 43cc1890 one 60 fps run read 1.17 (fail). It was poked where the core's drift read only 0.82 of the floater's current, and the bench reads the core's drift from two Wave Aux snapshots while the river's current varies in between and along the line. The new core's runs pass, but two runs don't prove the spread is gone. The earlier 0.14 and the energy gain came from the test bench: it switched the bodies' static head off after clearing the waves, so their hollows sprang back as waves on the probe line. |
| Banks the waves don't see | A simulation whose NinjaLive has neither SDF nor height-field collision has no banks for the waves: its Wave Aux is wet everywhere. The demo river's waves run across its banks as if its whole 60 m window were open water 300 cm deep, and leave only through its sponge. |
| Pool in editor Play at 60 fps (fixed) | The same collective mode. Editor Play reads the waves back a frame sooner than a loaded standalone game and steps unevenly, so 2× damping still let the demo pool grow there (0.3–1.4 cm by 300 s, two runs of two). 3× decays (two of two). |
| Eddies in a current (fixed in core 97b976f6) | Inside an eddy, where the current's drift changes sign from one cell to the next, the core's Catmull-Rom advection let small spots grow. Offline, on the demo river's own current, a spot grew ×7 in 40 s while the total energy fell. The core now averages the drift over a few cells, and offline the spot then decays. ripple-advection on that core: 1.05 / 1.02 of the core's drift at 60 / 30 fps, with no energy gain. |
| Mean level drifting under a current (the river creeping up, the pool sinking) | **The river, before interface version 2.** With the demo river running, the whole wave field gained a mean level that kept rising: B averaged about 16 cm over the 60 m window after 265 s at fixed 60 fps, rising steadily. With User.Wave.CurrentCoupling 0 it stayed level. The old core 43cc1890 did the same (16.9 cm), so the drift averaging was not the cause. A uniform level is a mode nothing damps; the sponge damped v only, and the window's border held water in.<br>**Addressed in interface version 2:** the open-edge sponge relaxes η as well as v, and the window's mean of s = η + P is relaxed at Wave Mean Relax (then 0.5/s). To be re-measured with the 265 s river run.<br>**The pool, 2026-10-18.** On an interface-4 core, the walled demo pool with its current sank instead: −15 cm in open water by 16 s, and from −40 to −67 cm over the bench suite's minutes. The bodies sank with it.<br>**Diagnosis: breaking at the floating hulls' hollow edges pumps ⟨v⟩.**<br>• The static head is rastered with no taper, so a box hull's hollow (η = −P) is a one-cell step. The cells under a source are left out of the breaking test.<br>• Where advection, the hull's motion or the raster moves part of the hollow into source-free cells, its wall slope (P ÷ cell, about 6) is far over Wave Breaking Slope.<br>• Those cells are refilling (v > 0), and breaking scales their whole v down. So every tick it takes a one-signed amount off the sum of v, the uniform vertical velocity ⟨v⟩.<br>• Nothing restored ⟨v⟩: the operator does not see it, and only Wave Damping and the wall band decay it (about 0.06/s). The pin on the mean of s parked the level at ⟨v⟩ / r, so the level sank as ⟨v⟩ grew.<br>• Confirmed on the GPU: at Wave Breaking Slope 100 the sink went away at 0.5 /s, so breaking is the pump. A stronger pin bounded the level (−0.7 cm at 5 /s, about 0 at 30 and 60), as the ⟨v⟩ / r picture predicts. These runs do not measure the 1 / r scaling itself: at 0.5 /s the level was still falling at 16 s, so it was not yet a parked ⟨v⟩ / r, and the readings at 30 and 60 /s are too small to tell.<br>• The river's creep fits the same pump with the other sign (port only): judged on η + P, before interface version 2, breaking damped a hollow's leading edge instead, and the level crept up.<br>**Mitigated in this plugin build:** the core's mean relax runs at 30 /s on a pre-5 core (`fnb.Waves.MeanRelax`, automatic), which holds the level. ⟨v⟩ itself still grows.<br>**Interface version 5** pins a walled window's ⟨v⟩ (`fnb.Waves.VMeanRelax`), which removes the integrator, and the s rate drops to 5 /s. Open windows keep the s pin at 30 /s. The pump itself, breaking at the hollow's step edge, is still there. See [The mean level](#the-mean-level). |
| Short ripples over distance (`ripple-dispersion`) | A poke's rings arrive within 0.1 % of theory at 1, 2 and 4 m, at 30 and 60 fps alike. Their crests drift above the theory's height as they travel: +1 %, +5 % and +11 %. The normalised RMS grows to 17 % at 2 m and 30 % at 4 m.<br>• Stepping the theory as the core steps it removes only a fifth of that.<br>• Switching Wave Damping off moves measurement and theory alike, so damping is not the cause.<br>What is left is the grid operator's dispersion: its fit of ω(k) bends slightly off the true curve, and a dispersing ring shows that as crest heights and phases that drift with distance. |
| Towed wakes (`ripple-kelvin`) | A boat towed at 1.75 m/s leaves a wake with the exact linear wake's shape and phase (correlation 0.88–0.93), but only 0.52–0.58 of its height, at 30 and 60 fps alike, as long as Wave Breaking Slope is 0.4.<br>The cause is breaking, and not where it was first thought. Interface version 2 judges breaking on η alone across source-free cells, so the static head's own edge is out of the judgement — and the wake still came out at 0.52. The steep water is the hull's own near wake: a convex hull with vertical topsides steps from its draft to nothing at the waterline, and the exact linear wake of that step is steeper than 0.4 over thousands of cells around and behind the hull (a port of the core on a 203 L hull with a 6 cm step at its edge: 4710 such cells 8 s into the tow, 99th-percentile slope 0.77 at 3–7 m behind), which the 20/s breaking decay damps every tick where the wake is born. Leaving 2–12 cells around the hull out of the judgement does not recover it (0.46–0.55 of theory); tapering the head does not either (0.48–0.57, and it costs the wake band itself).<br>• Wave Breaking Slope 0.7: 0.88 of theory, correlation 0.99.<br>• Wave Breaking Slope 1.0 (the default from the 2026-10-17 build): 1.00–1.03, correlation 0.99, RMS 14–17 %, lag 0; one cusp line sits at the 2° tolerance.<br>The slope is a net for cell-sized spikes (slopes of several); a breaking bow wave is drawn by the [foam](#foam) channel (interface version 4), not by damping the wave. A project that keeps 0.4 in its ini keeps the half-height wakes. |
| Riding a passing wave (`ripple-coupling`) | A 0.6 m, 0.8 kg beach ball rides the incident crest from the boat's static head:<br>• **60 fps:** 84 % (passes).<br>• **30 fps:** 76 % (fails).<br>A body much smaller than the wave should ride it fully.<br>**Radiation damping.** It acts on a body's absolute heave, against the wave-free surface, which keeps the scheme's energy balanced. For the ball it is 69 kg/s, 1.6 times critical. A body whose heave is held by its waterplane stiffness k and damped by c against still water rides a wave of frequency ω by k / \|k − mω² + i c ω\|. For the ball that is about 0.82 on these 1.0–1.1 s crests and 0.67 on a 0.7 m wave. So light bodies under-ride short waves, right at the test's 80 % floor, and the 76–84 % measured is that. A real body's damping acts on its motion relative to the water. Doing that needs a matching relative source in the core, and is a follow-up.<br>**Small crests scatter.** Single runs on 1.1–1.2 cm crests read 95 % at 60 fps, and 40 % and 44 % at 30 fps. The 30 fps reading did not come back on the 5 cm crest of the full group.<br>**FluidNinja's own surface.** The boat's landing also stirs FluidNinja's own surface, which the ball rides too: up to 1.9 cm about 1.5 s after the drop, which the wave probe does not see. The test judges the ring's largest crest, which comes after that. |
| Volume in a closed basin | The field's own volume (Σ η) is not kept exactly.<br>In theory, after a body is lifted out of the walled demo pool and dropped back in:<br>• at the lift the field gains the body's hollow (m / ρ);<br>• once the body is back in, its static head raises the mean of η + P by m / ρ again (Archimedes' rise of the level);<br>• Wave Mean Relax takes that rise off as e^(−r t) from the impact, so the field heads back to its volume at rest (at 30 /s, within a fraction of a second).<br>`ripple-weight` reports the field's volume against (m / ρ) e^(−r t), with r the core's rate from the status line.<br>Measured after the drop on earlier builds, against m / ρ kept:<br>• the static head alone: 0.1–2 m³ off;<br>• with the impulse: up to 24 m³, about 6 cm of mean level.<br>The core's masked operator at walls, its wall band and breaking each change Σ v. |
| Impulse energy | With Wave Impulse on, a 3 m drop puts 0.9–7 times the drop's own energy m g h into the waves' potential energy alone (static head alone: 0.7–5 %). In `ripple-evers` the impulse made every first crest and trough 2–3 times Evers'; a sinker now sends none ([A body that sinks](#a-body-that-sinks)), so that drop no longer measures the impulse. The impulse source counts the hull's stopping impulse on top of the water its static head already moves. Lower Wave Impulse Scale to taste.<br>**Interface version 2** also removes the breaking test's own-tick damping of each impulse (judged on η + P + P_dyn, a one-tick spike broke on itself and was damped by exp(−20·dt) ≈ 0.72), so impulse rings come out about 1.4 × larger than the numbers above until Wave Impulse Scale is recalibrated against the Evers first crests. To be re-measured (`ripple-evers`). |
| Plunging bodies and leading crests (`ripple-evers`) | Measured before the graded release, on the Phase 3 build at Wave Breaking Slope 1.0, static head alone, γ 0: the falling block's first crest at 4.5 m read 22.8 cm. The bench's image-slide reading of Evers et al. (2019) gives 11.65 cm (6.41–14.56), and its direct whole-block reading 18.12 cm (9.97–22.65). At slope 0.4 the crest read 11.0 cm, but only because the breaking damped the near field, the same damping that halves the towed Kelvin wake.<br>The source was at fault, not the breaking. The block sinks (15.2 t against the 11.3 m³ its hull displaces), so its head was capped at half its 174 cm waterline width: 87 cm, 40 % of its volume. That cap was dropped in the one tick its top went under, and the 87 cm hollow sprang back. The graded release ([A hull going under](#a-hull-going-under)) lets that hollow fill over 0.42 s instead. Measured with it on the GPU: the displacement-only first crest read 24.6 cm at 4.5 m, but that was the reader catching a late 1.4 s packet of short steep waves; the real leading crest was about 3 cm.<br>**Addressed by the deep volume head** ([A body that sinks](#a-body-that-sinks), `fnb.Waves.PlungeSource`), which keeps a sinker's whole displaced volume and spreads it with depth. On the numpy port all 54 measures fall inside the scatter for both drops. To be measured on the GPU. |
| Deep volume heads | • **The period.** The first waves are 2.1–2.8 s long at the bench's probes, against Evers' 3.3–3.5 s. No linear volume source tried reaches 3 s. A slower descent did not lengthen them, and neither did a depth floor. The gain is about three times less short-wave energy, not added long waves. A cavity or crater term is the next physical step.<br>• **Trough to crest.** Its first troughs are about 1.9 times its first crests, against Evers' 1.24: the crests sit in the lower half of the scatter and the troughs in the upper half.<br>• **Breaking.** The core does not judge breaking where P is above 0.001 cm (its free-cell mask), so it is off over the head's whole disc (4 h, 12 m on the bench) while the head is there. In an earlier port run with breaking judged everywhere, it acted in only 88 cell-ticks over the whole drop, with a decay of at most 0.5/s. Judging the free cells from a hull footprint mask instead needs a core patch.<br>• **Open water.** The kernel assumes open water over a flat bed of the depth under the body. In a pool narrower than about 8 h, or over a sloping bed, the head's tail is not what the walls and the slope would make of it.<br>• **Hull shape.** The equivalent rectangle and column are exact for an upright box. A tilted, tumbling or rounded hull gets the rectangle and column with the same second moments. The exact route for those, a per-column span from the raster convolved with the kernel, is documented but not built: done directly, it costs about 2.5 × 10⁸ kernel evaluations a frame for the bench's block.<br>• **Channel A.** The exported A = P is above 0 over the head's whole disc. The plugin's own readers use B or B − A, but FluidNinja's surface material and FluidNinjaUnderwater have not been checked for a threshold on A that would hide hull interiors.<br>• **Feel.** A sinker feels no waves (Feel 0) and has no radiation damping or added mass from the waves layer, as before for a sinker.<br>• **Foam.** A plunging sinker makes no foam of the waves' own: it sends no impulses (no impact foam), writes no speed into A (no waterline foam), and breaking is off over its disc (no whitecaps). The splash system is not affected: only the waves' impulse loop leaves its impulses out.<br>• **The depth.** The kernel's h is the simulation's depth (its Wave Sims or component Depth Cm, else Wave Depth), or the trace's with Wave Depth From Ground. On a simulation that takes its depth from FluidNinja's height field without a Depth Cm, that is only the cap, not the core's depth there. The log line at the start of each plunge gives the h used.<br>• **Other sinkers.** Every body that sinks takes this path, not only the bench's block: a wrecking ball or an anchor gets the column and square of its second moments (a sphere: a square 1.55 R across and a column 1.55 R tall), not yet looked at for glass or spikes. |
| Light bodies that bob at rest (`ripple-weight`) | The static head under a resting body matches the volume its hull has under the water within 1 %. The 20 kg crate, the lightest body dropped, fails in two ways:<br>• **30 fps (+37 %).** It floats at its weight but is still moving when read. The readback lags a moving body's pose by a few frames, and at 30 fps the bench has half as many readings, spread over up to 5 L, to average.<br>• **60 fps, in one run.** The crate did not float at its weight: its hull held 11 L for 20 kg (the 80 kg crate held 68 L). It hit the water at 5.5 m/s instead of about 7.3 m/s, and its box read tilted. The static head still matched that hull within 2.4–2.9 %. Every other run floated the crates at their weight, so it rested on or against something. Not yet isolated. |
| Linear waves | The field is linear, apart from its breaking and shore damping. Very steep waves and plunging breakers come from the splash system, not the field; from interface version 4 the breaking is at least *visible* — it feeds the [foam](#foam)'s whitecaps — and `WaveChoppy`/`WaveChoppiness` sharpen the drawn crests the Tessendorf way, both render-side only. |
| Wake trails and whitewater | FluidNinja's point input cannot draw them (under 40 cm/s it does nothing; over, it paints dye that speeds a river up — the wake-law failure). From interface version 4 the [foam](#foam) channel is the answer: wake trails, bow foam, whitecaps and impact rings as a surface layer that writes nothing into FluidNinja's velocity or density. The point input keeps only the plunge rings. FluidNinja's whitewater *particles* are not driven automatically; the foam target is there for a particle material to sample (a documented follow-up in [Foam](#foam)). |
| One field per simulation | Waves live in each NinjaLive's window. They do not cross from one simulation into another. |
| The wave grid's scale-down | At S above 1 the grid carries no wave shorter than a few wave cells, the drawn surface is bilinear between wave cells (a faint grid in the normal at 4 up close), and the gather reads B on the pressure texels (bilinear from the wave grid) but B_self on the wave cells, so a body's own waves are taken out to within that second interpolation. The core's drift is still the current at the wave cell's centre, averaged over a few wave cells, not the mean over the cell's block of pressure cells: a wet-weighted block mean under the binomial costs 27 S² samples a wave cell, which at 4 made Wave Prepare and Wave Aux cost what they do at 1. The bench numbers at 2 and 4 are to be measured (Testing). |
