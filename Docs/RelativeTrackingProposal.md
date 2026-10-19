# Proposal: FluidNinja tracking relative to the water

This note is for FluidNinja LIVE's developers. It explains why floating bodies stay off FluidNinja's own tracking today, what makes a tracked floater speed a current up, a change to `NinjaLiveCore` that we prototyped and measured, and how to test it with FluidNinja Buoyancy.

## The problem

FluidNinja tracks a primitive that generates overlap events and that its NinjaLive actor selects, by object type (**Overlap Filter Inclusive Obj Type**) or by **Track Actor Primitive Components With Tag**. Every tick it paints the primitive's pivot into the simulation:
1. The `ArrayReader` emitter takes the pivot's velocity from its position change (`ParticleVelocity`). `WorldToUnit` turns it into `UnitVelocity`: in world mode, `0.0005 × v` (cm/s) rotated into the simulation.
2. `PainterSourceData`, in PainterAndSim's TrackPoint Painter stage, hands `ArrayBrushVelocity × UnitVelocity` and the pivot's position to `Ninja_Painter`, which paints a brush stroke of **velocity and density**.
3. The composite keeps the field's advected, pressure-projected velocity as it is, and adds that tick's forcing on top, multiplied by `0.5 × density` (the advected density plus `0.1 × EraserStrength ×` the brush's density). The forcing has three parts:
   - the brush velocity (`Painter.xy × 20 × VeloFromBrushMotion`);
   - the river's own driver (`VelocityFieldState`, the velocity offset and input texture, plus `VeloStrength ×` the painter texture), faded out where the brush velocity is strong (`1 − saturate(|brush|²)`);
   - the landscape gradient.

   The brush's density is stored in the field and advected with the water, so wherever painted density goes, the river's driver pushes harder each tick.

That suits a character, whose world velocity is its push. A floating body carried by a current moves *with* the water, so it paints the current back into the water every tick, and the density it paints makes the river's driver push harder along its path. The current then speeds itself up.

Measured with this plugin's demo river test (`fnb.Demo.RiverTest`: eight floaters, 10 s after the river starts, fixed 60 fps):

| Setup | Distance in 10 s | Water under the floaters |
|---|---|---|
| No interaction | 14.8 m | 1.9 m/s |
| FluidNinja's tracking today | 27.4 m | 4.8 m/s, up to 6 m/s (this plugin's read limit) |

## What drives the runaway

We switched parts of the brush off at runtime, in the same test (fixed 60 fps):

| Tracked floaters paint | Distance in 10 s | Water under the floaters |
|---|---|---|
| World velocity and density (today) | 27.4 m | 4.8 m/s |
| World velocity, no density | 14.6 m | 2.4 m/s |
| Velocity relative to the water, with density | 22.8 m | 3.9 m/s |
| **Velocity relative to the water, no density** | **13.7 m** | **1.9 m/s** |
| Nothing (no interaction) | 14.8 m | 1.9 m/s |

- **Switching the brush's density off removes most of the runaway.** FluidNinja already has the switch for it: **Selectively Kill Brush Density Keep Velocity** on the NinjaLive component (/LiveInputPoints /BrushKillers). Set to 1 (ARRAY), tracked primitives and bones still stir the water but paint no density. 4 (ARRAY and CHAOS) and 5 (ALL) do the same for them; 5 also takes the density out of data-channel points, which include this plugin's wakes.
- **It is the density, not the weaker stroke.** The switch does more than take the density out. In the TrackPoint Painter a stroke with no density is blended in with alpha (1 − Input Feedback)³ (0.51 here) instead of about 1, so it paints roughly half the velocity, and `Kill Brush Below This Velocity` no longer fades the stroke. Two runs of today's tracking with density on separate the two:
  - half the painted velocity (`Array Brush Velocity` 0.66, which `Brush Velocity Pow` 1.6 turns into 0.51): 26.5 m, 4.7 m/s;
  - no brush velocity at all (`Velo From Brush Motion` 0): 24.1 m, 4.1 m/s.
- **The rest is the world velocity.** With density off, today's tracking still leaves about 25% more water speed under the floaters than no interaction. Velocity relative to the water removes it.
- **Relative velocity alone isn't enough.** With density on, relative tracking still runs away. A gain on the relative velocity didn't bring it down: 0.7 gave 22.9 m and 4.3 m/s, 1.5 gave 20.4 m and 3.8 m/s (earlier prototype runs). The brush paints density whenever the velocity it gets is above `Kill Brush Below This Velocity` (0.01, about 20 cm/s), and a body carried by a current probably moves through the water faster than that much of the time.
- **Not the cause.** Switching off the brush's radial "puncture" push (`Brush Puncture` and `Brush Velocity Mult Z` at 0) didn't remove the runaway: today's tracking still went 26.4 to 26.8 m (5.5 m/s under the floaters), and relative tracking with density 20.8 to 21.1 m (3.2 to 3.3 m/s). These runs had density on, where the puncture fades with the body's speed.

## The proposed change

Two parts:
1. **Density.** Set **Selectively Kill Brush Density Keep Velocity** to 1 on every NinjaLive that tracks floating bodies. This is an existing setting. Set it on the NinjaLive in the level, on its **NinjaLiveComponent** (**Live Input Points → Brush Killers**). Saved there, it reaches the simulation: NinjaLive passes it on as the user parameter `Painter.SelectivelyKillBrushDensityKeepVelocity`, and PainterAndSim gets `SelectivelyKillPointSourceDensity` 1 (checked in the demo level, see [How to test it](#how-to-test-it)). Changing it during play was not tested.
2. **Relative velocity.** A new user parameter turns it on: `TrackVelocityRelativeToWater` (float, default 0 = today's behaviour, on above 0.5). When it is on, `PainterSourceData`'s ArrayReader branch paints a tracked pivot's velocity relative to the water at the pivot. A body drifting with the current then paints no velocity of its own; a body moved through the water (shoved, dropped, rowed) paints its motion through it. Set it in the level too, under the NinjaLiveComponent's **User Parameters**: the saved value reaches the simulation (checked in the demo level).

### Where it is done

Inside PainterAndSim, in `PainterSourceData`, where the tracked point's velocity is read (`Core/Functions/PainterSourceData`). The water is PainterAndSim's own `CompositeGrid.Composite` from the previous tick, sampled at the pivot's `UnitPosition + System.WorldOffsetDelta` (as the composite samples the previous grid). That is the velocity the advection uses, in cells per tick. No new data interface or render-target binding is needed.

Don't read the water through `User.RT_VelocityDensity` from another emitter. We tried that first, in `ArrayReader`, and nothing could read the simulation any more:
- a second Render Target 2D data interface with **Inherit User Parameter Settings** on, bound to `User.RT_VelocityDensity`, copies the texture's settings in `PerInstanceTick`;
- in `PerInstanceTickPostSimulate` it puts them back whenever the texture differs (`InitAutoFormat` + `UpdateResourceImmediate`, `NiagaraDataInterfaceRenderTarget2D.cpp`);
- PainterAndSim's own interface sizes the texture to the simulation first, and the second one then reverts it to the Blueprint's 1×1 placeholder (we read that order from the source; the result we measured every frame);
- the simulation keeps running, but the river's exported buffer is a cleared 1×1 texture.

Binding the texture alone, without sampling it, was enough to cause this.

### The code

`PainterSourceData`, ArrayReader branch: a Custom HLSL node between the tracked point's `UnitVelocity` and the `ArrayBrushVelocity` multiply. Its inputs:
- `UV`: `ArrayReader` `UnitVelocity`.
- `Water`: `CompositeGrid` Sample Previous Grid (Composite) at the pivot.
- `IsScreen`: `ArrayReader` `IsScreenPosition`.
- `InverseDeltaTime`, `GridCellSizeInUnitSpace`, `SimSpeedCoefficient` and `System.WorldGridExtents`, from the stage's parameter map.
- `NumTimeSteps`: `Engine.System.NumTimeSteps` from the stage's parameter map, the simulation ticks in this frame.
- `RelativeTracking` / `CameraFacing`: new `Ninja_Painter` module inputs. In `NinjaLiveCore` they are linked to `User.TrackVelocityRelativeToWater` and to `System.TransformMatrices2D.CameraFacing`, which is what `ArrayReader`'s `WorldToUnit` uses.

```hlsl
// Relative tracking: paint the tracked body's velocity relative to the water under it.
// UV = UnitVelocity (world mode: 0.0005 x cm/s), Water = CompositeGrid.Composite at the pivot (cells per tick).
// A tracked position moves once a frame: with N simulation ticks in a frame, UV is N x the body's velocity on the first tick
// and 0 on the others, so that tick subtracts N ticks of water and the others paint as they are (nothing).
float Steps = max((float)NumTimeSteps, 1.0);
float2 WaterUnit = 0.0005 * Water.xy * GridCellSizeInUnitSpace * SimSpeedCoefficient * WorldGridExtents * InverseDeltaTime;
float SpeedCmS = 2000.0 * length(UV.xy) / Steps;
float Gate = (RelativeTracking > 0.5 && !CameraFacing && !IsScreen) ? smoothstep(2.0, 10.0, SpeedCmS) : 0.0;
Velocity = UV;
Velocity.xy = lerp(UV.xy, UV.xy - Steps * WaterUnit, Gate);
```

Notes:
- **Units.** `Water.xy × GridCellSizeInUnitSpace × SimSpeedCoefficient` is the water's step in unit space per tick, exactly as the advection moves it. `× WorldGridExtents × InverseDeltaTime` makes it cm/s, and `× 0.0005` matches world-mode `UnitVelocity`. The body's and the water's velocity are in the same units whatever the brush does with them afterwards (`BrushVelocityPow`, the clamp, `VeloFromBrushMotion`).
- **Frame rate.** `NinjaLiveCore` has Fixed Tick Delta 1/60. Niagara substeps, and the GPU dispatcher runs every pending tick, so the simulation advects 60 times a second at any frame rate and `InverseDeltaTime` is always 60. A tracked position still updates once a frame. With N ticks in a frame, `UnitVelocity` is N × the body's velocity on the first tick and 0 on the others, which is how FluidNinja paints a tracked body below 60 fps today. So the node takes N ticks of water off on that first tick, gates on the body's own speed (`|UnitVelocity| / N`), and leaves the other ticks at 0: a frame paints N × (body − water). An earlier version took one tick of water off every tick and gated on `max(|UnitVelocity|, |Previous.UnitVelocity|)`. That cancels at one or two ticks a frame, but from three ticks a frame (below 30 fps, or a hitch) the gate closed on the later ticks and a drifting body still painted part of the current.
- **What stays as it is.**
  - The vertical velocity, which drives the brush's puncture (splashes).
  - Camera-facing and screen-position tracking, whose `UnitVelocity` has other units.
  - Bodies slower than 2 cm/s, faded in up to 10 cm/s. So a tracked thing that stands still (a water volume, a parked prop) paints no velocity of its own, rather than braking the current. With density off it still paints the brush's radial puncture push (see the cost of the density switch below).
  - Per NinjaLive, not per body. `TrackVelocityRelativeToWater` is a setting of the NinjaLive, so it changes every point that NinjaLive tracks, characters and skeletal-mesh bones included, not only floating bodies: each paints its velocity through the water. One slower than the current brakes it, as the crate below does; one standing still paints nothing of its own (the 2 cm/s gate above). We measured only floating bodies.
- **Switch off** (`TrackVelocityRelativeToWater` 0, the default): the node returns `UnitVelocity` unchanged, so painting is as today.

Three assets change: `Core/Functions/PainterSourceData`, `Core/Modules/Ninja_Painter` (the two inputs) and `NinjaLiveCore` (the user parameter and the two links).

## Measurements with both parts

The demo river test, 10 s after the river starts:

| | Fixed 60 fps | Fixed 45 fps | Fixed 30 fps | Unfixed (about 60 fps) |
|---|---|---|---|---|
| No interaction | 14.8 m, 1.9 m/s | 14.4 m, 1.7 m/s | 15.2 m, 1.8 m/s | 15.1 m, 1.8 m/s |
| Today's tracking, density off | 14.6 m, 2.4 m/s | 14.5 m, 2.5 m/s | 15.4 m, 2.4 m/s | 15.0 m, 2.3 m/s |
| **Relative tracking, density off** | **13.7 m, 1.9 m/s** | **13.4 m, 1.9 m/s** | **14.1 m, 1.8 m/s** | **14.2 m, 2.0 m/s** |

At every frame rate the plugin reads the current at FluidNinja's 60 simulation steps a second. 45 fps gives the uneven case (one or two simulation ticks a frame), 30 fps two ticks every frame.

Other checks:
- **With no interaction, the river is unchanged.** The plain river run (floaters present, `fnb.Buoyancy.Tracking 0` so none is handed over) with the switch on and density off: 14.8 m and 1.9 m/s at fixed 60 fps, the same as with neither setting.
- **Three ticks a frame.** With the brush's velocity tripled (`Array Brush Velocity` 3) to make the difference visible, and density off, at fixed 20 fps (measured before the flow-scale fix, with Flow Scale Multiplier 3, which gives the same flow scale at 20 fps): relative tracking left 1.6 m/s under the floaters, against 1.8 m/s with the earlier one-tick version and 4.5 m/s with today's world-velocity tracking (density off too). At 60 fps, where both versions do the same: 1.9 and 1.8 m/s (4.0 m/s with world velocity).
- **A body moved through the water still stirs it.** A boat shoved through the pool (fixed 60 fps, two runs each): with both parts the water under it moved 39 to 44 cm/s along the boat's motion, against −31 to −7 cm/s with no interaction and 19 to 27 cm/s with today's tracking. Its overall speed stayed near no interaction's: 1.11 to 1.12 m/s, against 1.04 to 1.14 m/s (1.47 to 1.52 m/s with today's tracking).
- **A body slower than the water brakes it.** A crate moving slower than the pool's current (about 0.5 m/s, with 1.6 m/s of water under it with no interaction) took the water under it down to 0.46 to 0.54 m/s with both parts, against 2.00 to 2.06 m/s with today's tracking (1.04 to 1.13 m/s with today's tracking and density off). Its velocity relative to the water points upstream, so the brush pushes the water towards the crate's speed, as drag would. The strength comes from the brush settings (`Array Brush Velocity`, `Velo From Brush Motion`), which we didn't tune.
- **The cost of the density switch.**
  - Tracked primitives and bones no longer leave dye or foam trails. A whitewater setup that relies on them would need its own source.
  - The switch also removes `Kill Brush Below This Velocity`'s fade. So every tracked body, parked ones included, paints the brush's radial puncture push at full strength each tick: `Brush Puncture` + 10 × |vertical velocity × `Brush Velocity Mult Z`| (0.1 + 10 |vz| in the demo). Its effect on parked bodies was not measured.

## How to test it

FluidNinja Buoyancy supports only the modified FluidNinja. Its floating bodies stir the water through FluidNinja's own tracking.
- **The per-body setting.** **Interaction** defaults to FluidNinja Tracking. FluidNinja Tracking and Wakes, or Wakes alone, can still be chosen explicitly.
- **What hand-over means.** A FluidNinja Tracking body gets Generate Overlap Events, plus the NinjaLive's tracking tag if it sets one, and it never writes wakes. Taken back, it loses the tag again and Generate Overlap Events returns to what it was.
  - **The tag** goes on the body's primitive (its Component Tags). With the overlap events the plugin turns on, that is enough, even when the body's object type is not in the NinjaLive's filter; the actor's Tags aren't needed. We checked this in the demo river, with **Track Actor Primitive Components With Tag** set on the river's NinjaLive actor (**Live Interaction**) and PhysicsBody taken out of its **Overlap Filter Inclusive Obj Type**. The floaters went 13.7 m (1.9 m/s), as when that NinjaLive tracks them by object type. With the tag cleared they went 14.6 m (1.7 m/s), untracked. Judge this one by the distance: the water speed is about the same whether they are tracked or not.
  - **Selection.** A body is handed over only if a NinjaLive whose interaction volume overlaps it will select it: its object type is in that NinjaLive's **Overlap Filter Inclusive Obj Type**, or that NinjaLive tracks by tag. Inside a volume that doesn't select it, the log reports an error naming its object type, and the test lists it as not tracked. With PhysicsBody taken out of the river NinjaLive's filter only (the pool's keeps it) and no tag, the river floaters were not handed over and went 14.8 m (1.9 m/s), the same as no interaction. A body outside every volume waits, with a log line, until it enters one.
- **When it happens.** Only while every NinjaLive in the level does both:
  - turns on the switch named in **Project Settings → FluidNinja Buoyancy → FluidNinja Tracking → Relative Tracking Switch** (default `TrackVelocityRelativeToWater`: a bool or number property on the NinjaLive actor or a component, or a float user parameter; on above 0.5);
  - sets the setting named in **Tracked Density Switch** (default `SelectivelyKillBrushDensityKeepVelocity`) to 1, 4 or 5.
- **No fallback.** Otherwise the bodies are not tracked and do not stir the water. The log reports an error naming what is missing. They are never handed to world-velocity tracking unless you force it.
- **The console variable.** `fnb.Buoyancy.Tracking`: 0 never hands bodies over, 1 hands them over when both are set (the default), 2 always does. 2 only forces the hand-over: FluidNinja still paints what the NinjaLive settings say, so it reproduces today's behaviour only with both settings at 0 (step 6).

To test:
1. Open the demo level (`FluidNinjaBuoyancy/Demo/Maps/FNB_DemoPool`).
2. With Play stopped, on each NinjaLive actor, select its **NinjaLiveComponent**:
   - set **Selectively Kill Brush Density Keep Velocity** to 1 (**Live Input Points → Brush Killers**). Leave the copy under **User Parameters → DEBUG ONLY** (`Painter.SelectivelyKillBrushDensityKeepVelocity`) as it is: NinjaLive sets it from this one at Play;
   - under **User Parameters**, set `TrackVelocityRelativeToWater` to 1.

   Save the level. Saved this way, with nothing set at runtime, both reach the simulation: the river test (step 5, run in a standalone game at fixed 60 fps, twice) gave 13.7 m and 2.0 m/s, with every NinjaLive reported as relative with no density.
3. Press Play and check `stat fps`: compare with the table's column nearest that frame rate. Below 30 fps (three or more simulation ticks a frame) the floaters go less far for about the same water speed (about 9 to 10 m at fixed 20 fps), so there compare step 5 with step 4's baseline from the same session. The first run after the core change may still be compiling shaders: run each test twice and use the second.
4. Type `fnb.Demo.RiverTest wakes` in the console. After about 15 s the log lists each floater's route, the water under it and how far it went: the baseline.
5. Type `fnb.Demo.RiverTest tracking`. The test's report should list each NinjaLive as painting tracked velocity relative to the water, with no density, and each floater's route as `FluidNinja tracking`. The numbers should land near no interaction (the tables above). The "handed to FluidNinja's own tracking (relative to the water, no density)" lines come earlier in the log: at Play start, and about a second after step 4's test ends.
6. For today's behaviour, stop Play, set both settings back to 0 on each NinjaLiveComponent, press Play, type `fnb.Buoyancy.Tracking 2` and run `fnb.Demo.RiverTest tracking` again: about 28 m and 4.8 m/s at 60 fps (not measured at other frame rates). Afterwards type `fnb.Buoyancy.Tracking 1`: the console variable keeps its value until the editor closes.

The distance varies by a metre or two from run to run. Judge mainly by the water speed under the floaters.
