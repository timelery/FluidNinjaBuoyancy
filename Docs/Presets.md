# FluidNinja Buoyancy: presets

A preset is what an object is made of and how it behaves in the water, kept apart from its shape: which kind of hull to build, where its mass comes from, which way up it rests, its drag, and its wake and splash settings. The shape is read from the object's own mesh when the preset is applied, so one **Crate** preset serves every crate, whatever its size, and one **BeachBall** every ball.

The plugin ships a library in `/FluidNinjaBuoyancy/Presets` (turn on **Settings → Show Plugin Content** in the Content Browser); the table below lists it.

## Applying a preset

| Where | How |
|---|---|
| A placed actor | Right-click it → **FluidNinja Buoyancy → Apply Preset →** the preset's name. |
| The component | **Preset** in the FluidNinja Buoyancy component's Details panel: pick a preset and apply it. |
| Blueprint, C++, Python | `UFluidNinjaBuoyancyPresetLibrary::ApplyPreset(Actor, Preset, OutReport)`, in Blueprint **Apply Buoyancy Preset**; **Apply Buoyancy Preset To Component** for a component that already exists. In Python, `unreal.FluidNinjaBuoyancyPresetLibrary.apply_preset(actor, preset)` gives back the report on success and `None` when nothing was applied (Unreal's Python convention for a function that returns a bool with an output). |

Applying reads the object's primitive (the component's **Primitive Name**, else the actor's root): its bounds and the volume of its simple collision. It then writes to the actor's FluidNinja Buoyancy component, adding one if there is none:

- **the hull**, built as the preset says: the simple collision's surface cut at the water line (**From Collision**), cubes filling it (**Voxels**), **One Sphere** fitted to the bounds, a **Sphere Row** along the longest axis, or a **Sphere Ring** in the widest plane;
- **the mass**: **Density** × the collision's volume, written as the primitive's mass override, so a bigger object of the same stuff weighs more; or the component's **Rest Draft** mode, so it floats that deep whatever it is; or **Keep Body Mass**, which leaves the mass alone;
- **the attitude**: as placed, longest axis flat (a log), or longest axis up (a buoy);
- **the centre of mass**, shifted along the longest axis by the preset's share of its half-length (an arrow's head);
- the drag coefficients, Wake, Wet Line and the splash settings.

In the editor the change is one undo step, and the actor is marked dirty. Everything the preset wrote stays editable on the component afterwards, as if you had typed it in. It fails, with the reason in the report, when the actor has no primitive, no simple collision, or no mesh the hull can be built from; otherwise the report says what was set: the volume found, the mass, the pontoons.

## The shipped presets

| Preset | Hull | Mass | For |
|---|---|---|---|
| **BeachBall** | One sphere | Density 8 kg/m³ (hollow: 0.8 kg at 60 cm) | A light ball that bobs and is carried by the current. It never sleeps and makes no wake, so it rides the water others push. |
| **Cannonball** | One sphere | Density 7800 kg/m³ (iron) | A ball that sinks, with a splash the size of its drop. |
| **WreckingBall** | One sphere | Density 7800 kg/m³ | The same iron, kept as its own preset so a crane's ball is found by name. |
| **Crate** | Voxels (64) | Density 180 kg/m³ (a 1 m crate: about 175 kg) | A wooden crate: floats high and flat, makes wakes, rides the current. |
| **Log** | From collision (20 cm) | Density 500 kg/m³ (pine) | A log: half under, longest axis flat, however the mesh was modelled. |
| **PineBlock** | Voxels (216) | Density 500 kg/m³ | A solid block that floats half under. |
| **CorkBlock** | Voxels (216) | Density 240 kg/m³ | A solid block that floats a quarter under. |
| **IceBlock** | Voxels (216) | Density 917 kg/m³ | A solid block with a tenth above the water. |
| **SteelBlock** | Voxels (216) | Density 7850 kg/m³ | A solid block that sinks. |
| **Arrow** | Sphere row (9) | Density 790 kg/m³ (30 g of 38 cm³) | An arrow: the centre of mass shifted 0.32 toward the head, so it floats nose down; a streamlined shaft's drag (pressure 0.05 / 0.08, suction 0.05 / 0.05, air 0.02); no settling, no sleep; splashes from 2 W. |
| **RingBuoy** | Sphere ring (8) | Rest draft 4.5 cm | A ring buoy carried by the current; no wake. |
| **AirMattress** | From collision (20 cm) | Rest draft 1.2 cm | A large, light, flat body that stable steps keep calm; no wake. |
| **Barrel** | From collision (15 cm) | Rest draft 25 cm | A barrel adrift: it lies on its side and makes wakes. |
| **BoatHull** | From collision (30 cm) | Rest draft 13 cm | A small boat's hull that makes wakes, splashing from 300 W. Propulsion (Throttle, Rudder) is left to the component. |

Values not in the table are the preset class's defaults.

## Making your own

In the Content Browser, right-click → **Miscellaneous → Data Asset → FluidNinja Buoyancy Preset**. Its Details panel has the settings above under Hull, Mass, Balance, Hydrodynamics, Water and Spray, and a **Description** that the Apply Preset menu shows. For the density, water is 1000 kg/m³; pine 500, cork 240, ice 917, iron 7800; a beach ball about 8. Above 1000 it sinks.

## Requirements and gotchas

- **The object needs simple collision**: a box, sphere, capsule or convex hull. Complex (per-poly) collision is ignored, and a mesh without simple collision cannot have a hull built from it.
- **Static meshes only.** Skeletal meshes are swimmers, not bodies; see [Swimmers](FluidNinjaWaves.md#swimmers).
- **The arrow's +X.** A **Sphere Row** runs along the mesh's longest axis, and the centre-of-mass shift moves toward that axis' positive end. Model an arrow with its longest axis along +X, the head at +X.
- **Interaction is written only when the preset's Set Interaction is on.** Off, the component keeps the Interaction it already has. A preset made in one project should not hand bodies to FluidNinja's tracking in a project whose NinjaLives do not track; see [FluidNinja's own tracking, relative to the water](UserGuide.md#fluidninjas-own-tracking-relative-to-the-water).
- **Rain drops are not bodies.** A drop is smaller than a wave cell, so a hull cannot press the water. Use **Add Water Impulse** at each hit, or a poke from a Niagara collision, instead; see [Blueprint and C++ functions](FluidNinjaWaves.md#blueprint-and-c-functions).
- **Regenerating the library.** `Tools/make_presets.py` creates or updates every shipped preset from the values in the script, headless (`UnrealEditor-Cmd.exe <Project.uproject> -run=pythonscript -script="<path>/Tools/make_presets.py"`) or from the editor's Python console. It rewrites every value of the shipped presets, so keep your own presets elsewhere, in your project's content.
