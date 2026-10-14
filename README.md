# FluidNinja Buoyancy

Physics bodies that float on **FluidNinja LIVE** water exactly as it is drawn: they ride its waves, drift with its current, and push back into the simulation, so their bow waves and wakes rock everything around them.

FluidNinja Buoyancy is a code plugin for Unreal Engine that sits on top of FluidNinja LIVE. You don't change FluidNinja's assets. You add one component to anything that should float.

> FluidNinja LIVE is a separate product. It is not part of this repository, and you need it in your project.

## Features

- **Floats on the water you see.** Every frame the plugin takes an orthographic depth capture of the water surface: FluidNinja's river spline pieces, pool planes and Niagara water sheets, including FluidNinja's pressure waves and tile displacement. Bodies float on that.
- **Currents.** It reads the simulation's velocity buffer, so bodies drift where the dye flows.
- **Wakes.** Moving hulls write their bow waves and wakes into FluidNinja's point input (its Niagara Data Channel), and a dropped body writes a ring. The simulation turns them into ripples and foam. Only motion through the water is written, so bodies drifting with the current leave it as it is.
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
| FluidNinja LIVE | Installed at its default location, `Content/FluidNinjaLive`. Tested with the FluidNinja LIVE release whose `NinjaLiveComponent` reports version 1.9. |
| Project | C++ or Blueprint-only. Blueprint-only projects need the prebuilt binaries from a release. |
| Engine plugins | Niagara, Control Rig and ACL Plugin. They are enabled by default in UE5 projects; the demo character's Animation Blueprint uses the last two. |

## Installation

**From a release (prebuilt binaries)**

1. Download `FluidNinjaBuoyancy-<version>-UE5.8-Win64.zip` from the Releases page.
2. Unzip it into your project's `Plugins` folder, so you have `<YourProject>/Plugins/FluidNinjaBuoyancy/FluidNinjaBuoyancy.uplugin`.
3. Open the project. The plugin is enabled by default. If it isn't, enable it under **Edit → Plugins → Physics → FluidNinja Buoyancy**.

**From source**

1. Clone or copy this repository into `<YourProject>/Plugins/FluidNinjaBuoyancy`.
2. Right-click the `.uproject` and choose **Generate Visual Studio project files**, then build the editor target. Or open the project and let Unreal build the plugin.

## Quick start

1. In the Content Browser, turn on **Settings → Show Plugin Content**.
2. Open **FluidNinja Buoyancy Content → Demo → Maps → FNB_DemoPool**.
3. Press **Play**.

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
4. Leave **Wake** on: that is how the body stirs the simulation. Leave **Generate Overlap Events** off on it, as a StaticMeshActor has it. FluidNinja's own overlap tracking paints a body's world velocity, so a floater carried by the current would speed that current up. See [Interacting with FluidNinja](Docs/UserGuide.md#interacting-with-fluidninja).
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
- [Proposal: FluidNinja tracking relative to the water](Docs/RelativeTrackingProposal.md). It explains why floating bodies stay off FluidNinja's own tracking today, proposes a change to FluidNinja, and explains how to test it with this branch.

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
