# Proposal: FluidNinja tracking relative to the water

This note is for FluidNinja LIVE's developers. It explains why floating bodies stay off FluidNinja's own tracking today, what makes a tracked floater speed a current up, a change to `NinjaLiveCore` that we prototyped and measured, and how to test it with this branch of FluidNinja Buoyancy (`fluidninja-relative-tracking`).

## The problem

FluidNinja tracks a primitive that generates overlap events and that its NinjaLive actor selects, by object type (**Overlap Filter Inclusive Obj Type**) or by **Track Actor Primitive Components With Tag**. Every tick it paints the primitive's pivot into the simulation:
1. The `ArrayReader` emitter takes the pivot's velocity from its position change (`ParticleVelocity`). `WorldToUnit` turns it into `UnitVelocity`: in world mode, `0.0005 × v` (cm/s) rotated into the simulation.
2. `PainterSourceData`, in PainterAndSim's TrackPoint Painter stage, hands `ArrayBrushVelocity × UnitVelocity` and the pivot's position to `Ninja_Painter`, which paints a brush stroke of **velocity and density**.
3. The composite adds the brush to the field. It also scales the field's velocity by its density (`0.5 × density`), so painted density is fluid that carries the current.

That suits a character, whose world velocity is its push. A floating body carried by a current moves *with* the water, so it paints the current back into the water every tick, with density that keeps it going. The current then speeds itself up.

Measured with this plugin's demo river test (`fnb.Demo.RiverTest`: eight floaters, 10 s after the river starts, fixed 60 fps):

| Setup | Distance in 10 s | Water under the floaters |
|---|---|---|
| No interaction | 14.8 m | 1.9 m/s |
| FluidNinja's tracking today | 27.6 m | 4.8 m/s, up to 6 m/s (this plugin's read limit) |

## What drives the runaway

We isolated the parts of the brush by switching them off one at a time, at runtime, in the same test:

| Tracked floaters paint | Distance in 10 s | Water under the floaters |
|---|---|---|
| World velocity and density (today) | 27.6 m | 4.8 m/s |
| World velocity, no density | 14.6 m | 2.4 m/s |
| Velocity relative to the water, with density | 20.4 to 22.9 m | 3.6 to 4.3 m/s |
| **Velocity relative to the water, no density** | **13.7 m** | **2.0 m/s** |
| Nothing (no interaction) | 14.8 m | 1.9 m/s |

- **Most of the runaway is the brush's density.** FluidNinja already has the switch for it: **Selectively Kill Brush Density Keep Velocity** on the NinjaLive component (/LiveInputPoints /BrushKillers). Set to 1 (ARRAY), tracked primitives and bones still stir the water but paint no density. 4 (ARRAY and CHAOS) and 5 (ALL) do the same for them; 5 also takes the density out of data-channel points, which include this plugin's wakes.
- **The rest is the world velocity.** With density off, today's tracking still leaves about 25% more water speed under the floaters than no interaction. Velocity relative to the water removes it.
- **Relative velocity alone isn't enough.** The brush paints full density whenever its velocity is above `Kill Brush Below This Velocity` (0.01, about 20 cm/s). A body carried by a current still moves through it faster than that much of the time, probably because the water around it keeps accelerating. A gain on the relative velocity (0.7 to 1.5) didn't change the result.
- **Not the cause.** The brush's radial "puncture" push (`Brush Puncture`, `Brush Velocity Mult Z`) was switched off too: no change.

## The proposed change

Two parts:
1. **Density.** Set **Selectively Kill Brush Density Keep Velocity** to 1 on every NinjaLive that tracks floating bodies. This is an existing setting. FluidNinja reads it when the simulation starts (emitter spawn), so it belongs in the level.
2. **Relative velocity.** A new user parameter turns it on: `TrackVelocityRelativeToWater` (float, default 0 = today's behaviour, on above 0.5). When it is on, `PainterSourceData`'s ArrayReader branch paints a tracked pivot's velocity relative to the water at the pivot. A body drifting with the current then paints nothing; a body moved through the water (shoved, dropped, rowed) paints its motion through it.

### Where it is done

Inside PainterAndSim, in `PainterSourceData`, where the tracked point's velocity is read (`Core/Functions/PainterSourceData`). The water is PainterAndSim's own `CompositeGrid.Composite` from the previous tick, sampled at the pivot's `UnitPosition + System.WorldOffsetDelta` (as the composite samples the previous grid). That is the velocity the advection uses, in cells per tick. No new data interface or render-target binding is needed.

Don't read the water through `User.RT_VelocityDensity` from another emitter. We tried that first, in `ArrayReader`, and it stops the simulation:
- a second Render Target 2D data interface with **Inherit User Parameter Settings** on, bound to `User.RT_VelocityDensity`, copies the texture's settings in `PerInstanceTick`;
- in `PerInstanceTickPostSimulate` it puts them back whenever the texture differs (`InitAutoFormat` + `UpdateResourceImmediate`, `NiagaraDataInterfaceRenderTarget2D.cpp`);
- PainterAndSim's own interface sizes the texture to the simulation first, so every frame the second one reverts it to the Blueprint's 1×1 placeholder;
- the simulation keeps running, but nothing can read it: the river's exported buffer is a cleared 1×1 texture.

Binding the texture alone, without sampling it, was enough to cause this.

### The code

`PainterSourceData`, ArrayReader branch: a Custom HLSL node between the tracked point's `UnitVelocity` and the `ArrayBrushVelocity` multiply. Its inputs:
- `UV` / `PrevUV`: `ArrayReader` `UnitVelocity` and `Previous.UnitVelocity`.
- `Water`: `CompositeGrid` Sample Previous Grid (Composite) at the pivot.
- `IsScreen`: `ArrayReader` `IsScreenPosition`.
- `InverseDeltaTime`, `GridCellSizeInUnitSpace`, `SimSpeedCoefficient` and `System.WorldGridExtents`, from the stage's parameter map.
- `RelativeTracking` / `CameraFacing`: new `Ninja_Painter` module inputs. In `NinjaLiveCore` they are linked to `User.TrackVelocityRelativeToWater` and to `System.TransformMatrices2D.CameraFacing`, which is what `ArrayReader`'s `WorldToUnit` uses.

```hlsl
// Relative tracking: paint the tracked body's velocity relative to the water under it.
// UV = UnitVelocity, PrevUV = Previous.UnitVelocity (world mode: 0.0005 x cm/s), Water = CompositeGrid.Composite at the pivot.
float2 WaterUnit = 0.0005 * Water.xy * GridCellSizeInUnitSpace * SimSpeedCoefficient * WorldGridExtents * InverseDeltaTime;
float SpeedCmS = 2000.0 * max(length(UV.xy), length(PrevUV.xy));
float Gate = (RelativeTracking > 0.5 && !CameraFacing && !IsScreen) ? smoothstep(2.0, 10.0, SpeedCmS) : 0.0;
Velocity = UV;
Velocity.xy = lerp(UV.xy, UV.xy - WaterUnit, Gate);
```

Notes:
- **Units.** `Water.xy × GridCellSizeInUnitSpace × SimSpeedCoefficient` is the water's step in unit space per tick, exactly as the advection moves it. `× WorldGridExtents × InverseDeltaTime` makes it cm/s, and `× 0.0005` matches world-mode `UnitVelocity`. The body's and the water's velocity are in the same units whatever the brush does with them afterwards (`BrushVelocityPow`, the clamp, `VeloFromBrushMotion`).
- **Frame rate.** `NinjaLiveCore` has Fixed Tick Delta 1/60. Niagara substeps, and the GPU dispatcher runs every pending tick, so the simulation advects 60 times a second at any frame rate and `InverseDeltaTime` is always 60. Below 60 fps a tracked position updates once a frame, so at 30 fps `UnitVelocity` alternates between 2v and 0. The relative term then swings by ± the water speed between ticks. Measured, it cancels on average (table below).
- **What stays as it is.**
  - The vertical velocity, which drives the brush's puncture (splashes).
  - Camera-facing and screen-position tracking, whose `UnitVelocity` has other units.
  - Bodies slower than 2 cm/s, faded in up to 10 cm/s. So a tracked thing that stands still (a water volume, a parked prop) still paints nothing, rather than braking the current.
- **Switch off** (`TrackVelocityRelativeToWater` 0, the default): the node returns `UnitVelocity` unchanged, so painting is as today.

Three assets change: `Core/Functions/PainterSourceData`, `Core/Modules/Ninja_Painter` (the two inputs) and `NinjaLiveCore` (the user parameter and the two links).

## Measurements with both parts

The demo river test, 10 s after the river starts:

| | Fixed 60 fps | Fixed 45 fps * | Fixed 30 fps * | Unfixed (about 60 fps) |
|---|---|---|---|---|
| No interaction | 14.8 m, 1.9 m/s | 14.1 m, 1.7 m/s | 15.4 m, 1.7 m/s | 15.1 m, 1.9 m/s |
| Today's tracking, density off | 14.6 m, 2.4 m/s | 14.6 m, 2.4 m/s | 15.5 m, 2.4 m/s | 14.9 m, 2.4 m/s |
| **Relative tracking, density off** | **13.7 m, 2.0 m/s** | **13.5 m, 1.9 m/s** | **14.2 m, 1.8 m/s** | **14.2 m, 2.0 m/s** |

\* This plugin's current below 60 fps was corrected for these runs. Its flow scale assumed `min(SamplingFPS, frame rate)` simulation steps a second, so at 30 fps it read and applied half the real current. 45 fps gives the uneven case: one or two simulation ticks a frame.

Other checks:
- **The river with no floaters is unchanged** with density off: 7.7 m at fixed 30 fps, 14.8 m at fixed 60 fps, as before.
- **A body moved through the water still stirs it.** A boat shoved through the pool: the water under it was 1.43 to 1.45 m/s with density off, against 1.47 to 1.52 m/s with density on and 1.04 to 1.14 m/s with no interaction (two runs each).
- **The cost of the density switch.** Tracked primitives and bones no longer leave dye or foam trails. A whitewater setup that relies on them would need its own source.

## How to test it with this branch

The branch (`fluidninja-relative-tracking`) is the version of FluidNinja Buoyancy for the modified FluidNinja. Its floating bodies stir the water through FluidNinja's own tracking, and `main` keeps the wakes for FluidNinja as it is today.
- **The per-body setting.** **Interaction** defaults to FluidNinja Tracking on this branch. FluidNinja Tracking and Wakes, or Wakes alone, can still be chosen explicitly.
- **What hand-over means.** A FluidNinja Tracking body gets Generate Overlap Events, plus the NinjaLive's tracking tag if it sets one, and it never writes wakes.
- **When it happens.** Only while every NinjaLive in the level does both:
  - turns on the switch named in **Project Settings → FluidNinja Buoyancy → FluidNinja Tracking → Relative Tracking Switch** (default `TrackVelocityRelativeToWater`: a bool or number property on the NinjaLive actor or a component, or a float user parameter; on above 0.5);
  - sets the setting named in **Tracked Density Switch** (default `SelectivelyKillBrushDensityKeepVelocity`) to 1, 4 or 5.
- **No fallback.** Otherwise the bodies are not tracked and do not stir the water. The log reports an error naming what is missing. They are never handed to world-velocity tracking unless you force it.
- **The console variable.** `fnb.Buoyancy.Tracking`: 0 never hands bodies over, 1 hands them over when both are set (the default), 2 always does, to reproduce today's behaviour.

To test:
1. Open the demo level (`FluidNinjaBuoyancy/Demo/Maps/FNB_DemoPool`).
2. On each NinjaLive actor, select its **NinjaLiveComponent**:
   - set **Selectively Kill Brush Density Keep Velocity** to 1;
   - under **User Parameters**, set `TrackVelocityRelativeToWater` to 1.
3. Press Play.
4. Type `fnb.Demo.RiverTest wakes` in the console. After about 15 s the log lists each floater's route, the water under it and how far it went: the baseline.
5. Type `fnb.Demo.RiverTest tracking`. The log should say each floater was handed to FluidNinja's tracking (relative to the water, no density), and the numbers should land near no interaction (the table above).
6. For today's behaviour, type `fnb.Buoyancy.Tracking 2`, set both settings back to 0, restart Play and run the test again: about 28 m and 4.8 m/s.

The distance varies by a metre or two from run to run. Judge mainly by the water speed under the floaters. The numbers above are at 60 fps; this plugin's flow scale reads too low below 60 fps until its fix lands.
