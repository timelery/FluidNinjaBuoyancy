# Proposal: FluidNinja tracking relative to the water

This note is for FluidNinja LIVE's developers. It explains why floating bodies stay off FluidNinja's own tracking today, proposes a small change to that tracking, and describes how to test the change with this branch of FluidNinja Buoyancy (`fluidninja-relative-tracking`).

## The problem

FluidNinja tracks a primitive that generates overlap events and that its NinjaLive actor selects, by object type (**Overlap Filter Inclusive Obj Type**) or by **Track Actor Primitive Components With Tag**. It paints the primitive's pivot every frame with its **world velocity**:
1. The `ArrayReader` emitter takes the pivot's velocity from the position change: the `ParticleVelocity` module.
2. `WorldToUnit` turns it into `UnitVelocity`.
3. `PainterSourceData` paints `ArrayBrushVelocity × UnitVelocity`.
4. The composite adds the brush on top of the field.

That suits a character, whose world velocity is its push. A floating body carried by a current moves *with* the water. Its world velocity is the current, so every frame it paints the current back into the water, and the current speeds itself up.

Measured in this plugin's demo river: eight floaters, `DirectVelocity (0.5, 0)`, 10 s after the river starts.

| Setup | Water under the floaters | Distance in 10 s |
|---|---|---|
| No interaction | 1.3 to 2.4 m/s, mean 1.7 | 9 to 16 m, mean 11.8 |
| FluidNinja Buoyancy's wakes (relative by design) | 1.1 to 2.8 m/s, mean 2.0 | 11 to 21 m, mean 14.9 |
| FluidNinja's tracking (overlap events on the floaters) | 2.2 to 6.0 m/s, mean 5.1 | 20 to 37 m, mean 28 |

6 m/s is this plugin's read limit, so the true speed is at least that. Two runs with tracking agreed. Shoving a boat through still pool water with tracking on gave about 1.9 m/s of water under a boat that never passed 1 m/s.

## The proposed change

Paint a tracked pivot's velocity **relative to the water at the pivot**: `v_painted = v_pivot − v_water(pivot)`. A body drifting with the current then paints nothing. A body moved through the water (shoved, dropped, rowed) paints its motion through it. The paint fades by itself as the water catches up with the body, so it never drives the water past the body.

A natural place for the change is the `ArrayReader` emitter's Particle Update, between `ParticleVelocity` and `WorldToUnit`:
1. Sample the simulation's velocity at the pivot. `NinjaSampleSimBuffers` already does this from `User.RT_VelocityDensity`, with `TraceMeshPosDouble = System.SimPosDouble` and `TraceMeshSize = System.WorldGridExtents / 100`. Sampling the emitter's own grid directly would avoid the one-frame lag.
2. `Particles.Velocity = Strength × (Particles.Velocity − float3(RG × SimSpeed × (WorldGridExtents.x / ResolutionX) × steps per second, 0))` for x and y, with Strength around 0.35. That is the buffer velocity in cm/s, the same conversion as the advection's `GridCellSizeInUnitSpace × SimSpeedCoefficient`.
3. Leave the vertical velocity alone. It drives the brush's puncture, which is how a dropped or dunked body already makes a splash.
4. Keep the result finite: clamp the steps per second (the first frame can have a zero delta time) and the sampled value. `0 × ∞` is NaN, and NaN painted into the painter grid stays there.

Two smaller suggestions:
- **Where the switch lives.** A property on the NinjaLive actor or its NinjaLive component, for example a bool `TrackVelocityRelativeToWater`, is the safer choice. In our dev copy, every edit to `NinjaLiveCore` that added something stopped the demo river's simulation. That included a system with only a new user parameter and the added modules disabled. We could not isolate the cause; see below.
- **Per tracked point.** `TrackedParams.w` is read but unused today. Setting it to 1 for physics-simulating primitives would make the change per tracked point, leaving characters, bones and the mouse brush as they are.

A strength factor below 1 is needed: see the measurements below. This plugin's wakes, too, give the water at the hull only 35% of the hull's speed through it, because 100% makes a shallow hull act like a wall through the whole simulated depth.

## Evidence: the change reproduced from outside

We could not prototype the change inside `NinjaLiveCore` reliably, so it was reproduced from outside.
- **Why not inside.** Editing the asset through UE 5.8's Niagara external-edit API changed the demo river in our dev copy:
  - every edit that added something (modules in `ArrayReader`, enabled or disabled, or a user parameter) left the river's simulation still, while the pool kept flowing;
  - a read-only open and re-save through the API slowed the river by about 30% (one run);
  - a plain re-save left it unchanged.
- **How it was reproduced instead.** FluidNinja draws data-channel points with the same stroke code as tracked pivots, so each floater was written as one data-channel point at its pivot:
  - brush size: the tracker's own `BrushScale` (0.0875 × bounds radius), converted to the data channel's `0.05 × ParticleBrushSize`;
  - velocity: scaled to what the tracker would paint, `(0.05, 0.05, 0.1) × ArrayBrushVelocity / ParticleBrushVelocity`;
  - painted with the world velocity ("stock") or with the velocity relative to the water ("relative").

| Reproduced tracking | Water under the floaters | Distance in 10 s |
|---|---|---|
| Stock (world velocity) | mean 4.5 m/s, up to the 6 m/s limit | mean 31 m |
| Relative, full strength | mean 3.0 m/s (1.0 to 5.5) | mean 12.8 m |
| Relative, 1.5 × the water subtracted | mean 4.1 m/s, up to the limit | mean 34 m |
| **Relative, strength 0.35** | **mean 1.9 m/s (1.1 to 2.4)** | **mean 15.4 m** |

- **Stock** matches the real tracking above (5.1 m/s, 28 m), which shows the reproduction is faithful.
- **Relative at strength 0.35** lands exactly where this plugin's wakes do (2.0 m/s, 14.9 m): no runaway, and no local peaks.
- **A strength below 1 matters.** The water under the pivot is changed by the pivot's own paint, so the subtraction is a feedback loop:
  - at full strength it is marginal (local peaks up to 5.5 m/s under three floaters, though the floaters travel normally);
  - when over-corrected (1.5 × the water) it oscillates into a runaway of its own.
- **The units are approximate here.** The reproduction also converts FluidNinja's buffer to cm/s with this plugin's formula, which isn't calibrated in this river. Inside FluidNinja the subtraction would be in its own units.

So the recommendation is `v_painted = Strength × (v_pivot − v_water)` with Strength around 0.35, rather than the full difference.

## How to test it with this branch

The branch (`fluidninja-relative-tracking`) can hand bodies to FluidNinja's tracking, but only when FluidNinja says it tracks relative to the water:
- **The per-body setting.** **Interaction** has three values: Wakes (the default), FluidNinja Tracking, and FluidNinja Tracking and Wakes.
- **What hand-over means.** A body that asks for tracking gets Generate Overlap Events, plus the NinjaLive's tracking tag if it sets one, and it writes no wakes.
- **When it happens.** Only while every NinjaLive in the level turns on the switch named in **Project Settings → FluidNinja Buoyancy → FluidNinja Tracking → Relative Tracking Switch**. The default name is `TrackVelocityRelativeToWater`. The switch can be a bool or number property on the NinjaLive actor or a component, or a float user parameter; it is on above 0.5.
- **Otherwise** the body makes wakes, so the branch is harmless on today's FluidNinja.
- **The console variable.** `fnb.Buoyancy.Tracking`: 0 never hands bodies over, 1 hands them over when the switch is on (the default), 2 always does, to reproduce today's behaviour.

To test:
1. Open the demo level (`FluidNinjaBuoyancy/Demo/Maps/FNB_DemoPool`) and press Play.
2. Type `fnb.Demo.RiverTest wakes` in the console. After about 15 s the log lists each floater's route, the water under it and how far it went, with the reference numbers. That is the baseline.
3. Type `fnb.Buoyancy.Tracking 2`, then `fnb.Demo.RiverTest tracking`. Today's FluidNinja paints world velocity: expect about 5 m/s and 28 m.
4. With your change and the switch on, type `fnb.Buoyancy.Tracking 1`, then `fnb.Demo.RiverTest tracking`. The log should say each floater was handed to FluidNinja's tracking relative to the water. The numbers should land near the wakes: about 2 m/s and 15 m.
