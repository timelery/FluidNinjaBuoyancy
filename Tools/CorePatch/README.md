# FluidNinja core patch

The toolchain that turns FluidNinja LIVE's vendor files into the modified core this plugin needs, checks an installed core, and takes the modification off again. It lives here, in the plugin, so that any project with the plugin built can run it; nothing in it runs at play time.

| File | What |
|---|---|
| `apply_fluidninja_waves.py` | The script: steps `unapply`, `tracking`, `core`, `windsea`, `material`, `describe`, `check`, `inspect`, `verify` (below). |
| `FluidNinjaWaves/*.hlsl` | The HLSL of the five wave modules the `core` step builds (`//@include` snippets `_catmullrom`, `_drift`, `_source`, `_mask`, `_depth`, `_wash`, `_foamaged`, `_windsea` are expanded by the script). |
| `VERSIONS.md` | Every known core by sha8: base, interface version, build, the plugin commit that requires it, its zip. The sha8 / `core-<sha8>` convention. |
| `TRACKING.md` | The relative-tracking change (the part of the core still made by hand): what it is, what the helpers cover, what is missing. |
| `package_core.ps1` | Zips a project's modified core as `…_core-<sha8>.zip` with a README and hashes. |
| `../../Source/FluidNinjaBuoyancyEditor/Private/FNBDevLibrary.*` | The C++ helpers the script calls from Python (`unreal.FNBDevLibrary`): Niagara external edits, graph edits, stages, a synchronous material compile. Editor module; never in a cooked game. |

The layers of the modified core, in order:

1. **Relative tracking** ("pass3"): `NinjaLiveCore`, `Core/Functions/PainterSourceData` and `Core/Modules/Ninja_Painter`, changed by hand in the Niagara editor on the vendor 2.0.0.56 files (`Docs/RelativeTrackingProposal.md`). Shipped as `FluidNinjaLive_2.0.0.56_RelativeTracking_UE5.8.zip`, core `3bee517c`. The script cannot make this layer yet (`tracking`, TRACKING.md); it starts from it.
2. **The waves**: what `core` and `material` apply on top of pass3 (`Docs/FluidNinjaWaves.md`). The result is a new core (`e85f0659` today, interface version 6) and the SURFACE material function with the wave terms.

## Requirements

- A project with FluidNinja LIVE under `Content/FluidNinjaLive`, the Python Editor Script Plugin and Editor Scripting Utilities enabled, and this plugin **built** (a C++ project, or a prebuilt plugin: the editor module `FluidNinjaBuoyancyEditor` must load, because it carries `FNBDevLibrary`). The script says so when `unreal.FNBDevLibrary` is missing.
- The editor closed. Every step runs in a commandlet; `material` must run in a fresh process before any Play (FluidNinja material edits after PIE can crash the editor).
- `unapply` alone also runs under plain Python 3 (no editor), given `--project=<dir>`.

## Running

```
"C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "<project>.uproject" -run=pythonscript -script="<project>/Plugins/FluidNinjaBuoyancy/Tools/CorePatch/apply_fluidninja_waves.py <steps and options>" -unattended -nop4 -nosplash
```

Forward slashes in the script path. The engine finds the script by its `.py` extension, so spaces in its path need no quotes of their own; an option value with spaces does (`\"...\"` inside the `-script` value), or put the options in the environment variable `FNB_WAVES_ARGS` (and the steps in `FNB_WAVES_STEPS`). The commandlet reads `-script=` from the raw command line and takes a quoted value only when the quote follows the `=` directly: PowerShell, which wraps a whole argument with spaces in quotes (`"-script=C:/.../Unreal Projects/....py"`), cuts the path at its first space. Launch from `cmd` or a `.cmd` file (verbatim command line) with the steps and options in the environment variables.

The report is `<project>/Saved/FluidNinjaWaves_apply.txt`. Its last lines are the installed core's SHA-256, `CORE sha8 <x> tag core-<x>`, and `RESULT PASS` or `RESULT FAIL`. On FAIL the process exits non-zero (the commandlet reports a script error), so a build script can gate on it. The same lines go to the log, prefixed `FNBWAVES`.

Steps run in this fixed order whatever the command line's order: `unapply`, `tracking`, `core`, `windsea`, `material`, `describe`, `check`, `inspect`, `verify`. With no step given, `core material`.

| Step | Writes | What |
|---|---|---|
| `verify` | nothing | Reports the installed core's sha256 / sha8 and what VERSIONS.md knows it as; whether it is vendor, pass3 or waves-applied; `User.Wave.InterfaceVersion` and `CoreBuild`; the four wave stages and the `Ninja_WaveParams` module; the six `Core/Modules/Waves` assets with the build their hashes belong to; whether `PainterSourceData` / `Ninja_Painter` are the pass3 files; whether the SURFACE function carries the waves (`WaveHeightScale`, `WaveNormalScale`) the foam and choppiness (`WaveFoamBuffer`, `WaveFoamStrength`, `WaveChoppiness`) and the aged foam (`WaveFoamAgedColor`). PASS only when the core is waves-applied on the pass3 base at interface version 2 or later with everything present (a WARNING below the toolchain's own version, 6: a version 2 core runs the waves at scale-down 1, without foam and without the closed-window ⟨v⟩ pin, a version 3 one without foam or the pin, a version 4 one without the pin (`User.Wave.VMeanRelax`) and the in-step clamp of the mean-relax rates, so the plugin's automatic `fnb.Waves.MeanRelax` stays at 30 /s there, a version 5 one without the aged foam, the wash and the foam's own breaking slope; and a WARNING on a SURFACE function with the waves but not the foam, or with the foam but not the aged foam); every reason for a FAIL is a `VERIFY FAIL` line. |
| `core` | NinjaLiveCore, `Core/Modules/Waves/*` | Applies the waves to the core: user parameters, data interfaces, the `Ninja_WaveParams` module, the four stages, the modules built from `FluidNinjaWaves/*.hlsl`; compiles (GPU shaders included) and saves only when the compile is clean and the protected vendor files are unchanged. Refuses a core that already has the waves. Warns when the base is stock (the result then has no relative tracking). Stamps `User.Wave.InterfaceVersion` (6: the aged foam `FoamAged` with `User.Wave.FoamAgedDecay` / `.FoamTransfer`, the foam's spreading `.FoamDiffusion` / `.FoamAgedDiffusion`, its own breaking slope `.FoamSlope`, the plugin's wash `User.WaveWashTexture` with `.FoamWash`, and the foam target as (F1, F2, v, 0), all foam only, so (η, v) is 5's bit for bit; on top of 5's closed-window ⟨v⟩ pin and in-step clamp of both mean-relax rates, 4's foam channel and 3's wave grid scale-down) and `User.Wave.CoreBuild` (today, YYYYMMDD). |
| `windsea` | NinjaLiveCore, `Core/Modules/Waves/Ninja_WaveExport` | Interface version 7's wind sea alone, added in place to an installed interface 6 core (no `unapply`, so anything else that core carries stays): the user parameter `User.WaveWindSeaTexture`, the spawn DI `Emitter.WaveWindSea` bound to it, `Ninja_WaveExport`'s new inputs and HLSL (`_windsea.hlsl`: B += the table's long band), `Wave.InterfaceVersion` 7 and `Wave.CoreBuild`, the descriptions. Compiles and saves only when nothing new fails. Refuses a core below interface 6 or one that has the wind sea. The `core` step builds the same in from the pass3 core. |
| `material` | the SURFACE function | The waves: WPO += (0, 0, B × WaveHeightScale), normal += the slope of B. The foam (interface version 4): base colour lerped toward `WaveFoamColor` and roughness raised by FoamMask = saturate(`WaveFoamBuffer` × `WaveFoamStrength`), behind the on-by-default `WaveFoam` static switch, plus a `FoamMask` function output. The choppiness: WPO.xy += `WaveChoppiness` × ½ΔB × WaveHeightScale toward the crests, behind the off-by-default `WaveChoppy` static switch. The aged foam (interface version 6): AgedMask = saturate(`WaveFoamBuffer`.G × `WaveFoamAgedStrength`) behind the same `WaveFoam` switch, the base colour lerped toward `WaveFoamAgedColor` (a sub-surface turquoise) by it before the fresh foam's lerp, roughness unchanged. It adds what the function lacks: all of it on the vendor function, the foam, choppiness and aged foam on one with the waves only, the aged foam alone on an interface 4 / 5 function; it refuses only one that has the aged foam. Recompiles the vendor surface materials to check. |
| `describe` | NinjaLiveCore, the SURFACE function | On an already patched core and material: the descriptions (tooltips) of every parameter the toolchain adds, and where they are shown, so users tell them from FluidNinja's own. The core's user parameters go into the categories *FluidNinja Buoyancy: Waves*, *Wave Foam*, *Relative Tracking*, *Plugin Bindings* and *Core Version* of its user parameter hierarchy, all under a **FluidNinja Buoyancy** section: a NinjaLive's NinjaLiveComponent shows them as those groups under User Parameters, with a FluidNinja Buoyancy section button over them. The SURFACE function's parameters go into the material parameter groups *FluidNinja Buoyancy: Waves* and *FluidNinja Buoyancy: Wave Foam*. It fails when a parameter the toolchain adds is in no category. The core and material steps do the same. It changes both files' hashes (KNOWN_CORES, KNOWN_SURFACE, VERSIONS.md). |
| `check` | nothing | One line: waves present, base, interface version, build, stages. |
| `inspect` | `Saved/FluidNinjaWaves_inspect.txt` | Dumps the node graphs of `PainterSourceData` and `Ninja_Painter` (every node, named by what it is, every pin with its links), the PainterAndSim stages and the relative-tracking lines of the compiled GPU HLSL. For TRACKING.md. |
| `unapply` | NinjaLiveCore, PainterSourceData, Ninja_Painter, the SURFACE function; deletes `Core/Modules/Waves` | Back to the pass3 core. `--from=<zip or folder>` holds the pass3 NinjaLiveCore, PainterSourceData and Ninja_Painter; `--surface=<zip, folder or .uasset>` the vendor SURFACE function when `--from` has none. Every file's SHA-256 must match the known pass3 / vendor hash (`--allow-unknown` copies it anyway, with a WARNING). The replaced files are backed up to `Saved/FluidNinjaWaves_unapply_backup_<time>`. A file step: it runs first, before anything loads the assets, and refuses when the core is already loaded in the process. |
| `tracking` | nothing | Not available: reports why and fails. See TRACKING.md. |

Options: `--from=`, `--surface=`, `--allow-unknown` (unapply); `--project=<dir>` (outside the editor only).

## Typical runs

**Is this project's core right for this plugin build?**

```
... -script="<plugin>/Tools/CorePatch/apply_fluidninja_waves.py verify" ...
```

`RESULT PASS` and `CORE sha8 <x> tag core-<x>`: the core this plugin build requires (VERSIONS.md lists them: `1bed4654` is interface version 2, `9a11d0f1` interface 3, `da2548bd` interface 4, `9adbcbf6` / `6f4dcf53` / `d6a02f62` interface 5, `891573a6` / `e85f0659` interface 6, the current release `e85f0659` in `FluidNinjaLive_2.0.0.56_Waves_RelativeTracking_UE5.8_core-e85f0659.zip`). A `VERIFY FAIL interface version None; this plugin needs 2 or more` means a core from before interface version 2 (`43cc1890`, `97b976f6`): re-apply. A `WARNING interface version <n> is below this toolchain's 6` means the core works, without the wave grid's scale-down (below 3), without the foam channel (below 4), without the closed-window ⟨v⟩ pin, `User.Wave.VMeanRelax` (below 5), and without the aged foam, the wash and the foam's own breaking slope (below 6). Without the pin a walled window's level is held by the pin on the mean of s alone, at the plugin's automatic 30 /s. Re-apply when you want `fnb.Waves.ScaleDown` above 1, the foam, the ⟨v⟩ pin or the aged foam, or install the release zip: a `d6a02f62` core prints this WARNING until then.

**Re-apply the waves** (after a plugin change to the core, or to rebuild a core from the pass3 files):

```
... -script="<plugin>/Tools/CorePatch/apply_fluidninja_waves.py unapply --from=<folder>/FluidNinjaLive_2.0.0.56_RelativeTracking_UE5.8.zip --surface=<folder>/MF_NinjaOutput_BaseFunction_SURFACE.uasset core material verify" ...
```

One process: `unapply` copies the pass3 files and deletes the Waves folder before anything loads, `core` and `material` apply the waves, `verify` checks the result. Then open the editor once (the first open recompiles the shaders) and run the plugin's waves tests (`Docs/FluidNinjaWaves.md`, Testing). The new core's sha8 is on the report's last lines: record it in VERSIONS.md and the script's `KNOWN_*` tables, tag the plugin commit `core-<sha8>`.

**Back to the pass3 core only** (no editor needed):

```
python apply_fluidninja_waves.py unapply --from=<RelativeTracking zip> --surface=<vendor SURFACE .uasset> --project=<project dir>
```

**A new FluidNinja version.** There is no pass3 core for it yet. The relative-tracking change has to be made by hand first (`Docs/RelativeTrackingProposal.md`, "The code"; TRACKING.md says exactly which nodes), then `core material verify` on it. `inspect` on the current pass3 files shows the graph the hand edit has to reproduce.

## Versions

A core is named by its **sha8**, the first 8 hex digits of the SHA-256 of `NinjaLiveCore.uasset`; the plugin commit that requires a core is tagged **`core-<sha8>`**; a core zip is named `…_core-<sha8>.zip`. VERSIONS.md has every known core; the script's `KNOWN_CORES`, `KNOWN_PROTECTED`, `KNOWN_SURFACE` and `KNOWN_MODULES` are the same facts for `verify` and `unapply`. Keep both in step.

`package_core.ps1 -Project <dir> -OutDir <dir>` zips a project's four modified files and the Waves folder as `FluidNinjaLive_<version>_Waves_RelativeTracking_<engine>_core-<sha8>.zip`, with a README.txt naming the tag and every file's SHA-256. Run `verify` first. The zip carries no script: the toolchain is this folder, in the plugin.

## Changing the toolchain

- The module HLSL: edit `FluidNinjaWaves/*.hlsl`, re-apply (`unapply … core material verify`), test, record the new sha8. The interface with the plugin (`User.Wave.*`, the source texture, Wave Aux, the export) is versioned by `INTERFACE_VERSION` in the script and the plugin's `Waves.cpp`: bump both when they must change together.
- The helpers: `Source/FluidNinjaBuoyancyEditor/Private/FNBDevLibrary.h` documents each one (type keywords, script names, what addresses a simulation stage). They are plain `UFUNCTION(BlueprintCallable)` statics, reachable as `unreal.FNBDevLibrary.<snake_case_name>`.
- The development host (`FluidNinjaBuoyancyDev`) runs this folder through its `Plugins/FluidNinjaBuoyancy` junction into the plugin repository; it keeps no copy of the script or the helpers.
