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
- the drag coefficients, Wake, Wet Line and the splash settings;
- the hull flow (**Displacement** or **Planing**) with its Planing Pressure Coefficient, and the spray sheets with their deadrise;
- when the preset's **Set Propulsion** is on, a propeller: the component's Propulsion turned on with the preset's engine, gear and propeller, placed by **Prop Location Share** (see [Motorboat fields](#motorboat-fields));
- the keel, by every preset: **Keel Area Cm2** and **Keel Depth Cm**, and the Keel Location placed by **Keel Location Share** (an area of 0 turns the component's keel off).

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
| **Motorboat** | From collision (25 cm) | Rest draft 14 cm | A 4–6 m runabout with a 60 kW outboard: Hull Flow Planing, so it climbs over the hump onto the plane; spray sheets and a rooster tail; wakes; splashing from 300 W. Set Propulsion writes the propeller model (5800 rpm, gear 2, a 28 cm propeller at pitch ratio 1.3, steerable thrust, rudder 500 cm² at 30°), placed by Prop Location Share (−1.05, 0, −1.6): just behind the transom and below the bottom. A 2000 cm² keel, 15 cm deep, at Keel Location Share (−0.35, 0, −1), on the keel line a third of the length from the stern, holds it in its turns. The mesh's bow must be +X. Drive it with Throttle (its sign shifts gear) and Rudder from Blueprint. |

Values not in the table are the preset class's defaults.

## Making your own

In the Content Browser, right-click → **Miscellaneous → Data Asset → FluidNinja Buoyancy Preset**. Its Details panel has the settings above under Hull, Mass, Balance, Hydrodynamics, Water, Spray and Motorboat, and a **Description** that the Apply Preset menu shows. For the density, water is 1000 kg/m³; pine 500, cork 240, ice 917, iron 7800; a beach ball about 8. Above 1000 it sinks.

## Motorboat fields

The fields a fast boat needs. Each is written to the component setting of the same name; the user guide's [Planing hulls](UserGuide.md#planing-hulls), [Keel](UserGuide.md#keel), [Propulsion](UserGuide.md#propulsion) and [Spray sheets](UserGuide.md#spray-sheets) say what those do. **Spray Sheets** and **Spray Deadrise Deg** are under Spray; the rest are under Motorboat.

| Field | Default | Range | What it writes, and when to change it |
|---|---|---|---|
| Spray Sheets | off | on / off | The component's Spray Sheets: whisker sheets off the chines and a rooster tail while the hull drives fast. Turn it on for fast hulls. |
| Spray Deadrise Deg | 18 | 0–45 | The component's Spray Deadrise Deg: the bottom's V, which sets how fast the whisker sheets leave (2 V sin of the deadrise). Set it to the hull's own: flat bottoms 0–10, runabouts 14–20, deep-V offshore hulls 22–26. |
| Hull Flow | Displacement | Displacement, Planing | The component's Hull Flow. Planing for a hull meant to rise onto the plane (the planing law, a ventilated transom and the hull's wave-making resistance); Displacement for everything else. Every shipped preset but Motorboat uses Displacement. |
| Planing Pressure Coefficient | 0.6 | 0.1–1.5 | The component's Planing Pressure Coefficient, k of the planing law p = k × ½ρV² × cos θ (Planing only). About 0.4 for a long, narrow wetted bottom, 0.8 for a short, wide one. Higher lifts the hull sooner and planes it flatter. |
| Set Propulsion | off | on / off | Gives the body a propeller: turns the component's Propulsion on (and Console Control off) with the fields below, Thrust Direction +X, and the propeller and rudder at Prop Location Share. Off leaves the component's propulsion as it is. The Motorboat preset turns it on. |
| Propulsion Model | Propeller | Simple, Propeller | The component's Propulsion Model. Propeller for an engine and propeller; Simple for a fixed push (Max Thrust Newtons stays as the component has it). |
| Engine Power Kw | 60 | 1–2000 | The engine's power at the rev limit (60 kW is an 80 hp outboard). More gives harder acceleration and a higher top speed with a propeller sized for it. |
| Engine Max Rpm | 5800 | 1000–15000 | The rev limit. An outboard: 5000–6300; an inboard: 3000–5500. |
| Gear Ratio | 2 | 0.25–10 | Engine turns per propeller turn. Higher pushes harder at low speed; lower is for top speed. |
| Prop Diameter Cm | 28 | 5–300 | The propeller's diameter (28 cm is an 11 in outboard propeller). Size it to the engine: too big and the engine never reaches its rev limit, too small and it over-revs. |
| Prop Pitch Ratio | 1.3 | 0.4–2 | Pitch over diameter. Higher is a faster propeller that is harder to turn; planing boats use 1.1–1.6. |
| Steerable Thrust | on | on / off | The component's Steerable Thrust: an outboard or sterndrive whose thrust turns with the rudder. Off: a fixed shaft and a rudder. The preset puts the rudder where the propeller is, so with it off, move the component's Rudder Location aft of the propeller after applying. |
| Prop Location Share | (−1.05, 0, −1.6) | any | Where the propeller goes (the component's Thrust Location, and its Rudder Location: the outboard's lower unit): shares of the mesh's half extents, at the component's scale, from the centre of the mesh's bounding box. X −1 is the stern (the bow is +X), Z −1 the bottom; beyond ±1 is outside the bounds. The default hangs an outboard's propeller just behind the transom and below the bottom of a runabout; change it for a drive placed elsewhere. |
| Rudder Area Cm2 | 500 | 0 or more | The component's Rudder Area Cm2: the side force of the lower unit or rudder. Bigger turns the boat harder. |
| Max Rudder Angle | 30 | 0–60 | The component's Max Rudder Angle, in degrees: how far the drive or rudder swings at Rudder ±1. |
| Keel Area Cm2 | 0 | 0 or more | The component's Keel Area Cm2, written by every preset whatever Set Propulsion says: the hull's lateral plane under the water (keel, skeg, pump housing, strakes) that resists sliding sideways. More area, less slide in the turns. 0 turns the keel off, as floating props want. The Motorboat preset uses 2000. A jet ski: 1000–2000; a 5 m runabout: 1500–3000; an 8 m RIB: 4000–8000. |
| Keel Location Share | (−0.35, 0, −1) | any | Where the keel's side force acts (the component's Keel Location, the hull's centre of lateral resistance): shares of the mesh's half extents from the centre of its bounding box, as Prop Location Share. The default is on the keel line about a third of the length from the stern; aft of the centre of mass the keel also keeps the boat running straight. |
| Keel Depth Cm | 15 | 1 or more | The component's Keel Depth Cm: the keel's vertical span, centred on its location. Its share under the water scales the keel's force. A jet ski or runabout: 10–20; an 8 m RIB: 20–30. |

Some of these defaults differ from the component's own: the preset's Propulsion Model is Propeller (the component's Simple), Steerable Thrust is on (off), Rudder Area Cm2 is 500 (900) and Max Rudder Angle is 30 (35). The component's other propeller settings (Engine Idle Rpm, Engine Response Seconds, Engine Inertia Kg M2, Prop Wake Fraction, Reverse Efficiency, Ventilated Thrust Factor, Left Handed Prop, the thrust and torque curves) and its whitewater (Prop Wash Foam, Stern Wash Foam) are left as they are.

## Requirements and gotchas

- **The object needs simple collision**: a box, sphere, capsule or convex hull. Complex (per-poly) collision is ignored, and a mesh without simple collision cannot have a hull built from it.
- **Static meshes only.** Skeletal meshes are swimmers, not bodies; see [Swimmers](FluidNinjaWaves.md#swimmers).
- **The arrow's +X.** A **Sphere Row** runs along the mesh's longest axis, and the centre-of-mass shift moves toward that axis' positive end. Model an arrow with its longest axis along +X, the head at +X.
- **Interaction is written only when the preset's Set Interaction is on.** Off, the component keeps the Interaction it already has. A preset made in one project should not hand bodies to FluidNinja's tracking in a project whose NinjaLives do not track; see [FluidNinja's own tracking, relative to the water](UserGuide.md#fluidninjas-own-tracking-relative-to-the-water).
- **Every preset writes Hull Flow, Planing Pressure Coefficient, Spray Sheets, Spray Deadrise Deg and the keel.** Applying BoatHull, or any preset but Motorboat, to a body set to Planing puts it back to Displacement, turns its spray sheets off and removes its keel (Keel Area Cm2 0). Only the propeller waits for Set Propulsion.
- **Set Propulsion turns Console Control off.** Drive the boat from Blueprint (Throttle, Rudder), or turn Console Control back on to test it with `fnb.Boat.Throttle` and `fnb.Boat.Rudder`.
- **The Motorboat's bow is +X.** Prop Location Share's X −1 is the mesh's −X end. A hull modelled with its bow along another axis gets its propeller and keel at the wrong end: turn the mesh, or set Thrust Location, Rudder Location and Keel Location on the component after applying.
- **Rain drops are not bodies.** A drop is smaller than a wave cell, so a hull cannot press the water. Use **Add Water Impulse** at each hit, or a poke from a Niagara collision, instead; see [Blueprint and C++ functions](FluidNinjaWaves.md#blueprint-and-c-functions).
- **Regenerating the library.** `Tools/make_presets.py` creates or updates every shipped preset from the values in the script, headless (`UnrealEditor-Cmd.exe <Project.uproject> -run=pythonscript -script="<path>/Tools/make_presets.py"`) or from the editor's Python console. It rewrites every value of the shipped presets, so keep your own presets elsewhere, in your project's content.
