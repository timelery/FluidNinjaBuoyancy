# FluidNinja Buoyancy

Physics bodies that float on **FluidNinja LIVE** water exactly as it is drawn: they ride its waves, drift with its current, and push back into the simulation, so their bow waves and wakes rock everything around them.

FluidNinja Buoyancy is a code plugin for Unreal Engine that sits on top of FluidNinja LIVE. You add one component to anything that should float.

> **FluidNinja Buoyancy needs a modified FluidNinja core**: `FluidNinjaLive_2.0.0.56_Waves_RelativeTracking_UE5.8_core-e85f0659.zip` (core `e85f0659`, interface version 6), which has two changes. FluidNinja as it ships is not supported.
>
> 1. **Relative tracking.** Floating bodies stir the water through FluidNinja's own tracking: the plugin hands each body to it. Stock FluidNinja paints a tracked body's world velocity, which makes a current speed itself up around floaters. The change paints a tracked body's velocity relative to the water instead. Without it, bodies are not tracked and do not stir the water; there is no fallback. Details: [Proposal: FluidNinja tracking relative to the water](Docs/RelativeTrackingProposal.md).
> 2. **FluidNinja Waves.** Floating bodies and swimmers make real water waves in FluidNinja's own simulation. Without it, the plugin floats bodies as before and logs that the core has no waves. Details: [FluidNinja Waves](Docs/FluidNinjaWaves.md).

> FluidNinja LIVE is a separate product. It is not part of this repository, and you need it in your project, with the change above.

## Features

- **Floats on the water you see.** Every frame the plugin takes an orthographic depth capture of the water surface: FluidNinja's river spline pieces, pool planes and Niagara water sheets, including FluidNinja's pressure waves and tile displacement. Bodies float on that.
- **Currents.** It reads the simulation's velocity buffer, so bodies drift where the dye flows.
- **Stirs the water through FluidNinja's own tracking.** By default each body is handed to FluidNinja's tracking (the plugin turns its Generate Overlap Events on and adds the NinjaLive's tracking tag, if it uses one), and the modified FluidNinja paints its motion relative to the water, so bodies drifting with the current leave it as it is. A body can instead, or as well, write the plugin's own wakes into FluidNinja's point input (its Niagara Data Channel): bow waves, wakes and a ring when dropped.
- **Three hull models:**
  - **Triangles**, for boats and rafts. The hull is cut at the water line on every physics step and gets hydrostatic pressure, pressure drag, suction, skin friction and slamming.
  - **Voxels**, for crates, logs and debris.
  - **Spheres (pontoons)**, for balls and small or numerous props.
- **Motorboats.**
  - **Planing hulls.** The bottom lifts with its trim by the planing law, the transom ventilates, and the hull pays the wave-making resistance of its own pressure (Havelock), so a boat climbs a hump before it planes.
  - **A propeller model.** An engine's power and rev limit drive, through a gearbox, a propeller with open-water thrust and torque curves, so a boat finds its own acceleration and top speed. The throttle's sign shifts gear, the propeller ventilates when it breaks the surface, and its torque leans the hull.
  - Spray sheets off the chines and a rooster tail, a propeller trail in the foam, a hook for an engine sound, and a **Motorboat** preset. See [Planing hulls](Docs/UserGuide.md#planing-hulls) and [Propulsion](Docs/UserGuide.md#propulsion).
- **Real waves in FluidNinja's simulation (FluidNinja Waves).**
  - Each body's weight presses a hollow into the water, and slams and impacts push their impulse in.
  - The rings spread at the right speed for their length and the water's depth. They reflect off walls or are absorbed at open edges and pool gutters, ride the current, and break when too steep.
  - Every other floater rocks as they pass. A body never feels its own waves twice: its own field is taken out and its drag put back as radiation damping.
  - Foam: whitecaps, wake trails and impact rings. With a core at interface version 6 it also has a long-lived aged foam and takes in the bodies' whitewater: propeller trails, stern wash and where spray lands. `AddWaterFoam` whitens the water anywhere. See [Foam v2](Docs/UserGuide.md#foam-v2-aged-foam-and-whitewater).
- **Swimmers.** The player's pawn displaces water as it swims. Its hands, forearms, feet and head splash and press rings where they go in, sized by how fast. AI swimmers, divers and pawns on other movement systems join through `RegisterSwimmer`. Swimmers also write wake points and a plunge ring into FluidNinja's point input, and `GetCurrentAtLocation` gives your swim code the current around a swimmer (both work with Waves off too). `AddWaterImpulse` and `AddWavePoke` let anything else push the water: a cannonball, an arrow, a wave maker.
- **Water birds.** A body moved by its animation rather than by physics, such as a bird on root motion or Mover, gets its water contacts from the **FluidNinja Water Bird** component. Spheres on its bones hand the water the momentum they carry in and out of it, so a plunge dive, a landing, paddling and a head dip make concentric rings with fine ripples and a small droplet splash. The component also gives the bird's movement its floating height and bob frequency. See [Water birds](Docs/WaterBirds.md).
- **Stable at any frame rate.** Light bodies such as balls and air mattresses stay stable with Unreal's default physics, one step per frame at 30 fps or more, as well as with substepping or async physics. See [Physics and frame rate](Docs/UserGuide.md#physics-and-frame-rate).
- **Effects:**
  - a wet line on the hull (material function `MF_FNB_HullWetness`);
  - spray where the hull slams into the water (`NS_FNB_HullSplash`, or any Niagara system you choose). Every splash is on one project-wide scale, the cube root of the slam's power, so sizes compare across bodies and drop heights;
  - spray sheets and a rooster tail off fast hulls (`NS_FNB_HullSpray`, or any Niagara system with the same five user parameters);
  - hooks for three hull sounds (slap, wash and creak) and for an engine loop driven by the engine's rpm and load.
- **Scales up.** One GPU pass gathers every body's water. Hulls have distance levels of detail, and settled bodies far from the camera go to sleep.
- **One quality switch.** The scalability group `sg.WaterQuality` (Low to Cinematic; it follows `sg.EffectsQuality` by default) caps and scales the capture, the gather's budget and cadence, the hull and effect distances, the wave grid and the self fields in one go, with every tier a cvar section a project can override. See [Quality](Docs/UserGuide.md#quality).
- **Check Setup.** Tools → FluidNinja Buoyancy: Check Setup (or `fnb.CheckSetup`) checks the modified core, every NinjaLive's switches, the Wave Sims and the surface tags into a message log, with a Fix button on each failing line. See [Check Setup](Docs/UserGuide.md#check-setup).
- **Test bench.** An optional in-game panel and the `fnb.Test` console command run settle, capsize, drop, wind, throttle and readback tests, and write the results as JSON. It is its own module (`FluidNinjaBuoyancyDeveloper`, type DeveloperTool), so nothing of it reaches Test or Shipping builds.
- **Blueprint access.** `GetReadout`, `GetStatus`, the `OnSlam` and `OnWaterContactChanged` events, and Throttle and Rudder for propulsion, with the propeller's readouts (rpm, thrust, load, ventilation).

## Requirements

| | |
|---|---|
| Unreal Engine | 5.8. This is the version it was built and tested with. Other 5.x versions need a rebuild and may need small code changes. |
| Platform | Windows, DirectX 12. This is what it was tested on. |
| FluidNinja LIVE | 2.0.0.56, installed at its default location, `Content/FluidNinjaLive`, with the modified core: relative tracking and FluidNinja Waves (see Installation). |
| Project | C++. There are no prebuilt binaries, so the plugin is built from source. |
| Engine plugins | Niagara, Control Rig and ACL Plugin. They are enabled by default in UE5 projects; the demo character's Animation Blueprint uses the last two. |

## Installation

**1. The FluidNinja change**

The modified core comes as `FluidNinjaLive_2.0.0.56_Waves_RelativeTracking_UE5.8_core-e85f0659.zip` (zip SHA-256 `8f4876fcb2cc543ee8f8b9bf0386c52e84b0780716ea139eff476a8e5e279096`): core `e85f0659`, interface version 6, core build 20261019, the current release (tag `core-e85f0659`). The plugin still runs on an older FluidNinja Waves core (interface 2 or later), without the features its interface lacks; Check Setup lists them. It was made from FluidNinja LIVE 2.0.0.56, saved with UE 5.8, and is not in this repository. Its `README.txt` lists every file with its SHA-256.

1. Close Unreal Editor and back up these four files from your project:
   - `Content/FluidNinjaLive/NinjaLiveCore.uasset`
   - `Content/FluidNinjaLive/Core/Functions/PainterSourceData.uasset`
   - `Content/FluidNinjaLive/Core/Modules/Ninja_Painter.uasset`
   - `Content/FluidNinjaLive/OutputMaterials/Base/MF_NinjaOutput_BaseFunction_SURFACE.uasset`
2. Copy the zip's `Content` folder into your project folder, overwriting them. This also adds `Content/FluidNinjaLive/Core/Modules/Waves`.
3. Open the project. The first open recompiles NinjaLiveCore's shaders and FluidNinja's surface materials.
4. NinjaLiveCore's User Parameters now list `TrackVelocityRelativeToWater`, `WaveSimEnabled`, `WaveSourceTexture`, `WaveAuxTarget` and the `Wave.*` parameters. At their defaults FluidNinja works exactly as before.

For another FluidNinja version, the plugin's [Tools/CorePatch](Tools/CorePatch/README.md) re-applies the waves to the vendor files, checks an installed core (`verify`) and takes the waves off again (`unapply`); see [Re-applying](Docs/FluidNinjaWaves.md#re-applying-after-a-fluidninja-update). Cores are named by hash and the plugin commits that require them are tagged `core-<sha8>` ([VERSIONS.md](Tools/CorePatch/VERSIONS.md)). The relative-tracking edit is still made by hand, following [The code](Docs/RelativeTrackingProposal.md#the-code); [TRACKING.md](Tools/CorePatch/TRACKING.md) says what scripting it would take.

**2. The plugin, from source**

1. Clone the repository into `<YourProject>/Plugins/FluidNinjaBuoyancy`:
   ```
   git clone https://github.com/timelery/FluidNinjaBuoyancy.git Plugins/FluidNinjaBuoyancy
   ```
2. Right-click the `.uproject` and choose **Generate Visual Studio project files**, then build the editor target. Or open the project and let Unreal build the plugin.

**3. Every NinjaLive in a level**

With Play stopped, select each NinjaLive actor's **NinjaLiveComponent**:
- set **Selectively Kill Brush Density Keep Velocity** to 1 (or 4), so tracked bodies paint no density;
- under **User Parameters**, set `TrackVelocityRelativeToWater` to 1.

Bodies are handed over only while every NinjaLive in the level has both; otherwise the log names what is missing. Optionally, set **Live Interaction → Track Actor Primitive Components With Tag** to track bodies by tag; without one, the NinjaLive picks them by object type (its **Overlap Filter Inclusive Obj Type**, whose default includes PhysicsBody). **Tools → FluidNinja Buoyancy: Check Setup** checks both on every NinjaLive, with a Fix button for each.

## Quick start

1. In the Content Browser, turn on **Settings → Show Plugin Content**.
2. Open **FluidNinja Buoyancy Content → Demo → Maps → FNB_DemoPool**.
3. Set both settings on the level's two NinjaLive actors, as in Installation step 3.
4. Press **Play**. The log lists each floating body as handed to FluidNinja's own tracking.

The demo has two waters, both simulated by FluidNinja:
- **A 20 × 20 m pool with a wooden dock.** Floating on it are a rowing boat, two beach balls, a ring buoy, a crate and an air mattress.
- **A 45 m river beside it.** At its head wait a rowing boat, two crates, three beach balls, a barrel and a ring buoy. Click **Start river** (top left) or press **Enter**, and the current carries them down to the basin at the far end. **Reset** (or **R**) puts them back. **Lantern night** (or **L**) turns the lights off and sets 20 lit paper lanterns on the slow river to drift down it in the dark.

You play the UE5 mannequin:

| Action | Keyboard and mouse | Gamepad |
|---|---|---|
| Move | WASD | Left stick |
| Look | Mouse | Right stick |
| Jump | Space | Bottom face button |
| Swim up | Hold Space while swimming | Hold the bottom face button |
| Start or stop the river | Enter, or the panel's button | |
| Reset the river | R, or the panel's button | |
| Lantern night: lights off, 20 lit lanterns on the slow river | L, or the panel's button | `fnb.Demo.Night` tunes the dark live |
| Drop a heavy ball into the lanterns (during a lantern night, from anywhere) | B | `fnb.Demo.DropBall` |
| Mouse cursor, to click the river's panel | Tab | |

Walk out along the dock, push the floating things, or jump into the deep end and swim. While swimming, looking down and moving forward dives. The mouse looks around; to click the river's panel, press Tab for the mouse cursor (a click on a button goes back to looking). Type `fnb.TestHUD 1` in the console (`~`) to show the test bench panel.

## Making your own things float

1. Make sure the water surface actor carries one of the surface tags. FluidNinja's river spline pieces (`SplineSource`) and sea surfaces (`SeaSurface`) already do. For anything else, such as a pool plane or a lake mesh, add the tag `FluidNinjaBuoyancySurface`.
2. Add a **FluidNinja Buoyancy** component to an actor whose root component is a static mesh with simple collision (a box, sphere, capsule or convex hull).
3. Set **Rest Draft** to how deep it should sit in calm water. The plugin works out the mass from it.
4. Leave **Interaction** at **FluidNinja Tracking** and **Generate Overlap Events** off: the plugin turns overlap events on, and adds the NinjaLive's tracking tag, when it hands the body to FluidNinja's tracking. Its object type must be in the NinjaLive's object-type filter (PhysicsBody is, by default), or the NinjaLive must track by tag. See [FluidNinja's own tracking, relative to the water](Docs/UserGuide.md#fluidninjas-own-tracking-relative-to-the-water).
5. Press Play. The body waits until its water has been read, then is set down on it at its rest draft.

The Blueprints in `Demo/Blueprints` (`BP_FNB_Boat`, `BP_FNB_BeachBall`, `BP_FNB_Crate` and the rest) are ready-made examples you can drop into your own FluidNinja levels.

## Documentation

- [Illustrated architecture and buoyancy guide](Docs/ArchitectureAndBuoyancy/README.md) — modified core architecture, GPU/Chaos integration, physics and math, object recipes, interactive calculator, and 184-property reference. [Read the PDF](Docs/ArchitectureAndBuoyancy/FluidNinja_Architecture_and_Buoyancy_Guide.pdf).
- [User guide](Docs/UserGuide.md). It covers:
  - how the plugin works;
  - setting it up on your own FluidNinja water;
  - every setting;
  - hull sounds (how and where to add them), the wet line, spray and wakes;
  - motorboats: planing hulls, the propeller model, spray sheets, the engine sound and the aged foam;
  - physics and frame rate;
  - console commands and the test bench;
  - the demo level, its river switch and its character's push;
  - troubleshooting and known limitations.
- [Proposal: FluidNinja tracking relative to the water](Docs/RelativeTrackingProposal.md). It explains why stock FluidNinja's tracking speeds a current up around floating bodies, describes the change to FluidNinja the plugin needs, with measurements, and explains how to test it.
- [Presets](Docs/Presets.md). It describes the presets that give any mesh a material and a behaviour (a beach ball, a crate, a log, an arrow, a boat hull, a motorboat), the three ways to apply one, the shipped library and how to make your own.
- [Water birds](Docs/WaterBirds.md). The FluidNinja Water Bird component: water contacts for bodies moved by animation, such as diving and swimming birds. It covers setting up the parts, the momentum model, entries and splashes, the ring layer, floating the bird, tuning and diagnostics.
- [Performance](Docs/Performance.md). What the plugin costs, measured on the demo pool, an open-world level and an 11-tank test level, how to measure it, and what is not measured yet.
- [Core patch toolchain](Tools/CorePatch/README.md). The script that applies, verifies and removes the plugin's changes to FluidNinja's core, [the known cores by hash](Tools/CorePatch/VERSIONS.md) and [the relative-tracking gap](Tools/CorePatch/TRACKING.md).

## Repository layout

```
FluidNinjaBuoyancy.uplugin
Source/FluidNinjaBuoyancy          runtime module: component, subsystem, physics callback
Source/FluidNinjaBuoyancyDeveloper the test bench (fnb.Test, fnb.TestHUD); DeveloperTool, never in Test or Shipping builds
Source/FluidNinjaBuoyancyShaders   the GPU gather (global compute shader)
Source/FluidNinjaBuoyancyDemo      the demo level's player character, its push, game mode, river switch and lantern night (floating lantern, night lighting) (with Content/Demo, deletable as one piece; see the user guide, The demo level)
Source/FluidNinjaBuoyancyEditor    editor module: the presets UI, Check Setup, and FNBDevLibrary, the Python helpers of Tools/CorePatch
Config/DefaultFluidNinjaBuoyancy.ini  the quality tiers of sg.WaterQuality ([WaterQuality@0..3], [WaterQuality@Cine])
Tools/CorePatch                    the FluidNinja core patch: apply / verify / unapply script, module HLSL, VERSIONS.md, TRACKING.md
Shaders/Private                    FluidNinjaBuoyancyGather.usf
Content/Materials                  MF_FNB_HullWetness (wet line)
Content/Effects                    NS_FNB_HullSplash (spray)
Content/Audio                      SA_FNB_Hull (hull sound attenuation; no sounds ship)
Content/Demo                       demo meshes, materials, Blueprints, the paper lantern (Demo/Lantern), the UE5 mannequin and the FNB_DemoPool level
Docs/UserGuide.md
LICENSE
```

## License

FluidNinja Buoyancy is released under the [MIT License](LICENSE). You can use, copy, change and redistribute it, including in commercial projects, as long as you keep the copyright and license notice.

The MIT License covers this repository's code, shaders, documentation and its own content. It does not cover:
- **The UE5 mannequin** in `Content/Demo/Characters/Mannequins` (Manny, Quinn and the Third Person template's animations). These are Epic Games content from the engine's templates and stay under the Unreal Engine EULA.
- **FluidNinja LIVE.** It isn't included here and keeps its own license.
