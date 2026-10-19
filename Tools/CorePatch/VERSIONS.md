# Core versions

The modified FluidNinja LIVE core (NinjaLiveCore with relative tracking and the waves) is versioned by its file hash, not by a name:

- **sha8**: the first 8 hex digits of the SHA-256 of `Content/FluidNinjaLive/NinjaLiveCore.uasset`. `apply_fluidninja_waves.py` prints it on its last lines (`CORE sha8 <x> tag core-<x>`), after every step, and `verify` says what this table knows the installed core as.
- **Release tag**: a plugin commit that requires a core is tagged `core-<sha8>` in the plugin repository (`git tag core-1bed4654 <commit>`). A plugin build and a core match when the plugin's tag names the installed core.
- **Zip name**: a core zip is named `FluidNinjaLive_<FluidNinja version>_Waves_RelativeTracking_<engine>_core-<sha8>.zip` from now on (`package_core.ps1` does that). The two 2026-10-17 zips share one name; the hash tells them apart.
- **Where the hashes live**: here, for people, and in the script's `KNOWN_CORES`, `KNOWN_PROTECTED`, `KNOWN_SURFACE` and `KNOWN_MODULES`, for `verify` and `unapply`. Add a new build to both.

The "Plugin support" column says what this plugin build does with each core (it needs interface version 2 or later).

## NinjaLiveCore builds

| sha8 | Base | `User.Wave.InterfaceVersion` | `User.Wave.CoreBuild` | Plugin support | Zip | Notes |
|---|---|---|---|---|---|---|
| `7b9ed72f` | vendor: stock FluidNinja LIVE 2.0.0.56 (saved by UE 5.6) | – | – | refused (no relative tracking, no waves) | the vendor package | The file every untouched FluidNinja 2.0.0.56 project has. |
| `3bee517c` | pass3: relative tracking, made by hand in the Niagara editor on the vendor core (`Docs/RelativeTrackingProposal.md`) | – (no waves) | – | refused (no waves); the base every waves core below is applied on | `FluidNinjaLive_2.0.0.56_RelativeTracking_UE5.8.zip` | Goes with PainterSourceData `9986466c` and Ninja_Painter `9f2c488c` (both hand-made). What `unapply` puts back. |
| `43cc1890` | pass3 + waves (no interface parameters) | none | none | refused: re-apply (interface version 2 or later required) | `FluidNinjaLive_2.0.0.56_Waves_RelativeTracking_UE5.8.zip`, first 2026-10-17 build | SURFACE function `6e8fde9b`. |
| `97b976f6` | pass3 + waves, the current averaged over a few cells before it carries the waves (`_drift.hlsl`), no interface parameters | none | none | refused: re-apply (interface version 2 or later required) | `FluidNinjaLive_2.0.0.56_Waves_RelativeTracking_UE5.8.zip`, second 2026-10-17 build (same name as the first) | SURFACE function `6e8fde9b`. |
| `1bed4654` | pass3 + waves, interface version 2 (breaking judged on η, the sponge on η, `Wave.MeanRelax`) | 2 | 20261017 | runs at scale-down 1, without foam and without the ⟨v⟩ pin | none (would be `…_UE5.8_core-1bed4654.zip`) | SURFACE function `7c8c7e99`: the same material edit as `6e8fde9b`, re-saved. |
| `9a11d0f1` | pass3 + waves, interface version 3 (the wave grid decoupled from the pressure buffer: `User.Wave.ScaleDown`, `Emitter.Wave.ScaleDown` / `.Cells` / `.CellCm`, the block mask and depth, bilinear export; Wave Breaking Slope default 1.0) | 3 | 20261017 | runs with the scale-down, without foam and without the ⟨v⟩ pin | none (would be `…_UE5.8_core-9a11d0f1.zip`) | SURFACE function `b564724a` (the same material edit, re-saved). Modules: Params `7adbcf3f`, Prepare `584bbbc2`, Step `c1232230`, Export `75b0c5c3`, Aux `3a85de66`, RT_WaveAux_Unbound `a69738ba`. |
| `da2548bd` | pass3 + waves, interface version 4 (the foam channel: the Foam attribute on the wave grid, `User.WaveFoamTarget` + `Emitter.WaveFoam`, `User.Wave.Foam` / `.FoamDecay` / `.FoamBreaking` / `.FoamWake` / `.FoamImpact`, the source texture's A = the hull's speed through the water; the SURFACE function gains the foam — `WaveFoam`, `WaveFoamBuffer`, `WaveFoamStrength`, `WaveFoamColor`, the `FoamMask` output — and the choppiness — `WaveChoppy`, `WaveChoppiness`) | 4 | 20261018 | runs with the foam, without the ⟨v⟩ pin | none (would be `…_UE5.8_core-da2548bd.zip`) | The foam texture needs a Linear Color sampler. SURFACE function `59974a79`. Modules: Params `4a767f3e`, Prepare `7b563ba4`, Step `3171b915`, Export `74d77c0f`, Aux `9aced596`, RT_WaveAux_Unbound `ebe29e62`. At `User.Wave.Foam` 0 (η, v) is bit for bit what interface 3 computes. |
| `9adbcbf6` | pass3 + waves, interface version 5 (`User.Wave.VMeanRelax`: the closed-window ⟨v⟩ pin, which relaxes a walled window's mean vertical velocity toward 0; both mean-relax rates clamped in Wave Step against its Dt, `Wave.MeanRelax` to 0.5 / Dt and `Wave.VMeanRelax` to 1 / Dt, each only past a 0.01 % margin) | 5 | `20261018` | the full interface | `FluidNinjaLive_2.0.0.56_Waves_RelativeTracking_UE5.8_core-9adbcbf6.zip` (2026-10-18, zip SHA-256 `d2fb1ed2`) | SURFACE function `3a19e7cd` (the same material edit as `59974a79`, re-saved: the material step re-runs after unapply). Modules: Params `59c42b52`, Prepare `38ce28b9`, Step `96806186`, Export `ce5abc17`, Aux `9fee8fd2`, RT_WaveAux_Unbound `01902f23`. At `User.Wave.VMeanRelax` 0 (the asset default) and `Wave.MeanRelax` up to 0.5 / Dt, equality included, (η, v) is bit for bit what interface 4 computes: the margin lets a rate the plugin already clamped to 0.5 / its tick on the CPU pass through untouched, instead of being swapped for the GPU's own rounded 0.5 / Dt. A plugin that does not set `User.Wave.VMeanRelax` therefore runs on it as on `da2548bd`. |
| `6f4dcf53` | `9adbcbf6` with the descriptions (tooltips) of its user parameters, written by the `describe` step; stages, modules and arithmetic unchanged | 5 | 20261018 | the full interface | none (`9adbcbf6`'s zip runs the same; a new zip would carry the descriptions) | |
| `d6a02f62` | `6f4dcf53` with its 26 custom user parameters in the categories *FluidNinja Buoyancy: Waves*, *Wave Foam*, *Relative Tracking*, *Plugin Bindings* and *Core Version* of its user parameter hierarchy, under a **FluidNinja Buoyancy** section (the `describe` step's categorizing), so a NinjaLive's User Parameters show them apart from FluidNinja's own; stages, modules and arithmetic unchanged | 5 | 20261018 | the full interface (current release) | `FluidNinjaLive_2.0.0.56_Waves_RelativeTracking_UE5.8_core-d6a02f62.zip` (2026-10-19) | Made by `describe` from `6f4dcf53` / `b55966c6`. SURFACE function `1225f204`. A Niagara re-save changes the core's hash even when nothing in it changed, so a second `describe` run gives another hash with the same content: copy this file rather than re-running. Hashes seen while building it, never released: `2de43d49`, `7b5353c1`, `ad9d81f3`, `c4f5ad0a`. |

Intermediate builds that were never a release: `605940bc` (before the Wave Aux stage) and `8c621f9c` (before `RT_WaveAux_Unbound`). `verify` reports them as unknown.

## The other files a core goes with

| File | sha8 | What |
|---|---|---|
| `Core/Functions/PainterSourceData.uasset` | `79a2eab0` | vendor |
| | `9986466c` | pass3 (hand-made; every core from `3bee517c` on) |
| `Core/Modules/Ninja_Painter.uasset` | `eb48a5d0` | vendor |
| | `9f2c488c` | pass3 (hand-made; every core from `3bee517c` on) |
| `OutputMaterials/Base/MF_NinjaOutput_BaseFunction_SURFACE.uasset` | `79803426` | vendor (what `unapply` puts back) |
| | `6e8fde9b` | with the waves, cores `43cc1890` and `97b976f6` |
| | `7c8c7e99` | with the waves, core `1bed4654` |
| | `b564724a` | with the waves, core `9a11d0f1` |
| | `59974a79` | with the waves, the foam and the choppiness, core `da2548bd` (interface version 4) |
| | `3a19e7cd` | the same edit, re-saved by the interface-5 apply, core `9adbcbf6` (interface version 5) |
| | `b55966c6` | `3a19e7cd` with the descriptions (tooltips) of its parameters, written by the `describe` step, core `6f4dcf53` |
| | `1225f204` | `b55966c6` with its 8 parameters in the material parameter groups *FluidNinja Buoyancy: Waves* and *FluidNinja Buoyancy: Wave Foam* (they were in *Waves*), core `d6a02f62`. `describe` re-saves it only when a description or group differs, so a second run leaves it as it is. |
| `Core/Modules/Waves/*` | per build | `KNOWN_MODULES` in the script lists the six assets of each waves build; `verify` prints which build each installed module came from. |

Full SHA-256 values: the script's tables (`KNOWN_*`). Compute one with `Get-FileHash -Algorithm SHA256 <file>` or `verify`.

## Adding a build

1. Run `core material` on the pass3 files (README.md). The report's last lines give the new sha8.
2. Add the core, its SURFACE function and its six `Core/Modules/Waves` assets to the script's `KNOWN_*` tables and to the tables above.
3. Tag that plugin commit `core-<sha8>` and package the zip with `package_core.ps1`.
