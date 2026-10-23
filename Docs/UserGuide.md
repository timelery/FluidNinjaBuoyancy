# FluidNinja Buoyancy: user guide

This guide covers:
1. [How it works](#how-it-works)
2. [Setting it up on your FluidNinja water](#setting-it-up-on-your-fluidninja-water)
3. [The FluidNinja Buoyancy component](#the-fluidninja-buoyancy-component)
4. [Project settings](#project-settings)
5. [Hull sounds](#hull-sounds)
6. [Wet line](#wet-line)
7. [Spray](#spray)
8. [Wakes](#wakes)
9. [Waves and swimmers](#waves-and-swimmers)
10. [Physics and frame rate](#physics-and-frame-rate)
11. [Console commands and the test bench](#console-commands-and-the-test-bench)
12. [The demo level](#the-demo-level)
13. [Performance](#performance)
14. [Quality](#quality)
15. [Troubleshooting](#troubleshooting)
16. [Known limitations](#known-limitations)

The setup check (**Tools → FluidNinja Buoyancy: Check Setup**) is under [Setting it up](#check-setup).

---

## How it works

FluidNinja LIVE simulates water on the GPU and draws it with a material: a surface mesh displaced by the simulation's pressure, plus tiled waves advected by its velocity. FluidNinja Buoyancy floats bodies on that drawn surface.

| Piece | What it does |
|---|---|
| **Surface Mirror** | An orthographic depth capture, looking straight down, of only the water surface's own primitives: the actors carrying a [surface tag](#surface-tags). It draws only the surface pieces under some floating body. Its near plane sits just above the highest body, so water passing higher up (a river over a pool) is left out. A waterfall's face (steeper than 45°) is never drawn. |
| **Flow sources** | For each FluidNinja simulation that a tagged surface shows, the plugin finds:<br>• its velocity buffer (the surface material's `VelocityDensityBuffer` texture);<br>• its units (the NinjaLive component's `SimSpeed`, `SamplingFPS` and resolution, and the fixed tick of its `NinjaLiveCore` Niagara system);<br>• where it lies.<br>Location comes from the surface material's `TraceMeshPos` / `TraceMeshSize` when the simulation sets them there (a pool), otherwise from FluidNinja's material parameter collection (a simulation that follows the player, such as the river template). |
| **GPU gather** | One compute pass resamples the capture and each flow source's velocity buffer onto a small grid around every body. One asynchronous readback brings all of them back. Heights are carried forward by their rate of change over the few frames the readback takes. |
| **Physics callback** | A Chaos sim callback runs on every physics step: each frame, each substep, or each async step. It computes the water's forces on each body at its current pose and applies them. |
| **Force model** | After Kerner, *Water interaction model for boats in video games* (2015). Every hull triangle is clipped at the water line it actually meets. Each wet piece gets:<br>• hydrostatic pressure, applied at its centre of pressure;<br>• pressure drag, or suction where it pulls away;<br>• skin friction (ITTC 1957 line);<br>• slamming while its wetted area grows.<br>The dry part gets air drag from the wind. Voxel and sphere hulls use the same terms per point. A sphere's drag acts on the part of its section that is under water (a circular segment), and the skin friction on its wetted cap also slows its spin, so a ball rolls along the water rather than spinning in place. |
| **Far field** | A body outside the capture uses the top of a **water volume** under it: a Physics Volume with *Water Volume* ticked. If there is none, it is held asleep where it is until its water can be read again. |

Everything FluidNinja-specific is read by name: actor tags, material parameter names, NinjaLive component properties and the data channel asset. The plugin has no compile-time dependency on FluidNinja.

---

## Setting it up on your FluidNinja water

### Surface tags

The water surface actors must carry one of the tags in **Project Settings → Game → FluidNinja Buoyancy → Surface Tags**. The defaults are:

| Tag | Used by |
|---|---|
| `SplineSource` | FluidNinja's river spline pieces (the `SplineGenerator` utility and the river presets tag them). |
| `SeaSurface` | FluidNinja's sea and area surfaces. |
| `FluidNinjaBuoyancySurface` | This plugin's own tag. Put it on any other surface: a pool plane, a lake mesh, a Niagara water sheet. |

From each tagged actor the capture uses:
- static and spline-mesh pieces that are roughly horizontal;
- Niagara components, when the actor is a Niagara actor.

> **Tip:** NinjaLive's *Apply 2nd Out Mat To Actors With Tag* can use `FluidNinjaBuoyancySurface` too. The demo does this: one tag lets FluidNinja find the surface to draw on, and lets the plugin find the surface to float on.

### Where the simulation is

There are two ways a simulation can say where it lies:

| Setup | What the plugin reads | Example |
|---|---|---|
| One simulation that follows the player | FluidNinja's material parameter collection (`MP_NinjaMaterialParamCollection01`): `TraceMeshPos`, `TraceMeshSize`. | The river template. |
| A simulation that drives its own surface material | The surface material instance's `TraceMeshPos`, `TraceMeshSize` and `TraceMeshPosLWCOffset`. | A pool (`NP_Water_Sparse_Pool1`), the demo. |

Both work at once. Each body reads the current of the simulation whose surface lies under it.

The capture square (**Capture Width**, 80 m by default) is centred on the simulation that follows the player if there is one. Otherwise it is centred on the middle of all the floating bodies, so a pool and a river beside it share one square. Bodies outside the square use the [far field](#far-field-water-volumes).

### The current

The current is read from the texture parameter named in **Velocity Buffer Parameter** (`VelocityDensityBuffer`) on the surface's material. Its speed is computed as: buffer value × `SimSpeed` × cell size × simulation steps per second × **Flow Scale Multiplier**.

The simulation steps per second come from FluidNinja's `NinjaLiveCore` Niagara system. It has a fixed tick of 1/60 s, so Niagara steps it 60 times a second at any frame rate (twice a frame at 30 fps), and the current is the same at every frame rate. If `NinjaLiveCore` has no fixed tick, it steps once a frame, and the plugin uses `SamplingFPS`, at most the frame rate. The panel's status shows which one each simulation uses.

To check that the current matches what you see:
1. In Play, run `fnb.Buoyancy.CalibrateFlow`.
2. It measures how fast FluidNinja's dye actually moves and reports the multiplier that would match.
3. `fnb.Buoyancy.CalibrateFlow apply` sets that multiplier when the match is good. In the editor's Play, it also saves it to `DefaultGame.ini`.

Run it on a straight stretch of flow. Turbulent plunge pools re-create their foam and fool the measurement.

### Far field: water volumes

Outside the capture, a body floats on the top of the Physics Volume (with **Water Volume** ticked) under it. The plugin learns, while the body is in the capture, how far the drawn water sits from the volume's top. It then uses that offset out of the capture.

If there is no volume, a body that was afloat is held asleep until its water can be read again, rather than sinking.

### Interacting with FluidNinja

Floating bodies stir the simulation through FluidNinja's own tracking, which the modified core paints relative to the water (the default; see [FluidNinja's own tracking, relative to the water](#fluidninjas-own-tracking-relative-to-the-water)), or through the plugin's **wakes**, chosen per body with **Interaction**. The table compares the wakes with FluidNinja's tracking as it ships, which paints world velocity. That is why the plugin hands a body to FluidNinja's tracking only while every NinjaLive tracks relative to the water.

| | This plugin's wakes | FluidNinja's tracking as it ships (world velocity) |
|---|---|---|
| **Switch it on** | **Wake** on the component, **Wakes** in the project settings, and **DataChannelPointReaderEnabled** on in the NinjaLive component (FluidNinja's presets turn it on). No tags or overlap events are needed. | **Generate Overlap Events** on the primitive, and the NinjaLive actor selecting it: by object type (**Overlap Filter Inclusive Obj Type**, whose default includes PhysicsBody) or by **Track Actor Primitive Components With Tag**. A tag alone, without overlap events, isn't tracked. |
| **What reaches FluidNinja** | Up to 16 points along the hull's water line, written to FluidNinja's data channel (**Wake Data Channel**, `NinjaDataChannel1_Global` by default). A point is written where the hull moves into the water: sideways through it, or sinking into it, which sends the displaced water outward (a drop makes a ring). Each point carries only what the water there still lacks of the hull's speed *through the water*, so a body drifting with the current writes nothing. Data channel points add velocity only, no dye. | The body's pivot point, painted every frame with the body's *world* velocity and sized from its bounds. |
| **In a current** | Bodies drift with it and leave it as it is. | A floater moves with the current, so its world velocity *is* the current, and it paints the current back into the water every frame. The current then speeds itself up. |

Measured in the demo river, with eight floaters and the river switched on:

| | Overlap events off (wakes only) | Overlap events on |
|---|---|---|
| Water under the floaters after 10 s | 1.0 to 2.8 m/s | 1.7 to at least 6 m/s (6 m/s is the plugin's read limit, **Max Flow Speed**) |
| Distance travelled in 10 s | 11 to 21 m | 17 to 37 m |

Two runs with overlap events on agreed, and the run with them off matched version 1.0.0.

Points to know:
- **A StaticMeshActor has Generate Overlap Events off by default.** That is right for a floating body: the plugin turns it on itself when it hands the body to FluidNinja's tracking. If one needs overlap events for gameplay, leave PhysicsBody out of the NinjaLive actor's object-type filter.
- **FluidNinja's tracking is the right tool for characters,** and for anything that moves under its own power: its world velocity is its push.
- **A body with Wake off logs it.** At Begin Play it logs "Wake is off, so it floats but leaves no ripples".
- **A body that stirs the water rides the current around it.** With Wake on, or Generate Overlap Events on, a body takes its current from a ring of water outside its reach (its bounds plus 50 cm), not from the water it has just pushed. Otherwise it would be carried along by its own wake.
  - The plugin goes by the flag. It can't tell whether a simulation's filter actually picks the body up. A body with overlap events on for gameplay reasons also rides the ring current, which differs from the current under it only in eddies smaller than the body.

**Changed in 1.0.1.** In 1.0.0:
- the wakes were horizontal only, so a body lifted out of the water or dropped into it wrote nothing;
- a single-pontoon sphere (the beach ball) had no water line at all. Its one piece faced away from the body's own centre, which gives no direction, so it had no wake, wet line or bow spray;
- the demo's balls, ring buoys and air mattress have Wake off, so they don't stir the water. That is still so in 1.0.1, by choice; see [Wakes](#wakes).

1.0.1 makes these changes:
- the wakes include sinking;
- a sphere body's water line runs round each sphere in eight pieces;
- a body FluidNinja tracks itself rides the ring current;
- Begin Play logs a body with Wake off.

### FluidNinja's own tracking, relative to the water

FluidNinja's tracking would suit floating bodies if it painted a pivot's velocity relative to the water at the pivot rather than its world velocity, and no density. A body drifting with the current would then paint no velocity of its own. FluidNinja already has the density half: **Selectively Kill Brush Density Keep Velocity** on the NinjaLive component, set to 1 (ARRAY), lets tracked primitives stir the water without painting density. FluidNinja weights each tick's push on the water (its river driver and its brushes) by the local density, so painted density makes the river push harder where floaters go. In the demo river, switching it on removes most of what tracked floaters add to the current (the switch also weakens the velocity the brush paints, so the two are not fully separated). The velocity half is not in FluidNinja yet. [Docs/RelativeTrackingProposal.md](RelativeTrackingProposal.md) proposes it (a `NinjaLiveCore` change we prototyped), with measurements, and explains how to test it.

**The plugin requires that modified FluidNinja (the waves zip includes it).** There is no fallback to the plugin's wakes.
- **Interaction**, on the component, chooses how the body stirs the simulation:
  - **FluidNinja Tracking** (the default);
  - **FluidNinja Tracking and Wakes**;
  - **Wakes**, chosen explicitly.
- **What hand-over does.** A body is handed to the tracking by turning Generate Overlap Events on and adding the NinjaLive's tracking tag, if it has one. A FluidNinja Tracking body never writes wakes. Taken back, it loses the tag again and Generate Overlap Events returns to what it was.
  - **The tag** goes on the body's primitive (its Component Tags). With the overlap events the plugin turns on, that is enough, even when the body's object type is not in the NinjaLive's filter; the actor's Tags aren't needed. Checked in the demo river: a NinjaLive with **Track Actor Primitive Components With Tag** set (on the actor, under **Live Interaction**) and PhysicsBody taken out of its **Overlap Filter Inclusive Obj Type** still tracked the floaters (13.7 m in the river test, against 14.6 m untracked with the tag cleared).
  - **Selection.** A body is handed over only if a NinjaLive whose **InteractionVolume** overlaps it will select it: its object type is in that NinjaLive's **Overlap Filter Inclusive Obj Type**, or that NinjaLive tracks by tag (project settings Object Type Filter Property and Interaction Volume Component). Inside a volume that doesn't select it, the log reports an error naming its object type, and `fnb.Demo.RiverTest` lists it as NOT TRACKED. A body outside every volume isn't handed over until it enters one (a log line says so); once a second the plugin checks again. Bounds are compared as axis-aligned boxes.
- **When it happens.** Only while every NinjaLive in the level does both:
  - turns on its **Relative Tracking Switch** (project setting below), which is what the modified FluidNinja provides: the user parameter `TrackVelocityRelativeToWater` on its NinjaLiveComponent (**User Parameters**), set to 1;
  - sets its **Tracked Density Switch** (project setting below), FluidNinja's **Selectively Kill Brush Density Keep Velocity** on its NinjaLiveComponent (**Live Input Points → Brush Killers**), to 1, 4 or 5. 5 (ALL) also takes the density out of this plugin's wakes.

  Otherwise a FluidNinja Tracking body is not tracked and does not stir the water, and the log reports an error naming everything that is missing (again whenever that changes). Once a second the plugin checks again, and takes a body back if the switch goes off.

  Set both on the NinjaLive in the level and save it. Saved that way, with nothing set at runtime, both reach the simulation (checked in the demo river: 13.7 m and 2.0 m/s at fixed 60 fps). Changing them during play was not tested. The switch belongs to the NinjaLive, so it changes every point that NinjaLive tracks, characters and bones included.
- **A warning.** A body with Generate Overlap Events on that FluidNinja would track with world velocity or density logs a warning once.
- **The console.** `fnb.Buoyancy.Tracking` 0 never hands a body over, 1 hands it over when both are set (the default), and 2 always does, for testing. FluidNinja still paints what the NinjaLive settings say, so to reproduce today's runaway also set both back to 0 in the level before Play. The console variable keeps its value until the editor closes.
- **The demo's test.** `fnb.Demo.RiverTest wakes | tracking` measures how the floaters stir the demo river over 10 s, against reference numbers.

### Check Setup

**Tools → FluidNinja Buoyancy: Check Setup** checks everything this chapter asks for and writes a page to the **FluidNinja Buoyancy** message log (Window → Developer Tools → Message Log): one line per check, PASS, FAIL or WARN, with a **Fix** button on the lines a property or a tag puts right. `fnb.CheckSetup` in the console does the same, in Play too. It checks:

| What | PASS means | Fix |
|---|---|---|
| The modified core | `/Game/FluidNinjaLive/NinjaLiveCore` exists and is FluidNinja Waves interface 2, core build 20261017 or later (its `User.Wave.InterfaceVersion` and `User.Wave.CoreBuild`), with `User.WaveAuxTarget` and the relative-tracking parameter `TrackVelocityRelativeToWater`. A stock core, an older waves core and a core without the tracking half each get their own FAIL. | none: install the zip's core |
| Each NinjaLive: relative tracking | Its NinjaLiveCore's user parameter `TrackVelocityRelativeToWater` is 1 (the [Relative Tracking Switch](#fluidninjas-own-tracking-relative-to-the-water)). | sets it to 1 |
| Each NinjaLive: tracked density | **Selectively Kill Brush Density Keep Velocity** is 1, 4 or 5 (the Tracked Density Switch). | sets it to 1 |
| Each NinjaLive: the data channel reader | **DataChannelPointReaderEnabled** is on, so the [wakes](#wakes) and the swimmers' plunge rings reach FluidNinja. | turns it on |
| Each NinjaLive: its surface | The actors it draws its second output material on (**Apply 2nd Out Mat To Actors With Tag**; failing that, any actor whose surface material shows its velocity buffer) carry a [surface tag](#surface-tags), and their material has the **Pressure Buffer Parameter** (`PressureDivergenceBuffer`) the waves are read from. | adds `FluidNinjaBuoyancySurface` to the actor's tags |
| The Wave Sims | Every entry names a NinjaLive in the level by its name or label (a typo silently runs that pool at Wave Depth with open edges), every **FluidNinja Wave Sim** component sits on a NinjaLive, and no actor is set up twice (two entries; an entry and a component, where the component wins). | none: rename or delete the entry |
| The surface tags | At least one actor in the level carries a surface tag and has something horizontal to draw. | none |
| Physics | The project's stepping mode (reported; every mode works) and **Stable Steps** on. | none |

In Play the checks run on the Play world, and a Fix changes its copies, which Play ends: run it on the level with Play stopped to save a fix. The fixes are one undo step each.

---

## The FluidNinja Buoyancy component

Add it to an actor whose root component (or the component named in **Primitive Name**) is a primitive with **simple collision**: boxes, spheres, capsules or convex hulls. Complex (per-poly) collision is ignored. The component turns physics simulation on at Begin Play.

**Analyze Hull** (a button in the Details panel) builds the hull in the editor and logs:
- its volume and size;
- the mass for the rest draft;
- the metacentric heights (stability) in roll and pitch.

### Hull

| Setting | Default | Meaning |
|---|---|---|
| Primitive Name | None | The floating primitive. None uses the actor's root. |
| Hull Model | Triangles | **Triangles**: the hull surface clipped at the water line (boats, rafts). **Voxels**: cubes filling the hull (logs, crates, debris). **Spheres**: pontoons, each lifting its cap volume (balls, small or numerous props). |
| Hull Resolution | 40 cm | The hull is rebuilt from the simple collision as an even shell of triangles about this size. |
| Max Hull Triangles | 1000 | Above this, the shell is made coarser. |
| Lod Distances | 30 m, 80 m | Triangles model: beyond the first distance from the camera the hull is twice as coarse, beyond the second four times. |
| Voxel Size / Voxel Count | 0 / 48 | Voxels model: cube size, or about this many cubes when the size is 0. |
| Pontoons | empty | Spheres model: location, radius, lift weight and drag weight of each sphere (body frame). Empty means two rows along the hull's bottom. A ball is one pontoon at its centre; the demo's ring buoy uses eight around the ring. |
| Sleep When Settled / Sleep Distance | on / 60 m | A body that has barely moved for 2 s, is out of sight and is farther than this goes to sleep. It wakes when seen, approached, hit or moved by anything else (a force from game code). |

### Floating

| Setting | Default | Meaning |
|---|---|---|
| Mass Mode | Rest Draft | **Rest Draft**: the mass is set to what the hull displaces at Rest Draft. **Body Mass**: the body keeps its own mass and floats wherever that puts it, or sinks. |
| Rest Draft | 15 cm | How deep the hull's lowest point sits in calm water. |
| Settle On Start | on | Held still at Begin Play until its water is read, then set level at its rest draft and released. There is no drop or slam at the start. |
| Rest Attitude | 0 | How the body floats relative to its own frame. A barrel or log modelled standing up floats lying down with Pitch 90 (the demo barrel does this). |
| Max Depenetration Speed | 100 cm/s | Low, so a body placed into something eases out instead of launching. |
| Center Of Mass Offset | 0 | Lower is more stable. Analyze Hull reports the metacentric height. |
| Inertia Scale | 1 | A hollow hull turns more slowly than a solid one of the same shape. |
| Buoyancy Multiplier | 1 | Art control on the lift. 1 is Archimedes. |

Tips:
- **Uniform cubes:** a cube floats flat only below about 21 % or above 79 % draft. In between, it rests on an edge, as a real one would. Near those limits it is barely stable, and a current can roll it; lower **Center Of Mass Offset Z** (a weighted crate) to steady it.
- **Very light bodies:** a ball needs only a few cm of draft (the demo ball rests 3 cm deep at 0.8 kg). Light bodies are stable because of [stable steps](#physics-and-frame-rate).

### Hydrodynamics

| Setting | Default | Meaning |
|---|---|---|
| Pressure Drag Linear / Quadratic / Falloff | 0.3 / 1 / 0.5 | Drag where a face pushes into the water: linear and quadratic in speed per 1 m/s, and how it falls off with the face's angle to the flow. |
| Suction Drag Linear / Quadratic / Falloff | 0.3 / 0.5 / 0.5 | The same where a face pulls away. |
| Viscous Multiplier | 1 | Skin friction. |
| Slam Multiplier | 0.5 | Slamming when faces drive into the water. |
| Flow Coupling | 1 | How much the current carries the body: 0 ignores it, 1 is physical. |
| Air Drag Coefficient | 0.8 | Drag on the dry part, from the air and the wind. |
| Angular Damping | 0 | Extra damping about roll, pitch and yaw while wet, in 1/s. |

### Propulsion

Set **Propulsion** on to give the body a propeller and rudder (body frame).

- **Throttle** (−1 to 1) and **Rudder** (−1 to 1) can be set from Blueprint.
- With **Console Control** on, `fnb.Boat.Throttle` and `fnb.Boat.Rudder` set them for testing.
- Thrust only pushes while the thrust point is under water.
- The rudder works with the flow past it, including the propeller wash.
- The rudder assumes the bow is +X.

### Waves, Wake, Wet Line, Spray, Sound, Events

These are covered in their own sections below. **Waves** (on) and **Wave Feel Scale** (1) are described in [FluidNinja Waves](FluidNinjaWaves.md#settings). The events are:

| Event | Fires when |
|---|---|
| **OnSlam** (watts, location) | A slam is stronger than **Slam Event Watts** (200 W). |
| **OnWaterContactChanged** (in water) | The body enters or leaves the water. |

### Blueprint functions

| Function | Returns |
|---|---|
| **Get Readout** | A struct: in water, displaced litres, wetted area, water force (N), mass, slam power, entry power (on the frame the body goes into the water; see [Spray](#spray)), how old the water is, the current under the body, hull level, asleep, wind force. |
| **Get Status** | The same as one line of text. |

On the **FluidNinja Buoyancy Subsystem** (a world subsystem; see [FluidNinja Waves](FluidNinjaWaves.md#blueprint-and-c-functions)):

| Function | What it does |
|---|---|
| **Add Water Impulse** | Presses the water down at a point (a cannonball, an arrow, a foot stamp), into the waves at the next frame. |
| **Add Wave Poke** | A Gaussian pressure head for one tick: a wave maker. |
| **Register Swimmer** / **Unregister Swimmer** | Makes a skeletal mesh a swimmer, such as an AI swimmer, a diver, or a pawn on another movement system: it displaces water, and its body parts splash and press rings. |
| **Get Swimmer Stats** | What a swimmer's body parts did to the water. |
| **Get Current At Location** | FluidNinja's current around a place (cm/s), on a ring that leaves a swimmer's own wake out: for your swim code to carry swimmers with the river. Read back from the GPU, two or three frames old. |

---

## Project settings

All settings are under **Project Settings → Game → FluidNinja Buoyancy** and are saved to `DefaultGame.ini`.

| Category | Setting | Default | Meaning |
|---|---|---|---|
| Water | Enabled | on | Master switch. The console variable `fnb.Buoyancy` overrides it for the session (-1, the default, follows this setting). |
| | Surface Tags | SplineSource, SeaSurface, FluidNinjaBuoyancySurface | See [surface tags](#surface-tags). |
| | Ninja Parameter Collection | `/Game/FluidNinjaLive/OutputMaterials/MP_NinjaMaterialParamCollection01` | Where a simulation that follows the player is. |
| | Velocity Buffer Parameter | VelocityDensityBuffer | The surface material's velocity texture parameter. |
| Surface Mirror | Capture Width / Resolution | 80 m / 1024 | The capture's square. 1024 px over 80 m gives 7.8 cm per texel. |
| | Near Plane Above Bodies | 2 m | Water higher than this above the highest body is left out. |
| | Footprint Margin / Lead | 1 m / 0.15 s | The water read around each body, and how far ahead of a moving body. |
| | GPU Gather | on | One pass and one readback for all bodies. Off: two copies and readbacks per body. |
| | Max Gather Grid / Max Gather Bodies | 128 / 256 | Grid points across per body; bodies gathered per frame (more take turns). |
| | Max Extrapolation | 0.08 s | How long heights are carried forward by their rate of change. |
| Flow | Flow Scale Multiplier | 1 | See [the current](#the-current). |
| | Max Flow Speed | 6 m/s | Currents above this are clamped. |
| Physics | Water Density / Air Density | 1000 / 1.2 kg/m³ | |
| | Stable Steps | on | See [Physics and frame rate](#physics-and-frame-rate). |
| Wind | Wind | 0 | The wind on the dry part of hulls (cm/s, world). |
| Wake | Wakes / Wake Data Channel / Max Wake Points | on / `NinjaDataChannel1_Global` / 256 | See [Wakes](#wakes). |
| FluidNinja Tracking | Tracking Tag Property | `TrackActorPrimitiveComponentsWithTag` | The NinjaLive property (actor or component) naming the tag it tracks. A body handed over gets that tag. |
| | Object Type Filter Property | `OverlapFilterInclusiveObjType` | The NinjaLive property (actor, or failing that a component) listing the object types it tracks. A body is handed over only if a NinjaLive whose interaction volume overlaps it has its object type in this list, or tracks by tag. |
| | Interaction Volume Component | `InteractionVolume` | The NinjaLive component whose bounds are where it tracks. A NinjaLive without it selects nothing. |
| | Relative Tracking Switch | `TrackVelocityRelativeToWater` | How a NinjaLive says it tracks relative to the water: a bool or number property of this name on the actor or a component, or a float user parameter; on above 0.5. See [FluidNinja's own tracking, relative to the water](#fluidninjas-own-tracking-relative-to-the-water). |
| | Tracked Density Switch | `SelectivelyKillBrushDensityKeepVelocity` | How a NinjaLive says tracked bodies paint no density: a number property of this name on the actor or a component, or an int user parameter (`User.Painter.<name>` or `User.<name>`); 1, 4 or 5 count. FluidNinja's Selectively Kill Brush Density Keep Velocity. |
| Effects | Splash System | `NS_FNB_HullSplash` | Default spray. |
| | Splash Reference Watts / Splash Strength Ceiling | 70 W / 12 | Every splash on one scale. See [Spray](#spray). |
| Waves | Waves, Wave Depth, Wave Sims and more | on, 200 cm, the demo's two | FluidNinja Waves. See [FluidNinja Waves](FluidNinjaWaves.md#settings). A NinjaLive carrying a **FluidNinja Wave Sim** component is set up by that instead of a Wave Sims entry; see [Waves and swimmers](#waves-and-swimmers). |
| | Wave Follow, Wave Follow Target / Collection, Wave Follow Edge Fade Cm, Wave Follow Max Height Cm | on, `RT_FNB_WaveFollow` / `MPC_FNB_WaveFollow`, 300 cm, 60 cm | The wave height under the view, copied each frame for materials that ride the waves (`MF_FNB_WaveFollow`): floating plants and litter bob and tilt with wakes and ripples without being bodies. See [Wave Follow](WaveFollow.md). |
| Swimmers | Swimmer Waves, Swimmer Volume Litres | on, 76 L | Swimmers push the water aside (FluidNinja Waves' static head): the player's capsule and each registered swimmer's body. Moved here from Waves (the ini keys are unchanged). |
| | Swimmer Contacts, Player Contacts and more | on, on | Swimmers' body-part splashes and rings. See [Swimmers](FluidNinjaWaves.md#swimmers). |
| | Swimmer Wakes, Swimmer Wake Strength / Gain / Size / Min Speed, Swimmer Current Ring Inner / Outer | on, 0.35 / 0.2 / 70 cm / 15 cm/s, 70 / 160 cm | Swimmers' wake points and plunge rings into FluidNinja, and where the current around a swimmer is read. See [Wakes and the current](FluidNinjaWaves.md#wakes-and-the-current). |
| | Slap / Wash / Creak Sound | none | Default hull sounds. See [Hull sounds](#hull-sounds). |
| | Hull Attenuation | `SA_FNB_Hull` | Default attenuation for hull sounds. |

---

## Hull sounds

**No hull sounds ship with the plugin.** The component plays three kinds of sound if you give it some. Without them it is silent; nothing else changes.

### The three sounds

| Sound | Kind | When it plays | What the plugin drives |
|---|---|---|---|
| **Slap** | One-shot | When the hull slams into the water harder than **Slap Min Watts** (150 W). At most every 0.08 s, at the slam point. | Volume from the slam's power: 0.35 + 0.35 × log10(power ÷ Slap Min Watts), clamped to 0.2–2, × Sound Volume. Pitch Shift is random in −1.5…+1.5. |
| **Wash** | **Looping** | Continuously while the body is in the water and not asleep, attached to the hull. | Volume follows the length of the water line (full at 3 m) and how fast the water runs past it (full at **Wash Full Speed**, 2.5 m/s). There is a little even at rest. Pitch Shift goes from −3 when still to +2 at full speed. The loop stops when its volume falls to almost nothing. |
| **Creak** | One-shot | When the hull's spin changes faster than **Creak Min Angular Acceleration** (2 rad/s²), or on a slam over 4 × Slap Min Watts. At most every 0.7–1.3 s. | Volume 0.4–1.0 with how hard it was thrown, × Sound Volume. Pitch Shift is random in −2…+1. |

Notes:
- Volume is set with the audio component's volume multiplier, so it works with any sound asset.
- **Pitch Shift** is a float parameter, in semitones, that the plugin sets by name on every sound it plays. A **MetaSound Source** with a float input named exactly `Pitch Shift` can use it to vary its pitch. Sound Waves and Sound Cues ignore it.
- All three play with the **Hull Attenuation**. The default, `SA_FNB_Hull`, is full volume within 3 m and fades out over the next 35 m.

### Where to set them

| Where | Scope |
|---|---|
| **Project Settings → Game → FluidNinja Buoyancy → Effects → Slap Sound / Wash Sound / Creak Sound / Hull Attenuation** | The defaults for every floating body. |
| **The component's Sound category → Slap Sound / Wash Sound / Creak Sound / Attenuation** | Per body. Overrides the project default, so a wooden boat and a steel barrel can sound different. |
| **The component's Sound category → Sound Volume, Slap Min Watts, Wash Full Speed, Creak Min Angular Acceleration** | Per body: loudness and how easily each sound triggers. |

### How to add them

1. **Record or source** three kinds of audio:
   - several short water slaps or splashes against a hull, 0.2–0.6 s each;
   - a seamless loop of water lapping or running along a hull, 4–10 s, edited so it loops without a click;
   - a few wooden (or metal) creaks, 0.4–1 s each.

   Mono files are best, because they are spatialized.
2. **Import** the WAV files into your project (drag them into the Content Browser). Tick **Looping** on the wash's Sound Wave.
3. **Optional, recommended:** build a **MetaSound Source** for each sound.
   - Slap and creak: pick one of the several files at random with *Random Get (WaveAsset)* and play it with *Wave Player*.
   - Wash: play the loop with *Wave Player*, Loop on.
   - Add a float input named `Pitch Shift` and connect it to the Wave Player's **Pitch Shift** pin.
   - Without a MetaSound, a Sound Wave or Sound Cue works too. It just won't vary in pitch.
4. **Assign** them in Project Settings for every body, or on each component for per-body sounds. Leave **Hull Attenuation** on `SA_FNB_Hull`, or make your own Sound Attenuation asset.
5. **Optional:** give the sounds a Sound Class (for your volume sliders) and a Concurrency setting. Many bodies slapping at once can otherwise stack up.
6. **Test:** in the demo, run `fnb.TestHUD 1`, select a body, and press **Ctrl+Shift+J** to drop it from 3 m (a slap) or **Ctrl+Shift+K** to heel it 30° (creaks as it rights itself). The boat's wash plays as soon as it moves through the water.

---

## Wet line

With **Wet Line** on, the component writes four floats to the hull's **Custom Primitive Data**, starting at index **Wet Line Data Index** (0):
- the water line plane in the mesh's local space (z = A + B·x + C·y);
- how far above it the hull is still wet.

The wet band dries at **Drying Speed** (1.5 cm/s).

To show it:
1. Add the material function **`MF_FNB_HullWetness`** (in `FluidNinjaBuoyancy/Materials`) to your hull's material.
2. Feed it your Base Color and Roughness.
3. Use its two outputs instead.

Below the line the hull is darker and glossier; there is a sheen right at the line. The function's `HullWaterPlane` parameter reads Custom Primitive Data 0–3. If you change Wet Line Data Index, change that parameter's Primitive Data Index to match, in a copy of the function.

The demo material `M_FNB_Prop` shows the function in use.

---

## Spray

Where a hull slams into the water (over **Splash Min Watts**, 300 W) or its water line drives into the water faster than **Bow Spray Min Push** (1.5 m/s), the component spawns its **Splash System**, at most every **Splash Interval** (0.12 s).

- **A slam's splash is on the project's scale**, the same for every body. Its strength is the cube root of a power over **Splash Reference Watts** (Effects, 70 W), so eight times the power makes twice the splash.
  - A splash weaker than 0.2 is not drawn. It is at most the body's **Max Splash Strength** (12) or the project's **Splash Strength Ceiling** (12), whichever is lower.
  - The strength scales the splash system's size: 1.5 times the strength is about 3.4 times the splash's volume.
- **An entry's splash follows the body's weight and speed.** An entry is a body going into the water after being clear of it: a drop, a throw, a boat landing off a wave. The power that sizes its splash is the body's **entry power**: its weight × its sinking speed³ ÷ (2 × its height at that moment).
  - That is its kinetic energy, delivered in the time it takes to sink through its own height.
  - Heavier and faster bodies splash bigger. A body slapping in flat (short) splashes more than one going in end first (tall).
  - Nothing of the hull's pieces goes into it, so a drop splashes the same at every Hull Resolution, level of detail, Voxel Size and Hull Model.
  - Examples, measured on bodies dropped flat from 3 m (at 30 and 60 fps, within 2%):

    | Body | Entry power | Splash | From the slam alone |
    |---|---|---|---|
    | 175 kg crate (voxels) | 40 kW | 8.3 | 8.2 |
    | 91 kg log, 40 cm across | 57 kW | 9.3 | 4.2 |
    | 159 kg log, 40 cm across | 96 kW | 11.1 | 4.5 |
    | 238 kg row boat | 70 kW | 10.0 | 4.5 |

    The same log at its coarsest level of detail gave 93 kW (splash 11.0), where its slam jumped from 6.5 to 68 kW.
  - The splash still fires where and when the hull slams (over Splash Min Watts). The entry power only sizes the first splash within 0.25 s of the entry.
  - Other slams use the slam's own power: the body's whole slam power that step, its hull pieces driving in at once summed into one slam (**Get Readout → Slam Watts**), so it is the same at every Hull Resolution, level of detail and Voxel Size. These are a bow slapping into a wave, or a body slamming again while it is still partly wet.
  - Slaps, creaks and On Slam keep the slam's power.
  - **Get Readout → Entry Watts** shows the entry power on the frame it happens. The bench's `drop` test reports it with the splash strength it gives.
- **Up to three more splashes** go where the water line plunges in hardest, at least half as fast as anywhere, each smaller by the square root of how much slower.
- **A slam of strength 4 or more also throws a crown**: four to eight splashes facing outward around where it went in, each 0.6 of the slam's strength.
- **When nothing slams**, a bow spray faces outward from the hull where the water line drives in hardest (at least Bow Spray Min Push). It is on the same scale as a slam: its power is that of the water the piece of water line drives in, water density × the piece's area × its speed³ (a hand-sized 400 cm² piece when the physics gave no area), up to Max Splash Strength.
- **Splash Scale** multiplies every splash and spray from the body.

Swimmers' strokes and dives use the same scale; see [Swimmers](FluidNinjaWaves.md#swimmers).

- The default is **`NS_FNB_HullSplash`**: a burst of about 44 droplets, made from the engine's Directional Burst template.
- Any Niagara system works. Set your own per body (**Spray → Splash System**) or project-wide (**Effects → Splash System**).
- Leave it empty for no spray.

---

## Wakes

With **Wake** on (component) and **Wakes** on (project; `fnb.Buoyancy.Wake` overrides it, -1 the default following the setting, 0 killing every wake for an A/B), each frame the pieces of water line that drive into the water are written to FluidNinja's point input as moving brush points. FluidNinja then makes the bow wave, the wake and the foam. This is how a body with Interaction **Wakes** (or **FluidNinja Tracking and Wakes**) stirs the simulation. By default bodies are handed to FluidNinja's own tracking instead; see [Interacting with FluidNinja](#interacting-with-fluidninja).

A piece drives into the water in two ways. Moving sideways, it pushes at its speed through the water along the hull's outward normal. Sinking, it pushes outward at about its sinking speed: a hull of water plane A and water line L displaces water over a layer about A / L deep. A drop or a dunk therefore sends a ring out, and a body rising out of the water writes nothing. A sphere's water line is its own circle, in eight pieces, each facing out from that sphere's centre.

| Setting | Default | Meaning |
|---|---|---|
| Interaction | FluidNinja Tracking | Wakes, FluidNinja Tracking, or both. FluidNinja's tracking is used only while every NinjaLive tracks relative to the water and paints no density for tracked bodies; see [FluidNinja's own tracking, relative to the water](#fluidninjas-own-tracking-relative-to-the-water). |
| Wake Brush Size | 60 cm | The brush size at a push of 2 m/s, scaled 0.4–2× with the push. |
| Wake Min Push | 20 cm/s | Slower pieces leave no wake (sideways and sinking speed together), so a body bobbing gently at rest stays quiet. |
| Wake Strength | 0.35 | How much of the hull's speed through the water the water at the hull takes on. 1 would make it a wall. A hull stirs only the water near the surface. |
| Wake Gain | 0.15 | The share of what the water lacks that each frame gives it. FluidNinja adds brush velocity every frame, so this closes on the target without overshooting. |

A body that makes wakes takes its current from the water around it, not from its own wake. The same applies to a body that FluidNinja tracks itself (Generate Overlap Events on).

That has a price for light bodies. Reading the current outside its own reach, a body no longer rides the water that others push right next to it: a swimmer's bow wave, another hull's wake. So the demo's light bodies (balls, ring buoys and the air mattress) have Wake off. They ride the water rather than stir it. Since 1.0.1 a sphere body has a proper water line, so Wake does work on them; it is a choice per body. Measured in the demo pool: with Wake on, the swimmer tipped the ring buoy onto its edge and held it there in both of two pushes. With Wake off it was pushed ahead instead, both times. In the river, Wake on or off made no difference.

---

## Waves and swimmers

With the modified FluidNinja core (the waves zip) and **Waves** on, floating bodies and swimmers make real water waves in FluidNinja's own simulation:
- each body's weight presses a hollow into the water, and its slams, strokes and impacts push their impulse in;
- the rings spread at the right speed for their length and the water's depth;
- they reflect off walls or are absorbed at open edges, are carried along by the current, and break when they get too steep;
- FluidNinja's surface draws them, and every other body rides them.

Swimmers take part too:
- the player's pawn pushes the water aside as a body lying along the water, holding Swimmer Volume Litres;
- its hands, forearms, feet and head splash and press rings where they go in;
- other characters join through **Register Swimmer**.

Two swimmer features don't use the wave stages, and work with Waves on or off:
- **Swimmer wakes**: a swimmer writes its push into FluidNinja's point input, and a ring where it plunges in. FluidNinja ignores points under about 40 cm/s, so at the default gain the plunge ring is what shows; see [Wakes and the current](FluidNinjaWaves.md#wakes-and-the-current).
- **Get Current At Location**: the current around a swimmer, for your own swim code to carry it with the river. The plugin moves no swimmer itself.

**Get Water At Location** answers everything swim code asks of the water at a point in one call: the still surface, the wave height and normal, the current and how old the reading is. A **FluidNinja Swimmer** component (or any object implementing **FluidNinja Swimmer Provider**) gives the plugin a character's pose from a **Swimmer Profile** data asset (Manny, MetaHuman and quadruped presets), fires **On Entry** when it dives, and turns its strokes into wakes and rings; **Poke At** presses a wading step into the waves. Swimmer providers, profiles, the water query, dives and strokes: [Swimmers](Swimmers.md).

**Foam**: whitecaps where waves break, wake trails a moving hull's waterline sheds and impact rings where something lands, accumulated by the wave core (interface version 4) and drawn by the surface material; never fed back into the waves. Settings → Waves → **Foam**, **Foam Decay**, **Foam Breaking**, **Foam Wake**, **Foam Impact**; the surface material's **WaveFoamColor** and **WaveFoamStrength**, and **WaveChoppy** / **WaveChoppiness** for sharper crests (off by default). Details: [Foam](FluidNinjaWaves.md#foam).

Each simulation's water is described in one of two places:
- a **FluidNinja Wave Sim** component on its NinjaLive actor (Add Component → FluidNinja Wave Sim): **Waves**, **Depth Cm** (or **Depth From Bed**), **Walled** with **Wall Absorption** and **Wall Band Cm**, **Scale Down Override** and **Importance**. The level carries the facts about the water with the actor, so they travel with it when it is copied or renamed, and the demo's two NinjaLives are the pattern (a closed pool 4 m deep; a river lane 3 m deep).
- a **Wave Sims** entry in the project settings, keyed by the actor's name or label: the older way, kept for levels you cannot edit. A NinjaLive with the component ignores an entry naming it (logged once), and [Check Setup](#check-setup) reports an entry whose name matches no actor.

Everything about it is on its own page: [FluidNinja Waves](FluidNinjaWaves.md). That covers setting it up per simulation (the [Wave Sim component](FluidNinjaWaves.md#wave-sim-component) and the Wave Sims list: depth, walls, the wave grid's scale-down), every setting, splashes, swimmers, the functions, the interface with the core, re-applying it after a FluidNinja update, testing and troubleshooting.

---

## Physics and frame rate

The plugin applies its forces in a Chaos sim callback, so it follows whatever physics stepping the project uses:

| Project physics | What happens |
|---|---|
| **Default**: one step per frame, the step capped at Max Physics Delta Time (1/30 s) | One force evaluation per frame. |
| **Substepping** on | One per substep. |
| **Async physics** on, for example 1/120 s | One per async step. |

None of these is required.

### Stable steps

The solver integrates forces explicitly. For a heavy boat a 1/30 s step is fine. For a light body it is not:
- the water holds a beach ball like a stiff spring, so it bobs faster than the frame rate;
- the drag of a large, light air mattress would stop it in a fraction of a step.

Explicitly integrated, such a body overshoots, and then explodes.

**Stable Steps** (on by default; console `fnb.Buoyancy.Implicit 0/1`) works out, each step, how stiff and how damped the water holds the body: the waterplane, its second moment and the drag's growth with speed. The part beyond what the step resolves (ω·dt above 0.5, damping·dt/mass above 0.5) is applied implicitly (backward Euler), as if the step had been cut finer. Bodies the step resolves well are left exactly as they are, so heavy hulls behave the same as without it.

Measured on the demo level (14 floating bodies in the pool and the river), running every test of the [test bench](#console-commands-and-the-test-bench):

| Physics | Stable Steps off | Stable Steps on |
|---|---|---|
| Default, fixed 30 fps (1/30 s steps) | 66 of 78 passed. Dropped from 3 m, the beach balls, ring buoys and air mattress blew up (thrown at up to 500 m/s), and the boats were still drifting afterwards. The mattress also blew up in the 16 m/s wind and floated far off in the walk-away test. | 78 of 78 |
| Default, fixed 60 fps | 76 of 78. The dropped air mattress blew up; a beach ball floated 22 cm off in the walk-away test. | 78 of 78 |
| Default, uncapped (about 60 fps) | 74 of 78. The dropped ring buoys and air mattress blew up; the mattress blew up in the 16 m/s wind. | 78 of 78 |
| Async physics, 120 Hz | 77 of 78. The dropped air mattress blew up. | 78 of 78 |

Light bodies fail without Stable Steps even at 120 Hz: the drag on a large, light raft stops it in less than a step. With Stable Steps every body comes back to rest at its draft. Bodies the step already resolves, such as the boat and crates riding calm water, move the same with it on or off. It steps in only for light bodies and during hard impacts.

---

## Console commands and the test bench

| Command | Meaning |
|---|---|
| `fnb.Buoyancy -1/0/1/2` | -1 (the default): the **Enabled** setting decides. 0 off, 1 on, 2 on with the debug view (the water line, forces and the water read back). |
| `fnb.Buoyancy.Wake -1/0/1` | -1 (the default): the **Wakes** setting decides. 0 no wakes at all, the swimmers' included (A/B); 1 the bodies' wakes on. |
| `fnb.Buoyancy.Tracking 0/1/2` | Bodies whose Interaction asks for FluidNinja's tracking: 0 never, 1 (the default) when every NinjaLive turns its Relative Tracking Switch on and sets its Tracked Density Switch to 1, 4 or 5, 2 always, for testing (with both NinjaLive settings at 0, FluidNinja then paints their world velocity and density: today's runaway). |
| `fnb.Demo.RiverTest wakes \| tracking` | Demo level: how the floaters stir the river over 10 s, against reference numbers. |
| `fnb.Buoyancy.Gather -1/0/1` | -1 (the default): the **GPU Gather** setting decides. 0 per-body readbacks, 1 the gather (A/B). |
| `fnb.Buoyancy.Implicit -1/0/1` | -1 (the default): the **Stable Steps** setting decides. 0 plain explicit forces, 1 stable steps (A/B). |
| `fnb.Buoyancy.CalibrateFlow [apply]` | Measure the current against the visible dye. See [the current](#the-current). |
| `fnb.Boat.Throttle <-1..1>`, `fnb.Boat.Rudder <-1..1>` | Test controls for bodies with Propulsion and Console Control on. |
| `fnb.TestHUD 0/1/2` | The test bench panel: hidden (the default), in PIE, or in any non-shipping game. |
| `fnb.Test <verb>` | Drive the test bench. `fnb.Test help` lists the verbs. |
| `fnb.Demo.River start\|stop\|reset\|lanterns\|day` | The demo level's river: start or stop its current, put its floating things back at its head, or start or end the lantern night. See [the demo level](#the-demo-level). |
| `fnb.Demo.Night [moon lux] [sky light scale] [exposure bias]` | Tunes a running lantern night live. See [the lantern night](#the-lantern-night). |
| `fnb.Demo.DropBall` | During a lantern night: drops a large, heavy ball into the middle of the lanterns (key B). |
| `fnb.Demo.EnginePush 0/1` | The demo character pushes bodies with its own push (0, the default) or the engine's (1, for A/B). |
| `fnb.CheckSetup` | [Check Setup](#check-setup) from the console: on the Play world in Play, else on the level. |
| `fnb.Memory` | In Play: every GPU allocation the plugin holds (the Surface Mirror, the gather and copy readbacks, the current probes, the waves' source textures, Wave Aux targets, self-field atlas and pools), one line each with its size and format, and the total in MB. The status line (`fnb.Buoyancy 2`) carries the total. |
| `sg.WaterQuality -1..4` | The water's quality tier: -1 (the default) follows `sg.EffectsQuality` (3 on a desktop), 0 Low, 1 Medium, 2 High, 3 Epic (the calibrated defaults), 4 Cinematic (the premium tier). See [Quality](#quality). |
| `fnb.Lod.DistanceScale <scale>` | Scales every body's Lod Distances Cm for the hull's levels of detail and the gather's spacing and cadence (1 as set; 0.5 brings the coarser levels twice as near). Set by the tier. |
| `fnb.Lod.Bias 0/1/2` | Every hull this many levels of detail coarser than its distance asks. Set by the tier. |
| `fnb.Lod.SleepDistanceScale <scale>` | Scales every body's Sleep Distance Cm. Set by the tier. |
| `fnb.Gather.FarEveryNFrames <n>` | The gather's cadence by level of detail: a body within its first Lod Distance every frame, beyond it every n frames, beyond the second every 2n; between gathers its patch carries forward, never past Max Extrapolation Seconds. Set by the tier (2 at Epic, the default). |
| `fnb.Gather.BudgetPerFrame <n>` | At most n bodies gathered in one frame (0: Max Gather Bodies alone). Over the budget, bodies whose patch would go past Max Extrapolation Seconds come first (the longest waiting first), then the nearest and fastest; the rest keep their last patch (`stat FluidNinjaBuoyancy`: Bodies waiting). Set by the tier. |
| `fnb.Gather.MaxGrid <n>` | Caps Max Gather Grid (0: the setting). Set by the tier. |
| `fnb.Capture.Scale <scale>`, `fnb.Capture.EveryNFrames <n>` | The Surface Mirror's resolution (a scale on Capture Resolution, 64 to 4096 texels) and how often it is redrawn (between redraws the gather reads the last one). Set by the tier. |
| `fnb.Wake.MaxPoints <n>`, `fnb.Waves.MaxImpulses <n>` | Caps on Max Wake Points and Max Wave Impulses (0: the setting). Set by the tier. |
| `fnb.Effects.SplashBudgetPerFrame <n>` | The most splash systems all bodies may spawn in one frame (near bodies the whole budget, far ones half; -1 no limit). Set by the tier (8 at Epic, the default). |
| `fnb.Effects.ReachScale <scale>`, `fnb.Effects.Crown 0/1`, `fnb.Effects.WetLine 0/1` | How far the effects reach (a scale on Lod Distances Cm for the effects alone), whether hard slams throw a crown, whether the wet line is kept. Set by the tier. |
| `fnb.Waves.ScaleDown 1/2/4`, `fnb.Waves.MaxActiveSims <n>`, `fnb.Waves.SelfFieldDistanceCm <cm>` | FluidNinja Waves' tier knobs: the wave grid's scale-down, how many simulations run waves (the nearest to the camera), how far from the camera bodies keep an own field. See [FluidNinja Waves](FluidNinjaWaves.md#settings). Set by the tier. |

### The test bench

`fnb.TestHUD 1` shows a panel at the top right of the game viewport, and labels over every floating body. The panel shows:
- the water and the current;
- the wind;
- the plugin's costs;
- a line per body;
- the selected body in detail;
- the latest test results.

The bench is its own module, `FluidNinjaBuoyancyDeveloper` (type DeveloperTool): it is compiled and loaded in the editor, in Play In Editor and `-game`, and in Debug and Development game builds, and never exists in Test or Shipping builds (a Test build that wants it sets `bBuildDeveloperTools = true` in its Target.cs). `fnb.Test` and `fnb.TestHUD` exist only where the module does.

**Hotkeys** (Ctrl+Shift with):

| Key | Action |
|---|---|
| B | Hide the panel |
| R | Reset bodies |
| PageUp / PageDown | Select a body |
| T | Run all tests |
| W | Cycle the test wind |
| C | Controlled (flat, still) water |
| D | Debug view |
| K | Heel the selected body 30° |
| J | Drop the selected body from 3 m |

Shift+F1 frees the mouse in PIE.

**Tests** (`fnb.Test run <name>`):

| Test | What it checks |
|---|---|
| `settle` | Every body is put back and settles at its rest draft, still. |
| `capsize` | Stable bodies heeled 30° right themselves. |
| `drop` | Bodies dropped from 3 m don't blow up and come back to rest at their draft. Slam power, entry power, the entry's splash strength and the mass are reported. |
| `wind` | A test wind of 8 m/s, then 16 m/s, drifts bodies downwind, faster in the stronger wind. The wind blows the way the view faces. A long hull slips more easily along its length than across it, so it drifts between the wind and its own axis. For example, a lying barrel turned 55° to the wind drifts about 37° off it. Up to 45° off passes. |
| `throttle` | Bodies with propulsion: speed, heading, turning. |
| `gather` | The GPU gather agrees with per-body readbacks. |
| `walkaway` | The player moves 70 m away and back; nothing sinks while out of the capture. |
| `all` | Every test in turn. |
| `ripples` | The wave tests in turn (about 7 minutes). See [Testing](FluidNinjaWaves.md#testing). |

The settle, capsize, drop and wind tests run on controlled flat, still water in free water, where bodies ignore the scenery. That way a live current or a pool wall cannot swamp the measurement.

Other verbs:

| Verb | Action |
|---|---|
| `reset [selected]` | Reset all bodies, or only the selected one. |
| `select <next\|prev\|name>` | Select a body. |
| `water <live\|controlled [height]>` | Switch between live and controlled water. |
| `wind <project\|calm\|m/s [toward degrees]>` | Set the test wind. |
| `push [N.s]`, `heel [deg]`, `drop [m]` | Disturb the selected body. |
| `here` | Bring the selected body in front of the view. |
| `boat <throttle> [rudder]` | Drive bodies with propulsion. |
| `scenery <on\|off>` | Bodies meet or ignore the scenery. |
| `debug` | Toggle the debug view. |
| `calibrate [apply] [view]` | Calibrate the current at the selected body, or around the view. |
| `hud` | Show or hide the panel. |
| `results` | Print the latest results. |

Results go to:
- the panel;
- the log (`LogFluidNinjaBuoyancy`);
- `Saved/FluidNinjaBuoyancy/TestResults/<time>_<test>.json` and `latest.json`.

---

## The demo level

`/FluidNinjaBuoyancy/Demo/Maps/FNB_DemoPool` is a standard (non-World-Partition) level. Its GameMode override is the plugin's **FluidNinja Buoyancy Demo Game Mode**, so it plays in any project without setup.

**Leaving the demo out.** The demo is its own module (`FluidNinjaBuoyancyDemo`) and its own content folder (`Content/Demo`, about 111 MB); nothing else in the plugin depends on either. Unreal has no per-module opt-in switch in a .uplugin, so a project that wants the plugin without the demo deletes `Source/FluidNinjaBuoyancyDemo` and `Content/Demo` from its copy of the plugin (nothing else references them). A packaged game is not the concern: unreferenced plugin content is not cooked, so the demo's 111 MB never ships unless your levels reference it; belt and braces, add `/FluidNinjaBuoyancy/Demo` to **Project Settings → Packaging → Directories to Never Cook**.

| Part | What it is |
|---|---|
| The pool | 20 × 20 m. The deep end is 4 m deep; seven steps lead up to the north deck. It is built from engine cubes with the demo's procedural tile materials. |
| `Pool_FluidNinja` | A NinjaLive simulation from FluidNinja's `NP_Water_Sparse_Pool1` preset (1600 × 1600 cells over 20.25 m). It draws on `Pool_WaterSurface`, FluidNinja's `SM_plane_300x300` with `MI_Water_Pool_Caustics_Clear`, through the tag `FluidNinjaBuoyancySurface`. `Pool_WaterVolume` gives the deep end a water volume. |
| The dock | `SM_FNB_Dock`: a 6.5 m wooden pier on posts along the west wall. |
| The river | North of the pool, from FluidNinja's river setup: a 45 m lane from its head (west) to a basin (east), 13 m wide, with a 4 m deep channel down the middle between stepped banks. See [the river](#the-river) below. |
| The player | **FluidNinja Buoyancy Demo Character** (module `FluidNinjaBuoyancyDemo`): the UE5 mannequin (Manny) with the Third Person template's unarmed Animation Blueprint, in `Demo/Characters/Mannequins`, and a third-person camera. It starts on the deck between the pool and the river's head.<br>• Move: WASD or the left stick.<br>• Look: the mouse or the right stick.<br>• Jump: Space or the bottom face button; held while swimming, swim up.<br>It reads the keys directly, so it needs no input mappings in your project. It swims in the water volumes and walks down the steps. It pushes floating bodies it walks into (see [the push](#the-push) below), and its weight presses down on any it stands on. |
| The ground | Grass out to 150 m around the decks, so the player can walk off them, and so the test bench's walk-away test has somewhere to go. |
| The floating things | Each a Blueprint in `Demo/Blueprints`, listed below. In the pool: a boat, two beach balls, a ring buoy, a crate and an air mattress. At the river's head: a boat, two crates, three beach balls, a barrel and a ring buoy. The boats, crates and barrel stir the water with the plugin's wakes (Interaction: Wakes, Wake on), so a drop writes its impact ring into FluidNinja's flow; FluidNinja's tracking would skip that. The light bodies (balls, ring buoys, air mattress) keep Interaction's default, FluidNinja's tracking, with Wake off, so they need the modified FluidNinja; see [FluidNinja's own tracking, relative to the water](#fluidninjas-own-tracking-relative-to-the-water). |

| Blueprint | Hull model | Rest draft | Mass | Notes |
|---|---|---|---|---|
| `BP_FNB_Boat` | Triangles | 13 cm | ≈ 203 kg | A 3.4 m rowing boat. Its centre of mass is lowered 12 cm. It makes wakes. |
| `BP_FNB_BeachBall` | Spheres (1 pontoon) | 3 cm | ≈ 0.8 kg | Angular damping 1. Continuous collision (CCD), so a fast bounce never passes into a wall. The player cannot step up onto it. |
| `BP_FNB_RingBuoy` | Spheres (8 pontoons) | 4.5 cm | ≈ 3.8 kg | Continuous collision. The player cannot step up onto it. |
| `BP_FNB_Crate` | Voxels (64) | 15 cm | ≈ 96 kg | Weighted: its centre of mass is lowered 15 cm, and Angular Damping is 1. A uniform cube this light is barely stable (3 cm metacentric height), so the pool's current rolled it over and over. |
| `BP_FNB_Barrel` | Triangles | 25 cm | ≈ 89 kg | Rest Attitude Pitch 90: modelled standing, floats lying down. |
| `BP_FNB_AirMattress` | Triangles | 1.2 cm | ≈ 16 kg | A light, large, flat body that stable steps keep calm. Continuous collision. |

### The river

| Part | What it is |
|---|---|
| `River_FluidNinja` | A NinjaLive simulation (FluidNinja's `NP_Water_Sparse_Pool1` preset, 2048 × 2048 cells) with the tag `FNB_RiverSimulation`. It draws FluidNinja's `MI_Water_River_Large_White_TilemapUsedForRiver3` on `River_WaterSurface`, which carries `FNB_RiverWater` (for FluidNinja) and `FluidNinjaBuoyancySurface` (for the plugin).<br>Its **User Input Based Interaction** is No User Input, like the pool's: FluidNinja's default paints the water with the mouse and shows the cursor, so the mouse would not look around.<br>FluidNinja pauses a simulation when the player leaves its **Activation Volume Size** box (`SimActivatedByPawnProximity`). Here the box is 120 m instead of the default 80 m, so the river keeps flowing while the player watches from the pool's far deck. |
| Water volumes | `River_LaneWater` (the channel), `River_ShallowS` / `River_ShallowN` (the stepped banks) and `River_BasinWater`, for swimming and the far field. |
| The floaters | The eight bodies at the head carry the tag `FNB_RiverFloater`. |
| `River_Switch` | A **FluidNinja Buoyancy Demo River** actor that starts, stops and resets the river. |

The river is still when play begins. The switch holds the simulation still: no direct velocity, and its velocity noise and bitmap velocity input are off, so the water calms. It also moors each floater softly where it starts, so the floaters wait at the head.

Start the river from the panel at the top left of the screen, which has three buttons:
- **Start river** sets the simulation's direct velocity, gives back its turbulence and lets the floaters go. They drift down the river from its head to the basin: the balls, crates, barrel and ring buoy in about 25 s, the deeper boat in about 30 s. The button then reads **Stop river**.
- **Reset** stops the current and puts every floater back at its start (and ends a lantern night).
- **Lantern night** turns the level's lights off and sets 20 lit paper lanterns on the slow river; see [the lantern night](#the-lantern-night) below. The button then reads **Lights on**, which takes the lanterns away, turns the lights back on and stops the river.

The mouse looks around as usual. Press Tab for the mouse cursor to click the buttons; a click on either goes back to mouse look.

| Key or command | Action |
|---|---|
| Enter | Start or stop the river. |
| R | Reset. |
| L | Start or end the lantern night. |
| B | During a lantern night: drop a heavy ball into the middle of the lanterns, from anywhere. |
| Tab | Show or hide the mouse cursor (shown, the mouse points at the panel; hidden, it looks around). |
| `fnb.Demo.River start\|stop\|reset\|lanterns\|day` | The same from the console (`lanterns` starts a lantern night, or sets a fresh group; `day` ends it). |
| `fnb.Demo.Night [moon lux] [sky light scale] [exposure bias]` | Tunes a running lantern night live; no arguments prints the values now. |
| `fnb.Demo.DropBall` | Drops a heavy ball into the lanterns, as B does. |

FluidNinja takes up new values at its next parameter update (**ParamUpdateFrequency**, 1 s by default), so the current starts and stops up to a second after the click.

The switch's settings (Details panel, **River**):

| Setting | Default | Meaning |
|---|---|---|
| Simulation Tag | `FNB_RiverSimulation` | The tag on the river's NinjaLive actor. |
| Floater Tag | `FNB_RiverFloater` | The tag on the bodies that ride the river. |
| Flow Velocity | (0.5, 0) | FluidNinja's direct velocity while the river runs (DirectVelocityX/Y, world space). |
| Mooring Period | 6 s | While the river is still, each floater is held where it started by a soft spring of this period. 0 does not hold them. |
| Show Panel | on | The Start / Reset / Lantern night panel. |
| Lanterns (Lantern Night) | 20, 4.6 × 2.6 m, 55 cm apart, 150 cm clearance | The lantern night's group: Count, Half Length Cm, Half Width Cm, Spacing Cm, Clearance Cm, and Lantern Class (none: `BP_FNB_Lantern`). |
| Lantern Flow Velocity | (0.16, 0) | FluidNinja's direct velocity on the lantern night: the slow setting, about 0.6 m/s on the demo river. Its direction is also the way the group is laid out downstream. |
| Night Look | moon 0.5 lux, sky lights 0, exposure bias -3 | How dark the lantern night is (below). |

### The lantern night

**Lantern night** (the panel's third button, **L**, or `fnb.Demo.River lanterns`) shows light bodies riding a slow river in the dark:

1. The floaters go back to the head of the river and stay moored there, so the lanterns have the water to themselves.
2. The level's lights go off: a **FluidNinja Buoyancy Demo Night** actor hides every light (the sun, the sky light, any lamps), the sky atmosphere, the height fog and clouds. It adds a faint, shadowless, pale blue moon and holds a fixed exposure (an unbound post process at priority 10000), so the scene is dim rather than pitch black and the candles read as candles. Nothing in the level is changed or saved; destroying the night actor puts everything back.
3. 20 **FluidNinja Buoyancy Demo Lantern** actors are set on the water just downstream of the floaters, at random headings, in a loose group thickest in the middle, no two closer than 55 cm and none within 150 cm of another body.
4. The river runs at its slow setting (Lantern Flow Velocity), and the lit lanterns drift down to the basin.

Pressing it again sets a fresh group. **Lights on** (or `fnb.Demo.River day`) takes them away, turns the lights back on and stops the river.

**Drop a ball into them (B).** While the night runs, press **B** anywhere (or type `fnb.Demo.DropBall`): a large, heavy ball (**FluidNinja Buoyancy Demo Ball**, 1 m across, 200 kg, glowing a cool blue with a soft light round it so you can follow it and see its splash and rings in the dark) is let go 4 m above the middle of the lanterns, wherever they have drifted, so you can follow them down the river and drop balls into them. This is what real buoyancy adds over FluidNinja's own particle-driven floaters:
- The ball plunges well below the surface, slows as the water it displaces grows, and bobs back up. It settles about 40% under, as its 200 kg against the 524 L it holds says it should.
- FluidNinja Waves carry its impact ring outward. The lanterns ride the ring up and down and tilt on its slope.
- Its wake (Interaction Wakes, Wake on) writes the impact into FluidNinja's flow, and the outflow pushes the lanterns aside. Lanterns it lands on or rolls into are knocked away by the collision.
- Afterwards it floats among them, and the current carries it with them.

At most 4 balls stay in the water (Max Balls on the night actor; another drop takes away the oldest), and the night's end takes them all away. The ball's Diameter Cm, Mass Kg, Glow Color, Glow Strength (6) and Glow Lumens (60; its paint is `Demo/Materials/M_FNB_BallGlow`), and the night's Drop Ball Key, Drop Height Cm and Max Balls, are properties on the two classes. A ball heavier than the water its volume holds (524 kg at 1 m) sinks.

**The lantern.** `Demo/Lantern/BP_FNB_Lantern` is a 36 × 36 cm, 34 cm tall paper lantern on a wooden base, with **流体忍者** (FluidNinja) on its four panels. A candle inside lights it: a shadow-casting point light (12 lumens) and a shadowless one for the light through the paper (8% of it), a separate flame mesh, and paper and ember glow, all driven by one smooth flicker. Its variables (category Candle): Lit, Candle Lumens, Paper Glow, Flicker Strength, Flicker Speed and Phase Offset; `SetLit(false)` puts it out. The Blueprint only draws the lantern; it has no collision.

**The floating base.** `AFluidNinjaBuoyancyDemoLantern` floats it: a hidden 36 × 36 × 6 cm box with a FluidNinja Buoyancy component (Triangles hull at 3 cm, Rest Draft 0.9 cm, so about 1.2 kg, like a light paulownia base), set on the water where it is spawned (Settle On Start) rather than dropped. It is a light floater: Interaction Wakes with Wake off, so it rides the current and the waves others make and writes no wakes of its own. Each lantern gets its own flicker phase. Its static `SpawnGroup` and `GroupCenterBeyond` place a group on any river; its Lantern Class takes any lantern Blueprint whose pivot is the bottom centre of its base.

**Tuning the dark.** Twenty candles light the water near them; the moon and the exposure set how much else shows. While a night runs, try values from the console:

| Command | Effect |
|---|---|
| `fnb.Demo.Night` | Prints the values now. |
| `fnb.Demo.Night 0.5 0 -3` | The defaults: moon 0.5 lux, the level's sky lights off, exposure bias -3 (the lantern's own candle-only look). |
| `fnb.Demo.Night 0.2` | A darker night: less of the banks and water shows. |
| `fnb.Demo.Night 1.5 0 -2.5` | A brighter one. Each step of exposure bias doubles the whole image, lanterns included. |
| `fnb.Demo.Night 0.5 0.005` | Adds a little of the level's sky light back as an even ambient. A sky light holding a day capture is very bright at night exposure: keep it tiny. |

Put the values you settle on in the river switch's **Night Look**. Each lantern has two candle lights, one of them casting shadows; on a slow GPU, switch Cast Shadows off on the Blueprint's CandleLight.

To build a river switch of your own, tag a NinjaLive actor and its floating bodies, place the actor and set the two tags. Anything that sets the NinjaLive component's `DirectVelocityX` / `DirectVelocityY` does the same job, for example a Blueprint.

### The push

By default the engine's Character Movement pushes every simulated body with the same impulse and force, whatever it weighs (Push Force Factor 750000, Initial Push Force Factor 500), at the contact point. A 200 kg boat feels a firm shove, but a 0.8 kg beach ball is fired off at several metres a second, spinning. Measured on the demo level, walking into a beach ball with the engine's push threw it at up to 150 m/s, spinning at 9.6 rad/s. With the demo's push it reached 2.9 m/s without spinning.

The demo character's movement (**FluidNinja Buoyancy Demo Movement**) pushes a body toward the speed it walks or swims into it, never past it. It pushes with at most **Max Push Force** (350 N, under **Character Movement: Physics Interaction**), at the body's centre of mass. A ball is shoved along at walking pace without being spun; a crate follows more slowly; a boat has to be leaned on to get going. `fnb.Demo.EnginePush 1` switches back to the engine's push for comparison.

For your own characters, the same idea is one override of `UCharacterMovementComponent::ApplyImpactPhysicsForces`. See `FluidNinjaBuoyancyDemoMovement.cpp`.

All the demo props, the dock and the materials were made for this plugin. Everything else the level uses ships with Unreal Engine or with FluidNinja LIVE:
- FluidNinja LIVE's own simulation, surface mesh and material;
- the engine's basic cube;
- the UE5 mannequin from the engine's Third Person template.

---

## Performance

What the plugin costs:

| Cost | Detail |
|---|---|
| GPU | One depth capture of only the surface pieces under bodies, and one small compute pass. The capture redraws those pieces at their finest level of detail; keep surface meshes reasonably sized. |
| Game thread | Building each body's water patch from the readback, the wakes, and the effects. |
| Physics thread | Per body per step: one pass over its hull triangles (or voxels or spheres). Distance LODs halve and quarter the triangles; asleep bodies cost nothing. |

The test bench panel (`fnb.TestHUD 1`) shows the measured costs: game thread, physics pre-tick and physics thread per frame. `stat FluidNinjaBuoyancy` has the same with the frame's counters: bodies awake, asleep, in the capture and by level of detail (LOD 0, 1, 2), bodies gathered, waiting (over the gather budget) and deferred (not their frame), gather points, wake points, impulse pieces, self fields and cells, the readback's age in frames, splash spawns and the wave simulations running. `-csv` runs record them under the `FNB` category. `fnb.Memory` lists the GPU memory held, and the status line (`fnb.Buoyancy 2`) carries the total.

To trade quality for performance, set the tier: [Quality](#quality).

---

## Quality

One switch scales the water from "floats and drifts" to "every body with an own field on a 2048² mirror": the scalability group **`sg.WaterQuality`**. It is `-1` by default, which **follows `sg.EffectsQuality`**, so the engine's own presets (Low to Cinematic, the game user settings' Effects quality, `Scalability::SetQualityLevels`) carry the water with them. A desktop project runs the engine's effects quality at 3, Epic, unless something sets it lower, and **Epic is the calibrated defaults**: the water exactly as the project settings and the plugin's calibration describe it, nothing capped or scaled. High, Medium and Low save cost below it; Cinematic is the premium tier above it. Set `sg.WaterQuality` to `0`–`4` to pin the water's tier apart from the effects. The tier is applied once the engine's own scalability is in (post engine init), again whenever `sg.WaterQuality` changes, and, while it follows, whenever `sg.EffectsQuality` changes by any route (the console, a device profile, the settings menu). The status line (`fnb.Buoyancy 2`, the test bench panel) starts with the tier in use.

A tier is a section of console variables: `[WaterQuality@0]` … `[WaterQuality@3]` and `[WaterQuality@Cine]` in the plugin's `Config/DefaultFluidNinjaBuoyancy.ini`, each key an `fnb.*` variable flagged for scalability. **Every value is a cap or a scale on the project settings**, so a team's tuned values survive a tier change, and **Epic is the project settings as they are**: every cap 0 (the setting), every scale 1, waves on the 4 nearest simulations and own fields within 60 m.

| Console variable | Low | Medium | High | Epic (default) | Cinematic | What it does |
|---|---|---|---|---|---|---|
| `fnb.Capture.Scale` | 0.25 | 0.5 | 0.75 | 1 | 2 | Scales Capture Resolution: the Surface Mirror's texels across (1024 at the default: 256 / 512 / 768 / 1024 / 2048). A body's gather points go with the square: a crate takes about 1,300 at High, 2,200 at Epic, 8,800 at Cinematic. |
| `fnb.Capture.EveryNFrames` | 3 | 2 | 1 | 1 | 1 | The mirror is redrawn every n frames; between redraws the gather reads the last one. |
| `fnb.Gather.MaxGrid` | 32 | 64 | 96 | 0 | 0 | Caps Max Gather Grid, the grid points across a body's water (0: the setting, 128). |
| `fnb.Gather.BudgetPerFrame` | 16 | 64 | 128 | 0 | 0 | Bodies gathered in one frame at most; the nearest and fastest first, the rest keep their last patch (0: Max Gather Bodies alone). |
| `fnb.Gather.FarEveryNFrames` | 4 | 3 | 2 | 2 | 2 | Bodies beyond their first Lod Distance are gathered every n frames, beyond the second every 2n. |
| `fnb.Lod.Bias` | 2 | 1 | 0 | 0 | 0 | Every hull this many levels of detail coarser than its distance asks. |
| `fnb.Lod.DistanceScale` | 0.5 | 0.75 | 1 | 1 | 1.5 | Scales Lod Distances Cm for the hulls and the gather. |
| `fnb.Lod.SleepDistanceScale` | 0.5 | 0.75 | 0.85 | 1 | 1.5 | Scales Sleep Distance Cm. |
| `fnb.Wake.MaxPoints` | 64 | 128 | 192 | 0 | 0 | Caps Max Wake Points (0: the setting, 256). |
| `fnb.Waves.MaxImpulses` | 1024 | 4096 | 16384 | 0 | 0 | Caps Max Wave Impulses (0: the setting, 32768); past it neighbouring pieces are merged. |
| `fnb.Waves.ScaleDown` | 4 | 2 | 1 | 1 | 1 | The wave grid's scale-down: the waves run on the pressure buffer's cells over this (a tick costs about 1 / S²). |
| `fnb.Waves.MaxActiveSims` | 1 | 2 | 3 | 4 | 0 | Only this many simulations run waves: those whose nearest floating body or swimmer is nearest the camera (0: all). |
| `fnb.Waves.SelfFieldDistanceCm` | 1500 | 3000 | 4500 | 6000 | 0 | Bodies farther than this from the camera get no own field: they make waves and feel none (0: every body gets one). |
| `fnb.Waves.Foam` | 0 | 1 | 1 | 1 | 1 | The foam channel on the simulations' surfaces; the waves and the physics are the same either way. |
| `fnb.Effects.SplashBudgetPerFrame` | 2 | 4 | 6 | 8 | 16 | Splash systems all bodies together may spawn in one frame (-1: no limit). |
| `fnb.Effects.ReachScale` | 0.5 | 0.75 | 0.85 | 1 | 1.5 | Scales Lod Distances Cm for the effects alone: how far splashes, spray and crowns are drawn. |
| `fnb.Effects.Crown` | 0 | 0 | 1 | 1 | 1 | A slam of strength 4 or more throws a crown. |
| `fnb.Effects.WetLine` | 0 | 1 | 1 | 1 | 1 | The wet line is written. |

Low is "floats, drifts, no frills": a quarter-size mirror redrawn every third frame, far bodies on coarse hulls gathered rarely, the wave grid at a quarter, one wave simulation. Medium shows the waves on half the grid, with own fields only near the camera.

High is Epic's water on a three-quarter mirror with the far and crowded work trimmed. The 768 px mirror gives every body 0.56 of Epic's gather points, and the grid cap of 96 keeps the largest bodies in step (at the default Max Gather Grid, 128; a project that raised it is cut to 96 here, as Medium cuts it to 64). Three simulations run waves, own fields reach 45 m, the gather budget and the wake and impulse caps bind only in crowds, settled unseen bodies sleep from 51 m, and six splashes a frame are drawn to 0.85 of the distances. Every body keeps Epic's hull and the mirror is still redrawn every frame, so the hull's forces are Epic's: a body differs only in reading its water on the coarser mirror, and from 45 m a body makes waves but feels none.

**Epic is the calibrated defaults**, and the tier a desktop project runs when nothing is set. Cinematic, the premium tier, doubles the mirror (about four times the gather points), gives every body an own field, runs waves on every simulation, and pushes the hull, sleep and effect distances half as far again.

**Overriding a tier.** Two places, each the usual way:
- your project's `Config/DefaultFluidNinjaBuoyancy.ini`, with the same sections: the engine layers it on the plugin's file, so a key you set there replaces the plugin's for that tier (and `Config/<Platform>/<Platform>FluidNinjaBuoyancy.ini` does the same per platform);
- your project's `Config/DefaultScalability.ini`, with the same sections: the plugin applies them after its own, so a key there wins over both.

Earlier versions of the plugin had High (`[WaterQuality@2]`) hold the defaults and Epic double the mirror, so a stock desktop project ran the costlier tier. Overrides written for the defaults under `[WaterQuality@2]` belong under `[WaterQuality@3]` now, and those for the old Epic under `[WaterQuality@Cine]`; a project that pins `sg.WaterQuality 2` now gets the High saving tier, and pins `3` for the defaults.

Keys you leave out keep the plugin's values. A variable set from the console keeps its value over any tier until it is cleared (the console outranks scalability, as it does the engine's own groups), and a variable set in `DefaultEngine.ini`'s `[SystemSettings]` outranks the tiers too.

**Switching at runtime.** Every value is read where it is consumed, so a tier takes effect within a frame or two: the mirror re-sizes on its next redraw, the hulls change level over their 10 % band, the budget and the cadence on the next gather. The wave grid's scale-down starts the simulations' wave fields afresh, and a simulation `fnb.Waves.MaxActiveSims` keeps off starts afresh when it comes back, as a simulation does at Play; see [The wave grid](FluidNinjaWaves.md#the-wave-grid).

**What to look at.** With nothing set, the status line reads `tier Epic (sg.WaterQuality -1 follows sg.EffectsQuality 3); capture 80 m at 1024 px` on the default Capture Width and Resolution. `stat FluidNinjaBuoyancy`: at Low on the demo level the capture goes to 256 px, Bodies LOD 2 rises as the Lod Distances shrink, Bodies gathered falls to the budget with Bodies waiting and deferred taking the rest, Gather points falls with the grid cap, and Wave sims active reads 1. The status line's first words name the tier, and `fnb.Memory` shows the mirror and the waves' targets at their new sizes.

---

## Troubleshooting

| Symptom | Check |
|---|---|
| Bodies fall through the water | The surface actor lacks a [surface tag](#surface-tags). Or the body is outside the capture square with no water volume under it. Enable the debug view (`fnb.Buoyancy 2`) and look at the panel: "no capture" means no tagged surface under any body. |
| Bodies float at the wrong height | Do they float on the drawn surface? FluidNinja's material displaces it (waves, pressure), so the drawn surface can sit some cm off the mesh. Bodies over a river spline piece: is that piece in the capture? Steep pieces (falls) are left out on purpose. |
| No current | The surface material must have the `VelocityDensityBuffer` texture parameter set by FluidNinja. The panel's status lists each simulation found ("NOT PLACED" means its square is unknown). |
| Bodies drift too fast or too slowly | Use **Flow Scale Multiplier** (project), **Flow Coupling** (body) or `fnb.Buoyancy.CalibrateFlow`. |
| No wakes | Is DataChannelPointReaderEnabled on in the NinjaLive component? Are Wake on (component) and Wakes on (project)? Has `fnb.Buoyancy.Wake 0` been left set (set it back to -1, the setting)? |
| A body makes no ripples | Its **Wake** is off (the log says "Wake is off, so it floats but leaves no ripples"; the demo's light bodies are set so on purpose), or the NinjaLive component's DataChannelPointReaderEnabled is off. A body drifting with the current writes nothing by design; push it, drop it or row it. Don't turn on Generate Overlap Events for floating bodies instead: see [Interacting with FluidNinja](#interacting-with-fluidninja). |
| The current speeds up around floating things, up to Max Flow Speed | FluidNinja's own tracking paints them: they have Generate Overlap Events on and PhysicsBody is in the NinjaLive actor's **Overlap Filter Inclusive Obj Type**. Turn one of the two off. See [Interacting with FluidNinja](#interacting-with-fluidninja). |
| No wet line | The hull's material lacks `MF_FNB_HullWetness`, or Wet Line Data Index clashes with other Custom Primitive Data. |
| No sound | No sounds ship. See [Hull sounds](#hull-sounds). The wash also needs its Sound Wave set to Looping. |
| A body jitters or capsizes | Run **Analyze Hull**. A negative metacentric height capsizes: lower **Center Of Mass Offset Z**. For a cube between 21 % and 79 % draft, resting on an edge is correct. |
| "Shader Model 6 (SM6) is required to use Virtual Shadow Maps" | A project setting, not the plugin. Tick SM6 under **Project Settings → Platforms → Windows → D3D12 Targeted Shader Formats**, or turn Virtual Shadow Maps off. |
| A body launches out of something at the start | It starts inside other collision. Settle On Start sweeps it down from above, and Max Depenetration Speed limits the push; move it clear. |
| The mouse doesn't turn the camera in Play; a mouse cursor shows | FluidNinja's NinjaLive actor paints the water with the mouse by default (**User Input Based Interaction**: Mouse Single), and shows the cursor for it. Set it to **No User Input** on each NinjaLive actor, as the demo's are. |
| A body is "held" | It left the capture with no water volume under it. It floats again when its water is read. |
| No waves, or waves that look wrong | See [FluidNinja Waves: Troubleshooting](FluidNinjaWaves.md#troubleshooting). The waves line of Get Status, or of the test bench panel, says why a simulation runs none. |

---

## Known limitations

| Limitation | Detail |
|---|---|
| FluidNinja version | The plugin reads FluidNinja by name. If a future FluidNinja release renames these, update the project settings or the plugin:<br>• the `VelocityDensityBuffer`, `TraceMeshPos` and `TraceMeshSize` parameters;<br>• the `SimSpeed`, `SamplingFPS` and `ResolutionX` properties;<br>• the `NinjaLiveCore` Niagara system (its fixed tick sets the simulation steps per second);<br>• the `NinjaDataChannel1_Global` channel and its `Position`, `PositionPrevious` and `VelocityAndSize` attributes. |
| A GPU is required | The water is read from a GPU capture. A dedicated server, which does not render, has no live water. Bodies there fall back to water volumes. Multiplayer replication of floating bodies is not handled by the plugin. |
| Tested setup | Unreal Engine 5.8 on Windows with DirectX 12. Other engine versions need a rebuild and may need code changes. |
| One capture square | Up to Capture Width wide (80 m by default), centred on the simulation that follows the player, or else on the middle of all the bodies. Bodies farther away use water volumes. |
| Inside a boat | Water inside an open boat is not modelled. A hull floats on its whole outer shape, as if decked over. |
| FluidNinja Waves | See [FluidNinja Waves: Known limitations](FluidNinjaWaves.md#known-limitations). These include a slow build-up of river floaters at 60 fps that is still being investigated. |
