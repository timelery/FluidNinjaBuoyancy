# FluidNinja Buoyancy

Physics bodies that float on **FluidNinja LIVE** water exactly as it is drawn: they ride its waves, drift with its current, and push back into the simulation, so their bow waves and wakes rock everything around them.

FluidNinja Buoyancy is a code plugin for Unreal Engine that sits on top of FluidNinja LIVE. You add one component to anything that should float.

> **This branch (`fluidninja-relative-tracking`) needs a modified FluidNinja.** Here floating bodies stir the water through FluidNinja's own tracking: the plugin hands each body to it. Stock FluidNinja paints a tracked body's world velocity, which makes a current speed itself up around floaters, so this branch needs a change to three FluidNinja core assets (`NinjaLiveCore`, `PainterSourceData`, `Ninja_Painter`) that paints a tracked body's velocity relative to the water instead. Without it, bodies on this branch are not tracked and do not stir the water; there is no fallback. The `main` branch works with FluidNinja as it is, through the plugin's own wakes. The change, its measurements and how to test it: [Proposal: FluidNinja tracking relative to the water](Docs/RelativeTrackingProposal.md).

> FluidNinja LIVE is a separate product. It is not part of this repository, and you need it in your project, with the change above.

## Features

- **Floats on the water you see.** Every frame the plugin takes an orthographic depth capture of the water surface: FluidNinja's river spline pieces, pool planes and Niagara water sheets, including FluidNinja's pressure waves and tile displacement. Bodies float on that.
- **Currents.** It reads the simulation's velocity buffer, so bodies drift where the dye flows.
- **Stirs the water through FluidNinja's own tracking.** By default each body is handed to FluidNinja's tracking (the plugin turns its Generate Overlap Events on and adds the NinjaLive's tracking tag, if it uses one), and the modified FluidNinja paints its motion relative to the water, so bodies drifting with the current leave it as it is. A body can instead, or as well, write the plugin's own wakes into FluidNinja's point input (its Niagara Data Channel): bow waves, wakes and a ring when dropped.
- **Three hull models:**
  - **Triangles**, for boats and rafts. The hull is cut at the water line on every physics step and gets hydrostatic pressure, pressure drag, suction, skin friction and slamming.
  - **Voxels**, for crates, logs and debris.
  - **Spheres (pontoons)**, for balls and small or numerous props.
- **Stable at any frame rate.** Light bodies such as balls and air mattresses stay stable with Unreal's default physics, one step per frame at 30 fps or more, as well as with substepping or async physics. See [Physics and frame rate](Docs/UserGuide.md#physics-and-frame-rate).
- **Effects:**
  - a wet line on the hull (material function `MF_FNB_HullWetness`);
  - spray where the hull slams into the water (`NS_FNB_HullSplash`, or any Niagara system you choose);
  - hooks for three hull sounds: slap, wash and creak.
- **Scales up.** One GPU pass gathers every body's water. Hulls have distance levels of detail, and settled bodies far from the camera go to sleep.
- **Test bench.** An optional in-game panel and the `fnb.Test` console command run settle, capsize, drop, wind, throttle and readback tests, and write the results as JSON.
- **Blueprint access.** `GetReadout`, `GetStatus`, the `OnSlam` and `OnWaterContactChanged` events, and Throttle and Rudder for simple propulsion.

## Requirements

| | |
|---|---|
| Unreal Engine | 5.8. This is the version it was built and tested with. Other 5.x versions need a rebuild and may need small code changes. |
| Platform | Windows, DirectX 12. This is what it was tested on. |
| FluidNinja LIVE | 2.0.0.56, installed at its default location, `Content/FluidNinjaLive`, with the relative-tracking change to three core assets (see Installation). |
| Project | C++. There are no prebuilt binaries for this branch, so the plugin is built from source. |
| Engine plugins | Niagara, Control Rig and ACL Plugin. They are enabled by default in UE5 projects; the demo character's Animation Blueprint uses the last two. |

## Installation

**1. The FluidNinja change**

1. Close Unreal Editor and back up these three files from your project:
   - `Content/FluidNinjaLive/NinjaLiveCore.uasset`
   - `Content/FluidNinjaLive/Core/Functions/PainterSourceData.uasset`
   - `Content/FluidNinjaLive/Core/Modules/Ninja_Painter.uasset`
2. Replace them with the modified versions (made from FluidNinja LIVE 2.0.0.56 and saved with UE 5.8; they are not in this repository), or make the same edit by hand in the Niagara editor, following [The code](Docs/RelativeTrackingProposal.md#the-code) in the proposal.
3. Open the project. The first open recompiles NinjaLiveCore's shaders. NinjaLiveCore's User Parameters now list `TrackVelocityRelativeToWater`; at 0, its default, FluidNinja paints exactly as before.

**2. The plugin, from source**

1. Clone this branch into `<YourProject>/Plugins/FluidNinjaBuoyancy`:
   ```
   git clone -b fluidninja-relative-tracking https://github.com/timelery/FluidNinjaBuoyancy.git Plugins/FluidNinjaBuoyancy
   ```
2. Right-click the `.uproject` and choose **Generate Visual Studio project files**, then build the editor target. Or open the project and let Unreal build the plugin.

**3. Every NinjaLive in a level**

With Play stopped, select each NinjaLive actor's **NinjaLiveComponent**:
- set **Selectively Kill Brush Density Keep Velocity** to 1 (or 4), so tracked bodies paint no density;
- under **User Parameters**, set `TrackVelocityRelativeToWater` to 1.

Bodies are handed over only while every NinjaLive in the level has both; otherwise the log names what is missing. Optionally, set **Live Interaction → Track Actor Primitive Components With Tag** to track bodies by tag; without one, the NinjaLive picks them by object type (its **Overlap Filter Inclusive Obj Type**, whose default includes PhysicsBody).

## Quick start

1. In the Content Browser, turn on **Settings → Show Plugin Content**.
2. Open **FluidNinja Buoyancy Content → Demo → Maps → FNB_DemoPool**.
3. Set both settings on the level's two NinjaLive actors, as in Installation step 3.
4. Press **Play**. The log lists each floating body as handed to FluidNinja's own tracking.

The demo has two waters, both simulated by FluidNinja:
- **A 20 × 20 m pool with a wooden dock.** Floating on it are a rowing boat, two beach balls, a ring buoy, a crate and an air mattress.
- **A 45 m river beside it.** At its head wait a rowing boat, two crates, three beach balls, a barrel and a ring buoy. Click **Start river** (top left) or press **Enter**, and the current carries them down to the basin at the far end. **Reset** (or **R**) puts them back.

You play the UE5 mannequin:

| Action | Keyboard and mouse | Gamepad |
|---|---|---|
| Move | WASD | Left stick |
| Look | Mouse | Right stick |
| Jump | Space | Bottom face button |
| Swim up | Hold Space while swimming | Hold the bottom face button |
| Start or stop the river | Enter, or the panel's button | |
| Reset the river | R, or the panel's button | |
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

- [User guide](Docs/UserGuide.md). It covers:
  - how the plugin works;
  - setting it up on your own FluidNinja water;
  - every setting;
  - hull sounds (how and where to add them), the wet line, spray and wakes;
  - physics and frame rate;
  - console commands and the test bench;
  - the demo level, its river switch and its character's push;
  - troubleshooting and known limitations.
- [Proposal: FluidNinja tracking relative to the water](Docs/RelativeTrackingProposal.md). It explains why stock FluidNinja's tracking speeds a current up around floating bodies, describes the change to FluidNinja this branch needs, with measurements, and explains how to test it.

## Repository layout

```
FluidNinjaBuoyancy.uplugin
Source/FluidNinjaBuoyancy          runtime module: component, subsystem, physics callback, test bench
Source/FluidNinjaBuoyancyShaders   the GPU gather (global compute shader)
Source/FluidNinjaBuoyancyDemo      the demo level's player character, its push, game mode and river switch
Shaders/Private                    FluidNinjaBuoyancyGather.usf
Content/Materials                  MF_FNB_HullWetness (wet line)
Content/Effects                    NS_FNB_HullSplash (spray)
Content/Audio                      SA_FNB_Hull (hull sound attenuation; no sounds ship)
Content/Demo                       demo meshes, materials, Blueprints, the UE5 mannequin and the FNB_DemoPool level
Docs/UserGuide.md
LICENSE
```

## License

FluidNinja Buoyancy is released under the [MIT License](LICENSE). You can use, copy, change and redistribute it, including in commercial projects, as long as you keep the copyright and license notice.

The MIT License covers this repository's code, shaders, documentation and its own content. It does not cover:
- **The UE5 mannequin** in `Content/Demo/Characters/Mannequins` (Manny, Quinn and the Third Person template's animations). These are Epic Games content from the engine's templates and stay under the Unreal Engine EULA.
- **FluidNinja LIVE.** It isn't included here and keeps its own license.
