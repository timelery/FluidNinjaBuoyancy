# Tags: reading the water and stirring it

Two different jobs in FluidNinja Buoyancy use tags, and they are easy to mix up. One controls how floating bodies **read** the water. The other controls how they **stir** it.

## 1. Surface tags: how bodies read the water

The plugin finds the water by actor tag: **Project Settings → Game → FluidNinja Buoyancy → Surface Tags** (defaults `SplineSource`, `SeaSurface` and the plugin's own `FluidNinjaBuoyancySurface`). From each tagged surface it takes two things:
- **Height.** A top-down capture of that surface, so bodies sit on FluidNinja's actual surface, with its waves and pressure bumps.
- **Current.** The surface's FluidNinja material carries the simulation's velocity buffer (`VelocityDensityBuffer`), and the bodies' drift comes from it.

A surface whose actor carries none of these tags is invisible to the plugin. Bodies over it fall back to the top of a water volume, if there is one, which is flat water with no FluidNinja current. Otherwise they sink. Tag the surface's actor (or add its tag to Surface Tags) and they ride FluidNinja's water. See [Surface tags](UserGuide.md#surface-tags) in the user guide.

## 2. Stirring the water: wakes, or FluidNinja's own tracking

This is the other direction: the body pushes the water. There are two routes, chosen per body with **Interaction**.

**Wakes**
- The plugin writes points along the hull's water line into FluidNinja's data channel (`NinjaDataChannel1_Global` by default).
- Works with stock FluidNinja and any NinjaLive that reads the data channel (`DataChannelPointReaderEnabled`). No overlap events, tags, object-type filter or core change.
- An approximation from outside: it reads the water back to the CPU, a frame or more old, and writes corrections along the water line.

**FluidNinja's own tracking** (branch `fluidninja-relative-tracking`)
- The plugin turns the body's Generate Overlap Events on and adds the NinjaLive's tracking tag, if it has one. FluidNinja then paints the body with its own brush.
- Needs the modified FluidNinja core with the relative switch (`TrackVelocityRelativeToWater`) on, and Selectively Kill Brush Density Keep Velocity at 1, 4 or 5, on **every** NinjaLive in the level. Without that, tracked floaters make the current speed itself up: in the demo river they went 27.4 m at 4.8 m/s, against 14.8 m at 1.9 m/s untracked.
- With both set, the river behaves like untracked water (13.7 m, 1.9 m/s), and the core change costs nothing measurable.
- The switch is per NinjaLive, so it applies to everything that NinjaLive tracks, characters included when its filter has Pawn.
- The test bench found one cost: a light body parked in a current rode up to 16 cm higher with relative tracking and density off together.

**Within tracking: by object type or by tag**
- **By object type.** The NinjaLive picks up every body of a type inside its interaction volume (**Overlap Filter Inclusive Obj Type**; PhysicsBody is in the default). Any physics object with overlap events on gets tracked, intended or not.
- **By tag.** Set **Track Actor Primitive Components With Tag** on the NinjaLive and take PhysicsBody out of its filter. Only bodies that the plugin hands over carry the tag, on their primitive's Component Tags, so tracking becomes opt-in. The actor's Tags aren't needed.

## The combinations

| Setup | What you get | When to use it |
|---|---|---|
| Surface tags + wakes | Bodies ride FluidNinja's water and stir it through the data channel. Stock FluidNinja, no core dependency. | The safe default (`main`). |
| Surface tags + tracking by tag | Bodies ride the water, and FluidNinja paints them with its own brush. Needs the modified core and every NinjaLive set up. | The relative-tracking route, with tracking opt-in per body. |
| Surface tags + tracking by object type | As above, but every body of that object type in the volume is tracked. | Only where every such body should be tracked. |
| Wakes and tracking on one body (Interaction "FluidNinja Tracking and Wakes") | FluidNinja's brush at the pivot plus the water-line wakes. | Untested. Only if one route alone is too weak. |

Surface tags are needed in every case: without them, bodies don't ride FluidNinja's water at all. Wakes and tracking are the two ways to stir it.

For the tracking setup and its measurements, see [FluidNinja's own tracking, relative to the water](UserGuide.md#fluidninjas-own-tracking-relative-to-the-water) and [the proposal](RelativeTrackingProposal.md).
