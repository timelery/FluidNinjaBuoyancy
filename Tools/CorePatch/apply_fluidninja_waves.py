"""FluidNinja Waves core patch: applies, verifies and removes the FluidNinja Buoyancy layer on FluidNinja LIVE's NinjaLiveCore and the
SURFACE output material function. It lives in the plugin (Tools/CorePatch) and runs in any project that has FluidNinja LIVE, the Python
Editor Script Plugin, Editor Scripting Utilities and this plugin built: the plugin's editor module (FluidNinjaBuoyancyEditor) carries the
FNBDevLibrary helpers it calls (unreal.FNBDevLibrary). README.md next to this file has the full instructions, VERSIONS.md the cores.

Steps, run in this order whatever the command line's order (several can be combined):
  unapply   Back to the pass3 (relative tracking) core: --from=<zip or folder> holds the pass3 NinjaLiveCore, PainterSourceData and
            Ninja_Painter (FluidNinjaLive_2.0.0.56_RelativeTracking_UE5.8.zip), --surface=<zip, folder or .uasset> the vendor SURFACE
            function (when --from has none). Each file's SHA-256 is checked against the known hashes (KNOWN_*; --allow-unknown copies
            anyway, with a WARNING). The current files are backed up to Saved/FluidNinjaWaves_unapply_backup_<time>, the four files are
            copied over and Content/FluidNinjaLive/Core/Modules/Waves is deleted. A file step: it runs before anything loads the assets,
            and under plain Python too (python apply_fluidninja_waves.py unapply --from=... --project=<project dir>).
  tracking  Vendor -> pass3 (the relative-tracking change). NOT AVAILABLE: that change is a Niagara graph edit this toolchain cannot
            reproduce yet; TRACKING.md has what it consists of and what is missing. The step reports that and FAILs.
  core      NinjaLiveCore / PainterAndSim, edited in place, inert at the defaults (stages gated off, 1 x 1 grids, RT_Pressure stays RG16f):
            - user parameters User.WaveSimEnabled, User.WaveSourceTexture and User.Wave.* (see USER_PARAMS);
            - emitter spawn data interfaces Emitter.WaveGrid (Grid2DCollection, Float, attribute Wave = (eta, v)), Emitter.WaveOp
              (RenderTarget2D RGBA32f, PostStage Blur2 mips, TF_Bilinear, not user-bound), Emitter.WaveSource (Texture, bound to
              User.WaveSourceTexture) and Emitter.WaveAux (RenderTarget2D bound to User.WaveAuxTarget, the reader's target; its default
              is Waves/RT_WaveAux_Unbound, 1 x 1, so a NinjaLive nothing binds logs no "RenderTarget UserParam is required" error), as Set
              Parameters modules after NinjaInitGrids;
            - emitter update module Ninja_WaveParams after Ninja_InitParams002;
            - simulation stages Wave Prepare (after Pressure), Wave Step (after Wave Prepare), Wave Aux (after Wave Step: the mask, depth
              and drift per cell for FluidNinja Buoyancy's own fields) and Wave Export (after Export PressureAndDivergence), gated by
              Emitter.Wave.Enabled / Emitter.Wave.AuxEnabled / Emitter.Wave.ExportEnabled;
            - the wave grid decoupled from the pressure buffer (interface version 3): User.Wave.ScaleDown (S = 1, 2 or 4) runs the stages on
              the pressure buffer's cells over S (Ninja_WaveParams sizes WaveGrid / WaveOp so and publishes Emitter.Wave.ScaleDown, .Cells
              and .CellCm; Wave Prepare and Wave Aux judge the mask over each wave cell's S x S block, Wave Step and Wave Aux average the
              depth over it, Wave Export upsamples eta bilinearly to every pressure texel), so a tick costs 1 / S^2 of the fluid's at S > 1;
            - the foam channel F (interface version 4): a float attribute Foam on Emitter.WaveGrid that Wave Step accumulates from what
              breaks (the breaking decay x |v|), what a moving hull's waterline shears off (|grad P| x the hull's speed through the water,
              the source texture's A) and what hits (|y|), decayed at User.Wave.FoamDecay and clamped to 1; Wave Prepare carries it with
              the current as it carries eta; Wave Aux exports it into a render target the plugin owns (User.WaveFoamTarget, bound to
              Emitter.WaveFoam; its default is the same 1 x 1 RT_WaveAux_Unbound), which the surface material samples. User.Wave.Foam,
              .FoamDecay, .FoamBreaking, .FoamWake, .FoamImpact tune it. Nothing of it feeds back into (eta, v);
            - the closed-window <v> pin (interface version 5): User.Wave.VMeanRelax (1/s, default 0 = off) through Ninja_WaveParams into
              Emitter.Wave.VMeanRelax; in a walled window (Wave.EdgeOpen < 0.5) Wave Step relaxes the mean vertical velocity over wet cells
              (WaveOp's top mip, .z / .y) toward 0 and takes the matching step out of eta, so a one-signed pump on it (breaking at the
              hulls' hollow edges under a current) no longer sinks the level. Wave Step clamps it to 1 / Dt and Wave.MeanRelax to 0.5 / Dt
              against the Dt it integrates with, each only past a 0.01 % margin. At the default 0 and Wave.MeanRelax up to 0.5 / Dt,
              equality included, (eta, v) is interface 4's, bit for bit;
            - richer, longer-lived whitewater (interface version 6), foam only ((eta, v) is interface 5's bit for bit for every input): a
              second foam attribute FoamAged (F2, the aged foam) next to Foam (F1, fresh) on Emitter.WaveGrid, carried by Wave Prepare as
              F1 is (handed to Wave Step as a half-float hi + lo pair, lo stored x 2048 so a GPU that flushes half-float denormals keeps
              it, so its slow decay survives the 16-bit foam target; FluidNinjaWaves/_foamaged.hlsl); Wave Step gives
              the foam its own breaking threshold (User.Wave.FoamSlope), lets User.Wave.FoamTransfer of what F1 loses become F2, decays F2
              at User.Wave.FoamAgedDecay, spreads both (User.Wave.FoamDiffusion / .FoamAgedDiffusion, masked explicit Laplacian), and adds
              the plugin's wash: User.WaveWashTexture (bound to Emitter.WaveWash, a Texture DI; W x H RGBA16F on the source texture's
              cells: R / G = F1 / F2 rates per second scaled by User.Wave.FoamWash, B = the steady part of the impulse head, which makes no
              impact foam); Wave Aux exports (F1, F2, v, 0) into the foam target;
            - the wind sea (interface version 7): User.WaveWindSeaTexture (bound to Emitter.WaveWindSea, a Texture DI), the plugin's
              (N + 1) x 1 RGBA32F table of incident wind waves, whose long band Wave Export adds to B at each texel's world centre
              (FluidNinjaWaves/_windsea.hlsl), faded out across an open window's sponge band; nothing else reads it;
            - module assets in /Game/FluidNinjaLive/Core/Modules/Waves (HLSL in FluidNinjaWaves/*.hlsl next to this script).
  windsea   The wind sea alone onto an installed interface 6 core, in place (no unapply, so whatever else the core carries stays): the
            user parameter, the spawn DI, Ninja_WaveExport's new inputs and HLSL, Wave.InterfaceVersion 7 and Wave.CoreBuild, the
            descriptions. Refuses a core below interface 6 or one that has the wind sea. Headless, editor closed, like core.
  material  MF_NinjaOutput_BaseFunction_SURFACE (V8). The waves: WPO += (0, 0, B * WaveHeightScale) and the normal += slope of B (through
            the vendor MF_NormalFromHeightmap_SineWaves, channel B, scaled by WaveNormalScale and WaveHeightScale); both are exactly x + 0
            when B = 0. The foam (interface version 4): a WaveFoamBuffer texture parameter (the plugin binds each simulation's foam target
            to its surfaces; default the black RT_WaveAux_Unbound) sampled at the simulation's UV, FoamMask = saturate(F x
            WaveFoamStrength) behind a static switch WaveFoam (default on); the base colour is lerped toward WaveFoamColor (white) by it
            and the roughness has it added (saturated), both before MakeMaterialAttributes, so the vendor look is untouched at F = 0; a
            FoamMask function output for a project's own material. The choppiness (render-side nonlinearity): WPO.xy += WaveChoppiness x
            0.5 x (B two texels ahead - B two texels behind) x WaveHeightScale along each sim axis (tangent space, transformed to world):
            the Tessendorf horizontal displacement from the low-passed slope, which pulls the surface uphill toward the crests; behind a
            static switch WaveChoppy (default off: the vertex shader does none of it) and WaveChoppiness (default 0). The aged foam
            (interface version 6): AgedMask = saturate(WaveFoamBuffer.G x WaveFoamAgedStrength) behind the same WaveFoam switch (off:
            0); the base colour goes toward WaveFoamAgedColor (a sub-surface turquoise) by it before the fresh foam's lerp toward
            WaveFoamColor, roughness unchanged. The step adds what a function lacks: on a vendor function the waves, the foam and the
            choppiness and the aged foam; on one with the waves only, the rest; on an interface 4 / 5 function (waves and foam) the aged
            foam alone. It refuses only a function that already has the aged foam (WaveFoamAgedColor). The wind sea's wavelets
            (interface version 7): a Custom node sums the short band of WaveWindSeaTable (the plugin binds each simulation's wind-sea table;
            default the black RT_WaveAux_Unbound) at the world position into a world-space normal tilt, scaled by WaveWindSeaStrength (1),
            each wavelet faded out below about four pixels, transformed to tangent space and added to the normal chain; unbound it adds 0.
            The rings (V8, surface and plugin only; the core is untouched): a Custom node sums the plugin's ring table WaveRingTable
            (the recent impacts' sub-grid ripples: origin and count, the response table's layout, each ring's position, age + 16 x band
            and amplitude) through its response table WaveRingLUT (the exact linear ripple slope by radius and age per band, sampled
            bilinearly), both Linear Color texture objects whose default is the black RT_WaveAux_Unbound, into a world-space normal tilt
            scaled by WaveRingStrength (1), each ring faded out where its local wavelength spans fewer than about four pixels, transformed
            to tangent space and added to the normal chain after the wind sea's wavelets; unbound it adds 0.
            Run it in a fresh editor / commandlet process before any Play (FluidNinja material edits after PIE can crash).
  describe  The descriptions (tooltips) of the core's user parameters and of the SURFACE function's parameters (USER_PARAM_DESCRIPTIONS,
            SURFACE_PARAM_DESCRIPTIONS) onto an already patched core and material, and where they are shown: the core's user
            parameters into the "FluidNinja Buoyancy" section's categories of its user parameter hierarchy (USER_PARAM_CATEGORIES:
            Waves, Wave Foam, Relative Tracking, Plugin Bindings, Core Version), the SURFACE function's into the material parameter
            groups SURFACE_PARAM_GROUPS. The core and material steps do the same. It changes both files' hashes: add them to
            KNOWN_CORES / KNOWN_SURFACE and VERSIONS.md.
  check     One line: whether the core has the waves, its base, interface version, build and stages.
  inspect   No writes. Dumps the node graphs of PainterSourceData and Ninja_Painter (every node and pin, with links), the PainterAndSim
            stages and the relative-tracking lines of the compiled GPU HLSL to Saved/FluidNinjaWaves_inspect.txt, for TRACKING.md.
  verify    No writes. The installed core's sha256 / sha8 and what VERSIONS.md knows it as, whether it is vendor / pass3 / waves-applied,
            User.Wave.InterfaceVersion and CoreBuild, the wave stages and the Ninja_WaveParams module, the Waves module assets (and their
            hashes), the protected pass3 files, and whether the SURFACE function carries the waves and the foam. RESULT FAIL (and a
            non-zero exit code) unless the core is waves-applied on the pass3 base at interface version PLUGIN_MIN_INTERFACE_VERSION or
            later with everything present; a WARNING line when it is below INTERFACE_VERSION (the plugin runs it, without the newer
            interface's features), when the SURFACE function has the waves without the foam, or when it has the foam without the
            aged foam (interface version 6).

The core step never edits Ninja_Painter, PainterSourceData, CopyGridToRenderTarget*, the NinjaLive Blueprints or the data channel
(hash-checked). Start it from the pass3 (relative tracking) core: it consumes relative tracking but cannot create it (see tracking), so
on a stock vendor core it warns and the result has the waves without relative tracking; the report's "core is" line names the base it
found. It refuses a core that already has the waves (the material step: a function that already has interface 6's aged foam). It stamps the core with User.Wave.InterfaceVersion (INTERFACE_VERSION,
the contract the plugin requires) and User.Wave.CoreBuild (the run's date); both are printed in the report.

Run headless (editor closed), from the project that has the plugin (forward slashes; the engine finds the script by its .py extension, so
spaces in its path are fine; an option value with spaces needs quotes of its own):
  UnrealEditor-Cmd.exe <project>.uproject -run=pythonscript -script="<plugin>/Tools/CorePatch/apply_fluidninja_waves.py core material" -unattended -nop4 -nosplash
The report is written to <project>/Saved/FluidNinjaWaves_apply.txt and ends with the installed core's sha256, "CORE sha8 <x> tag core-<x>"
and "RESULT PASS" or "RESULT FAIL". The process exits non-zero on FAIL. Options can also come from the environment: FNB_WAVES_STEPS (the
steps, space or comma separated) and FNB_WAVES_ARGS (options such as --from=...), for command lines that are hard to quote.

Helpers used (unreal.FNBDevLibrary): DescribeNiagara, AddUserVariable, AddNiagaraSetParameterAfter, GetNiagaraInputDataInterface,
SetPropertyText, SetNiagaraBinding, AddNiagaraModuleAfter, AddSimulationStage, the graph helpers, SetModuleUsageBitmask,
CompileNiagaraSystem, GetNiagaraGpuHlsl, CompileMaterialErrors and, for inspect, DescribeNiagaraNode and DescribePins.
"""
import datetime
import glob
import hashlib
import os
import re
import shutil
import sys
import traceback
import zipfile

try:
    import unreal
    unreal.Paths.project_dir()  # the editor's own module; a stub package named unreal (type hints) fails here
except Exception:  # unapply is a file step and runs under plain Python too; every other step needs the editor.
    unreal = None

DEV = getattr(unreal, 'FNBDevLibrary', None) if unreal else None
EAL = unreal.EditorAssetLibrary if unreal else None
MEL = unreal.MaterialEditingLibrary if unreal else None

HERE = os.path.dirname(os.path.abspath(__file__))
HLSL_DIR = os.path.join(HERE, 'FluidNinjaWaves')
# Set by main() from the editor's project, --project=<dir> or FNB_PROJECT_DIR.
PROJECT_DIR = ''
CONTENT_DIR = ''
REPORT = ''
INSPECT_REPORT = ''

CORE = '/Game/FluidNinjaLive/NinjaLiveCore'
EMITTER = 'PainterAndSim'
WAVES = '/Game/FluidNinjaLive/Core/Modules/Waves'
# User.WaveAuxTarget's default: a 1 x 1 RGBA32f target. Emitter.WaveAux inherits its size and format from the bound target (the Wave Aux
# stage runs at the wave grid's size only), so with no target bound Niagara logs "RenderTarget UserParam is required but invalid" every
# tick; with this default a NinjaLive nothing binds logs nothing and its Wave Aux stage stays off.
AUX_UNBOUND = WAVES + '/RT_WaveAux_Unbound'
SURFACE = '/Game/FluidNinjaLive/OutputMaterials/Base/MF_NinjaOutput_BaseFunction_SURFACE'
NORMAL_FN = '/Game/FluidNinjaLive/OutputMaterials/Base/Functions/MF_NormalFromHeightmap_SineWaves'
PAINTER_SOURCE = '/Game/FluidNinjaLive/Core/Functions/PainterSourceData'
PAINTER_MODULE = '/Game/FluidNinjaLive/Core/Modules/Ninja_Painter'
TEMPLATE_MODULE = '/Niagara/DefaultAssets/DefaultModule.DefaultModule'
# Materials recompiled and checked after the material edit (missing ones are skipped).
CHECK_MATERIALS = ['/Game/FluidNinjaLive/OutputMaterials/Base/M_NinjaOutput_BaseMaterial_SURFACE',
                   '/Game/FluidNinjaLive/OutputMaterials/Surface_Water_Sparse/MI_Water_Pool_Caustics_Clear',
                   '/Game/FluidNinjaLive/OutputMaterials/Surface_Water_Sparse/MI_Water_River_Large_White_TilemapUsedForRiver3',
                   # Single Layer Water (the foam opacity's case): the lake's waters and its spawner's default.
                   '/Game/FluidNinjaLive/OutputMaterials/Surface_Water_Sparse/MI_Water_Murky_Caustics_v2_NoiseTile',
                   '/Game/FluidNinjaLive/OutputMaterials/Surface_Water_Sparse/MI_Water_SingleLayerWater_Placeholder']
# Vendor files this script must never change (relative to Content/FluidNinjaLive).
PROTECTED = ['Core/Modules/Ninja_Painter.uasset', 'Core/Functions/PainterSourceData.uasset', 'Core/Modules/Output/CopyGridToRenderTarget*.uasset',
             'NinjaLiveComponent.uasset', 'NinjaLive.uasset', 'Core/DataChannels/NinjaDataChannel1_Global.uasset']

# ---------------------------------------------------------------------------------------------------------------------------------
# Known files (VERSIONS.md keeps the same table for people). A core is named by its sha8, the first 8 hex digits of its file's
# SHA-256, and a plugin release that needs it is tagged core-<sha8>. Add a new build to VERSIONS.md and here together.
# ---------------------------------------------------------------------------------------------------------------------------------
# NinjaLiveCore.uasset: sha256 -> (kind, description). Kinds: vendor (stock), pass3 (relative tracking, no waves), waves (pass3 + waves).
KNOWN_CORES = {
    '7b9ed72f4f80c874d307b205e448739e1d11a3ba3acc2f692aea29aaa3d5de1e': ('vendor', 'stock FluidNinja LIVE 2.0.0.56 (saved by UE 5.6)'),
    '3bee517c051311c660f502fb68d0b0007e28a1fe7a3486547ba953f68ad5e32f': ('pass3', 'relative tracking, no waves: FluidNinjaLive_2.0.0.56_RelativeTracking_UE5.8.zip'),
    '43cc18903bba891b71ef7de4fe7445dcbe10cfe4258eb2c2ee762b4447f0e50d': ('waves', 'waves without the interface parameters, 2026-10-17 morning zip'),
    '97b976f6a519b934845afeb95fea194e796cbbf1040a7ec074aa6735c6611836': ('waves', 'waves with drift averaging, no interface parameters, 2026-10-17 update 2 zip'),
    '1bed46545bcfd910e75700b58296ccaeb7e83f06c206403a0a00bdb29ae46803': ('waves', 'waves, interface version 2, core build 20261017'),
    '9a11d0f1f98a202d2200d0b6ceb6fa08729047cc964ba0506cde45d83a1ac5bc': ('waves', 'waves, interface version 3 (the wave grid scale-down), core build 20261017'),
    'da2548bd85047feae127610032f325766879f2cdcc78a654acd2e127542a93ff': ('waves', 'waves, interface version 4 (the foam channel), core build 20261018'),
    '9adbcbf6cad6765f119429daca49065797b3aeb2abd54a26eb7be2ee07bc0c67': ('waves', 'waves, interface version 5 (the closed-window <v> pin), core build 20261018'),
    # The describe step's descriptions (tooltips) on 9adbcbf6's user parameters; nothing else changed (same stages, modules and interface).
    '6f4dcf536e3c3b30106b6d0577bce7fba937aefb48d3c83d590d3a5f583ea6f5': ('waves', 'waves, interface version 5, core build 20261018, with the parameter descriptions (9adbcbf6 + describe)'),
    # 6f4dcf53 with its user parameters in the FluidNinja Buoyancy section's categories (the describe step's categorizing); nothing else.
    'd6a02f62e4d3f981d6783220d83530e9f8a8b55e85935a74b1271b1378150fd6': ('waves', 'waves, interface version 5, core build 20261018, with the descriptions and the FluidNinja Buoyancy parameter categories (6f4dcf53 + describe); superseded by the interface 6 build 891573a6'),
    # Interface 6 (the aged foam, the wash, the foam's own breaking slope and spreading; (eta, v) as interface 5), core step + material +
    # describe in one process on the pass3 files. The core step's own save before describe was 4599d0b4 (never released). Superseded by
    # e85f0659: its aged-foam scratch stores lo unscaled, which a GPU that flushes half-float denormals reads as 0.
    '891573a6ff4daa602531b8dd9e9674e4def23ddb67e3931a262181716fa2b4b7': ('waves', 'waves, interface version 6 (the aged foam and the wash), core build 20261019, with the descriptions and categories; superseded by e85f0659 (the denormal-safe aged-foam scratch)'),
    # 891573a6 with the denormal-safe aged-foam scratch (Wave Prepare stores lo x 2048, FoamAgedLoScale in FluidNinjaWaves/_foamaged.hlsl;
    # Wave Step reads .y + .z / 2048) and corrected comments and the Wave.FoamWash tooltip; same interface 6 and parameters. Built by
    # unapply core material describe verify in one process; the core step's own save before describe was 2b6d2a19 (never released).
    'e85f0659fa04d14d6d154c593ea82298245230f0201bfc056d437da94d2d851d': ('waves', 'waves, interface version 6 (the aged foam and the wash, denormal-safe aged-foam scratch), core build 20261019, with the descriptions and categories: the current release'),
    # Not yet released (2026-10-20): the windsea step on a development install of 7a7d2183 (an unrecorded interface 6 core of the world-waves work, its
    # stages and modules e85f0659's apart from re-saved Params / Export / Aux, carrying the unused user parameters WaveWorldProjection,
    # WaveExternalWorld and Wave.LocalInterfaceVersion, which the step keeps): the wind sea, interface version 7, core build 20261019.
    '124eb42511df07bd190dee1e7eba08c6e846b5340f44f7452c977ce81981f844': ('waves', 'waves, interface version 7 (the wind sea added in place to 7a7d2183 by the windsea step), core build 20261019, not yet released'),
}
# Core/Functions/PainterSourceData.uasset and Core/Modules/Ninja_Painter.uasset: the vendor files and the hand-made pass3 ones.
KNOWN_PROTECTED = {
    '79a2eab07268a44e45bdf6c073fa65d67df96963e36b60cfe202696967132c69': ('vendor', 'PainterSourceData'),
    '9986466cd71809fc501b856327bce05d2079bea3cb9959890cc951da0914bc89': ('pass3', 'PainterSourceData'),
    'eb48a5d04ad76e088fcaf8e55b5b62da342f7ed8583b8005ac0936f2a3e8ccd6': ('vendor', 'Ninja_Painter'),
    '9f2c488c4452c4f0218b733972707ecc9c9373e61e17b46277fd8b3f815ac747': ('pass3', 'Ninja_Painter'),
}
# OutputMaterials/Base/MF_NinjaOutput_BaseFunction_SURFACE.uasset.
KNOWN_SURFACE = {
    '79803426743ba7649c7f393dcc3d0335d842e2f45a3e456eea6a594b77970ae5': ('vendor', 'stock SURFACE function'),
    '6e8fde9b0b8dc56a002c9685b73a8d0cbc468cefb44d515dbff6d05bc906a1c0': ('waves', 'SURFACE with the waves (cores 43cc1890, 97b976f6)'),
    '7c8c7e99b45c89d32e8aee559f27fadb79207b8da3b75e9392c3b7d6e281f54b': ('waves', 'SURFACE with the waves (core 1bed4654)'),
    'b564724ad5a33adebfb683ae83d77da6615e9bb0c7d5b8f25d7ed366c4bb6ad3': ('waves', 'SURFACE with the waves (core 9a11d0f1)'),
    '59974a799b74146d1c542b89ee5b6f0cd851c24587c8d65d71e899853de9c8f3': ('waves', 'SURFACE with the waves, the foam and the choppiness (core da2548bd)'),
    # The material step re-runs after unapply (which restores the vendor SURFACE), so the interface-5 apply re-saved the same edit under a
    # new hash.
    '3a19e7cd72caad327058817a24cafd1c4b2debcc8fe295ff2e8ac321d960c186': ('waves', 'SURFACE with the waves, the foam and the choppiness (core 9adbcbf6)'),
    'b55966c6e78a887e05606a936ed1906a4bdeabe8b78acba6017ba896ba3c968b': ('waves', 'SURFACE 3a19e7cd with the parameter descriptions (core 6f4dcf53)'),
    '1225f2045cbadec7e39fbb89f19ad3b34509135904bd57c0ac55a8b2df0d595c': ('waves', 'SURFACE b55966c6 with its parameters in the FluidNinja Buoyancy groups (core d6a02f62)'),
    # Interface 6: the vendor function with the waves, the foam, the choppiness and the aged foam, described and grouped.
    'aaf67fa04bacdea01fe939fe8a45a3cec5ff54ce2eea5db79a14ea7480911c1e': ('waves', 'SURFACE with the waves, the foam, the choppiness and the aged foam (core 891573a6, interface version 6)'),
    # The same edit, re-saved by the material step of the e85f0659 rebuild (unapply restores the vendor function first).
    'c9d1b84a23d1ecb2005ef3c31bbb3ea430f8a3467db0829a4201ff1e93f22ebe': ('waves', 'SURFACE with the waves, the foam, the choppiness and the aged foam (core e85f0659, interface version 6)'),
    # Not yet released (2026-10-19): the world-waves work's SURFACE (3706e883) with the foam drawn into Single Layer Water's opacity
    # (WaveFoamOpacity) and broken into lace (WaveFoamLace, WaveFoamLaceTexture, WaveFoamLaceTileCm) by the material step.
    '78fe848f041f28d3a2ed79fe65590170669f552c7863b1320d4c9bfbbc8450ef': ('waves', 'SURFACE with the foam opacity and the foam lace (core 7a7d2183, not yet released)'),
    # 78fe848f with the whitewater relief (WaveFoamHeightCm, WaveFoamRoughness), added by an incremental material step (2026-10-19).
    'f8946d0ae8db986fb0781bd47ac97456f3f5ae98517219e5f786c9ea81069436': ('waves', 'SURFACE with the foam opacity, the foam lace and the foam relief (core 7a7d2183, not yet released)'),
    # f8946d0a with the wind sea's wavelets (WaveWindSeaTable, WaveWindSeaStrength), added by an incremental material step (2026-10-20).
    '9b8a77b25cec5494aac62570598b7d6ece5c4438968d6afa5a6474b8c8a62319': ('waves', 'SURFACE with the foam opacity, the foam lace, the foam relief and the wind sea\'s wavelets (core 124eb425, interface version 7, not yet released)'),
    # 9b8a77b2 with the whitewater lift on the unlaced fresh foam (smooth_surface_foam_relief), an incremental material step (2026-10-20).
    '517c62901bcee211560574414921b8f00aa8ee0e39396e5ea91d8f4b6d748f3e': ('waves', 'SURFACE with the foam opacity, the foam lace, the foam relief lifted by the unlaced foam and the wind sea\'s wavelets (core 124eb425, interface version 7, not yet released)'),
    # 517c6290 with the rings (WaveRingTable, WaveRingLUT, WaveRingStrength), added by an incremental material step (2026-10-26).
    'a8b55370ddccdff08fee53e77ea529f2a880e5db60cb0253d6f249ca6692b517': ('waves', 'SURFACE with the foam opacity, the foam lace, the foam relief lifted by the unlaced foam, the wind sea\'s wavelets and the rings (core 124eb425, interface version 7, not yet released)'),
}
# Core/Modules/Waves/*.uasset: sha256 -> (core sha8 they were built with, asset name).
KNOWN_MODULES = {
    '9b2123f5fad72c1f7305f6ef4ed875f8d7899f4f2656663b6c1b73d0ebe3347c': ('43cc1890', 'Ninja_WaveAux'),
    '32d4e0a082a14d895e6aea474361673ea32009ef30188e6b1cfb18eac5b0db93': ('43cc1890', 'Ninja_WaveExport'),
    'b4bd71b717d7c98478e4446dc2de80608a3584f07ff6f8cc6b9f7f515c026757': ('43cc1890', 'Ninja_WaveParams'),
    '89fdc7d80fecf57e9fdfc6ca082aa13487798c3438efe1fc8ab234abffbfb176': ('43cc1890', 'Ninja_WavePrepare'),
    '9c5a0e8d925720b0885b2bad81e9ac3c025765e94699748ca2caabbb6dc24c59': ('43cc1890', 'Ninja_WaveStep'),
    'c556c4cb112333f362dfaa557e24991e9d01c90c6fcbecd333bcc6752be2b9bb': ('43cc1890', 'RT_WaveAux_Unbound'),
    'fecff87aee6475d2071c7c77598e53f0b2e1b04cf97426e75663882e78e6b56c': ('97b976f6', 'Ninja_WaveAux'),
    'a4f195e2bb869f668cbc39bdd89a65c1c4d26097d6af75ad973dce2c083e9709': ('97b976f6', 'Ninja_WaveExport'),
    '717ee59b1c00ccb16bc3c84243ea4266be790e5e259f0ab25ac25b90778b5a74': ('97b976f6', 'Ninja_WaveParams'),
    '72e1931c01fbe211c79bb4e0149dd44c0065edb4272b4ce1b9f2ab7b893af4bc': ('97b976f6', 'Ninja_WavePrepare'),
    '49296bab2e911302c06d099e08fb02d2aa15bf9420b3ac86cbf1f8bee69d02a4': ('97b976f6', 'Ninja_WaveStep'),
    'f26eb17bfd03b0e8dbd95c689e1e99fb3a41a73d6f3add260029aec347be85e5': ('97b976f6', 'RT_WaveAux_Unbound'),
    '0e5a8dd47b9424c0a668ea3ee7e58f07b915a9e7451355b84afdb8a389a70347': ('1bed4654', 'Ninja_WaveAux'),
    '151d363ef27627eb36278bd890e873b7a85924b24a1e0fc6245b2a6bf6301a41': ('1bed4654', 'Ninja_WaveExport'),
    '864daa5d6dd947f4f7ad524991948d32be5d452908151a48f61a7a1d3b5b7c1e': ('1bed4654', 'Ninja_WaveParams'),
    'a37da47811ef4d24a81616885ef24149eeba2848bc66b54da24dca00d55a1180': ('1bed4654', 'Ninja_WavePrepare'),
    'c18991720ca446556e2bae52dd46ddccf26e1c9b85921169ef276bde78a91cbe': ('1bed4654', 'Ninja_WaveStep'),
    '9d5975a30e71cec08af38fb7dc1a9560c793ea27104649701ca36e5cde480b3c': ('1bed4654', 'RT_WaveAux_Unbound'),
    '3a85de667ac51eacf75749b868e2d51b2378201738b223fcb042c2541fecb88e': ('9a11d0f1', 'Ninja_WaveAux'),
    '75b0c5c3efe15d129f1ef986eec908af952f4ef0445f6e52aca976e04ad96824': ('9a11d0f1', 'Ninja_WaveExport'),
    '7adbcf3fe2e5a56ee6bc73eec84fc4a16222309a06d7d75b00bcb9772b6c99ea': ('9a11d0f1', 'Ninja_WaveParams'),
    '584bbbc2a228515b0e79865f3f53e6398d0270f3e302f2d4fd00be774c8fcda7': ('9a11d0f1', 'Ninja_WavePrepare'),
    'c1232230aa6678e8add4f638174398ff1bd211daec6acc7c9b073d6ae9eff143': ('9a11d0f1', 'Ninja_WaveStep'),
    'a69738ba4f428e04eb48c3762e9d8c695c29eb88edfe8f29e7f55e5cb687e69e': ('9a11d0f1', 'RT_WaveAux_Unbound'),
    '9aced596a1d78bc16bfa585c24450f092403fd89447a568253628403af002aaa': ('da2548bd', 'Ninja_WaveAux'),
    '74d77c0fb4836ba9aff1901c6fa588727e8b2a4faeeeeb211d9dbd01153a920b': ('da2548bd', 'Ninja_WaveExport'),
    '4a767f3e4db98f540f606afaa4f50209b614befe76c00e594352359ed70c766f': ('da2548bd', 'Ninja_WaveParams'),
    '7b563ba46c8a4131576e4b1e3fd2cf5c993440c29b35defa9599068079532aa0': ('da2548bd', 'Ninja_WavePrepare'),
    '3171b915881f32009ebd809067ff7ee0fa192108e648ad41c579d6cb4103360d': ('da2548bd', 'Ninja_WaveStep'),
    'ebe29e62f5761aec0b611b3674c9d8184daca04b7a592ba45fce7196a92f6741': ('da2548bd', 'RT_WaveAux_Unbound'),
    # Interface 5: the core step rebuilds all six assets, so each has a new hash.
    '9fee8fd20034a1bdb57175b1d8df75b3b63fe1a6a75ec75991b475f45cade1cd': ('9adbcbf6', 'Ninja_WaveAux'),
    'ce5abc17b63e507397801011090ab80a6baa32cf5d73914f02e6955cf642feda': ('9adbcbf6', 'Ninja_WaveExport'),
    '59c42b520beac2c341e6e1119c77c92b473120dce96b3ed50a59fa5b05cf8ae4': ('9adbcbf6', 'Ninja_WaveParams'),
    '38ce28b9504f62fcbd7072104f8458bd950dbea6b062fcd6b8b6e978efa814b4': ('9adbcbf6', 'Ninja_WavePrepare'),
    '96806186d448efbd7f7e56e371775157d1bd6f38b4c7b93fc470c5826e2fd856': ('9adbcbf6', 'Ninja_WaveStep'),
    '01902f234c54599aedc0f5a2cc5ff824401d593ca93dafb1d561fc36f6f56ddc': ('9adbcbf6', 'RT_WaveAux_Unbound'),
    # Interface 6 (core 891573a6, superseded by e85f0659): all six rebuilt.
    '36370d46a866088281184fd19e356585759424dad492cff1f261edd02a6195a8': ('891573a6', 'Ninja_WaveAux'),
    'aa011d720917cff8df03513b3cf313ca3fe9939ce9862108e387caf07d4b5d54': ('891573a6', 'Ninja_WaveExport'),
    'ef7fbb7620a55b53d89f4d558b0e3a39c111f1e5225b9ec9802b162fc335b38e': ('891573a6', 'Ninja_WaveParams'),
    '66562ce4231e05bcb41724751b84ccf2e216caf7906614755afa3c8756ffbd06': ('891573a6', 'Ninja_WavePrepare'),
    '5cca5db601b967d1a4c87555d1e5de502a5baeb359d74cee5da900b23172a1f2': ('891573a6', 'Ninja_WaveStep'),
    '6c31f8a04595471be40c7ee5a4be26d1eedddbcc4241130f2062b158723a4293': ('891573a6', 'RT_WaveAux_Unbound'),
    # Interface 6 (core e85f0659, the current release: the denormal-safe aged-foam scratch): all six rebuilt.
    'e91d5a4cab401b47d5ef85563bbc89530dd0cf093317ddcc229119c33e96f47c': ('e85f0659', 'Ninja_WaveAux'),
    '23779ed702246b4fca581ed272694664b856449dbdc195c947478c6a39e5c7c9': ('e85f0659', 'Ninja_WaveExport'),
    'f801befa93e4e624eecbafa7627dae9d50fad0d314b8fae01e2892b5dcf4cebb': ('e85f0659', 'Ninja_WaveParams'),
    '8a0b5d770329875c497c4dcf37a2bb946516008ec18ce67a2adef92242fb47a2': ('e85f0659', 'Ninja_WavePrepare'),
    '7509a38867a779b8a2a00027328bd9c039021c1e6e1fdf86cfabd8efb30c15a7': ('e85f0659', 'Ninja_WaveStep'),
    '9aaa3fc6e47e36d959019061726db3e70b181b0fea69df119da100b8183ccdcb': ('e85f0659', 'RT_WaveAux_Unbound'),
    # Interface 7 by the windsea step (core 124eb425): only Ninja_WaveExport is rebuilt; the other five stay what the core had.
    '7e1227d38d443c487213a849f5987fbe2ea0b8ccadbdadd8291debace5ddbeff': ('124eb425', 'Ninja_WaveExport'),
}
# What unapply puts back: role, path under Content/FluidNinjaLive, the table its hash must be in, and the kind it must be.
UNAPPLY_FILES = [
    ('core', 'NinjaLiveCore.uasset', KNOWN_CORES, 'pass3'),
    ('PainterSourceData', 'Core/Functions/PainterSourceData.uasset', KNOWN_PROTECTED, 'pass3'),
    ('Ninja_Painter', 'Core/Modules/Ninja_Painter.uasset', KNOWN_PROTECTED, 'pass3'),
    ('surface', 'OutputMaterials/Base/MF_NinjaOutput_BaseFunction_SURFACE.uasset', KNOWN_SURFACE, 'vendor'),
]

# ENiagaraScriptUsage bits.
USAGE_EMITTER = (1 << 9) | (1 << 10)                  # EmitterSpawnScript | EmitterUpdateScript
USAGE_STAGE = (1 << 3) | (1 << 5) | (1 << 7)          # ParticleSpawnScript | ParticleUpdateScript | ParticleSimulationStageScript

# The handshake with the plugin (read on the game thread; bound to nothing in the HLSL). INTERFACE_VERSION is the contract the stages keep
# with the plugin: the User.Wave.* set, the source texture's layout and header, Wave Aux's and the export's channels, and what the step does
# with them; it moves when the plugin and the core must change together. 2: breaking judged on eta, the k = 0 sinks and Wave.MeanRelax (the
# source-texture header protocol is unchanged). 3: the wave grid decoupled from the pressure buffer, User.Wave.ScaleDown (S = 1, 2 or 4):
# the stages run on the pressure buffer's cells over S, the source texture and Wave Aux are at the wave grid's size, Wave Export upsamples
# eta to every pressure texel; the header protocol is unchanged, and at S = 1 a version 3 core does exactly what a version 2 core did. 4: the
# foam channel: the Foam attribute on the wave grid, User.WaveFoamTarget (the export, bound to Emitter.WaveFoam), User.Wave.Foam and the
# four Foam* tunings, and the source texture's A = the hull's speed through the water (0 from an older plugin: no waterline foam); nothing
# in (eta, v) changes, so a version 4 core with the foam off does exactly what a version 3 core did. The plugin requires 2 or more and
# refuses a core without the parameter; on a version 2 core it runs at S = 1 and says so once per simulation when a higher S was asked; it
# uses the foam on 4 or later and says "no foam" below. 5: the closed-window <v> pin and the in-step clamp of both rates:
# User.Wave.VMeanRelax (Emitter.Wave.VMeanRelax), with which Wave Step relaxes a walled window's mean vertical velocity toward 0, and Wave
# Step's clamp of Wave.MeanRelax to 0.5 / Dt and Wave.VMeanRelax to 1 / Dt against the Dt it integrates with (a core without a fixed
# tick can step 0.125 s), each only past a 0.01 % margin so that a rate the plugin already clamped to 0.5 / its tick passes through as
# sent. The asset default is 0, so a plugin that does not set it gets (eta, v) bit for bit as a version 4 core computes them (Wave.MeanRelax
# up to 0.5 / Dt, equality included); the plugin sets it only on 5 or later, because setting a parameter a core lacks adds a stray user
# parameter to its overrides. 6: richer, longer-lived whitewater, foam and export only: (eta, v) stays interface 5's bit for bit for every
# input (WaveOp, the Wave attribute, the breaking Decay on v, the sponge, the k = 0 pins and Wave Export are untouched, so the plugin's
# SelfStepCS port needs no change). It adds the aged foam F2 (a FoamAged attribute on the wave grid, advected as F1 = Foam is), the
# user parameters Wave.FoamAgedDecay, .FoamTransfer, .FoamDiffusion, .FoamAgedDiffusion, .FoamSlope, .FoamWash, and WaveWashTexture (the
# plugin's wash: W x H RGBA16F, no header row, on the source texture's cells; R = F1 rate /s, G = F2 rate /s, B = the steady part of the
# frame's impulse head in cm s, same units and sign as the source's G, which makes no impact foam; A reserved). The foam target becomes
# (F1, F2, v, 0), RGBA16F from the plugin (an R16F one keeps working: F1 only, F2's scratch reads 0). Wave Prepare's scratch in that target,
# read by Wave Step only, is (F1, hi(F2), lo(F2) x 2048, 0) rather than (F1, F2, 0, 0): a 16-bit channel cannot hold the slow F2 decay
# (0.02 /s is 3.3e-4 per 60 Hz tick, under half a half-float ulp in the lower part of each binade, so 0.6 stays 0.6), and hi + lo carries it
# to about 2^-22; lo is stored times 2048 (FoamAgedLoScale, FluidNinjaWaves/_foamaged.hlsl) because unscaled it is often a half-float
# denormal, which a GPU that flushes them turns into 0 (F2 stalls again: 0.2 stays 0.183 over 50 s). Wave Aux overwrites the target every
# tick, so the plugin never sees the scratch. At FoamSlope 0, FoamDiffusion 0, and no wash bound (or a wash whose B is 0, at FoamWash 0 or
# with R 0) F1 is interface 5's F exactly: the impact term takes the wash's B off unconditionally, FoamWash gates only the wash's R and G.
# The plugin sets the new parameters only on 6 or later (stray overrides again). 7: the wind sea, export only: the user parameter
# WaveWindSeaTexture (bound to the spawn DI Emitter.WaveWindSea, a Texture DI), the plugin's (N + 1) x 1 RGBA32F table of incident wind
# waves (texel 0 = origin x, origin y, the long band's count L, N; texels 1..N = kx, ky in rad/cm, amplitude cm, phase rad), whose long
# band Wave Export adds to B at each texel's world centre (FluidNinjaWaves/_windsea.hlsl), faded out across an open window's sponge band.
# (eta, v), the foam, Wave Aux and every other stage are interface 6's bit for bit; with no table bound B is too. The surface material
# draws the table's short band per pixel (the material step's wind-sea wavelets). The windsea step adds exactly this to an installed
# interface 6 core in place (the user parameter, the DI, the Export module's new inputs and HLSL, the version stamp), keeping whatever
# else that core carries; the core step builds it in from the pass3 core like the rest.
# CORE_BUILD is the run's date, YYYYMMDD, so a status line or a bug report can name the build.
INTERFACE_VERSION = 7
# The oldest interface the plugin still runs on (Waves.cpp, WaveRequiredInterfaceVersion): verify passes a core at this version or later, and
# warns when it is below INTERFACE_VERSION (the core then lacks what the current toolchain adds; the plugin runs it with those features off).
PLUGIN_MIN_INTERFACE_VERSION = 2
CORE_BUILD = datetime.date.today().strftime('%Y%m%d')

# name (without User.), type keyword, default
USER_PARAMS = [
    ('WaveSimEnabled', 'bool', 'false'),
    ('WaveSourceTexture', 'Texture', ''),
    ('Wave.DepthCm', 'float', '200'),
    ('Wave.DepthFromBed', 'float', '1'),
    ('Wave.Damping', 'float', '0.02'),
    ('Wave.SpongeCm', 'float', '300'),
    ('Wave.WallAbsorption', 'float', '0'),
    ('Wave.WallBandCm', 'float', '50'),
    ('Wave.EdgeOpen', 'float', '1'),
    ('Wave.ShoreDepthCm', 'float', '10'),
    ('Wave.BreakingSlope', 'float', '1.0'),
    ('Wave.Hyperviscosity', 'float', '0.5'),
    ('Wave.CurrentCoupling', 'float', '1'),
    # Tooltip: how fast the window's mean level is pulled back to 0 (1/s). It removes the one mode nothing else damps, a uniform level, which
    # crept up under a current. 0 turns it off; a closed basin then keeps the level a body's volume adds. From interface version 5 Wave Step
    # clamps it to 0.5 / Dt, so a rate meant for a 60 Hz fixed tick stays stable on a core that steps longer.
    ('Wave.MeanRelax', 'float', '0.5'),
    # Interface version 5, the closed-window <v> pin. Tooltip: how fast a walled window's mean vertical velocity is pulled to 0 (1/s; 0 off;
    # interface 5). A uniform v is invisible to the waves and only Damping and the wall band decay it, so a one-signed pump on it integrated
    # into a level that sank for minutes. Open windows (Wave.EdgeOpen 1) ignore it; Wave Step clamps it to 1 / Dt. The default 0 keeps
    # interface 4's arithmetic, so a plugin that does not set it runs exactly as before.
    ('Wave.VMeanRelax', 'float', '0'),
    # Tooltip: the wave grid's scale-down S (1, 2 or 4; interface version 3): the wave stages run on the pressure buffer's cells over S, cells of S
    # pressure cells, so a tick costs 1 / S^2 of what it does at 1. The plugin sets it from fnb.Waves.ScaleDown or the simulation's override, and
    # sizes its source texture and Wave Aux target to match. 1: the pressure buffer's own cells, exactly as interface version 2.
    ('Wave.ScaleDown', 'int', '1'),
    ('WaveAuxTarget', 'RenderTargetObject', AUX_UNBOUND + '.RT_WaveAux_Unbound'),
    # Interface version 4, the foam channel. Tooltips: Foam: 1 accumulates the foam (0 leaves the foam target at 0; the physics is the same
    # either way). FoamDecay: how fast the foam fades, 1/s (0.7: an e-fold in 1.4 s). FoamBreaking: scale of the foam the breaking makes
    # (the breaking decay x |v| / 100 cm/s). FoamWake: scale of the foam a moving hull's waterline sheds (its head's slope, clamped to 1, x
    # its speed through the water / 100 cm/s). FoamImpact: scale of the foam an impulse makes (|y| / 10 cm s). WaveFoamTarget: the reader's
    # render target at the wave grid's size (one channel, R16F does); Wave Aux writes F into it every tick; the default keeps it off.
    ('Wave.Foam', 'float', '1'),
    ('Wave.FoamDecay', 'float', '0.7'),
    ('Wave.FoamBreaking', 'float', '1'),
    ('Wave.FoamWake', 'float', '1'),
    ('Wave.FoamImpact', 'float', '1'),
    ('WaveFoamTarget', 'RenderTargetObject', AUX_UNBOUND + '.RT_WaveAux_Unbound'),
    # Interface version 6, the aged foam and the wash (foam only; USER_PARAM_DESCRIPTIONS has the tooltips). WaveWashTexture: the plugin's
    # wash (bound to Emitter.WaveWash, empty by default like WaveSourceTexture). FoamAgedDecay 1/s (e-fold 50 s), FoamTransfer 0..1,
    # FoamDiffusion / FoamAgedDiffusion cm2/s, FoamSlope cm/cm (0 = Wave.BreakingSlope), FoamWash a scale on the wash's rates.
    ('WaveWashTexture', 'Texture', ''),
    ('Wave.FoamAgedDecay', 'float', '0.02'),
    ('Wave.FoamTransfer', 'float', '0.5'),
    ('Wave.FoamDiffusion', 'float', '30'),
    ('Wave.FoamAgedDiffusion', 'float', '200'),
    ('Wave.FoamSlope', 'float', '0.3'),
    ('Wave.FoamWash', 'float', '1'),
    # Interface version 7, the wind sea (USER_PARAM_DESCRIPTIONS has the tooltip): the plugin's table of incident wind waves, bound to
    # Emitter.WaveWindSea, empty by default like WaveSourceTexture (an empty table adds nothing).
    ('WaveWindSeaTexture', 'Texture', ''),
    # The handshake (above): no stage reads these.
    ('Wave.InterfaceVersion', 'int', str(INTERFACE_VERSION)),
    ('Wave.CoreBuild', 'int', CORE_BUILD),
]

# The descriptions of the core's user parameters: the tooltips the Niagara editor and a NinjaLive's Niagara component (User Parameters)
# show. The core and describe steps write them. Each says what the parameter does, its unit, default and range, and where the plugin
# drives it from (it sets them every frame while it runs waves, so a value typed on a NinjaLive is overwritten then).
SETTINGS_PAGE = 'Project Settings -> FluidNinja Buoyancy -> Waves'
USER_PARAM_DESCRIPTIONS = {
    'WaveSimEnabled': 'FluidNinja Waves on this simulation (bool, default false). The FluidNinja Buoyancy plugin sets it true while it runs '
        'waves here (' + SETTINGS_PAGE + ', console fnb.Waves, and the simulation\'s FluidNinja Wave Sim component); false runs FluidNinja '
        'exactly as stock. A value set here is overwritten while the plugin runs.',
    'WaveSourceTexture': 'The wave sources the FluidNinja Buoyancy plugin rasters every frame (floating bodies\' static heads, impulses, '
        'swimmers): a float texture at the wave grid\'s size. Leave it empty; the plugin binds it at runtime.',
    'Wave.DepthCm': 'Water depth in cm where FluidNinja\'s height field is off (default 200; at least 1). It sets how fast waves travel: '
        'deeper is faster and longer (shallow-water speed is the square root of g x depth). The plugin drives it from ' + SETTINGS_PAGE +
        ' -> Wave Depth Cm, or from the FluidNinja Wave Sim component\'s Depth Cm when its depth override is on.',
    'Wave.DepthFromBed': '1: the depth comes from FluidNinja\'s height field where that is on (the river bed), Wave.DepthCm elsewhere; 0: '
        'Wave.DepthCm everywhere (default 1; 0 or 1). The plugin sets 1 unless the simulation\'s FluidNinja Wave Sim component overrides '
        'the depth.',
    'Wave.Damping': 'Damping of every wave, per second (default 0.02; 0 or more; a wave\'s height decays at half this rate). Higher calms '
        'the water sooner; at 0.02 rings cross a pool. The plugin drives it from ' + SETTINGS_PAGE + ' -> Wave Damping.',
    'Wave.SpongeCm': 'Width in cm of the absorbing band along an open window\'s edge (default 300; 0 or more; 0 reflects waves there). '
        'Wider absorbs longer waves. Used only where Wave.EdgeOpen is 1. The plugin drives it from ' + SETTINGS_PAGE + ' -> Wave Sponge Cm.',
    'Wave.WallAbsorption': 'Energy taken out of the waves reaching a walled border, per second, within Wave.WallBandCm of it (default 0 '
        'here, 0.4 from the plugin; 0 or more), as a pool\'s gutter does: higher leaves fewer echoes. A wall still returns about 80% of a '
        '2 m wave at 0.4. Used only where Wave.EdgeOpen is 0. The plugin drives it from ' + SETTINGS_PAGE + ' -> Wave Wall Absorption or '
        'the FluidNinja Wave Sim component.',
    'Wave.WallBandCm': 'Width in cm of the band along a wall where Wave.WallAbsorption acts (default 50; 0 or more). The plugin drives it '
        'from ' + SETTINGS_PAGE + ' -> Wave Wall Band Cm or the FluidNinja Wave Sim component.',
    'Wave.EdgeOpen': '1: the window\'s border is open water, waves leave through the sponge band; 0: a wall, waves reflect and the wall '
        'band absorbs some (default 1; 0 or 1). The plugin drives it from ' + SETTINGS_PAGE + ' -> Wave Edge Open or the FluidNinja Wave '
        'Sim component\'s Walled.',
    'Wave.ShoreDepthCm': 'Water shallower than this, in cm, absorbs waves, more the shallower, as a beach does (default 10; 0 or more; 0 '
        'turns it off). The plugin drives it from ' + SETTINGS_PAGE + ' -> Wave Shore Depth Cm.',
    'Wave.BreakingSlope': 'Waves steeper than this slope from cell to cell break and lose energy, and so do crests higher than 0.6 of the '
        'depth (default 1; at least 0.01). Lower breaks more (0.4 halved a hull\'s Kelvin wake); higher lets steeper waves stand. A net for '
        'cell-sized spikes, not a physical breaking limit. The plugin drives it from ' + SETTINGS_PAGE + ' -> Wave Breaking Slope.',
    'Wave.Hyperviscosity': 'Extra damping of the shortest waves the grid carries, falling off as k^4 for longer ones (default 0.5; 0 or '
        'more). It keeps the cell-sized ripples from a hull\'s sharp edge from ringing: higher smooths them sooner, 0 leaves them. The plugin '
        'drives it from ' + SETTINGS_PAGE + ' -> Wave Hyperviscosity.',
    'Wave.CurrentCoupling': 'How much FluidNinja\'s current carries the waves (default 1; 0 or more): 0 the waves ignore the current, 1 '
        'they are carried at the water\'s speed. The plugin drives it from ' + SETTINGS_PAGE + ' -> Wave Current Coupling.',
    'Wave.MeanRelax': 'How fast the window\'s mean level is pulled back to 0, per second (default 0.5; 0 or more; 0 off, and a closed basin '
        'then keeps the level a body\'s volume adds). It removes the one mode nothing else damps, a uniform level. From interface version 5 '
        'Wave Step clamps it to 0.5 / tick time. The plugin sets it per window (console fnb.Waves.MeanRelax, -1 automatic: open windows '
        '0.5/s, walled ones 5/s with Wave.VMeanRelax, 30/s on an older core).',
    'Wave.VMeanRelax': 'How fast a walled window\'s mean vertical velocity is pulled to 0, per second (interface version 5; default 0, off; '
        '0 or more; Wave Step clamps it to 1 / tick time). It stops a one-sided pump on it from sinking the level over minutes. Open windows '
        '(Wave.EdgeOpen 1) ignore it. The plugin sets it from console fnb.Waves.VMeanRelax (30).',
    'Wave.ScaleDown': 'The wave grid\'s scale-down S: 1, 2 or 4 (interface version 3; default 1). The wave stages run on cells of S x S '
        'pressure cells, so a tick costs 1 / S^2 of what it does at 1: larger is cheaper and coarser. The plugin drives it from console '
        'fnb.Waves.ScaleDown or the FluidNinja Wave Sim component\'s Scale Down Override, and sizes its textures to match.',
    'WaveAuxTarget': 'The render target Wave Aux exports the wave field into (height and velocity per wave cell), owned and bound by the '
        'FluidNinja Buoyancy plugin. The default, RT_WaveAux_Unbound (1 x 1), keeps the export off. Leave it; the plugin sets it at runtime.',
    'Wave.Foam': '1 accumulates the foam (whitecaps, wake trails, impact rings, and from interface version 6 the aged foam and the '
        'plugin\'s wash) into the foam target; 0 leaves it at 0 (interface version 4; default 1; 0 or 1). The waves are the same either way. '
        'The plugin drives it from ' + SETTINGS_PAGE + ' -> Foam and console fnb.Waves.Foam.',
    'Wave.FoamDecay': 'How fast the fresh foam fades, per second (interface version 4; default 0.7, an e-fold in about 1.4 s; 0 or more; 0 '
        'never fades). Higher: shorter, crisper whitecaps and wake; lower: fresh foam lingers (keep it above about 0.03: below that, at 60 '
        'Hz, the 16-bit foam target rounds a tick\'s fading away and some foam stops fading). From interface version 6, Wave.FoamTransfer of what it loses becomes aged foam. 0.3 to 3 is '
        'sensible. The plugin drives it from ' + SETTINGS_PAGE + ' -> Foam Decay.',
    'Wave.FoamBreaking': 'Scale of the foam breaking waves make: the breaking rate x the water\'s speed / 100 cm/s (interface version 4; '
        'default 1; 0 or more; 0 none). The whitecaps; from interface version 6 they start at Wave.FoamSlope. The plugin drives it from ' +
        SETTINGS_PAGE + ' -> Foam Breaking.',
    'Wave.FoamWake': 'Scale of the foam a moving hull\'s waterline sheds: its head\'s slope (at most 1) x its speed through the water / 100 '
        'cm/s (interface version 4; default 1; 0 or more; 0 none). The wake trails and bow foam. The plugin drives it from ' + SETTINGS_PAGE +
        ' -> Foam Wake.',
    'Wave.FoamImpact': 'Scale of the foam an impulse makes: |impulse| / 10 cm s (interface version 4; default 1; 0 or more; 0 none). Slams, '
        'pokes and swimmer strokes: the impact rings. From interface version 6 the steady part of the impulse the plugin marks in the wash '
        '(a planing hull\'s held pressure) is taken off first, so it makes none. The plugin drives it from ' + SETTINGS_PAGE + ' -> Foam '
        'Impact.',
    'WaveFoamTarget': 'The render target Wave Aux writes the foam into every tick at the wave grid\'s size, owned and bound by the FluidNinja '
        'Buoyancy plugin and sampled by the surface material (WaveFoamBuffer). From interface version 6 it holds (fresh foam, aged foam, the '
        'waves\' vertical velocity in cm/s, 0) and the plugin makes it RGBA16F; an R16F target (interfaces 4 and 5) still works, with the '
        'fresh foam only. The default, RT_WaveAux_Unbound, keeps it off. Leave it; the plugin sets it at runtime.',
    'WaveWashTexture': 'The foam wash the FluidNinja Buoyancy plugin rasters every frame (interface version 6): W x H RGBA16F on the same '
        'cells as the wave source texture (WaveSourceTexture) without its header row. R: fresh foam added per second (planing spray, bow '
        'foam); G: aged foam added per second (the sheet a fast hull leaves); B: the steady part of the frame\'s impulse head (cm s, as the '
        'source texture\'s G), which makes no impact foam; A unused. Wave.FoamWash scales R and G. Leave it empty; the plugin binds it at '
        'runtime, and unbound (black) it adds nothing.',
    'Wave.FoamAgedDecay': 'How fast the aged foam fades, per second (interface version 6; default 0.02, an e-fold in 50 s; 0 or more; 0 '
        'never fades). The aged foam is the thin, long-lived sheet of bubbles fresh foam leaves as it fades (Wave.FoamTransfer) and the '
        'plugin\'s wash adds: higher shortens the trails a boat leaves, lower lengthens them. 0.01 to 0.2 is sensible (e-folds of 100 s to '
        '5 s). A FluidNinja Buoyancy build that speaks interface 6 sets it from its foam settings; otherwise this default applies.',
    'Wave.FoamTransfer': 'The share of what the fresh foam loses each tick that becomes aged foam (interface version 6; default 0.5; 0 to 1, '
        'clamped). 0: fresh foam simply vanishes and the aged foam comes from the wash alone; 1: all of it turns into aged foam. Higher '
        'leaves brighter, longer trails behind whitecaps, wakes and impacts. 0.2 to 0.8 is sensible. Set by a plugin build that speaks '
        'interface 6; otherwise this default applies.',
    'Wave.FoamDiffusion': 'How fast the fresh foam spreads sideways, cm2/s (interface version 6; default 30; 0 or more; 0 none). Higher '
        'softens and widens the fresh foam (it spreads about the square root of 2 x this x its age: 30 widens it by about 9 cm over its '
        '1.4 s); it only spreads between wet cells. Wave Step caps it at 0.2 x cell^2 / tick (12 x cell^2 per second at 60 Hz: 300 cm2/s at '
        '5 cm cells, 1200 at 10 cm), so a larger value spreads at that cap. 0 to 100 is sensible. Set by a plugin build that speaks '
        'interface 6; otherwise this default applies.',
    'Wave.FoamAgedDiffusion': 'How fast the aged foam spreads sideways, cm2/s (interface version 6; default 200; 0 or more; 0 none). Higher '
        'widens and blurs the trails as they age (about the square root of 2 x this x their age: 200 widens a trail by about 1.4 m over '
        '50 s); lower keeps them sharp. It only spreads between wet cells. Wave Step caps it at 0.2 x cell^2 / tick (12 x cell^2 per second '
        'at 60 Hz: 300 cm2/s at 5 cm cells, 1200 at 10 cm), so on fine grids the default sits near the cap. 50 to 1000 is sensible. Set by a '
        'plugin build that speaks interface 6; otherwise this default applies.',
    'Wave.FoamSlope': 'The wave slope (cm per cm, cell to cell) at which whitecaps start, for the foam only (interface version 6; default '
        '0.3; 0 or more; 0 uses Wave.BreakingSlope, the physics\' own breaking threshold, as interfaces 4 and 5 did). Lower: whitecaps on '
        'gentler waves; higher: only on steep ones. The waves still break and lose energy at Wave.BreakingSlope; this changes no wave. 0.15 '
        'to 1 is sensible. Set by a plugin build that speaks interface 6; otherwise this default applies.',
    'Wave.FoamWash': 'Scale on the plugin\'s wash (WaveWashTexture R and G: the fresh and aged foam fast hulls add per second; interface '
        'version 6; default 1; 0 or more; 0 turns off the wash\'s fresh and aged rates (R, G); its B still removes the steady planing head '
        'from the impact foam). Higher: thicker planing spray and wake sheets around fast hulls; lower: thinner. 0.5 to 2 is sensible. Set '
        'by a plugin build that speaks interface 6; otherwise this default applies.',
    'WaveWindSeaTexture': 'The wind sea the FluidNinja Buoyancy plugin writes every frame (interface version 7): an (N + 1) x 1 RGBA32F '
        'table of incident wind waves. Texel 0 = (origin x, origin y in world cm, L = the long band\'s count, N = every component\'s); '
        'texels 1..N = (kx, ky in rad/cm, amplitude cm, phase rad at the origin this frame), the long band first. Wave Export adds the long '
        'band to the exported wave height B (so the floating bodies feel it and the surface draws it), faded out across an open window\'s '
        'sponge band; the surface material draws the short band per pixel. Nothing of it enters the wave state, the breaking or the foam. '
        'Set per simulation by its Wave Sims entry (Wind Sea Height Cm and the rest). Leave it empty; the plugin binds it at runtime, and '
        'empty it adds nothing.',
    'Wave.InterfaceVersion': 'Read-only stamp: the plugin contract this core implements (%d). The FluidNinja Buoyancy plugin reads it to '
        'choose what it may set and needs at least %d. Do not change it.' % (INTERFACE_VERSION, PLUGIN_MIN_INTERFACE_VERSION),
    'Wave.CoreBuild': 'Read-only stamp: the date this core was patched (YYYYMMDD), shown in the plugin\'s status line and bug reports. Do '
        'not change it.',
    # Made by the hand-made relative-tracking (pass3) patch (TRACKING.md), described here when the core has it.
    'TrackVelocityRelativeToWater': 'Relative tracking (the modified FluidNinja; default 0 = stock behaviour; on above 0.5). On, FluidNinja\'s '
        'tracking paints a tracked point\'s velocity relative to the water at the point instead of its world velocity, so a floater drifting '
        'with the current paints none. Set it to 1 on each NinjaLive\'s NinjaLiveComponent (User Parameters) for floating bodies with '
        'Interaction FluidNinja Tracking; it changes every point that NinjaLive tracks, characters included. Project Settings -> FluidNinja '
        'Buoyancy -> Relative Tracking Switch names this parameter.',
}

# The SURFACE function's parameters (the material step adds them): their descriptions, the tooltips a water material instance shows.
SURFACE_PARAM_DESCRIPTIONS = {
    'WaveHeightScale': 'FluidNinja Waves: scale on the drawn wave height (RT_PressureDivergence.B), both the vertex offset and the normal. '
        'Default 1, the simulated height; 0 or more; 0 hides the waves; above 1 exaggerates them.',
    'WaveNormalScale': 'FluidNinja Waves: strength of the normal from the wave slope. Default 0.1; 0 or more; about 1 / (0.005 x the '
        'simulation\'s size in cm) is physical; 0 shades the waves flat.',
    'WaveFoamBuffer': 'FluidNinja Waves: the simulation\'s foam target, sampled at the simulation UV like PressureDivergenceBuffer: R = '
        'fresh foam 0..1 per wave cell, G = aged foam 0..1 (interface version 6; 0 on older cores), B = the waves\' vertical velocity '
        '(cm/s, not drawn). The FluidNinja Buoyancy plugin binds it on the surfaces\' dynamic materials; unbound it is black. Leave it unset '
        'in a material.',
    'WaveFoam': 'FluidNinja Waves: the foam (whitecaps, wake trails, impact rings, and the aged foam\'s turquoise sheet) drawn into the base '
        'colour, and the fresh foam into the roughness (static switch, default on). Off: no foam sample at all, and a cheaper shader.',
    'WaveFoamColor': 'FluidNinja Waves: the colour the water goes toward where there is fresh foam (linear colour, default white), drawn '
        'over the aged foam\'s colour.',
    'WaveFoamAgedColor': 'FluidNinja Waves: the colour of the aged foam, the thin sheet of bubbles under the surface a wake or whitecap '
        'leaves (interface version 6; linear colour, default 0.40, 0.72, 0.66: a pale turquoise). The base colour goes toward it by the '
        'aged foam x WaveFoamAgedStrength, under the fresh foam\'s WaveFoamColor. Darker and bluer for deep or dark water, paler for '
        'shallow, sunlit water; alpha is unused.',
    'WaveFoamAgedStrength': 'FluidNinja Waves: scale on the aged foam (WaveFoamBuffer.G) before it is clamped to 1 (interface version 6). '
        'Default 0.6; 0 or more; 0 hides the aged foam, 1 shows a full aged sheet at the full WaveFoamAgedColor, above 1 brings faint old '
        'trails out. 0.3 to 1 is sensible. Needs WaveFoam on; the roughness is not changed by it.',
    'WaveFoamStrength': 'FluidNinja Waves: scale on the foam before it is clamped to 1. Default 1; 0 or more; 0 hides the foam; above 1 '
        'shows faint foam more strongly.',
    'WaveFoamLace': 'FluidNinja Waves: breaks the foam into lace with a foam texture (WaveFoamLaceTexture): where the foam is thick the '
        'surface is white on the texture\'s veins and about 60 % between them, thinner foam keeps the veins, thin foam only faint veins, '
        'as real whitewater breaks up into streaks and lace. With g = saturate(texture x 3): laced = saturate(foam x (0.55 + 0.45 g) x '
        'smoothstep(0, 1, (foam + 0.5 g - 0.35) x 2)); the foam drawn = lerp(foam, laced, WaveFoamLace), for the fresh and the aged foam '
        'alike (and so for the foam opacity). Default 0.8; range 0-1; 0 draws the foam as smooth patches.',
    'WaveFoamLaceTexture': 'FluidNinja Waves: the lace the foam breaks into (WaveFoamLace): white veins on black, sampled in world space '
        '(R). Default FluidNinja\'s T_foam2.',
    'WaveFoamLaceTileCm': 'FluidNinja Waves: the size of one repeat of the lace texture on the water, cm. Smaller gives finer lace. '
        'Default 400; at least 1.',
    'WaveFoamHeightCm': 'FluidNinja Waves: how far whitewater stands up from the water, cm: the fresh foam before the lace (FoamMask '
        'without WaveFoamLace, smooth from cell to cell) lifts the surface by this much (WorldPositionOffset), so a wake seen from low down '
        'shows its white ridges and the dark water between them lies lower, as aerated, churned water stands up in a real wake. The lace '
        'stays in the colour, the opacity and the roughness (WaveFoamRoughness): lifted by the laced foam, neighbouring vertices took 0 or '
        'the whole height and the foam stood up in sharp spikes. Default 0 (none); 10-30 for a planing wake. It moves the drawn surface '
        'only: the floating bodies feel the waves, not this.',
    'WaveFoamRoughness': 'FluidNinja Waves: how lumpy whitewater looks: the normal leans by this times the foam (FoamMask) times the '
        'slope of the lace texture (WaveFoamLaceTexture R at the lace\'s world UV, through MF_NormalFromHeightmap_SineWaves), so churned '
        'water catches the light in clumps instead of lying flat. Default 0 (none); 1-5 is sensible.',
    'WaveFoamOpacity': 'FluidNinja Waves: how far the foam makes the surface opaque, for Single Layer Water and translucent materials, '
        'whose Opacity decides how much of the base colour (the foam) shows over the water seen through: Opacity = max(the material\'s '
        'own, saturate((FoamMask + 0.5 x AgedMask) x WaveFoamOpacity)). Without it the foam is lerped into a base colour such water '
        'barely shows. Default 1; 0 or more; 0 leaves the opacity as the material has it (the foam then shows only on opaque materials), '
        'above 1 makes faint foam opaque sooner. Opaque materials ignore it. Needs WaveFoam on.',
    'WaveChoppy': 'FluidNinja Waves: the choppiness (WaveChoppiness) on (static switch, default off): the vertex shader reads the wave '
        'height four more times. Off: nothing of it is compiled.',
    'WaveChoppiness': 'FluidNinja Waves: how far the surface is pulled sideways toward the crests, in units of half the wave height '
        'difference two texels apart. Default 0 (none); 0 or more; 1 is the Tessendorf choppiness on waves a few texels long. Needs '
        'WaveChoppy on.',
    'WaveWindSeaTable': 'FluidNinja Waves wind sea (interface version 7): the simulation\'s wind-sea table, the same texture as the core\'s '
        'User.WaveWindSeaTexture. The surface draws its short band (the wavelets shorter than the simulation\'s wave cells can carry) per '
        'pixel as a normal tilt in world space; the core adds the long band to the wave height B. The FluidNinja Buoyancy plugin binds it on '
        'the surfaces\' dynamic materials while the simulation has a wind sea (Wave Sims: Wind Sea Height Cm above 0); unbound (the black '
        'default) it adds nothing and costs one texture load. Leave it unset in a material.',
    'WaveWindSeaStrength': 'FluidNinja Waves wind sea: scale on the wavelets\' slope (the short band of WaveWindSeaTable). Default 1, the '
        'physical slope of the spectrum the plugin sends; 0 hides the wavelets and skips their loop; above 1 exaggerates them (a matte or '
        'murky water shows wavelets more weakly than clear water does). 0.5 to 3 is sensible. Set it per simulation with its Wave Sims '
        'Surface Scalars. Wavelets shorter than about four pixels fade out, so distant water does not shimmer.',
    'WaveRingTable': 'FluidNinja Waves rings: the simulation\'s ring table, the recent water contacts (a bird diving, landing or dipping '
        'its head, a footstep, a dropped body) whose ripples are shorter than the simulation\'s wave cells can carry. The FluidNinja '
        'Buoyancy plugin writes it every frame while the simulation has rings, an (N + 2) x 1 RGBA32F table: texel 0 = (origin x, origin '
        'y in world cm, N, 0); texel 1 = (WaveRingLUT\'s radius cm, its seconds, its bands, its rows per band); texels 2.. = each ring\'s '
        '(x, y from the origin in cm, age s + 16 x band, amplitude). The surface draws every ring per pixel from WaveRingLUT as a normal '
        'tilt in world space; the long part of the same impact is in the wave height B already. The plugin binds it on the surfaces\' '
        'dynamic materials and unbinds it when the rings are gone; unbound (the black default) it adds nothing and costs one texture '
        'size query. Leave it unset in a material.',
    'WaveRingLUT': 'FluidNinja Waves rings: the simulation\'s ring response table, the exact linear ripple of a unit impulse (gravity and '
        'capillary waves on the simulation\'s depth, damped by the plugin\'s Wave Ring Viscosity) by distance from the impact (across, '
        'denser near the centre) and age (down, one block of rows per band): R = the slope of the water (cm per cm), G = its height (cm), '
        'B = the local wavelength (cm; the surface fades a ring out where it spans fewer than about four pixels). The FluidNinja '
        'Buoyancy plugin computes it per simulation and binds it with WaveRingTable; it is sampled bilinearly, so keep it a float '
        'texture. Unbound (the black default) the rings add nothing. Leave it unset in a material.',
    'WaveRingStrength': 'FluidNinja Waves rings: scale on the rings\' slope (the ripples WaveRingTable lists, drawn from WaveRingLUT), '
        'on top of the plugin\'s Wave Ring Strength setting, which already scales every ring\'s amplitude in the table. Default 1, the '
        'physical slope; 0 or more; 0 hides the rings and skips their loop; above 1 exaggerates them (a matte or murky water shows '
        'ripples more weakly than clear water does). 0.5 to 3 is sensible. Set it per simulation with its Wave Sims Surface Scalars. '
        'Ripples shorter than about four pixels fade out, so distant rings do not shimmer.',
}

# Where the parameters this toolchain adds to FluidNinja are shown, so a user tells them from FluidNinja's own at a glance. The core's
# user parameters go into categories of its user parameter hierarchy, all under one section: a NinjaLive's NinjaLiveComponent shows
# them as groups under User Parameters (and a "FluidNinja Buoyancy" section button over them), the Niagara editor's User Parameters
# hierarchy editor as categories. Every parameter USER_PARAMS adds, and the relative-tracking switch, must be in one of them (the
# describe step fails otherwise). The SURFACE function's parameters get material parameter groups of the same names.
USER_PARAM_SECTION = 'FluidNinja Buoyancy'
USER_PARAM_SECTION_TOOLTIP = ('Parameters the FluidNinja Buoyancy plugin added to FluidNinja\'s core (its modified NinjaLiveCore): the waves, '
                              'their foam, relative tracking, the plugin\'s runtime bindings and the core\'s version stamps. Not part of '
                              'stock FluidNinja.')
USER_PARAM_CATEGORIES = [
    ('FluidNinja Buoyancy: Waves',
     'FluidNinja Waves, added by the FluidNinja Buoyancy plugin: the height field the floating bodies, swimmers and impacts make waves in. '
     'The plugin sets these every frame while it runs waves on this simulation (Project Settings -> FluidNinja Buoyancy -> Waves, and the '
     'simulation\'s FluidNinja Wave Sim component), so a value typed here is overwritten then.',
     ['WaveSimEnabled', 'Wave.DepthCm', 'Wave.DepthFromBed', 'Wave.Damping', 'Wave.SpongeCm', 'Wave.WallAbsorption', 'Wave.WallBandCm',
      'Wave.EdgeOpen', 'Wave.ShoreDepthCm', 'Wave.BreakingSlope', 'Wave.Hyperviscosity', 'Wave.CurrentCoupling', 'Wave.MeanRelax',
      'Wave.VMeanRelax', 'Wave.ScaleDown']),
    ('FluidNinja Buoyancy: Wave Foam',
     'The foam FluidNinja Waves accumulates (whitecaps, wake trails, impact rings, and from interface version 6 the long-lived aged foam '
     'and the plugin\'s wash), added by the FluidNinja Buoyancy plugin; the surface material draws it (WaveFoamBuffer). Set by the plugin '
     'like the waves.',
     ['Wave.Foam', 'Wave.FoamDecay', 'Wave.FoamBreaking', 'Wave.FoamWake', 'Wave.FoamImpact', 'Wave.FoamAgedDecay', 'Wave.FoamTransfer',
      'Wave.FoamDiffusion', 'Wave.FoamAgedDiffusion', 'Wave.FoamSlope', 'Wave.FoamWash']),
    ('FluidNinja Buoyancy: Relative Tracking',
     'The modified FluidNinja\'s relative tracking, added for the FluidNinja Buoyancy plugin: tracked points paint their velocity relative '
     'to the water instead of their world velocity. Set it here, per NinjaLive; it applies to every point that NinjaLive tracks.',
     ['TrackVelocityRelativeToWater']),
    ('FluidNinja Buoyancy: Plugin Bindings',
     'Textures and render targets the FluidNinja Buoyancy plugin binds while it runs (the wave sources and the foam wash it rasters, the '
     'wind-sea table it writes, the wave and foam exports it reads back). Leave them as they are.',
     ['WaveSourceTexture', 'WaveAuxTarget', 'WaveFoamTarget', 'WaveWashTexture', 'WaveWindSeaTexture']),
    ('FluidNinja Buoyancy: Core Version',
     'Read-only stamps of the modified core: the plugin contract it implements and the date it was patched. The plugin reads them to '
     'check the core; do not change them.',
     ['Wave.InterfaceVersion', 'Wave.CoreBuild']),
]
SURFACE_PARAM_GROUPS = {
    'WaveHeightScale': 'FluidNinja Buoyancy: Waves',
    'WaveNormalScale': 'FluidNinja Buoyancy: Waves',
    'WaveChoppy': 'FluidNinja Buoyancy: Waves',
    'WaveChoppiness': 'FluidNinja Buoyancy: Waves',
    'WaveFoam': 'FluidNinja Buoyancy: Wave Foam',
    'WaveFoamBuffer': 'FluidNinja Buoyancy: Wave Foam',
    'WaveFoamColor': 'FluidNinja Buoyancy: Wave Foam',
    'WaveFoamStrength': 'FluidNinja Buoyancy: Wave Foam',
    'WaveFoamAgedColor': 'FluidNinja Buoyancy: Wave Foam',
    'WaveFoamAgedStrength': 'FluidNinja Buoyancy: Wave Foam',
    'WaveFoamOpacity': 'FluidNinja Buoyancy: Wave Foam',
    'WaveFoamLace': 'FluidNinja Buoyancy: Wave Foam',
    'WaveFoamLaceTexture': 'FluidNinja Buoyancy: Wave Foam',
    'WaveFoamLaceTileCm': 'FluidNinja Buoyancy: Wave Foam',
    'WaveFoamHeightCm': 'FluidNinja Buoyancy: Wave Foam',
    'WaveFoamRoughness': 'FluidNinja Buoyancy: Wave Foam',
    'WaveWindSeaTable': 'FluidNinja Buoyancy: Wind Sea',
    'WaveWindSeaStrength': 'FluidNinja Buoyancy: Wind Sea',
    'WaveRingTable': 'FluidNinja Buoyancy: Wave Rings',
    'WaveRingLUT': 'FluidNinja Buoyancy: Wave Rings',
    'WaveRingStrength': 'FluidNinja Buoyancy: Wave Rings',
}

V2 = '(X=0.000, Y=0.000)'
V3 = '0.000,0.000,0.000'
V4 = '0.000,0.000,0.000,0.000'
# Module graphs: map-get outputs (parameter, type, default, Custom HLSL input pin), Custom HLSL outputs (pin, type, map-set parameter).
MODULES = {
    'Ninja_WaveParams': dict(usage=USAGE_EMITTER, hlsl='Ninja_WaveParams.hlsl', gets=[
        ('User.WaveSimEnabled', 'bool', 'false', 'SimEnabled'),
        ('Emitter.FluidNinja.EnableOutputPressureDivergence', 'bool', 'false', 'ExportPD'),
        ('Emitter.RT_Pressure', 'RenderTarget2D', '', 'RT_Pressure'),
        ('Emitter.WaveGrid', 'Grid2DCollection', '', 'WaveGrid'),
        ('Emitter.WaveOp', 'RenderTarget2D', '', 'WaveOp'),
        ('Emitter.WaveAux', 'RenderTarget2D', '', 'WaveAux'),
        ('System.WorldOffsetDelta', 'vec3', V3, 'OffsetDelta'),
        ('Emitter.Wave.Remainder', 'vec2', V2, 'PrevRemainder'),
        ('User.Wave.DepthCm', 'float', '200.0', 'InDepthCm'),
        ('User.Wave.DepthFromBed', 'float', '1.0', 'InDepthFromBed'),
        ('User.Wave.Damping', 'float', '0.02', 'InDamping'),
        ('User.Wave.SpongeCm', 'float', '300.0', 'InSpongeCm'),
        ('User.Wave.WallAbsorption', 'float', '0.0', 'InWallAbsorption'),
        ('User.Wave.WallBandCm', 'float', '50.0', 'InWallBandCm'),
        ('User.Wave.EdgeOpen', 'float', '1.0', 'InEdgeOpen'),
        ('User.Wave.ShoreDepthCm', 'float', '10.0', 'InShoreDepthCm'),
        ('User.Wave.BreakingSlope', 'float', '1.0', 'InBreakingSlope'),
        ('User.Wave.Hyperviscosity', 'float', '0.5', 'InHyperviscosity'),
        ('User.Wave.CurrentCoupling', 'float', '1.0', 'InCurrentCoupling'),
        ('User.Wave.MeanRelax', 'float', '0.5', 'InMeanRelax'),
        # Interface version 5: the closed-window <v> pin's rate (1/s; 0 off), floored at 0 like MeanRelax; Wave Step clamps both.
        ('User.Wave.VMeanRelax', 'float', '0.0', 'InVMeanRelax'),
        # Interface version 3: the scale-down, and the window's extents (cm) for the wave cell it publishes.
        ('User.Wave.ScaleDown', 'int', '1', 'InScaleDown'),
        ('System.WorldGridExtents', 'vec2', V2, 'Extents'),
        # Interface version 4: the foam's switch, tuning and export target.
        ('User.Wave.Foam', 'float', '1.0', 'InFoam'),
        ('User.Wave.FoamDecay', 'float', '0.7', 'InFoamDecay'),
        ('User.Wave.FoamBreaking', 'float', '1.0', 'InFoamBreaking'),
        ('User.Wave.FoamWake', 'float', '1.0', 'InFoamWake'),
        ('User.Wave.FoamImpact', 'float', '1.0', 'InFoamImpact'),
        ('Emitter.WaveFoam', 'RenderTarget2D', '', 'WaveFoam'),
        # Interface version 6: the aged foam's and the wash's tuning, floored at 0 (FoamTransfer clamped to 0..1).
        ('User.Wave.FoamAgedDecay', 'float', '0.02', 'InFoamAgedDecay'),
        ('User.Wave.FoamTransfer', 'float', '0.5', 'InFoamTransfer'),
        ('User.Wave.FoamDiffusion', 'float', '30.0', 'InFoamDiffusion'),
        ('User.Wave.FoamAgedDiffusion', 'float', '200.0', 'InFoamAgedDiffusion'),
        ('User.Wave.FoamSlope', 'float', '0.3', 'InFoamSlope'),
        ('User.Wave.FoamWash', 'float', '1.0', 'InFoamWash'),
    ], sets=[
        ('Enabled', 'bool', 'Emitter.Wave.Enabled'),
        ('ExportEnabled', 'bool', 'Emitter.Wave.ExportEnabled'),
        ('AuxEnabled', 'bool', 'Emitter.Wave.AuxEnabled'),
        ('Scroll', 'vec2', 'Emitter.Wave.Scroll'),
        ('Remainder', 'vec2', 'Emitter.Wave.Remainder'),
        # Interface version 3: what the wave grid is, for the stages (ScaleDown) and anything else on the emitter (its cells and cell, cm).
        ('ScaleDown', 'int', 'Emitter.Wave.ScaleDown'),
        ('Cells', 'vec2', 'Emitter.Wave.Cells'),
        ('CellCm', 'float', 'Emitter.Wave.CellCm'),
        ('DepthCm', 'float', 'Emitter.Wave.DepthCm'),
        ('DepthFromBed', 'float', 'Emitter.Wave.DepthFromBed'),
        ('Damping', 'float', 'Emitter.Wave.Damping'),
        ('SpongeCm', 'float', 'Emitter.Wave.SpongeCm'),
        ('WallAbsorption', 'float', 'Emitter.Wave.WallAbsorption'),
        ('WallBandCm', 'float', 'Emitter.Wave.WallBandCm'),
        ('EdgeOpen', 'float', 'Emitter.Wave.EdgeOpen'),
        ('ShoreDepthCm', 'float', 'Emitter.Wave.ShoreDepthCm'),
        ('BreakingSlope', 'float', 'Emitter.Wave.BreakingSlope'),
        ('Hyperviscosity', 'float', 'Emitter.Wave.Hyperviscosity'),
        ('CurrentCoupling', 'float', 'Emitter.Wave.CurrentCoupling'),
        ('MeanRelax', 'float', 'Emitter.Wave.MeanRelax'),
        # Interface version 5: the closed-window <v> pin's rate, for Wave Step.
        ('VMeanRelax', 'float', 'Emitter.Wave.VMeanRelax'),
        # Interface version 4: FoamExport (the foam target has the grid's size and Wave Aux runs: it is written, 0 when off), FoamEnabled
        # (and User.Wave.Foam asks for it: Wave Prepare and Wave Step accumulate it), the tuning.
        ('FoamExport', 'bool', 'Emitter.Wave.FoamExport'),
        ('FoamEnabled', 'bool', 'Emitter.Wave.FoamEnabled'),
        ('FoamDecay', 'float', 'Emitter.Wave.FoamDecay'),
        ('FoamBreaking', 'float', 'Emitter.Wave.FoamBreaking'),
        ('FoamWake', 'float', 'Emitter.Wave.FoamWake'),
        ('FoamImpact', 'float', 'Emitter.Wave.FoamImpact'),
        # Interface version 6: for Wave Step.
        ('FoamAgedDecay', 'float', 'Emitter.Wave.FoamAgedDecay'),
        ('FoamTransfer', 'float', 'Emitter.Wave.FoamTransfer'),
        ('FoamDiffusion', 'float', 'Emitter.Wave.FoamDiffusion'),
        ('FoamAgedDiffusion', 'float', 'Emitter.Wave.FoamAgedDiffusion'),
        ('FoamSlope', 'float', 'Emitter.Wave.FoamSlope'),
        ('FoamWash', 'float', 'Emitter.Wave.FoamWash'),
    ]),
    'Ninja_WavePrepare': dict(usage=USAGE_STAGE, hlsl='Ninja_WavePrepare.hlsl', gets=[
        ('Emitter.WaveOp', 'RenderTarget2D', '', 'WaveOp'),
        ('Emitter.WaveGrid', 'Grid2DCollection', '', 'WaveGrid'),
        ('Emitter.WaveSource', 'Texture', '', 'WaveSource'),
        ('Emitter.CompositeGrid', 'Grid2DCollection', '', 'CompositeGrid'),
        ('Emitter.FieldCaptureGrid', 'Grid2DCollection', '', 'FieldCaptureGrid'),
        ('System.TransformMatrices2D.UnitToWorld', 'matrix', '', 'UnitToWorld'),
        ('Engine.Owner.LWCTile', 'vec4', V4, 'LWCTile'),
        ('Emitter.GridCellSizeInUnitSpace', 'vec2', V2, 'CellUnit'),
        ('Emitter.SimSpeedCoefficient', 'vec2', V2, 'SimSpeed'),
        ('Emitter.Wave.Scroll', 'vec2', V2, 'Scroll'),
        ('Emitter.Wave.Remainder', 'vec2', V2, 'Remainder'),
        ('Emitter.Wave.CurrentCoupling', 'float', '1.0', 'CurrentCoupling'),
        ('Emitter.Wave.EdgeOpen', 'float', '1.0', 'EdgeOpen'),
        ('Engine.DeltaTime', 'float', '0.016667', 'DeltaTime'),
        ('Engine.System.NumTimeSteps', 'int', '1', 'NumTimeSteps'),
        ('Emitter.SDFPainter.SDFAsCollisionMask', 'bool', 'false', 'SDFCollision'),
        ('Emitter.FluidNinja.EnableMeshDistanceFieldReader', 'bool', 'false', 'SDFReader'),
        ('Emitter.RVTPainter.RVTasCollisionMask', 'bool', 'false', 'HeightCollision'),
        ('Emitter.FluidNinja.EnableHeightFieldReaders', 'bool', 'false', 'HeightReaders'),
        ('Emitter.RVTPainter.ClampHeightLowerValues', 'bool', 'false', 'HeightClamp'),
        ('Emitter.RVTPainter.ClampingValue', 'float', '0.0', 'ClampingValue'),
        ('Emitter.Wave.ScaleDown', 'int', '1', 'ScaleDown'),
        # Interface version 4: the foam carried with the state, into the foam target as this tick's scratch for Wave Step.
        ('Emitter.WaveFoam', 'RenderTarget2D', '', 'WaveFoam'),
        ('Emitter.Wave.FoamEnabled', 'bool', 'false', 'FoamEnabled'),
    ], sets=[('Done', 'float', 'Output.Module.WavePrepared')]),
    'Ninja_WaveStep': dict(usage=USAGE_STAGE, hlsl='Ninja_WaveStep.hlsl', gets=[
        ('Emitter.WaveGrid', 'Grid2DCollection', '', 'WaveGrid'),
        ('Emitter.WaveOp', 'RenderTarget2D', '', 'WaveOp'),
        ('Emitter.WaveSource', 'Texture', '', 'WaveSource'),
        ('Emitter.FieldCaptureGrid', 'Grid2DCollection', '', 'FieldCaptureGrid'),
        ('System.TransformMatrices2D.UnitToWorld', 'matrix', '', 'UnitToWorld'),
        ('Engine.Owner.LWCTile', 'vec4', V4, 'LWCTile'),
        ('System.WorldGridExtents', 'vec2', V2, 'Extents'),
        ('Emitter.Wave.Remainder', 'vec2', V2, 'Remainder'),
        ('Engine.DeltaTime', 'float', '0.016667', 'DeltaTime'),
        ('Emitter.Wave.DepthCm', 'float', '200.0', 'DepthCm'),
        ('Emitter.Wave.DepthFromBed', 'float', '1.0', 'DepthFromBed'),
        ('Emitter.Wave.Damping', 'float', '0.02', 'Damping'),
        ('Emitter.Wave.SpongeCm', 'float', '300.0', 'SpongeCm'),
        ('Emitter.Wave.WallAbsorption', 'float', '0.0', 'WallAbsorption'),
        ('Emitter.Wave.WallBandCm', 'float', '50.0', 'WallBandCm'),
        ('Emitter.Wave.EdgeOpen', 'float', '1.0', 'EdgeOpen'),
        ('Emitter.Wave.ShoreDepthCm', 'float', '10.0', 'ShoreDepthCm'),
        ('Emitter.Wave.BreakingSlope', 'float', '1.0', 'BreakingSlope'),
        ('Emitter.Wave.Hyperviscosity', 'float', '0.5', 'Hyperviscosity'),
        ('Emitter.Wave.MeanRelax', 'float', '0.5', 'MeanRelax'),
        # Interface version 5: the closed-window <v> pin's rate (1/s; 0 off; a walled window only, clamped to 1 / Dt in the step).
        ('Emitter.Wave.VMeanRelax', 'float', '0.0', 'VMeanRelax'),
        ('Emitter.FluidNinja.EnableHeightFieldReaders', 'bool', 'false', 'HeightReaders'),
        ('Emitter.RVTPainter.ClampHeightLowerValues', 'bool', 'false', 'HeightClamp'),
        ('Emitter.RVTPainter.ClampingValue', 'float', '0.0', 'ClampingValue'),
        ('Emitter.Wave.ScaleDown', 'int', '1', 'ScaleDown'),
        # Interface version 4: the foam (the scratch Wave Prepare left in the foam target, the tuning, the frame's ticks for the impulses' share).
        ('Emitter.WaveFoam', 'RenderTarget2D', '', 'WaveFoam'),
        ('Emitter.Wave.FoamEnabled', 'bool', 'false', 'FoamEnabled'),
        ('Emitter.Wave.FoamDecay', 'float', '0.7', 'FoamDecay'),
        ('Emitter.Wave.FoamBreaking', 'float', '1.0', 'FoamBreaking'),
        ('Emitter.Wave.FoamWake', 'float', '1.0', 'FoamWake'),
        ('Emitter.Wave.FoamImpact', 'float', '1.0', 'FoamImpact'),
        ('Engine.System.NumTimeSteps', 'int', '1', 'NumTimeSteps'),
        # Interface version 6: the aged foam, the foam's own breaking slope, the spreading and the wash (texture and scale).
        ('Emitter.Wave.FoamAgedDecay', 'float', '0.02', 'FoamAgedDecay'),
        ('Emitter.Wave.FoamTransfer', 'float', '0.5', 'FoamTransfer'),
        ('Emitter.Wave.FoamDiffusion', 'float', '30.0', 'FoamDiffusion'),
        ('Emitter.Wave.FoamAgedDiffusion', 'float', '200.0', 'FoamAgedDiffusion'),
        ('Emitter.Wave.FoamSlope', 'float', '0.3', 'FoamSlope'),
        ('Emitter.Wave.FoamWash', 'float', '1.0', 'FoamWash'),
        ('Emitter.WaveWash', 'Texture', '', 'WaveWash'),
    ], sets=[('Done', 'float', 'Output.Module.WaveStepped')]),
    'Ninja_WaveExport': dict(usage=USAGE_STAGE, hlsl='Ninja_WaveExport.hlsl', gets=[
        ('Emitter.RT_Pressure', 'RenderTarget2D', '', 'RT_Pressure'),
        ('Emitter.PressureGrid', 'Grid2DCollection', '', 'PressureGrid'),
        ('Emitter.RT_PressureResolutionScaleDownFactor', 'int', '2', 'Factor'),
        ('Emitter.WaveGrid', 'Grid2DCollection', '', 'WaveGrid'),
        ('Emitter.WaveSource', 'Texture', '', 'WaveSource'),
        ('System.TransformMatrices2D.UnitToWorld', 'matrix', '', 'UnitToWorld'),
        ('Engine.Owner.LWCTile', 'vec4', V4, 'LWCTile'),
        ('Emitter.Wave.Remainder', 'vec2', V2, 'Remainder'),
        ('Emitter.Wave.ScaleDown', 'int', '1', 'ScaleDown'),
        # Interface version 7: the wind sea (the plugin's table) and what its fade across an open window's sponge band needs.
        ('Emitter.WaveWindSea', 'Texture', '', 'WaveWindSea'),
        ('System.WorldGridExtents', 'vec2', V2, 'Extents'),
        ('Emitter.Wave.SpongeCm', 'float', '300.0', 'SpongeCm'),
        ('Emitter.Wave.EdgeOpen', 'float', '1.0', 'EdgeOpen'),
    ], sets=[('Done', 'float', 'Output.Module.WaveExported')]),
    'Ninja_WaveAux': dict(usage=USAGE_STAGE, hlsl='Ninja_WaveAux.hlsl', gets=[
        ('Emitter.WaveAux', 'RenderTarget2D', '', 'WaveAux'),
        ('Emitter.CompositeGrid', 'Grid2DCollection', '', 'CompositeGrid'),
        ('Emitter.FieldCaptureGrid', 'Grid2DCollection', '', 'FieldCaptureGrid'),
        ('System.TransformMatrices2D.UnitToWorld', 'matrix', '', 'UnitToWorld'),
        ('Emitter.GridCellSizeInUnitSpace', 'vec2', V2, 'CellUnit'),
        ('Emitter.SimSpeedCoefficient', 'vec2', V2, 'SimSpeed'),
        ('Emitter.Wave.CurrentCoupling', 'float', '1.0', 'CurrentCoupling'),
        ('Emitter.Wave.EdgeOpen', 'float', '1.0', 'EdgeOpen'),
        ('Emitter.Wave.DepthCm', 'float', '200.0', 'DepthCm'),
        ('Emitter.Wave.DepthFromBed', 'float', '1.0', 'DepthFromBed'),
        ('Emitter.SDFPainter.SDFAsCollisionMask', 'bool', 'false', 'SDFCollision'),
        ('Emitter.FluidNinja.EnableMeshDistanceFieldReader', 'bool', 'false', 'SDFReader'),
        ('Emitter.RVTPainter.RVTasCollisionMask', 'bool', 'false', 'HeightCollision'),
        ('Emitter.FluidNinja.EnableHeightFieldReaders', 'bool', 'false', 'HeightReaders'),
        ('Emitter.RVTPainter.ClampHeightLowerValues', 'bool', 'false', 'HeightClamp'),
        ('Emitter.RVTPainter.ClampingValue', 'float', '0.0', 'ClampingValue'),
        ('Emitter.Wave.ScaleDown', 'int', '1', 'ScaleDown'),
        # Interface version 4: the foam export (this tick's Foam attribute at the window's cell centres, into the foam target).
        ('Emitter.WaveGrid', 'Grid2DCollection', '', 'WaveGrid'),
        ('Emitter.WaveFoam', 'RenderTarget2D', '', 'WaveFoam'),
        ('Emitter.Wave.FoamExport', 'bool', 'false', 'FoamExport'),
        ('Emitter.Wave.FoamEnabled', 'bool', 'false', 'FoamEnabled'),
        ('Emitter.Wave.Remainder', 'vec2', V2, 'Remainder'),
    ], sets=[('Done', 'float', 'Output.Module.WaveAuxWritten')]),
}

# Emitter spawn data interfaces: parameter, type, properties (export text), bindings.
SPAWN_DIS = [
    ('Emitter.WaveGrid', 'Grid2DCollection', [('NumCellsX', '1'), ('NumCellsY', '1'), ('bOverrideFormat', 'True'), ('OverrideBufferFormat', 'Float')], []),
    ('Emitter.WaveOp', 'RenderTarget2D', [('Size', '(X=1,Y=1)'), ('bInheritUserParameterSettings', 'False'), ('bOverrideFormat', 'True'),
                                          ('OverrideRenderTargetFormat', 'RTF_RGBA32f'), ('MipMapGeneration', 'PostStage'),
                                          ('MipMapGenerationType', 'Blur2'), ('OverrideRenderTargetFilter', 'TF_Bilinear')], []),
    ('Emitter.WaveSource', 'Texture', [], [('TextureUserParameter', 'User.WaveSourceTexture')]),
    # Written by Wave Aux into the reader's own target (its size and format: RGBA32f at the wave grid's size), never resized here.
    ('Emitter.WaveAux', 'RenderTarget2D', [('Size', '(X=1,Y=1)'), ('bInheritUserParameterSettings', 'True')],
     [('RenderTargetUserParameter', 'User.WaveAuxTarget')]),
    # Interface version 4: the foam export, into the reader's own target (one channel at the wave grid's size), never resized here. Wave
    # Prepare writes its scratch into it and Wave Aux the final F; Wave Step reads it between the two.
    ('Emitter.WaveFoam', 'RenderTarget2D', [('Size', '(X=1,Y=1)'), ('bInheritUserParameterSettings', 'True')],
     [('RenderTargetUserParameter', 'User.WaveFoamTarget')]),
    # Interface version 6: the plugin's foam wash (W x H RGBA16F on the source texture's cells), read by Wave Step only.
    ('Emitter.WaveWash', 'Texture', [], [('TextureUserParameter', 'User.WaveWashTexture')]),
    # Interface version 7: the plugin's wind-sea table ((N + 1) x 1 RGBA32F), read by Wave Export only.
    ('Emitter.WaveWindSea', 'Texture', [], [('TextureUserParameter', 'User.WaveWindSeaTexture')]),
]

# Stages: name, after, iteration source, enabled binding, module.
STAGES = [
    ('Wave Prepare', 'Pressure', 'Emitter.WaveOp', 'Emitter.Wave.Enabled', 'Ninja_WavePrepare'),
    ('Wave Step', 'Wave Prepare', 'Emitter.WaveGrid', 'Emitter.Wave.Enabled', 'Ninja_WaveStep'),
    ('Wave Aux', 'Wave Step', 'Emitter.WaveAux', 'Emitter.Wave.AuxEnabled', 'Ninja_WaveAux'),
    ('Wave Export', 'Export PressureAndDivergence', 'Emitter.RT_Pressure', 'Emitter.Wave.ExportEnabled', 'Ninja_WaveExport'),
]

lines = []
failures = []


def log(*parts):
    text = ' '.join(str(p) for p in parts)
    lines.append(text)
    if unreal:
        unreal.log('FNBWAVES ' + text)
    else:
        print('FNBWAVES ' + text)


def fail(text):
    failures.append(text)
    log('FAIL ' + text)
    raise RuntimeError(text)


def flush():
    os.makedirs(os.path.dirname(REPORT), exist_ok=True)
    with open(REPORT, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')


def package_file(asset_path):
    rel = asset_path[len('/Game/'):] if asset_path.startswith('/Game/') else asset_path
    return os.path.join(CONTENT_DIR, rel.replace('/', os.sep) + '.uasset')


def file_sha(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def known(table, digest):
    """(kind, description) of a hash in one of the KNOWN_* tables, ('unknown', ...) when it is in none."""
    return table.get(digest, ('unknown', 'not in this toolchain\'s tables (VERSIONS.md)'))


def protected_hashes():
    out = {}
    for pattern in PROTECTED:
        for path in sorted(glob.glob(os.path.join(CONTENT_DIR, 'FluidNinjaLive', pattern.replace('/', os.sep)))):
            out[os.path.relpath(path, CONTENT_DIR)] = file_sha(path)
    return out


def need_editor(step):
    if unreal is None:
        fail('the %s step runs inside the editor (UnrealEditor-Cmd -run=pythonscript); only unapply runs under plain Python' % step)
    if DEV is None:
        fail('unreal.FNBDevLibrary is missing: build the plugin first (its editor module FluidNinjaBuoyancyEditor carries the helpers)')


def read_hlsl(name):
    """A module's HLSL with its //@include snippets expanded."""
    def load(fname):
        with open(os.path.join(HLSL_DIR, fname), 'r', encoding='utf-8') as f:
            return f.read()
    text = load(name)
    for _ in range(4):
        text = re.sub(r'^//@include (\S+)\s*$', lambda m: load(m.group(1)).rstrip('\n'), text, flags=re.M)
    return text.replace('\r\n', '\n')


def find_nodes(graph, class_name):
    return [n for n in DEV.get_nodes(graph) if n.get_class().get_name() == class_name]


# ---------------------------------------------------------------------------------------------------------------------------------
# Module assets
# ---------------------------------------------------------------------------------------------------------------------------------
def build_module(folder, name, spec):
    path = folder + '/' + name
    if EAL.does_asset_exist(path):
        if not EAL.delete_asset(path) or os.path.exists(package_file(path)):
            fail('could not remove the old module %s (delete it with the editor closed and run again)' % path)
    module = EAL.duplicate_asset(TEMPLATE_MODULE, path)
    if not module:
        fail('duplicate %s -> %s' % (TEMPLATE_MODULE, path))
    graph = DEV.get_script_graph(module)
    gets = find_nodes(graph, 'NiagaraNodeParameterMapGet')
    sets = find_nodes(graph, 'NiagaraNodeParameterMapSet')
    if len(gets) != 1 or len(sets) != 1:
        fail('%s: the template module has %d map gets and %d map sets' % (name, len(gets), len(sets)))
    map_get, map_set = gets[0], sets[0]
    for param, kind, default, _pin in spec['gets']:
        err = DEV.add_map_get_output(map_get, param, kind, default)
        if err and err != 'EXISTS':
            fail('%s: map get %s (%s): %s' % (name, param, kind, err))
    hlsl = read_hlsl(spec['hlsl'])
    node, err = DEV.add_custom_hlsl_node(graph, hlsl, 600, 0)
    if node is None:
        fail('%s: custom HLSL node: %s' % (name, err))
    err = DEV.add_custom_hlsl_pins(node, [g[3] for g in spec['gets']], [g[1] for g in spec['gets']],
                                   [s[0] for s in spec['sets']], [s[1] for s in spec['sets']])
    if err:
        fail('%s: custom HLSL pins: %s' % (name, err))
    for param, _kind, _default, pin in spec['gets']:
        err = DEV.link_pins(map_get, param, node, pin)
        if err:
            fail('%s: link %s -> %s: %s' % (name, param, pin, err))
    for pin, kind, param in spec['sets']:
        err = DEV.add_map_set_input(map_set, param, kind)
        if err and err != 'EXISTS':
            fail('%s: map set %s: %s' % (name, param, err))
        err = DEV.link_pins(node, pin, map_set, param)
        if err:
            fail('%s: link %s -> %s: %s' % (name, pin, param, err))
    err = DEV.set_module_usage_bitmask(module, spec['usage'])
    if err:
        fail('%s: usage bitmask: %s' % (name, err))
    log('module %s: %d inputs, %d outputs, usage %d, %d HLSL lines' % (path, len(spec['gets']), len(spec['sets']),
                                                                      DEV.get_module_usage_bitmask(module), hlsl.count('\n')))
    return module


# ---------------------------------------------------------------------------------------------------------------------------------
# Core
# ---------------------------------------------------------------------------------------------------------------------------------
def make_unbound_aux():
    """User.WaveAuxTarget's default (AUX_UNBOUND): 1 x 1 RGBA32f, black, made or reset here."""
    target = unreal.load_asset(AUX_UNBOUND) if EAL.does_asset_exist(AUX_UNBOUND) else None
    if target is None:
        name = AUX_UNBOUND.rsplit('/', 1)[1]
        target = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, WAVES, unreal.TextureRenderTarget2D, unreal.TextureRenderTargetFactoryNew())
    if not isinstance(target, unreal.TextureRenderTarget2D):
        fail('could not make ' + AUX_UNBOUND)
    target.set_editor_property('size_x', 1)
    target.set_editor_property('size_y', 1)
    target.set_editor_property('render_target_format', unreal.TextureRenderTargetFormat.RTF_RGBA32F)
    target.set_editor_property('clear_color', unreal.LinearColor(0.0, 0.0, 0.0, 0.0))
    target.set_editor_property('auto_generate_mips', False)
    log('unbound Wave Aux target', AUX_UNBOUND, '%d x %d' % (target.size_x, target.size_y), target.render_target_format)
    return target


def cbuffer_members(hlsl):
    m = re.search(r'cbuffer FNiagaraExternalParameters\s*\{(.*?)\}', hlsl, re.S)
    return set(re.findall(r'^\s*\w+\s+(\w+);', m.group(1), re.M)) if m else set()


def compile_problems(result):
    """Error lines of a CompileNiagaraSystem report, and GPU emitters without a valid shader map (a disabled emitter has none)."""
    out = set()
    for l in result.splitlines():
        t = l.strip()
        if t.startswith('Error') or t.startswith('GPU Error'):
            out.add(t)
        elif t.startswith('GPUSHADER') and ('shadermap=valid' not in t or 'errors=0' not in t):
            out.add(' '.join(t.split()[:2]) + ' ' + ' '.join(x for x in t.split() if x.startswith('shadermap=')))
    return out


def stage_names(system):
    return [str(s.get_editor_property('simulation_stage_name')) for s in DEV.get_simulation_stages(system, EMITTER)]


def core_base(summary):
    """Which core DescribeNiagara's user parameters show: the pass3 (relative tracking) core carries User.TrackVelocityRelativeToWater."""
    return 'pass3 (relative tracking)' if 'User.TrackVelocityRelativeToWater' in summary else 'stock'


def core_has_waves(system, summary):
    return 'User.WaveSimEnabled' in summary or 'Wave Prepare' in stage_names(system)


def user_int(summary, name):
    """An int user parameter's default as DescribeNiagara prints it ("User.Wave.CoreBuild : int32 = NiagaraInt32(Value=20261017)"), else None."""
    m = re.search(r'^\s*' + re.escape(name) + r'\s*:\s*\S+\s*=\s*\w*\(Value=(-?\d+)\)', summary, re.M)
    return int(m.group(1)) if m else None


def apply_core():
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.scan_paths_synchronous(['/Niagara/DefaultAssets', '/Game/FluidNinjaLive'], True)
    registry.wait_for_completion()
    before = protected_hashes()
    system = unreal.load_asset(CORE)
    if not system:
        fail('no ' + CORE)
    summary = DEV.describe_niagara(system, '', '')
    if core_has_waves(system, summary):
        fail('the core already has the waves (interface version %s, build %s): start again from the pass3 relative-tracking core, not the vendor one'
             % (user_int(summary, 'User.Wave.InterfaceVersion'), user_int(summary, 'User.Wave.CoreBuild')))
    base = core_base(summary)
    log('core is', base)
    if base == 'stock':
        log('WARNING stock base: the result will have the waves but no relative tracking (this script consumes it and cannot add it); start from the pass3 core')
    names = stage_names(system)
    for needed in ['Pressure', 'Export PressureAndDivergence']:
        if needed not in names:
            fail('stage %s missing; stages: %s' % (needed, names))
    spawn_text = DEV.describe_niagara(system, EMITTER, 'EmitterSpawnScript')
    update_text = DEV.describe_niagara(system, EMITTER, 'EmitterUpdateScript')
    if 'MODULE NinjaInitGrids ' not in spawn_text or 'MODULE Ninja_InitParams002 ' not in update_text:
        fail('NinjaInitGrids / Ninja_InitParams002 not found in the emitter stacks')
    baseline = DEV.compile_niagara_system(system, True, True)
    problems_before = compile_problems(baseline)
    log('baseline compile:', baseline.splitlines()[0], '| known problems:', sorted(problems_before))
    hlsl_before = DEV.get_niagara_gpu_hlsl(system, EMITTER)
    members_before = cbuffer_members(hlsl_before)

    # User parameters (WaveAuxTarget's default is the unbound target, made first)
    unbound = make_unbound_aux()
    for name, kind, default in USER_PARAMS:
        stored = DEV.add_user_variable(system, name, kind, default)
        if stored.startswith('ERROR'):
            fail('user parameter %s: %s' % (name, stored))
        log('user parameter', stored, kind, default or '(none)')
    log('interface version', INTERFACE_VERSION, 'core build', CORE_BUILD)

    # Modules
    modules = {name: build_module(WAVES, name, spec) for name, spec in MODULES.items()}

    # Emitter spawn data interfaces, in order after NinjaInitGrids
    after = 'NinjaInitGrids'
    for param, kind, props, bindings in SPAWN_DIS:
        module_name = DEV.add_niagara_set_parameter_after(system, EMITTER, 'EmitterSpawnScript', after, param, kind)
        if module_name.startswith('ERROR'):
            fail('spawn %s: %s' % (param, module_name))
        di, err = DEV.get_niagara_input_data_interface(system, EMITTER, 'EmitterSpawnScript', module_name, param, True)
        if di is None:
            fail('spawn %s data interface: %s' % (param, err))
        for prop, text in props:
            if not DEV.set_property_text(di, prop, text) or DEV.get_property_text(di, prop) != text:
                fail('%s.%s = %s (is %s)' % (param, prop, text, DEV.get_property_text(di, prop)))
        for prop, user in bindings:
            err = DEV.set_niagara_binding(di, prop, user)
            if err:
                fail('%s.%s -> %s: %s' % (param, prop, user, err))
        log('emitter spawn', module_name, param, kind, '|', ' '.join(DEV.describe_changed_properties(di).split()))
        after = module_name

    # Emitter update module
    params_module = DEV.add_niagara_module_after(system, EMITTER, 'EmitterUpdateScript', 'Ninja_InitParams002', modules['Ninja_WaveParams'].get_path_name())
    if params_module.startswith('ERROR'):
        fail('emitter update Ninja_WaveParams: ' + params_module)
    log('emitter update module', params_module, 'after Ninja_InitParams002')

    # Stages
    for name, after_stage, source, enabled, module in STAGES:
        stage, err = DEV.add_simulation_stage(system, EMITTER, name, after_stage, source, enabled, modules[module], 1)
        if stage is None:
            fail('stage %s: %s' % (name, err))
        log('stage', name, 'after', after_stage, 'iterates', source, 'enabled by', enabled, err)

    # Compile
    result = DEV.compile_niagara_system(system, True, True)
    log('COMPILE\n' + result)
    new_problems = sorted(compile_problems(result) - problems_before)
    if new_problems or 'compiling=0' not in result.splitlines()[0]:
        fail('the core does not compile cleanly (nothing saved): %s' % new_problems[:20])
    for stage in DEV.get_simulation_stages(system, EMITTER):
        sname = str(stage.get_editor_property('simulation_stage_name'))
        for _n, _a, _s, enabled, _m in STAGES:
            if sname == _n:
                DEV.set_niagara_binding(stage, 'EnabledBinding', enabled)   # refreshes the editor's exists flag
    result = DEV.compile_niagara_system(system, True, False)
    if sorted(compile_problems(result) - problems_before) or 'compiling=0' not in result.splitlines()[0]:
        fail('recompile after the binding refresh: ' + result.splitlines()[0])
    log('STAGES\n' + DEV.describe_simulation_stages(system, EMITTER))
    hlsl_after = DEV.get_niagara_gpu_hlsl(system, EMITTER)
    added = sorted(cbuffer_members(hlsl_after) - members_before)
    removed = sorted(members_before - cbuffer_members(hlsl_after))
    log('GPU constants added:', added)
    log('GPU constants removed:', removed)
    stray = [m for m in added if 'Wave' not in m and not m.startswith('PREV_')]
    if stray or removed:
        fail('unexpected GPU constants (a misspelled parameter reads its default): added %s removed %s' % (stray, removed))
    for stage_name, *_ in STAGES:
        tag = '_' + stage_name.replace(' ', '')
        if tag not in hlsl_after:
            fail('GPU HLSL has no %s stage' % stage_name)

    describe_core(system)

    # Save
    if not EAL.save_loaded_asset(unbound, False):
        fail('save ' + AUX_UNBOUND)
    for module in modules.values():
        if not EAL.save_loaded_asset(module, False):
            fail('save ' + module.get_path_name())
    if not EAL.save_loaded_asset(system, False):
        fail('save ' + CORE)
    after_hashes = protected_hashes()
    changed = [k for k in before if before[k] != after_hashes.get(k)]
    if changed:
        fail('protected vendor files changed: %s' % changed)
    log('protected files unchanged:', len(before))
    digest = file_sha(package_file(CORE))
    log('saved', CORE, 'sha256', digest)
    log('saved core sha8', digest[:8], 'tag', 'core-' + digest[:8], '(add it to VERSIONS.md and KNOWN_CORES)')


def apply_windsea():
    """The windsea step: interface version 7's wind sea added to an installed interface 6 core in place, keeping everything else it carries
    (no unapply): the user parameter User.WaveWindSeaTexture, the spawn DI Emitter.WaveWindSea bound to it (after NinjaInitGrids), the
    Ninja_WaveExport module's new inputs (MODULES) and its HLSL (Ninja_WaveExport.hlsl with _windsea.hlsl), the version stamps
    (Wave.InterfaceVersion INTERFACE_VERSION, Wave.CoreBuild) and the descriptions. Refuses a core without the waves, below interface 6, or
    that has the wind sea already. Nothing is saved unless the core compiles with no new problems."""
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.scan_paths_synchronous(['/Niagara/DefaultAssets', '/Game/FluidNinjaLive'], True)
    registry.wait_for_completion()
    before = protected_hashes()
    system = unreal.load_asset(CORE)
    if not system:
        fail('no ' + CORE)
    summary = DEV.describe_niagara(system, '', '')
    if not core_has_waves(system, summary):
        fail('the core has no waves: run the core step from the pass3 core (it builds the wind sea in)')
    version = user_int(summary, 'User.Wave.InterfaceVersion')
    if version is None or version < 6:
        fail('interface version %s: the windsea step needs an interface 6 core (re-apply from the pass3 core instead)' % version)
    if 'User.WaveWindSeaTexture' in summary:
        fail('the core has the wind sea already (User.WaveWindSeaTexture, interface version %s)' % version)
    log('windsea: core at interface version', version, 'build', user_int(summary, 'User.Wave.CoreBuild'), '| base', core_base(summary))
    baseline = DEV.compile_niagara_system(system, True, True)
    problems_before = compile_problems(baseline)
    log('baseline compile:', baseline.splitlines()[0], '| known problems:', sorted(problems_before))
    members_before = cbuffer_members(DEV.get_niagara_gpu_hlsl(system, EMITTER))

    # The user parameter and the version stamps (adding an existing one sets its default).
    for name, kind, default in [('WaveWindSeaTexture', 'Texture', ''), ('Wave.InterfaceVersion', 'int', str(INTERFACE_VERSION)),
                                ('Wave.CoreBuild', 'int', CORE_BUILD)]:
        stored = DEV.add_user_variable(system, name, kind, default)
        if stored.startswith('ERROR'):
            fail('user parameter %s: %s' % (name, stored))
        log('windsea: user parameter', stored, kind, default or '(none)')

    # The spawn DI.
    param, kind, props, bindings = next(d for d in SPAWN_DIS if d[0] == 'Emitter.WaveWindSea')
    module_name = DEV.add_niagara_set_parameter_after(system, EMITTER, 'EmitterSpawnScript', 'NinjaInitGrids', param, kind)
    if module_name.startswith('ERROR'):
        fail('spawn %s: %s' % (param, module_name))
    di, err = DEV.get_niagara_input_data_interface(system, EMITTER, 'EmitterSpawnScript', module_name, param, True)
    if di is None:
        fail('spawn %s data interface: %s' % (param, err))
    for prop, text in props:
        if not DEV.set_property_text(di, prop, text) or DEV.get_property_text(di, prop) != text:
            fail('%s.%s = %s (is %s)' % (param, prop, text, DEV.get_property_text(di, prop)))
    for prop, user in bindings:
        err = DEV.set_niagara_binding(di, prop, user)
        if err:
            fail('%s.%s -> %s: %s' % (param, prop, user, err))
    log('windsea: emitter spawn', module_name, param, kind, '|', ' '.join(DEV.describe_changed_properties(di).split()))

    # The Export module, in place: its new inputs, then its HLSL.
    spec = MODULES['Ninja_WaveExport']
    export_path = WAVES + '/Ninja_WaveExport'
    module = unreal.load_asset(export_path)
    if not module:
        fail('no ' + export_path)
    graph = DEV.get_script_graph(module)
    gets = find_nodes(graph, 'NiagaraNodeParameterMapGet')
    customs = find_nodes(graph, 'NiagaraNodeCustomHlsl')
    if len(gets) != 1 or len(customs) != 1:
        fail('%s: %d map gets and %d custom HLSL nodes (one each expected)' % (export_path, len(gets), len(customs)))
    map_get, node = gets[0], customs[0]
    added_gets = []
    for get_param, get_kind, get_default, pin in spec['gets']:
        err = DEV.add_map_get_output(map_get, get_param, get_kind, get_default)
        if err == 'EXISTS':
            continue
        if err:
            fail('Ninja_WaveExport: map get %s (%s): %s' % (get_param, get_kind, err))
        added_gets.append((get_param, get_kind, pin))
    err = DEV.add_custom_hlsl_pins(node, [g[2] for g in added_gets], [g[1] for g in added_gets], [], [])
    if err:
        fail('Ninja_WaveExport: custom HLSL pins: %s' % err)
    for get_param, _kind, pin in added_gets:
        err = DEV.link_pins(map_get, get_param, node, pin)
        if err:
            fail('Ninja_WaveExport: link %s -> %s: %s' % (get_param, pin, err))
    hlsl = read_hlsl(spec['hlsl'])
    err = DEV.set_custom_hlsl(node, hlsl)
    if err:
        fail('Ninja_WaveExport: HLSL: %s' % err)
    log('windsea: module %s: inputs added %s, %d HLSL lines' % (export_path, [g[0] for g in added_gets], hlsl.count('\n')))

    # Compile
    result = DEV.compile_niagara_system(system, True, True)
    log('COMPILE\n' + result)
    new_problems = sorted(compile_problems(result) - problems_before)
    if new_problems or 'compiling=0' not in result.splitlines()[0]:
        fail('the core does not compile cleanly (nothing saved): %s' % new_problems[:20])
    hlsl_after = DEV.get_niagara_gpu_hlsl(system, EMITTER)
    added = sorted(cbuffer_members(hlsl_after) - members_before)
    removed = sorted(members_before - cbuffer_members(hlsl_after))
    log('GPU constants added:', added)
    log('GPU constants removed:', removed)
    stray = [m for m in added if 'Wave' not in m and not m.startswith('PREV_')]
    if stray or removed:
        fail('unexpected GPU constants (a misspelled parameter reads its default): added %s removed %s' % (stray, removed))
    if 'WaveWindSea' not in hlsl_after:
        fail('the GPU HLSL has no WaveWindSea: the Export stage does not read the table')

    describe_core(system)

    # Save
    if not EAL.save_loaded_asset(module, False):
        fail('save ' + export_path)
    if not EAL.save_loaded_asset(system, False):
        fail('save ' + CORE)
    after_hashes = protected_hashes()
    changed = [k for k in before if before[k] != after_hashes.get(k)]
    if changed:
        fail('protected vendor files changed: %s' % changed)
    log('protected files unchanged:', len(before))
    digest = file_sha(package_file(CORE))
    log('saved', CORE, 'sha256', digest)
    log('saved', export_path, 'sha256', file_sha(package_file(export_path)))
    log('saved core sha8', digest[:8], 'tag', 'core-' + digest[:8], '(add it to VERSIONS.md and KNOWN_CORES)')


# ---------------------------------------------------------------------------------------------------------------------------------
# Material function (V8)
# ---------------------------------------------------------------------------------------------------------------------------------
def expression(fn, name):
    for e in MEL.get_material_function_expressions(fn):
        if e.get_name() == name:
            return e
    return None


def input_of(fn, e, pin):
    names = [str(x) for x in MEL.get_material_expression_input_names(e)]
    ins = MEL.get_inputs_for_material_function_expression(fn, e)
    return ins[names.index(pin)] if pin in names and names.index(pin) < len(ins) else None


def connect(a, out_name, b, in_name):
    if not MEL.connect_material_expressions(a, out_name, b, in_name):
        fail('connect %s.%s -> %s.%s' % (a.get_name(), out_name, b.get_name(), in_name))


def parameter_names(fn, class_name):
    """The parameter names of the function's expressions of one class (MaterialExpressionScalarParameter, ...TextureObjectParameter, ...)."""
    names = set()
    for e in MEL.get_material_function_expressions(fn):
        if e.get_class().get_name() == class_name:
            names.add(str(e.get_editor_property('parameter_name')))
    return names


def describe_core(system):
    """Writes USER_PARAM_DESCRIPTIONS onto the core's user parameters it has; returns how many."""
    stored = DEV.describe_user_variables(system)
    present = {line.split('\t')[0] for line in stored.splitlines() if line}
    count = 0
    for name, text in USER_PARAM_DESCRIPTIONS.items():
        if 'User.' + name not in present:
            continue
        err = DEV.set_user_variable_description(system, name, text)
        if err:
            fail('description of User.%s: %s' % (name, err))
        count += 1
    log('core: described', count, 'user parameters')
    categorize_core(system, present)
    return count


def categorize_core(system, present):
    """Puts the core's custom user parameters into USER_PARAM_CATEGORIES under USER_PARAM_SECTION (its user parameter hierarchy).
    Fails when one the toolchain adds is in no category, so a new parameter cannot land in FluidNinja's ungrouped list again."""
    listed = {name for _, _, names in USER_PARAM_CATEGORIES for name in names}
    ours = {name for name, _, _ in USER_PARAMS} | {'TrackVelocityRelativeToWater'}
    if ours - listed:
        fail('user parameters in no USER_PARAM_CATEGORIES category: %s' % sorted(ours - listed))
    moved = 0
    for category, tooltip, names in USER_PARAM_CATEGORIES:
        names = [n for n in names if 'User.' + n in present]
        if not names:
            continue
        err = DEV.categorize_user_parameters(system, USER_PARAM_SECTION, USER_PARAM_SECTION_TOOLTIP, category, tooltip, names)
        if err:
            fail('category %s: %s' % (category, err))
        moved += len(names)
    log('core: categorized', moved, 'user parameters under', USER_PARAM_SECTION)
    for line in DEV.describe_user_parameter_hierarchy(system).splitlines():
        log('core: hierarchy', line)
    return moved


def describe_surface(fn):
    """Writes SURFACE_PARAM_DESCRIPTIONS onto the SURFACE function's parameter expressions it has; returns how many."""
    count = 0
    for e in MEL.get_material_function_expressions(fn):
        try:
            name = str(e.get_editor_property('parameter_name'))
        except Exception:
            continue
        # Only what differs, so a function already described and grouped is not re-saved (a re-save changes its hash).
        if name in SURFACE_PARAM_DESCRIPTIONS and str(e.get_editor_property('desc')) != SURFACE_PARAM_DESCRIPTIONS[name]:
            e.set_editor_property('desc', SURFACE_PARAM_DESCRIPTIONS[name])
            count += 1
        if name in SURFACE_PARAM_GROUPS and str(e.get_editor_property('group')) != SURFACE_PARAM_GROUPS[name]:
            e.set_editor_property('group', SURFACE_PARAM_GROUPS[name])
            count += 1
    log('surface: described and grouped,', count, 'changes')
    return count


def describe():
    """The describe step: the descriptions onto an already patched core and SURFACE function, nothing else."""
    system = unreal.load_asset(CORE)
    fn = unreal.load_asset(SURFACE)
    if not system or not fn:
        fail('no %s or %s' % (CORE, SURFACE))
    describe_core(system)
    if not EAL.save_loaded_asset(system, False):
        fail('save ' + CORE)
    if describe_surface(fn):
        MEL.update_material_function(fn, None)
        if not EAL.save_loaded_asset(fn, False):
            fail('save ' + SURFACE)
    core_digest = file_sha(package_file(CORE))
    surface_digest = file_sha(package_file(SURFACE))
    log('saved', CORE, 'sha256', core_digest, '| core sha8', core_digest[:8], '(add it to VERSIONS.md and KNOWN_CORES)')
    log('saved', SURFACE, 'sha256', surface_digest, '| surface sha8', surface_digest[:8], '(add it to KNOWN_SURFACE)')


def surface_has_waves(fn):
    """Whether the SURFACE function carries the waves: its WaveHeightScale / WaveNormalScale scalar parameters."""
    names = parameter_names(fn, 'MaterialExpressionScalarParameter')
    return 'WaveHeightScale' in names and 'WaveNormalScale' in names


def surface_has_foam(fn):
    """Whether the SURFACE function carries the foam and the choppiness (interface version 4): WaveFoamStrength / WaveChoppiness and the
    WaveFoamBuffer texture parameter."""
    scalars = parameter_names(fn, 'MaterialExpressionScalarParameter')
    textures = parameter_names(fn, 'MaterialExpressionTextureObjectParameter')
    return 'WaveFoamStrength' in scalars and 'WaveChoppiness' in scalars and 'WaveFoamBuffer' in textures


def surface_has_v6(fn):
    """Whether the SURFACE function carries interface version 6's aged foam: its WaveFoamAgedColor vector parameter."""
    return 'WaveFoamAgedColor' in parameter_names(fn, 'MaterialExpressionVectorParameter')


def surface_has_foam_opacity(fn):
    """Whether the SURFACE function lets the foam into the opacity (Single Layer Water and translucent materials): its WaveFoamOpacity."""
    return 'WaveFoamOpacity' in parameter_names(fn, 'MaterialExpressionScalarParameter')


def surface_has_foam_lace(fn):
    """Whether the SURFACE function breaks the foam into lace: its WaveFoamLace."""
    return 'WaveFoamLace' in parameter_names(fn, 'MaterialExpressionScalarParameter')


def surface_has_foam_relief(fn):
    """Whether the SURFACE function lifts and roughens the whitewater: its WaveFoamHeightCm."""
    return 'WaveFoamHeightCm' in parameter_names(fn, 'MaterialExpressionScalarParameter')


def parameter_expression(fn, class_name, name):
    """The function's expressions of one class (MaterialExpressionStaticSwitchParameter, ...) with this parameter name."""
    return [e for e in MEL.get_material_function_expressions(fn)
            if e.get_class().get_name() == class_name and str(e.get_editor_property('parameter_name')) == name]


def feeder(fn, e, pin):
    """(expression, output name) connected into e's input pin, so the connection can be moved onto a new node with connect(); (None, '')
    when the pin is unconnected."""
    source = input_of(fn, e, pin)
    if source is None:
        return None, ''
    out_name = MEL.get_input_node_output_name_for_material_expression(e, source)
    if isinstance(out_name, tuple):   # (ok, name) on an engine that returns the bool too
        out_name = out_name[1] if out_name[0] else None
    if out_name is None:
        fail('%s.%s: the output of %s feeding it could not be named' % (e.get_name(), pin, source.get_name()))
    return source, str(out_name)


def scalar_parameter(fn, name):
    for e in MEL.get_material_function_expressions(fn):
        if e.get_class().get_name() == 'MaterialExpressionScalarParameter' and str(e.get_editor_property('parameter_name')) == name:
            return e
    return None


def add_surface_waves(fn, normal_fn):
    """The waves (every interface version): WPO += (0, 0, B x WaveHeightScale), the normal += the slope of B. Refuses a function that has them."""
    if surface_has_waves(fn):
        fail('the material function already has the waves (WaveHeightScale / WaveNormalScale)')
    # The vendor nodes this edit hangs on, checked by their connections, not only their names.
    mma = expression(fn, 'MaterialExpressionMakeMaterialAttributes_0')
    wpo = expression(fn, 'MaterialExpressionAdd_53')
    sample = expression(fn, 'MaterialExpressionTextureSample_10')
    pd = expression(fn, 'MaterialExpressionTextureObjectParameter_3')
    normal_sum = expression(fn, 'MaterialExpressionAdd_27')
    normal_next = expression(fn, 'MaterialExpressionReroute_107')
    if not all([mma, wpo, sample, pd, normal_sum, normal_next]):
        fail('vendor nodes not found (MMA_0, Add_53, TextureSample_10, TextureObjectParameter_3, Add_27, Reroute_107)')
    if input_of(fn, mma, 'WorldPositionOffset') != wpo:
        fail('Add_53 does not feed MakeMaterialAttributes.WorldPositionOffset')
    if input_of(fn, sample, 'Tex') != pd or str(pd.get_editor_property('parameter_name')) != 'PressureDivergenceBuffer':
        fail('TextureSample_10 does not sample PressureDivergenceBuffer')
    if input_of(fn, normal_next, '') != normal_sum and MEL.get_inputs_for_material_function_expression(fn, normal_next)[0] != normal_sum:
        fail('Add_27 does not feed Reroute_107')
    sim_uv = input_of(fn, sample, 'UVs')
    if sim_uv is None:
        fail('TextureSample_10 has no UVs input')
    x0 = wpo.get_editor_property('material_expression_editor_x')
    y0 = wpo.get_editor_property('material_expression_editor_y')

    def new(cls, dx, dy):
        e = MEL.create_material_expression_in_function(fn, cls, x0 + dx, y0 + dy)
        if not e:
            fail('create ' + cls.__name__)
        return e

    # Scalars
    height_scale = new(unreal.MaterialExpressionScalarParameter, 200, 400)
    height_scale.set_editor_property('parameter_name', 'WaveHeightScale')
    height_scale.set_editor_property('default_value', 1.0)
    height_scale.set_editor_property('group', 'Waves')
    height_scale.set_editor_property('desc', 'FluidNinja Waves: scale on the wave height in RT_PressureDivergence.B (WPO and normal). 0 hides the waves.')
    normal_scale = new(unreal.MaterialExpressionScalarParameter, 200, 520)
    normal_scale.set_editor_property('parameter_name', 'WaveNormalScale')
    normal_scale.set_editor_property('default_value', 0.1)
    normal_scale.set_editor_property('group', 'Waves')
    normal_scale.set_editor_property('desc', 'FluidNinja Waves: normal strength of the wave slope (about 1 / (0.005 x sim size in cm) is physical).')

    # WPO: Add_53 + (0, 0, B * WaveHeightScale) -> WorldPositionOffset (Displacement stays on Add_53)
    height = new(unreal.MaterialExpressionMultiply, 400, 400)
    connect(sample, 'B', height, 'A')
    connect(height_scale, '', height, 'B')
    zero2 = new(unreal.MaterialExpressionConstant2Vector, 400, 300)
    zero2.set_editor_property('r', 0.0)
    zero2.set_editor_property('g', 0.0)
    offset = new(unreal.MaterialExpressionAppendVector, 600, 350)
    connect(zero2, '', offset, 'A')
    connect(height, '', offset, 'B')
    wpo_sum = new(unreal.MaterialExpressionAdd, 800, 200)
    wpo_sum.set_editor_property('desc', 'WPO + FluidNinja Waves')
    connect(wpo, '', wpo_sum, 'A')
    connect(offset, '', wpo_sum, 'B')
    connect(wpo_sum, '', mma, 'WorldPositionOffset')

    # Normal: Add_27 + (slope of B x WaveHeightScale, 0) -> Reroute_107 (the FlowMapNormals chain)
    nx0 = normal_sum.get_editor_property('material_expression_editor_x')
    ny0 = normal_sum.get_editor_property('material_expression_editor_y')

    def new_n(cls, dx, dy):
        e = MEL.create_material_expression_in_function(fn, cls, nx0 + dx, ny0 + dy)
        if not e:
            fail('create ' + cls.__name__)
        return e

    call = new_n(unreal.MaterialExpressionMaterialFunctionCall, -200, 300)
    call.set_editor_property('material_function', normal_fn)
    selector = new_n(unreal.MaterialExpressionConstant4Vector, -500, 380)
    selector.set_editor_property('constant', unreal.LinearColor(0.0, 0.0, 1.0, 0.0))
    no_sine = new_n(unreal.MaterialExpressionStaticBool, -500, 460)
    no_sine.set_editor_property('value', False)
    normal_scale_n = normal_scale
    connect(pd, '', call, 'Height Map')
    connect(sim_uv, '', call, 'Coordinates')
    connect(selector, '', call, 'Height Map Channel Selector')
    connect(no_sine, '', call, 'ApplySineWavesToGradient')
    connect(normal_scale_n, '', call, 'Normal Map Intensity')
    slope = new_n(unreal.MaterialExpressionComponentMask, 100, 300)
    slope.set_editor_property('r', True)
    slope.set_editor_property('g', True)
    slope.set_editor_property('b', False)
    slope.set_editor_property('a', False)
    connect(call, 'Result', slope, '')
    slope_scaled = new_n(unreal.MaterialExpressionMultiply, 250, 300)
    connect(slope, '', slope_scaled, 'A')
    connect(height_scale, '', slope_scaled, 'B')
    zero1 = new_n(unreal.MaterialExpressionConstant, 250, 400)
    zero1.set_editor_property('r', 0.0)
    slope3 = new_n(unreal.MaterialExpressionAppendVector, 400, 300)
    connect(slope_scaled, '', slope3, 'A')
    connect(zero1, '', slope3, 'B')
    normal_wave = new_n(unreal.MaterialExpressionAdd, 550, 150)
    normal_wave.set_editor_property('desc', 'Normal + FluidNinja Waves')
    connect(normal_sum, '', normal_wave, 'A')
    connect(slope3, '', normal_wave, 'B')
    connect(normal_wave, '', normal_next, '')
    if MEL.get_inputs_for_material_function_expression(fn, normal_next)[0] != normal_wave or input_of(fn, mma, 'WorldPositionOffset') != wpo_sum:
        fail('the new nodes are not wired in')
    log('surface: the waves added (WaveHeightScale, WaveNormalScale)')


def add_surface_foam(fn):
    """Interface version 4: the foam channel and the choppiness, on a function that carries the waves (just added, or an earlier build's).
    Everything hangs on MakeMaterialAttributes_0's own inputs (BaseColor, Roughness, WorldPositionOffset: each new node takes over the
    connection that fed the input and feeds the input itself) and on TextureSample_10 / TextureObjectParameter_3 (the pressure buffer and
    the simulation UV), so it is the same edit whatever fed those inputs before. Refuses a function that has the foam."""
    if surface_has_foam(fn):
        fail('the material function already has the foam (WaveFoamBuffer / WaveFoamStrength / WaveChoppiness)')
    mma = expression(fn, 'MaterialExpressionMakeMaterialAttributes_0')
    sample = expression(fn, 'MaterialExpressionTextureSample_10')
    pd = expression(fn, 'MaterialExpressionTextureObjectParameter_3')
    if not all([mma, sample, pd]):
        fail('vendor nodes not found (MMA_0, TextureSample_10, TextureObjectParameter_3)')
    if input_of(fn, sample, 'Tex') != pd or str(pd.get_editor_property('parameter_name')) != 'PressureDivergenceBuffer':
        fail('TextureSample_10 does not sample PressureDivergenceBuffer')
    sim_uv, sim_uv_out = feeder(fn, sample, 'UVs')
    if sim_uv is None:
        fail('TextureSample_10 has no UVs input')
    height_scale = scalar_parameter(fn, 'WaveHeightScale')
    if height_scale is None:
        fail('no WaveHeightScale parameter: the waves come first (the material step adds them)')
    base_feed, base_out = feeder(fn, mma, 'BaseColor')
    rough_feed, rough_out = feeder(fn, mma, 'Roughness')
    wpo_feed, wpo_out = feeder(fn, mma, 'WorldPositionOffset')
    if base_feed is None or rough_feed is None or wpo_feed is None:
        fail('MakeMaterialAttributes_0 has an unconnected BaseColor, Roughness or WorldPositionOffset (base %s, roughness %s, WPO %s)'
             % (base_feed, rough_feed, wpo_feed))
    # The foam buffer's default: the black 1 x 1 target the core step makes, so a surface nothing binds (the foam off, an older core)
    # reads F = 0; the sampler types are the vendor's own for its float pressure buffer (an R16F foam target is read the same way).
    unbound = unreal.load_asset(AUX_UNBOUND) if EAL.does_asset_exist(AUX_UNBOUND) else None
    if not isinstance(unbound, unreal.TextureRenderTarget2D):
        fail('no %s: the foam buffer needs it as its black default (the core step makes it; run core first)' % AUX_UNBOUND)
    x0 = mma.get_editor_property('material_expression_editor_x')
    y0 = mma.get_editor_property('material_expression_editor_y')

    def new(cls, dx, dy, desc=''):
        e = MEL.create_material_expression_in_function(fn, cls, x0 + dx, y0 + dy)
        if not e:
            fail('create ' + cls.__name__)
        if desc:
            e.set_editor_property('desc', desc)
        return e

    def scalar(name, default, dx, dy, desc):
        e = new(unreal.MaterialExpressionScalarParameter, dx, dy)
        e.set_editor_property('parameter_name', name)
        e.set_editor_property('default_value', default)
        e.set_editor_property('group', 'Waves')
        e.set_editor_property('desc', desc)
        return e

    def constant(value, dx, dy):
        e = new(unreal.MaterialExpressionConstant, dx, dy)
        e.set_editor_property('r', value)
        return e

    def binary(cls, a, a_out, b, b_out, dx, dy, desc=''):
        e = new(cls, dx, dy, desc)
        connect(a, a_out, e, 'A')
        connect(b, b_out, e, 'B')
        return e

    def tap(uv, uv_out, dx, dy):
        e = new(unreal.MaterialExpressionTextureSample, dx, dy)
        e.set_editor_property('sampler_type', sample.get_editor_property('sampler_type'))
        connect(pd, '', e, 'Tex')
        connect(uv, uv_out, e, 'UVs')
        return e

    # ---- FoamMask = saturate(WaveFoamBuffer.r x WaveFoamStrength), behind the static switch WaveFoam (on; off compiles the sample away)
    foam_tex = new(unreal.MaterialExpressionTextureObjectParameter, -1500, 700)
    foam_tex.set_editor_property('parameter_name', 'WaveFoamBuffer')
    foam_tex.set_editor_property('group', 'Waves')
    foam_tex.set_editor_property('texture', unbound)
    # Linear Color, not the pressure parameter's sampler: the foam target (and its default, RT_WaveAux_Unbound) is a float render target,
    # which a Color sampler refuses ("Sampler type is Color, should be Linear Color").
    foam_tex.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    foam_tex.set_editor_property('desc', 'FluidNinja Waves: the simulation\'s foam target (R = foam 0..1 per wave cell, sampled at the simulation UV like PressureDivergenceBuffer). FluidNinja Buoyancy binds it on the surfaces\' dynamic materials; unbound it is black.')
    foam_sample = new(unreal.MaterialExpressionTextureSample, -1300, 700)
    foam_sample.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    connect(foam_tex, '', foam_sample, 'Tex')
    connect(sim_uv, sim_uv_out, foam_sample, 'UVs')
    foam_strength = scalar('WaveFoamStrength', 1.0, -1300, 820, 'FluidNinja Waves: scale on the foam before it is clamped to 1. 0 hides the foam.')
    foam_mul = binary(unreal.MaterialExpressionMultiply, foam_sample, 'R', foam_strength, '', -1100, 700)
    foam_sat = new(unreal.MaterialExpressionSaturate, -950, 700)
    connect(foam_mul, '', foam_sat, '')
    foam_off = constant(0.0, -950, 800)
    foam_switch = new(unreal.MaterialExpressionStaticSwitchParameter, -800, 700, 'FoamMask (FluidNinja Waves)')
    foam_switch.set_editor_property('parameter_name', 'WaveFoam')
    foam_switch.set_editor_property('default_value', True)
    foam_switch.set_editor_property('group', 'Waves')
    foam_switch.set_editor_property('desc', 'FluidNinja Waves: the foam (whitecaps, wake trails, impact rings) drawn into the base colour and the roughness. Off: no foam sample at all.')
    connect(foam_sat, '', foam_switch, 'True')
    connect(foam_off, '', foam_switch, 'False')

    # ---- the colour: BaseColor = lerp(BaseColor, WaveFoamColor, FoamMask); the roughness: saturate(Roughness + FoamMask)
    foam_color = new(unreal.MaterialExpressionVectorParameter, -800, 850)
    foam_color.set_editor_property('parameter_name', 'WaveFoamColor')
    foam_color.set_editor_property('default_value', unreal.LinearColor(1.0, 1.0, 1.0, 1.0))
    foam_color.set_editor_property('group', 'Waves')
    foam_color.set_editor_property('desc', 'FluidNinja Waves: the colour the water goes toward where there is foam.')
    # RGB of the float4 parameter: the base colour chain is float3, and lerp(float3, float4) does not compile.
    foam_rgb = new(unreal.MaterialExpressionComponentMask, -650, 850)
    foam_rgb.set_editor_property('r', True)
    foam_rgb.set_editor_property('g', True)
    foam_rgb.set_editor_property('b', True)
    foam_rgb.set_editor_property('a', False)
    connect(foam_color, '', foam_rgb, '')
    base_lerp = new(unreal.MaterialExpressionLinearInterpolate, -500, 650, 'BaseColor + FluidNinja Waves foam')
    connect(base_feed, base_out, base_lerp, 'A')
    connect(foam_rgb, '', base_lerp, 'B')
    connect(foam_switch, '', base_lerp, 'Alpha')
    connect(base_lerp, '', mma, 'BaseColor')
    rough_add = binary(unreal.MaterialExpressionAdd, rough_feed, rough_out, foam_switch, '', -500, 800, 'Roughness + FluidNinja Waves foam')
    rough_sat = new(unreal.MaterialExpressionSaturate, -350, 800)
    connect(rough_add, '', rough_sat, '')
    connect(rough_sat, '', mma, 'Roughness')

    # ---- the FoamMask output, for a project's own surface material (the vendor's take the attributes)
    foam_out = new(unreal.MaterialExpressionFunctionOutput, -500, 950)
    foam_out.set_editor_property('output_name', 'FoamMask')
    foam_out.set_editor_property('sort_priority', 1)
    foam_out.set_editor_property('description', 'FluidNinja Waves: the foam at this point, 0..1 (saturate(WaveFoamBuffer x WaveFoamStrength); 0 with WaveFoam off).')
    connect(foam_switch, '', foam_out, '')

    # ---- choppiness: WPO.xy += WaveChoppiness x 0.5 x (B two texels ahead - B two texels behind) x WaveHeightScale, per sim axis, tangent
    # space to world: the Tessendorf horizontal displacement from the low-passed slope, uphill toward the crests. Behind a static switch
    # (off: no fetches in the vertex shader) and a scalar (0).
    chop = scalar('WaveChoppiness', 0.0, -1500, 1100, 'FluidNinja Waves: how far the surface is pulled sideways toward the crests, in units of half the wave height difference two texels apart (1: the Tessendorf choppiness on the waves a few texels long). Needs WaveChoppy on. 0: none.')
    texel = new(unreal.MaterialExpressionTextureProperty, -1500, 1200)
    texel.set_editor_property('property', unreal.MaterialExposedTextureProperty.TMTM_TEXEL_SIZE)
    connect(pd, '', texel, '')
    two = constant(2.0, -1500, 1300)
    offset = binary(unreal.MaterialExpressionMultiply, texel, '', two, '', -1350, 1200)
    axis_x = new(unreal.MaterialExpressionConstant2Vector, -1350, 1300)
    axis_x.set_editor_property('r', 1.0)
    axis_x.set_editor_property('g', 0.0)
    axis_y = new(unreal.MaterialExpressionConstant2Vector, -1350, 1400)
    axis_y.set_editor_property('r', 0.0)
    axis_y.set_editor_property('g', 1.0)
    offset_x = binary(unreal.MaterialExpressionMultiply, offset, '', axis_x, '', -1200, 1200)
    offset_y = binary(unreal.MaterialExpressionMultiply, offset, '', axis_y, '', -1200, 1350)
    uv_xp = binary(unreal.MaterialExpressionAdd, sim_uv, sim_uv_out, offset_x, '', -1050, 1100)
    uv_xm = binary(unreal.MaterialExpressionSubtract, sim_uv, sim_uv_out, offset_x, '', -1050, 1200)
    uv_yp = binary(unreal.MaterialExpressionAdd, sim_uv, sim_uv_out, offset_y, '', -1050, 1300)
    uv_ym = binary(unreal.MaterialExpressionSubtract, sim_uv, sim_uv_out, offset_y, '', -1050, 1400)
    b_xp = tap(uv_xp, '', -900, 1100)
    b_xm = tap(uv_xm, '', -900, 1200)
    b_yp = tap(uv_yp, '', -900, 1300)
    b_ym = tap(uv_ym, '', -900, 1400)
    slope_x = binary(unreal.MaterialExpressionSubtract, b_xp, 'B', b_xm, 'B', -750, 1150)
    slope_y = binary(unreal.MaterialExpressionSubtract, b_yp, 'B', b_ym, 'B', -750, 1350)
    slope = binary(unreal.MaterialExpressionAppendVector, slope_x, '', slope_y, '', -600, 1250)
    chop_scaled = binary(unreal.MaterialExpressionMultiply, slope, '', chop, '', -450, 1250)
    chop_height = binary(unreal.MaterialExpressionMultiply, chop_scaled, '', height_scale, '', -300, 1250)
    half = constant(0.5, -300, 1350)
    chop_half = binary(unreal.MaterialExpressionMultiply, chop_height, '', half, '', -150, 1250)
    zero = constant(0.0, -150, 1350)
    chop_tangent = binary(unreal.MaterialExpressionAppendVector, chop_half, '', zero, '', 0, 1250)
    chop_world = new(unreal.MaterialExpressionTransform, 150, 1250, 'tangent -> world')   # the defaults: tangent space to world space
    connect(chop_tangent, '', chop_world, '')
    wpo_chop = binary(unreal.MaterialExpressionAdd, wpo_feed, wpo_out, chop_world, '', 300, 1150, 'WPO + FluidNinja Waves choppiness')
    chop_switch = new(unreal.MaterialExpressionStaticSwitchParameter, 450, 1100)
    chop_switch.set_editor_property('parameter_name', 'WaveChoppy')
    chop_switch.set_editor_property('default_value', False)
    chop_switch.set_editor_property('group', 'Waves')
    chop_switch.set_editor_property('desc', 'FluidNinja Waves: the choppiness (WaveChoppiness) on: the vertex shader reads the wave height four more times. Off: nothing of it is compiled.')
    connect(wpo_chop, '', chop_switch, 'True')
    connect(wpo_feed, wpo_out, chop_switch, 'False')
    connect(chop_switch, '', mma, 'WorldPositionOffset')

    if input_of(fn, mma, 'BaseColor') != base_lerp or input_of(fn, mma, 'Roughness') != rough_sat or input_of(fn, mma, 'WorldPositionOffset') != chop_switch:
        fail('the foam nodes are not wired in')
    log('surface: the foam added (WaveFoamBuffer, WaveFoamStrength, WaveFoamColor, WaveFoam, the FoamMask output) and the choppiness (WaveChoppiness, WaveChoppy)')


def add_surface_aged(fn):
    """Interface version 6: the aged foam, on a function that carries the waves and the foam (just added, or an interface 4 / 5 build's).
    Every anchor is found in the graph by what it is and what it is wired to, never by a handle from add_surface_foam, so a function the
    interface 4 / 5 material step saved gets exactly the edit a vendor one does:
      - the TextureObjectParameter WaveFoamBuffer and the one TextureSample it feeds (the foam sample; Linear Color, so G is the aged foam);
      - MakeMaterialAttributes_0.BaseColor's feeder, the fresh foam's LinearInterpolate: its B comes from WaveFoamColor (through the RGB
        mask), its Alpha from the static switch WaveFoam.
    AgedMask = saturate(foam.G x WaveFoamAgedStrength) behind a second WaveFoam static switch (the same parameter, the same default True:
    off gives 0, and with both off the foam sample compiles away); the base colour that fed the fresh lerp's A goes through
    lerp(base, WaveFoamAgedColor.rgb, AgedMask) first, so the fresh foam draws on top. Roughness and normals unchanged. Refuses a function
    that has the aged foam."""
    if surface_has_v6(fn):
        fail('the material function already has the aged foam (WaveFoamAgedColor)')
    mma = expression(fn, 'MaterialExpressionMakeMaterialAttributes_0')
    if mma is None:
        fail('vendor node not found (MakeMaterialAttributes_0)')
    foam_tex = parameter_expression(fn, 'MaterialExpressionTextureObjectParameter', 'WaveFoamBuffer')
    if len(foam_tex) != 1:
        fail('the foam comes first: %d WaveFoamBuffer texture parameters (the material step adds one)' % len(foam_tex))
    foam_tex = foam_tex[0]
    samples = [e for e in MEL.get_material_function_expressions(fn)
               if e.get_class().get_name() == 'MaterialExpressionTextureSample' and input_of(fn, e, 'Tex') == foam_tex]
    if len(samples) != 1:
        fail('%d texture samples read WaveFoamBuffer, expected the foam sample alone' % len(samples))
    foam_sample = samples[0]
    fresh_lerp = input_of(fn, mma, 'BaseColor')
    if fresh_lerp is None or fresh_lerp.get_class().get_name() != 'MaterialExpressionLinearInterpolate':
        fail('MakeMaterialAttributes_0.BaseColor is not fed by the fresh foam\'s LinearInterpolate (%s)' % (fresh_lerp.get_name() if fresh_lerp else None))
    fresh_alpha = input_of(fn, fresh_lerp, 'Alpha')
    if (fresh_alpha is None or fresh_alpha.get_class().get_name() != 'MaterialExpressionStaticSwitchParameter'
            or str(fresh_alpha.get_editor_property('parameter_name')) != 'WaveFoam'):
        fail('the base colour lerp\'s Alpha is not the WaveFoam static switch')
    fresh_b = input_of(fn, fresh_lerp, 'B')
    fresh_color = MEL.get_inputs_for_material_function_expression(fn, fresh_b)[0] if fresh_b is not None else None
    if (fresh_color is None or fresh_color.get_class().get_name() != 'MaterialExpressionVectorParameter'
            or str(fresh_color.get_editor_property('parameter_name')) != 'WaveFoamColor'):
        fail('the base colour lerp\'s B is not WaveFoamColor (through its RGB mask)')
    base_feed, base_out = feeder(fn, fresh_lerp, 'A')
    if base_feed is None:
        fail('the fresh foam\'s lerp has no base colour on A')
    x0 = fresh_lerp.get_editor_property('material_expression_editor_x')
    y0 = fresh_lerp.get_editor_property('material_expression_editor_y')

    def new(cls, dx, dy, desc=''):
        e = MEL.create_material_expression_in_function(fn, cls, x0 + dx, y0 + dy)
        if not e:
            fail('create ' + cls.__name__)
        if desc:
            e.set_editor_property('desc', desc)
        return e

    def parameter(e, name):
        e.set_editor_property('parameter_name', name)
        e.set_editor_property('group', SURFACE_PARAM_GROUPS[name])
        e.set_editor_property('desc', SURFACE_PARAM_DESCRIPTIONS[name])
        return e

    # ---- AgedMask = saturate(WaveFoamBuffer.g x WaveFoamAgedStrength), behind WaveFoam (off: 0)
    aged_strength = parameter(new(unreal.MaterialExpressionScalarParameter, -650, -450), 'WaveFoamAgedStrength')
    aged_strength.set_editor_property('default_value', 0.6)
    aged_mul = new(unreal.MaterialExpressionMultiply, -500, -500)
    connect(foam_sample, 'G', aged_mul, 'A')
    connect(aged_strength, '', aged_mul, 'B')
    aged_sat = new(unreal.MaterialExpressionSaturate, -380, -500)
    connect(aged_mul, '', aged_sat, '')
    aged_off = new(unreal.MaterialExpressionConstant, -380, -420)
    aged_off.set_editor_property('r', 0.0)
    aged_switch = parameter(new(unreal.MaterialExpressionStaticSwitchParameter, -260, -500), 'WaveFoam')
    aged_switch.set_editor_property('default_value', fresh_alpha.get_editor_property('default_value'))
    connect(aged_sat, '', aged_switch, 'True')
    connect(aged_off, '', aged_switch, 'False')

    # ---- BaseColor: base -> lerp(base, WaveFoamAgedColor.rgb, AgedMask) -> the fresh foam's lerp (unchanged) -> MakeMaterialAttributes
    aged_color = parameter(new(unreal.MaterialExpressionVectorParameter, -260, -360), 'WaveFoamAgedColor')
    aged_color.set_editor_property('default_value', unreal.LinearColor(0.40, 0.72, 0.66, 1.0))
    aged_rgb = new(unreal.MaterialExpressionComponentMask, -120, -360)
    aged_rgb.set_editor_property('r', True)
    aged_rgb.set_editor_property('g', True)
    aged_rgb.set_editor_property('b', True)
    aged_rgb.set_editor_property('a', False)
    connect(aged_color, '', aged_rgb, '')
    aged_lerp = new(unreal.MaterialExpressionLinearInterpolate, -120, -200, 'BaseColor + FluidNinja Waves aged foam')
    connect(base_feed, base_out, aged_lerp, 'A')
    connect(aged_rgb, '', aged_lerp, 'B')
    connect(aged_switch, '', aged_lerp, 'Alpha')
    connect(aged_lerp, '', fresh_lerp, 'A')

    if input_of(fn, fresh_lerp, 'A') != aged_lerp or input_of(fn, mma, 'BaseColor') != fresh_lerp or input_of(fn, aged_lerp, 'A') != base_feed:
        fail('the aged foam nodes are not wired in')
    log('surface: the aged foam added (WaveFoamAgedColor, WaveFoamAgedStrength, behind WaveFoam) under the fresh foam\'s lerp')


def add_surface_foam_opacity(fn):
    """The foam into the opacity, on a function that carries the fresh and the aged foam. A Single Layer Water material (FluidNinja's lake,
    quiet and murky waters) or a translucent one shows its base colour only as far as its Opacity says: the foam, lerped into the base
    colour, stayed nearly invisible on them, while FluidNinja's own density, which feeds the opacity, showed. Anchors, found in the graph:
      - MakeMaterialAttributes_0.BaseColor's feeder, the fresh foam's LinearInterpolate: its Alpha is FoamMask (the WaveFoam switch);
      - that lerp's A, the aged foam's LinearInterpolate: its Alpha is AgedMask (the second WaveFoam switch);
      - MakeMaterialAttributes_0.Opacity's feeder, the material's own opacity.
    Opacity = max(opacity, saturate((FoamMask + 0.5 x AgedMask) x WaveFoamOpacity)); opaque materials ignore it. A function whose Opacity
    pin is unconnected is left as it is (logged). Refuses a function that has it."""
    if surface_has_foam_opacity(fn):
        fail('the material function already lets the foam into the opacity (WaveFoamOpacity)')
    mma = expression(fn, 'MaterialExpressionMakeMaterialAttributes_0')
    if mma is None:
        fail('vendor node not found (MakeMaterialAttributes_0)')
    fresh_lerp = input_of(fn, mma, 'BaseColor')
    if fresh_lerp is None or fresh_lerp.get_class().get_name() != 'MaterialExpressionLinearInterpolate':
        fail('MakeMaterialAttributes_0.BaseColor is not fed by the fresh foam\'s LinearInterpolate')
    fresh_mask = input_of(fn, fresh_lerp, 'Alpha')
    aged_lerp = input_of(fn, fresh_lerp, 'A')
    aged_mask = input_of(fn, aged_lerp, 'Alpha') if aged_lerp is not None and aged_lerp.get_class().get_name() == 'MaterialExpressionLinearInterpolate' else None
    for mask, what in ((fresh_mask, 'fresh'), (aged_mask, 'aged')):
        if (mask is None or mask.get_class().get_name() != 'MaterialExpressionStaticSwitchParameter'
                or str(mask.get_editor_property('parameter_name')) != 'WaveFoam'):
            fail('the %s foam\'s mask is not a WaveFoam static switch (the foam and the aged foam come first)' % what)
    opacity_feed, opacity_out = feeder(fn, mma, 'Opacity')
    if opacity_feed is None:
        log('surface: MakeMaterialAttributes_0.Opacity is unconnected: the foam opacity is not added')
        return
    x0 = mma.get_editor_property('material_expression_editor_x')
    y0 = mma.get_editor_property('material_expression_editor_y')

    def new(cls, dx, dy, desc=''):
        e = MEL.create_material_expression_in_function(fn, cls, x0 + dx, y0 + dy)
        if not e:
            fail('create ' + cls.__name__)
        if desc:
            e.set_editor_property('desc', desc)
        return e

    half = new(unreal.MaterialExpressionConstant, -760, 520)
    half.set_editor_property('r', 0.5)
    aged_half = new(unreal.MaterialExpressionMultiply, -620, 480)
    connect(aged_mask, '', aged_half, 'A')
    connect(half, '', aged_half, 'B')
    total = new(unreal.MaterialExpressionAdd, -500, 440)
    connect(fresh_mask, '', total, 'A')
    connect(aged_half, '', total, 'B')
    strength = new(unreal.MaterialExpressionScalarParameter, -500, 560)
    strength.set_editor_property('parameter_name', 'WaveFoamOpacity')
    strength.set_editor_property('group', SURFACE_PARAM_GROUPS['WaveFoamOpacity'])
    strength.set_editor_property('desc', SURFACE_PARAM_DESCRIPTIONS['WaveFoamOpacity'])
    strength.set_editor_property('default_value', 1.0)
    scaled = new(unreal.MaterialExpressionMultiply, -380, 480)
    connect(total, '', scaled, 'A')
    connect(strength, '', scaled, 'B')
    cover = new(unreal.MaterialExpressionSaturate, -260, 480)
    connect(scaled, '', cover, '')
    opacity = new(unreal.MaterialExpressionMax, -140, 440, 'Opacity + FluidNinja Waves foam')
    connect(opacity_feed, opacity_out, opacity, 'A')
    connect(cover, '', opacity, 'B')
    connect(opacity, '', mma, 'Opacity')
    if input_of(fn, mma, 'Opacity') != opacity or input_of(fn, opacity, 'A') != opacity_feed:
        fail('the foam opacity nodes are not wired in')
    log('surface: the foam let into the opacity (WaveFoamOpacity): Opacity = max(opacity, saturate((FoamMask + 0.5 AgedMask) x WaveFoamOpacity))')


def add_surface_foam_lace(fn):
    """The foam broken into lace: each WaveFoam switch's True input (the fresh foam's saturate(F x WaveFoamStrength), the aged foam's
    saturate(F2 x WaveFoamAgedStrength)) becomes lerp(m, laced, WaveFoamLace): laced = saturate(m (0.55 + 0.45 g) smoothstep(0, 1,
    (m + 0.5 g - 0.35) x 2)), g = saturate(3 x the R of WaveFoamLaceTexture (FluidNinja's T_foam2) at the world XY over
    WaveFoamLaceTileCm). Thick foam stays white on the veins and about 60 % between them, thin foam keeps
    only the lace's veins. The colour lerps and the foam opacity take the switches' outputs, so they all get the lace. Anchors: the two
    WaveFoam static switches feeding the fresh and the aged foam's LinearInterpolates (MakeMaterialAttributes_0.BaseColor's feeder and
    its A). Refuses a function that has it."""
    if surface_has_foam_lace(fn):
        fail('the material function already breaks the foam into lace (WaveFoamLace)')
    mma = expression(fn, 'MaterialExpressionMakeMaterialAttributes_0')
    fresh_lerp = input_of(fn, mma, 'BaseColor') if mma is not None else None
    aged_lerp = input_of(fn, fresh_lerp, 'A') if fresh_lerp is not None else None
    switches = []
    for lerp, what in ((fresh_lerp, 'fresh'), (aged_lerp, 'aged')):
        switch = input_of(fn, lerp, 'Alpha') if lerp is not None and lerp.get_class().get_name() == 'MaterialExpressionLinearInterpolate' else None
        if (switch is None or switch.get_class().get_name() != 'MaterialExpressionStaticSwitchParameter'
                or str(switch.get_editor_property('parameter_name')) != 'WaveFoam'):
            fail('the %s foam\'s WaveFoam switch was not found (the foam and the aged foam come first)' % what)
        mask, mask_out = feeder(fn, switch, 'True')
        if mask is None:
            fail('the %s foam\'s WaveFoam switch has nothing on True' % what)
        switches.append((switch, mask, mask_out))
    x0 = mma.get_editor_property('material_expression_editor_x')
    y0 = mma.get_editor_property('material_expression_editor_y')

    def new(cls, dx, dy, desc=''):
        e = MEL.create_material_expression_in_function(fn, cls, x0 + dx, y0 + dy)
        if not e:
            fail('create ' + cls.__name__)
        if desc:
            e.set_editor_property('desc', desc)
        return e

    def parameter(e, name):
        e.set_editor_property('parameter_name', name)
        e.set_editor_property('group', SURFACE_PARAM_GROUPS[name])
        e.set_editor_property('desc', SURFACE_PARAM_DESCRIPTIONS[name])
        return e

    # ---- lace = WaveFoamLaceTexture.R at world XY / WaveFoamLaceTileCm
    world = new(unreal.MaterialExpressionWorldPosition, -1700, 900)
    xy = new(unreal.MaterialExpressionComponentMask, -1580, 900)
    xy.set_editor_property('r', True)
    xy.set_editor_property('g', True)
    xy.set_editor_property('b', False)
    xy.set_editor_property('a', False)
    connect(world, '', xy, '')
    tile = parameter(new(unreal.MaterialExpressionScalarParameter, -1580, 980), 'WaveFoamLaceTileCm')
    tile.set_editor_property('default_value', 400.0)
    uv = new(unreal.MaterialExpressionDivide, -1460, 920)
    connect(xy, '', uv, 'A')
    connect(tile, '', uv, 'B')
    texture = parameter(new(unreal.MaterialExpressionTextureObjectParameter, -1460, 1020), 'WaveFoamLaceTexture')
    lace_asset = unreal.load_asset('/Game/FluidNinjaLive/Textures/T_foam2')
    if not lace_asset:
        fail('the lace texture /Game/FluidNinjaLive/Textures/T_foam2 is missing')
    texture.set_editor_property('texture', lace_asset)
    sample = new(unreal.MaterialExpressionTextureSample, -1320, 940, 'FluidNinja Waves foam lace')
    connect(texture, '', sample, 'Tex')
    connect(uv, '', sample, 'UVs')
    amount = parameter(new(unreal.MaterialExpressionScalarParameter, -1320, 1080), 'WaveFoamLace')
    amount.set_editor_property('default_value', 0.8)

    # ---- per mask m, with g = saturate(lace x 3) (the veins near 1, the dark between them low):
    #      laced = saturate(m x (0.55 + 0.45 g) x smoothstep(0, 1, (m + 0.5 g - 0.35) x 2))
    #      thick foam stays white on the veins and about 60 % between them; thinner foam keeps the veins; thin foam only faint veins.
    #      The switch's True gets lerp(m, laced, WaveFoamLace).
    three = new(unreal.MaterialExpressionConstant, -1320, 1280)
    three.set_editor_property('r', 3.0)
    veins_raw = new(unreal.MaterialExpressionMultiply, -1180, 960)
    connect(sample, 'R', veins_raw, 'A')
    connect(three, '', veins_raw, 'B')
    veins = new(unreal.MaterialExpressionSaturate, -1060, 960)
    connect(veins_raw, '', veins, '')
    body_scale = new(unreal.MaterialExpressionMultiply, -940, 960)   # 0.45 g
    k045 = new(unreal.MaterialExpressionConstant, -1060, 1040)
    k045.set_editor_property('r', 0.45)
    connect(veins, '', body_scale, 'A')
    connect(k045, '', body_scale, 'B')
    body = new(unreal.MaterialExpressionAdd, -820, 960)   # 0.55 + 0.45 g
    k055 = new(unreal.MaterialExpressionConstant, -940, 1040)
    k055.set_editor_property('r', 0.55)
    connect(k055, '', body, 'A')
    connect(body_scale, '', body, 'B')
    half_veins = new(unreal.MaterialExpressionMultiply, -940, 1120)   # 0.5 g
    k05 = new(unreal.MaterialExpressionConstant, -1060, 1120)
    k05.set_editor_property('r', 0.5)
    connect(veins, '', half_veins, 'A')
    connect(k05, '', half_veins, 'B')
    k035 = new(unreal.MaterialExpressionConstant, -820, 1200)
    k035.set_editor_property('r', 0.35)
    two = new(unreal.MaterialExpressionConstant, -820, 1260)
    two.set_editor_property('r', 2.0)
    for row, (switch, mask, mask_out) in enumerate(switches):
        dy = 1360 + row * 300
        thick = new(unreal.MaterialExpressionMultiply, -700, dy)   # m (0.55 + 0.45 g)
        connect(mask, mask_out, thick, 'A')
        connect(body, '', thick, 'B')
        reach = new(unreal.MaterialExpressionAdd, -700, dy + 90)   # m + 0.5 g
        connect(mask, mask_out, reach, 'A')
        connect(half_veins, '', reach, 'B')
        shifted = new(unreal.MaterialExpressionSubtract, -580, dy + 90)
        connect(reach, '', shifted, 'A')
        connect(k035, '', shifted, 'B')
        widened = new(unreal.MaterialExpressionMultiply, -460, dy + 90)
        connect(shifted, '', widened, 'A')
        connect(two, '', widened, 'B')
        edge = new(unreal.MaterialExpressionSmoothStep, -340, dy + 90)
        edge.set_editor_property('const_min', 0.0)
        edge.set_editor_property('const_max', 1.0)
        connect(widened, '', edge, 'Value')
        lacy = new(unreal.MaterialExpressionMultiply, -340, dy)
        connect(thick, '', lacy, 'A')
        connect(edge, '', lacy, 'B')
        laced = new(unreal.MaterialExpressionSaturate, -220, dy)
        connect(lacy, '', laced, '')
        mix = new(unreal.MaterialExpressionLinearInterpolate, -100, dy, 'FluidNinja Waves foam lace')
        connect(mask, mask_out, mix, 'A')
        connect(laced, '', mix, 'B')
        connect(amount, '', mix, 'Alpha')
        connect(mix, '', switch, 'True')
        if input_of(fn, switch, 'True') != mix:
            fail('the foam lace is not wired into a WaveFoam switch')
    log('surface: the foam broken into lace (WaveFoamLace, WaveFoamLaceTexture T_foam2, WaveFoamLaceTileCm) on the fresh and the aged foam')


def add_surface_foam_relief(fn, normal_fn):
    """The whitewater's relief: WorldPositionOffset += (0, 0, WaveFoamHeightCm x FoamMask), the fresh foam as drawn (lace included), and
    the normal += (WaveFoamRoughness x FoamMask x the slope of the lace, 0), the slope through MF_NormalFromHeightmap_SineWaves on
    WaveFoamLaceTexture's R at the lace's world UV: whitewater stands up from the water and its clumps catch the light. Both default 0,
    which adds exactly 0. Anchors: MakeMaterialAttributes_0's WorldPositionOffset feeder, the fresh foam's WaveFoam switch (BaseColor's
    LinearInterpolate's Alpha), the lace sample (the TextureSample on WaveFoamLaceTexture, and its UVs) and Reroute_107's input (the
    normal chain with the waves). Needs the foam lace. Refuses a function that has it."""
    if surface_has_foam_relief(fn):
        fail('the material function already has the foam relief (WaveFoamHeightCm)')
    if not surface_has_foam_lace(fn):
        fail('the foam relief needs the foam lace first (WaveFoamLace)')
    mma = expression(fn, 'MaterialExpressionMakeMaterialAttributes_0')
    normal_next = expression(fn, 'MaterialExpressionReroute_107')
    if mma is None or normal_next is None:
        fail('vendor nodes not found (MakeMaterialAttributes_0, Reroute_107)')
    fresh_lerp = input_of(fn, mma, 'BaseColor')
    switch = input_of(fn, fresh_lerp, 'Alpha') if fresh_lerp is not None else None
    if (switch is None or switch.get_class().get_name() != 'MaterialExpressionStaticSwitchParameter'
            or str(switch.get_editor_property('parameter_name')) != 'WaveFoam'):
        fail('the fresh foam\'s WaveFoam switch was not found (BaseColor\'s LinearInterpolate\'s Alpha)')
    wpo_feed, wpo_out = feeder(fn, mma, 'WorldPositionOffset')
    normal_ins = MEL.get_inputs_for_material_function_expression(fn, normal_next)
    normal_feed = normal_ins[0] if normal_ins else None
    if wpo_feed is None or normal_feed is None:
        fail('MakeMaterialAttributes_0.WorldPositionOffset or Reroute_107 is unconnected')
    normal_out = MEL.get_input_node_output_name_for_material_expression(normal_next, normal_feed)
    if isinstance(normal_out, tuple):
        normal_out = normal_out[1] if normal_out[0] else ''
    lace_tex = parameter_expression(fn, 'MaterialExpressionTextureObjectParameter', 'WaveFoamLaceTexture')
    lace_sample = None
    for e in MEL.get_material_function_expressions(fn):
        if e.get_class().get_name() == 'MaterialExpressionTextureSample' and lace_tex and input_of(fn, e, 'Tex') == lace_tex[0]:
            lace_sample = e
    if lace_sample is None:
        fail('the lace sample (a TextureSample on WaveFoamLaceTexture) was not found')
    lace_uv, lace_uv_out = feeder(fn, lace_sample, 'UVs')
    if lace_uv is None:
        fail('the lace sample has no UVs')
    x0 = mma.get_editor_property('material_expression_editor_x')
    y0 = mma.get_editor_property('material_expression_editor_y')

    def new(cls, dx, dy, desc=''):
        e = MEL.create_material_expression_in_function(fn, cls, x0 + dx, y0 + dy)
        if not e:
            fail('create ' + cls.__name__)
        if desc:
            e.set_editor_property('desc', desc)
        return e

    def parameter(name, default, dx, dy):
        e = new(unreal.MaterialExpressionScalarParameter, dx, dy)
        e.set_editor_property('parameter_name', name)
        e.set_editor_property('default_value', default)
        e.set_editor_property('group', SURFACE_PARAM_GROUPS[name])
        e.set_editor_property('desc', SURFACE_PARAM_DESCRIPTIONS[name])
        return e

    # ---- WPO += (0, 0, WaveFoamHeightCm x FoamMask)
    height = parameter('WaveFoamHeightCm', 0.0, 200, 1700)
    lift = new(unreal.MaterialExpressionMultiply, 350, 1700)
    connect(switch, '', lift, 'A')
    connect(height, '', lift, 'B')
    flat = new(unreal.MaterialExpressionConstant2Vector, 350, 1800)
    flat.set_editor_property('r', 0.0)
    flat.set_editor_property('g', 0.0)
    up = new(unreal.MaterialExpressionAppendVector, 500, 1750)
    connect(flat, '', up, 'A')
    connect(lift, '', up, 'B')
    wpo_sum = new(unreal.MaterialExpressionAdd, 650, 1700, 'WPO + FluidNinja Waves foam relief')
    connect(wpo_feed, wpo_out, wpo_sum, 'A')
    connect(up, '', wpo_sum, 'B')
    connect(wpo_sum, '', mma, 'WorldPositionOffset')

    # ---- normal += (WaveFoamRoughness x FoamMask x the lace's slope, 0)
    roughness = parameter('WaveFoamRoughness', 0.0, 200, 1950)
    call = new(unreal.MaterialExpressionMaterialFunctionCall, 350, 1950)
    call.set_editor_property('material_function', normal_fn)
    red = new(unreal.MaterialExpressionConstant4Vector, 200, 2050)
    red.set_editor_property('constant', unreal.LinearColor(1.0, 0.0, 0.0, 0.0))
    no_sine = new(unreal.MaterialExpressionStaticBool, 200, 2130)
    no_sine.set_editor_property('value', False)
    connect(lace_tex[0], '', call, 'Height Map')
    connect(lace_uv, lace_uv_out, call, 'Coordinates')
    connect(red, '', call, 'Height Map Channel Selector')
    connect(no_sine, '', call, 'ApplySineWavesToGradient')
    connect(roughness, '', call, 'Normal Map Intensity')
    slope = new(unreal.MaterialExpressionComponentMask, 550, 1950)
    slope.set_editor_property('r', True)
    slope.set_editor_property('g', True)
    slope.set_editor_property('b', False)
    slope.set_editor_property('a', False)
    connect(call, 'Result', slope, '')
    slope_foam = new(unreal.MaterialExpressionMultiply, 700, 1950)
    connect(slope, '', slope_foam, 'A')
    connect(switch, '', slope_foam, 'B')
    zero = new(unreal.MaterialExpressionConstant, 700, 2050)
    zero.set_editor_property('r', 0.0)
    slope3 = new(unreal.MaterialExpressionAppendVector, 850, 1950)
    connect(slope_foam, '', slope3, 'A')
    connect(zero, '', slope3, 'B')
    normal_sum = new(unreal.MaterialExpressionAdd, 1000, 1900, 'Normal + FluidNinja Waves foam relief')
    connect(normal_feed, str(normal_out or ''), normal_sum, 'A')
    connect(slope3, '', normal_sum, 'B')
    connect(normal_sum, '', normal_next, '')
    if input_of(fn, mma, 'WorldPositionOffset') != wpo_sum or MEL.get_inputs_for_material_function_expression(fn, normal_next)[0] != normal_sum:
        fail('the foam relief is not wired in')
    log('surface: the whitewater relief added (WaveFoamHeightCm, WaveFoamRoughness; both 0 by default)')


def relief_lift(fn):
    """The relief's lift and the WaveFoam switch it multiplies: (Multiply, switch) from MakeMaterialAttributes_0's WorldPositionOffset
    (WPO + FluidNinja Waves foam relief) through its AppendVector's B; (None, None) when the function has no relief laid out so."""
    mma = expression(fn, 'MaterialExpressionMakeMaterialAttributes_0')
    wpo_sum = input_of(fn, mma, 'WorldPositionOffset') if mma is not None else None
    if wpo_sum is None or str(wpo_sum.get_editor_property('desc')) != 'WPO + FluidNinja Waves foam relief':
        return None, None
    up = input_of(fn, wpo_sum, 'B')
    lift = input_of(fn, up, 'B') if up is not None else None
    switch = input_of(fn, lift, 'A') if lift is not None and lift.get_class().get_name() == 'MaterialExpressionMultiply' else None
    if (switch is None or switch.get_class().get_name() != 'MaterialExpressionStaticSwitchParameter'
            or str(switch.get_editor_property('parameter_name')) != 'WaveFoam'):
        return None, None
    return lift, switch


def surface_has_smooth_relief(fn):
    """Whether the SURFACE function lifts the whitewater by the unlaced fresh foam: its relief's WaveFoam switch takes on True something
    other than the foam lace's LinearInterpolate."""
    lift, switch = relief_lift(fn)
    if switch is None:
        return False
    on = input_of(fn, switch, 'True')
    return on is not None and str(on.get_editor_property('desc')) != 'FluidNinja Waves foam lace'


def smooth_surface_foam_relief(fn):
    """The whitewater lifted by the unlaced fresh foam: the relief's lift (WaveFoamHeightCm x the switch) gets a WaveFoam switch of its
    own (the same static parameter, so the same value) whose True is the fresh foam before the lace (m = saturate(F x WaveFoamStrength),
    the lace LinearInterpolate's A) and whose False is the fresh switch's. The lace stays in the colour, the opacity and the normal
    (WaveFoamRoughness). Lifted by the laced foam, each vertex of the water took 0 or the whole WaveFoamHeightCm by where it fell on the
    lace, and whitewater stood up as sharp white spikes a vertex wide (a 120 m test basin, 2026-10-20). Needs the relief.
    Refuses a function that has it."""
    if surface_has_smooth_relief(fn):
        fail('the material function already lifts the whitewater by the unlaced foam')
    lift, switch = relief_lift(fn)
    if lift is None:
        fail('the relief lift (WPO + FluidNinja Waves foam relief, its AppendVector, the Multiply on a WaveFoam switch) was not found')
    mix = input_of(fn, switch, 'True')
    if mix is None or str(mix.get_editor_property('desc')) != 'FluidNinja Waves foam lace':
        fail('the relief\'s WaveFoam switch does not take the foam lace on True')
    mask, mask_out = feeder(fn, mix, 'A')
    if mask is None:
        fail('the foam lace has nothing on A (the unlaced foam)')
    off, off_out = feeder(fn, switch, 'False')
    x = lift.get_editor_property('material_expression_editor_x')
    y = lift.get_editor_property('material_expression_editor_y')
    smooth = MEL.create_material_expression_in_function(fn, unreal.MaterialExpressionStaticSwitchParameter, x - 150, y - 150)
    if not smooth:
        fail('create MaterialExpressionStaticSwitchParameter')
    smooth.set_editor_property('parameter_name', 'WaveFoam')
    smooth.set_editor_property('default_value', switch.get_editor_property('default_value'))
    smooth.set_editor_property('group', SURFACE_PARAM_GROUPS['WaveFoam'])
    smooth.set_editor_property('desc', SURFACE_PARAM_DESCRIPTIONS['WaveFoam'])
    connect(mask, mask_out, smooth, 'True')
    if off is not None:
        connect(off, off_out, smooth, 'False')
    connect(smooth, '', lift, 'A')
    if not surface_has_smooth_relief(fn):
        fail('the relief lift is not wired to the unlaced foam')
    log('surface: the whitewater lift follows the unlaced fresh foam (the lace stays in the colour, the opacity and the roughness)')


def surface_has_wind_sea(fn):
    """Whether the SURFACE function draws the wind sea's wavelets (interface version 7): its WaveWindSeaStrength."""
    return 'WaveWindSeaStrength' in parameter_names(fn, 'MaterialExpressionScalarParameter')


# The wavelets' Custom node (add_surface_wind_sea): the short band of the plugin's wind-sea table as a world-space normal tilt (-dW/dx,
# -dW/dy, 0), W = sum over i = L + 1..N of a_i cos(k_i . (x - origin) + phase_i), each wavelet faded out where it spans fewer than about
# four pixels (2 pixels: gone, 4: whole), so distant water does not shimmer. No table (the black default), a table whose width is not
# N + 1, no short band, or WaveWindSeaStrength 0 returns 0 after one load.
WIND_SEA_HLSL = '''float3 Tilt = float3(0.0, 0.0, 0.0);
if (Strength > 0.0)
{
    uint W, H;
    Table.GetDimensions(W, H);
    if (W >= 2 && H == 1)
    {
        float4 Header = Table.Load(int3(0, 0, 0));
        int Long = int(Header.z + 0.5);
        int Total = int(Header.w + 0.5);
        if (Total == int(W) - 1 && Total > Long && Long >= 0)
        {
            float2 Rel = WorldPos.xy - Header.xy;
            float Footprint = max(max(length(ddx(WorldPos.xy)), length(ddy(WorldPos.xy))), 1e-3);
            float2 Grad = float2(0.0, 0.0);
            for (int I = Long + 1; I <= Total; ++I)
            {
                float4 C = Table.Load(int3(I, 0, 0));
                float Lambda = 6.2831853 / max(length(C.xy), 1e-6);
                float Fade = saturate(Lambda / Footprint * 0.5 - 1.0);
                Grad -= Fade * C.z * C.xy * sin(dot(C.xy, Rel) + C.w);
            }
            Tilt = float3(-Grad * Strength, 0.0);
        }
    }
}
return Tilt;'''


def add_surface_wind_sea(fn):
    """The wind sea's wavelets (interface version 7): normal += TransformVector(world -> tangent, the Custom node WIND_SEA_HLSL's tilt), the
    short band of WaveWindSeaTable (the plugin's wind-sea table, a Linear Color texture object whose default is the black RT_WaveAux_Unbound)
    at the absolute world position, scaled by WaveWindSeaStrength (1). The long band reaches the surface through B and the waves' normal
    already. Anchor: Reroute_107's input (the normal chain with the waves); the tilt is added on whatever feeds it when this runs, so it
    composes with the foam relief in either order. Unbound it adds exactly 0. Refuses a function that has it."""
    if surface_has_wind_sea(fn):
        fail('the material function already draws the wind sea\'s wavelets (WaveWindSeaStrength)')
    if not surface_has_waves(fn):
        fail('the wind sea\'s wavelets need the waves first (WaveHeightScale / WaveNormalScale)')
    normal_next = expression(fn, 'MaterialExpressionReroute_107')
    mma = expression(fn, 'MaterialExpressionMakeMaterialAttributes_0')
    if normal_next is None or mma is None:
        fail('vendor nodes not found (Reroute_107, MakeMaterialAttributes_0)')
    normal_ins = MEL.get_inputs_for_material_function_expression(fn, normal_next)
    normal_feed = normal_ins[0] if normal_ins else None
    if normal_feed is None:
        fail('Reroute_107 is unconnected')
    normal_out = MEL.get_input_node_output_name_for_material_expression(normal_next, normal_feed)
    if isinstance(normal_out, tuple):
        normal_out = normal_out[1] if normal_out[0] else ''
    unbound = unreal.load_asset(AUX_UNBOUND)
    if not unbound:
        fail('no ' + AUX_UNBOUND + ' (the core step makes it)')
    x0 = mma.get_editor_property('material_expression_editor_x')
    y0 = mma.get_editor_property('material_expression_editor_y')

    def new(cls, dx, dy, desc=''):
        e = MEL.create_material_expression_in_function(fn, cls, x0 + dx, y0 + dy)
        if not e:
            fail('create ' + cls.__name__)
        if desc:
            e.set_editor_property('desc', desc)
        return e

    table = new(unreal.MaterialExpressionTextureObjectParameter, 200, 2300)
    table.set_editor_property('parameter_name', 'WaveWindSeaTable')
    table.set_editor_property('texture', unbound)
    # Linear Color: the table is a float texture (and its default a float render target), which a Color sampler refuses.
    table.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    table.set_editor_property('group', SURFACE_PARAM_GROUPS['WaveWindSeaTable'])
    table.set_editor_property('desc', SURFACE_PARAM_DESCRIPTIONS['WaveWindSeaTable'])
    strength = new(unreal.MaterialExpressionScalarParameter, 200, 2420)
    strength.set_editor_property('parameter_name', 'WaveWindSeaStrength')
    strength.set_editor_property('default_value', 1.0)
    strength.set_editor_property('group', SURFACE_PARAM_GROUPS['WaveWindSeaStrength'])
    strength.set_editor_property('desc', SURFACE_PARAM_DESCRIPTIONS['WaveWindSeaStrength'])
    world = new(unreal.MaterialExpressionWorldPosition, 200, 2520)
    custom = new(unreal.MaterialExpressionCustom, 450, 2400, 'FluidNinja Waves wind sea: the wavelets\' world-space normal tilt')
    custom.set_editor_property('code', WIND_SEA_HLSL)
    custom.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    custom.set_editor_property('description', 'FNB_WindSeaWavelets')
    pins = []
    for name in ('Table', 'Strength', 'WorldPos'):
        pin = unreal.CustomInput()
        pin.set_editor_property('input_name', name)
        pins.append(pin)
    custom.set_editor_property('inputs', pins)
    connect(table, '', custom, 'Table')
    connect(strength, '', custom, 'Strength')
    connect(world, '', custom, 'WorldPos')
    to_tangent = new(unreal.MaterialExpressionTransform, 700, 2400, 'world -> tangent')
    to_tangent.set_editor_property('transform_source_type', unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_WORLD)
    to_tangent.set_editor_property('transform_type', unreal.MaterialVectorCoordTransform.TRANSFORM_TANGENT)
    connect(custom, '', to_tangent, '')
    normal_sum = new(unreal.MaterialExpressionAdd, 900, 2350, 'Normal + FluidNinja Waves wind sea')
    connect(normal_feed, str(normal_out or ''), normal_sum, 'A')
    connect(to_tangent, '', normal_sum, 'B')
    connect(normal_sum, '', normal_next, '')
    if MEL.get_inputs_for_material_function_expression(fn, normal_next)[0] != normal_sum:
        fail('the wind sea\'s wavelets are not wired in')
    log('surface: the wind sea\'s wavelets added (WaveWindSeaTable, WaveWindSeaStrength 1; unbound they add 0)')


def surface_has_rings(fn):
    """Whether the SURFACE function draws the rings (V8; surface and plugin only): its WaveRingStrength."""
    return 'WaveRingStrength' in parameter_names(fn, 'MaterialExpressionScalarParameter')


# The rings' Custom node (add_surface_rings): the plugin's ring table summed through its response table as a world-space normal tilt
# (-dEta/dx, -dEta/dy, 0). Table, (N + 2) x 1 RGBA32F: texel 0 = (origin x, origin y in world cm, N, 0), texel 1 = (LUT radius R cm,
# LUT seconds T, bands, rows per band M), texel 2 + i = (x, y from the origin cm, age s + 16 x band, amplitude in LUT unit impulses).
# LUT, Nr x (M x bands) RGBA16F, bilinear: across u = sqrt(r / R) (texel i at r = R ((i + 0.5) / Nr)^2), down row band x M + j at age
# T (j + 0.5) / M; R = d eta / dr of one unit impulse, B = the local wavelength cm. Per ring: D = the pixel from the ring, r = |D|,
# grad eta += amplitude x d eta / dr x D / r, each ring faded out where its wavelength spans fewer than about four pixels (2 pixels:
# gone, 4: whole), as the wind sea's wavelets are. The age is clamped half a row inside its band, so bilinear filtering never mixes two
# bands. N comes from the header, not the width (the plugin keeps spare texels). No table (the black 1 x 1 default: no Load at all), N 0
# (one Load), a LUT that is the 1 x 1 default, or WaveRingStrength 0 returns 0.
RINGS_HLSL = '''float3 Tilt = float3(0.0, 0.0, 0.0);
if (Strength > 0.0)
{
    uint W, H;
    Table.GetDimensions(W, H);
    if (W >= 3 && H == 1)
    {
        float4 Header = Table.Load(int3(0, 0, 0));
        int Count = min(int(Header.z + 0.5), 128);
        if (Count >= 1 && Count + 2 <= int(W))
        {
            float4 Layout = Table.Load(int3(1, 0, 0));
            float Radius = Layout.x;
            float Seconds = Layout.y;
            float Bands = floor(Layout.z + 0.5);
            float Rows = floor(Layout.w + 0.5);
            uint LW, LH;
            LUT.GetDimensions(LW, LH);
            if (Radius > 0.0 && Seconds > 0.0 && Bands >= 1.0 && Rows >= 1.0 && LW >= 2 && LH >= 2)
            {
                float2 Rel = WorldPos.xy - Header.xy;
                float Footprint = max(max(length(ddx(WorldPos.xy)), length(ddy(WorldPos.xy))), 1e-3);
                float2 Grad = float2(0.0, 0.0);
                for (int I = 0; I < Count; ++I)
                {
                    float4 C = Table.Load(int3(I + 2, 0, 0));
                    float2 D = Rel - C.xy;
                    float Dist = length(D);
                    float Band = floor(C.z / 16.0);
                    float Age = C.z - 16.0 * Band;
                    if (Dist < Radius && Age < Seconds && Band >= 0.0 && Band < Bands)
                    {
                        float LutU = sqrt(Dist / Radius);
                        float LutV = (Band * Rows + 0.5 + clamp(Age / Seconds * Rows - 0.5, 0.0, Rows - 1.0)) / (Rows * Bands);
                        float4 S = LUT.SampleLevel(LUTSampler, float2(LutU, LutV), 0.0);
                        float Fade = saturate(S.b / Footprint * 0.5 - 1.0);
                        Grad += Fade * C.w * S.r * D / max(Dist, 1e-3);
                    }
                }
                Tilt = float3(-Grad * Strength, 0.0);
            }
        }
    }
}
return Tilt;'''


def add_surface_rings(fn):
    """The rings (V8; surface and plugin only): normal += TransformVector(world -> tangent, the Custom node RINGS_HLSL's tilt), the plugin's
    ring table WaveRingTable (the recent water contacts' sub-grid ripples) read through its response table WaveRingLUT (the exact linear
    ripple per band by radius and age, sampled bilinearly through LUTSampler, the texture object's own sampler), both Linear Color texture
    objects whose default is the black RT_WaveAux_Unbound, at the absolute world position, scaled by WaveRingStrength (1). The long part of
    each impact reaches the surface through B and the waves' normal already. Anchor: Reroute_107's input (the normal chain with the waves
    and, when it ran first, the wind sea's wavelets); the tilt is added on whatever feeds it when this runs, so it composes with the wind
    sea and the foam relief in either order. Unbound it adds exactly 0. Refuses a function that has it."""
    if surface_has_rings(fn):
        fail('the material function already draws the rings (WaveRingStrength)')
    if not surface_has_waves(fn):
        fail('the rings need the waves first (WaveHeightScale / WaveNormalScale)')
    normal_next = expression(fn, 'MaterialExpressionReroute_107')
    mma = expression(fn, 'MaterialExpressionMakeMaterialAttributes_0')
    if normal_next is None or mma is None:
        fail('vendor nodes not found (Reroute_107, MakeMaterialAttributes_0)')
    normal_ins = MEL.get_inputs_for_material_function_expression(fn, normal_next)
    normal_feed = normal_ins[0] if normal_ins else None
    if normal_feed is None:
        fail('Reroute_107 is unconnected')
    normal_out = MEL.get_input_node_output_name_for_material_expression(normal_next, normal_feed)
    if isinstance(normal_out, tuple):
        normal_out = normal_out[1] if normal_out[0] else ''
    unbound = unreal.load_asset(AUX_UNBOUND)
    if not unbound:
        fail('no ' + AUX_UNBOUND + ' (the core step makes it)')
    x0 = mma.get_editor_property('material_expression_editor_x')
    y0 = mma.get_editor_property('material_expression_editor_y')

    def new(cls, dx, dy, desc=''):
        e = MEL.create_material_expression_in_function(fn, cls, x0 + dx, y0 + dy)
        if not e:
            fail('create ' + cls.__name__)
        if desc:
            e.set_editor_property('desc', desc)
        return e

    def texture(name, dy):
        e = new(unreal.MaterialExpressionTextureObjectParameter, 200, dy)
        e.set_editor_property('parameter_name', name)
        e.set_editor_property('texture', unbound)
        # Linear Color: both are float textures (and their default a float render target), which a Color sampler refuses.
        e.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
        e.set_editor_property('group', SURFACE_PARAM_GROUPS[name])
        e.set_editor_property('desc', SURFACE_PARAM_DESCRIPTIONS[name])
        return e

    # Below the wind sea's nodes (2300-2520).
    table = texture('WaveRingTable', 2700)
    lut = texture('WaveRingLUT', 2820)
    strength = new(unreal.MaterialExpressionScalarParameter, 200, 2940)
    strength.set_editor_property('parameter_name', 'WaveRingStrength')
    strength.set_editor_property('default_value', 1.0)
    strength.set_editor_property('group', SURFACE_PARAM_GROUPS['WaveRingStrength'])
    strength.set_editor_property('desc', SURFACE_PARAM_DESCRIPTIONS['WaveRingStrength'])
    world = new(unreal.MaterialExpressionWorldPosition, 200, 3040)
    custom = new(unreal.MaterialExpressionCustom, 450, 2850, 'FluidNinja Waves rings: the ripples\' world-space normal tilt')
    custom.set_editor_property('code', RINGS_HLSL)
    custom.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    custom.set_editor_property('description', 'FNB_WaveRings')
    pins = []
    # A Texture Object input X reaches the code as Texture2D X and SamplerState XSampler (HLSLMaterialTranslator, CustomExpression).
    for name in ('Table', 'LUT', 'Strength', 'WorldPos'):
        pin = unreal.CustomInput()
        pin.set_editor_property('input_name', name)
        pins.append(pin)
    custom.set_editor_property('inputs', pins)
    connect(table, '', custom, 'Table')
    connect(lut, '', custom, 'LUT')
    connect(strength, '', custom, 'Strength')
    connect(world, '', custom, 'WorldPos')
    to_tangent = new(unreal.MaterialExpressionTransform, 700, 2850, 'world -> tangent')
    to_tangent.set_editor_property('transform_source_type', unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_WORLD)
    to_tangent.set_editor_property('transform_type', unreal.MaterialVectorCoordTransform.TRANSFORM_TANGENT)
    connect(custom, '', to_tangent, '')
    normal_sum = new(unreal.MaterialExpressionAdd, 1100, 2600, 'Normal + FluidNinja Waves rings')
    connect(normal_feed, str(normal_out or ''), normal_sum, 'A')
    connect(to_tangent, '', normal_sum, 'B')
    connect(normal_sum, '', normal_next, '')
    if MEL.get_inputs_for_material_function_expression(fn, normal_next)[0] != normal_sum:
        fail('the rings are not wired in')
    log('surface: the rings added (WaveRingTable, WaveRingLUT, WaveRingStrength 1; unbound they add 0)')


def apply_material():
    fn = unreal.load_asset(SURFACE)
    normal_fn = unreal.load_asset(NORMAL_FN)
    if not fn or not normal_fn:
        fail('no %s or %s' % (SURFACE, NORMAL_FN))
    has_waves = surface_has_waves(fn)
    has_foam = surface_has_foam(fn)
    has_aged = surface_has_v6(fn)
    has_opacity = surface_has_foam_opacity(fn)
    has_lace = surface_has_foam_lace(fn)
    has_relief = surface_has_foam_relief(fn)
    has_smooth_relief = has_relief and surface_has_smooth_relief(fn)
    has_wind_sea = surface_has_wind_sea(fn)
    has_rings = surface_has_rings(fn)
    if has_opacity and has_lace and has_relief and has_smooth_relief and has_wind_sea and has_rings:
        fail('the material function already has everything (the foam opacity, the foam lace, the foam relief lifted by the unlaced foam, '
             'the wind sea\'s wavelets and the rings): nothing to add')
    if has_foam and not has_waves:
        fail('the material function has the foam without the waves: not a function this toolchain made')
    if has_waves:
        log('surface: carries the waves%s%s%s%s%s%s%s%s already: adding what it lacks' % (
            ', the foam' if has_foam else '', ', the aged foam' if has_aged else '', ', the foam opacity' if has_opacity else '',
            ', the foam lace' if has_lace else '', ', the foam relief' if has_relief else '',
            ' (lifted by the unlaced foam)' if has_smooth_relief else '', ', the wind sea\'s wavelets' if has_wind_sea else '',
            ', the rings' if has_rings else ''))
    if not has_waves:
        add_surface_waves(fn, normal_fn)
    if not has_foam:
        add_surface_foam(fn)
    if not has_aged:
        add_surface_aged(fn)
    if not has_opacity:
        add_surface_foam_opacity(fn)
    if not has_lace:
        add_surface_foam_lace(fn)
    if not has_relief:
        add_surface_foam_relief(fn, normal_fn)
    if not has_smooth_relief:
        smooth_surface_foam_relief(fn)
    if not has_wind_sea:
        add_surface_wind_sea(fn)
    # After the wind sea: the rings' Add takes whatever feeds Reroute_107 then (the wind sea's Add), so the two compose.
    if not has_rings:
        add_surface_rings(fn)
    describe_surface(fn)

    MEL.update_material_function(fn, None)
    errors = []
    for path in CHECK_MATERIALS:
        material = unreal.load_asset(path) if EAL.does_asset_exist(path) else None
        if not material:
            log('material check skipped (absent):', path)
            continue
        result = DEV.compile_material_errors(material)
        log('material compile', path, '->', result.replace('\n', ' | '))
        if result != 'OK':
            errors.append(path)
    if errors:
        fail('materials do not compile: %s (nothing saved)' % errors)
    if not EAL.save_loaded_asset(fn, False):
        fail('save ' + SURFACE)
    log('saved', SURFACE, 'sha256', file_sha(package_file(SURFACE)))


# ---------------------------------------------------------------------------------------------------------------------------------
# verify: no writes
# ---------------------------------------------------------------------------------------------------------------------------------
def verify():
    problems = []
    core_file = package_file(CORE)
    if not os.path.exists(core_file):
        fail('no ' + core_file)
    digest = file_sha(core_file)
    kind, desc = known(KNOWN_CORES, digest)
    log('VERIFY core', CORE, 'sha256', digest, '| sha8', digest[:8], '| known:', kind, '-', desc)
    system = unreal.load_asset(CORE)
    if not system:
        fail('no ' + CORE)
    summary = DEV.describe_niagara(system, '', '')
    base = core_base(summary)
    has_waves = core_has_waves(system, summary)
    version = user_int(summary, 'User.Wave.InterfaceVersion')
    build = user_int(summary, 'User.Wave.CoreBuild')
    state = 'waves-applied' if has_waves else ('pass3' if base.startswith('pass3') else 'vendor')
    log('VERIFY state', state, '| base', base, '| interface version', version, '| core build', build, '| this toolchain', INTERFACE_VERSION)
    names = stage_names(system)
    missing_stages = [s for s, *_ in STAGES if s not in names]
    log('VERIFY stages', [s for s, *_ in STAGES if s in names], '| missing', missing_stages)
    update_text = DEV.describe_niagara(system, EMITTER, 'EmitterUpdateScript')
    has_params = 'MODULE Ninja_WaveParams ' in update_text
    log('VERIFY emitter update Ninja_WaveParams', has_params)
    missing_modules = []
    for name in list(MODULES) + ['RT_WaveAux_Unbound']:
        path = WAVES + '/' + name
        file_path = package_file(path)
        if not (EAL.does_asset_exist(path) and os.path.exists(file_path)):
            missing_modules.append(name)
            log('VERIFY module', path, 'MISSING')
            continue
        module_digest = file_sha(file_path)
        module_build, _ = KNOWN_MODULES.get(module_digest, ('unknown', name))
        log('VERIFY module', path, 'sha8', module_digest[:8], '| built with core', module_build)
    protected_kinds = {}
    for label, asset in [('PainterSourceData', PAINTER_SOURCE), ('Ninja_Painter', PAINTER_MODULE)]:
        file_path = package_file(asset)
        if not os.path.exists(file_path):
            problems.append('%s is missing (%s)' % (label, file_path))
            continue
        protected_digest = file_sha(file_path)
        protected_kinds[label] = known(KNOWN_PROTECTED, protected_digest)[0]
        log('VERIFY protected', label, 'sha8', protected_digest[:8], '| known:', protected_kinds[label])
    surface_file = package_file(SURFACE)
    carries = False
    carries_foam = False
    carries_aged = False
    if os.path.exists(surface_file):
        surface_digest = file_sha(surface_file)
        fn = unreal.load_asset(SURFACE)
        carries = bool(fn) and surface_has_waves(fn)
        carries_foam = bool(fn) and surface_has_foam(fn)
        carries_aged = bool(fn) and surface_has_v6(fn)
        carries_opacity = bool(fn) and surface_has_foam_opacity(fn)
        carries_lace = bool(fn) and surface_has_foam_lace(fn)
        carries_relief = bool(fn) and surface_has_foam_relief(fn)
        carries_smooth_relief = carries_relief and surface_has_smooth_relief(fn)
        carries_wind_sea = bool(fn) and surface_has_wind_sea(fn)
        carries_rings = bool(fn) and surface_has_rings(fn)
        log('VERIFY surface', SURFACE, 'sha8', surface_digest[:8], '| known:', known(KNOWN_SURFACE, surface_digest)[0],
            '| carries the waves:', carries, '| the foam and the choppiness:', carries_foam, '| the aged foam (6):', carries_aged,
            '| the foam opacity:', carries_opacity, '| the foam lace:', carries_lace, '| the foam relief:', carries_relief,
            '(lifted by the unlaced foam: %s)' % carries_smooth_relief, '| the wind sea\'s wavelets (7):', carries_wind_sea,
            '| the rings (V8):', carries_rings)
        if carries_relief and not carries_smooth_relief:
            log('WARNING the SURFACE function lifts the whitewater by the laced foam (WaveFoamHeightCm x the lace): its vertices stand up as '
                'sharp spikes; run the material step')
        if carries_aged and not carries_opacity:
            log('WARNING the SURFACE function has the foam but not the foam opacity (WaveFoamOpacity): on Single Layer Water and translucent '
                'materials the foam barely shows; run the material step')
    else:
        log('VERIFY surface', SURFACE, 'MISSING')

    if not has_waves:
        problems.append('the core has no waves (apply core material)')
    if not base.startswith('pass3'):
        problems.append('the core is not on the pass3 base: no User.TrackVelocityRelativeToWater (relative tracking)')
    if has_waves and (version is None or version < PLUGIN_MIN_INTERFACE_VERSION):
        problems.append('interface version %s; this plugin needs %d or more (re-apply from the pass3 core)' % (version, PLUGIN_MIN_INTERFACE_VERSION))
    elif has_waves and version < INTERFACE_VERSION:
        log('WARNING interface version %d is below this toolchain\'s %d: the plugin runs it without the features the newer interface adds (3: User.Wave.ScaleDown, '
            'the wave grid stays at the pressure buffer\'s resolution; 4: the foam channel, User.WaveFoamTarget; 5: the closed-window <v> pin, '
            'User.Wave.VMeanRelax, and the in-step clamp of both mean-relax rates; 6: the aged foam, FoamAged, with User.Wave.FoamAgedDecay, '
            '.FoamTransfer, .FoamDiffusion, .FoamAgedDiffusion, the foam-only breaking slope .FoamSlope, the plugin\'s wash User.WaveWashTexture '
            'with .FoamWash, and the foam target as (F1, F2, v, 0); 7: the wind sea, User.WaveWindSeaTexture, added to B by Wave Export); '
            're-apply from the pass3 core to get them, or add the wind sea alone to an interface 6 core with the windsea step' % (version, INTERFACE_VERSION))
    if has_waves and version is not None and version >= 7 and 'User.WaveWindSeaTexture' not in summary:
        problems.append('interface version %d without User.WaveWindSeaTexture: the wind sea is missing (run the windsea step)' % version)
    log('VERIFY wind sea (7): User.WaveWindSeaTexture', 'User.WaveWindSeaTexture' in summary)
    if missing_stages:
        problems.append('stages missing: %s' % missing_stages)
    if has_waves and not has_params:
        problems.append('the emitter update stack has no Ninja_WaveParams module')
    if missing_modules:
        problems.append('Waves module assets missing: %s' % missing_modules)
    for label, protected_kind in protected_kinds.items():
        if protected_kind == 'vendor':
            problems.append('%s is the vendor file: no relative tracking' % label)
    if not carries:
        problems.append('the SURFACE function does not carry the waves (apply material)')
    elif not carries_foam:
        log('WARNING the SURFACE function carries the waves without the foam and the choppiness (an earlier build\'s material edit): '
            'the waves draw, the foam does not; re-run the material step to add them (it keeps the waves)')
    elif not carries_aged:
        log('WARNING the SURFACE function carries the waves and the foam without the aged foam (interface version 6: WaveFoamAgedColor, '
            'WaveFoamAgedStrength): the fresh foam draws, the aged foam (the foam target\'s G) does not; re-run the material step to add it '
            '(it keeps the rest)')
    if kind == 'unknown':
        log('WARNING the installed core is not in KNOWN_CORES / VERSIONS.md: a build nobody recorded, or a new one to add')
    for problem in problems:
        log('VERIFY FAIL', problem)
    failures.extend(problems)
    if not problems:
        log('VERIFY OK waves-applied on the pass3 base, interface version %s, core build %s' % (version, build))


# ---------------------------------------------------------------------------------------------------------------------------------
# inspect: no writes; the graphs the relative-tracking patch would have to edit (TRACKING.md)
# ---------------------------------------------------------------------------------------------------------------------------------
def inspect_graphs():
    out = ['FluidNinja Waves inspect, project ' + PROJECT_DIR]
    describe_node = getattr(DEV, 'describe_niagara_node', None)
    if describe_node is None:
        out.append('WARNING unreal.FNBDevLibrary.describe_niagara_node is missing (an older plugin build): nodes are named by class only')
    for path in [PAINTER_SOURCE, PAINTER_MODULE]:
        script = unreal.load_asset(path) if EAL.does_asset_exist(path) else None
        if not script:
            out.append('GRAPH %s MISSING' % path)
            continue
        file_path = package_file(path)
        digest = file_sha(file_path) if os.path.exists(file_path) else ''
        out.append('GRAPH %s sha8 %s known: %s' % (path, digest[:8], known(KNOWN_PROTECTED, digest)[0]))
        graph = DEV.get_script_graph(script)
        nodes = DEV.get_nodes(graph) if graph else []
        for index, node in enumerate(nodes):
            text = describe_node(node) if describe_node else node.get_class().get_name() + ' ' + node.get_name()
            out.append('  NODE #%d %s' % (index, text.replace('\n', '\n       ')))
            for line in DEV.describe_pins(node):
                out.append('    PIN ' + str(line))
    system = unreal.load_asset(CORE)
    if system:
        out.append('STAGES ' + CORE + '\n' + DEV.describe_simulation_stages(system, EMITTER))
        hlsl = DEV.get_niagara_gpu_hlsl(system, EMITTER)
        wanted = re.compile(r'FNB_|TrackVelocityRelativeToWater|Ninja_Painter\.RelativeTracking|Ninja_Painter\.CameraFacing|PainterSourceData_Emitter_Func\(')
        hits = [l.strip() for l in hlsl.splitlines() if wanted.search(l)]
        out.append('GPU HLSL relative-tracking lines (%d):' % len(hits))
        out.extend('  ' + h[:400] for h in hits)
    os.makedirs(os.path.dirname(INSPECT_REPORT), exist_ok=True)
    with open(INSPECT_REPORT, 'w', encoding='utf-8') as f:
        f.write('\n'.join(out) + '\n')
    log('INSPECT wrote', INSPECT_REPORT, '(%d lines)' % len(out))


# ---------------------------------------------------------------------------------------------------------------------------------
# tracking: vendor -> pass3. Not available; TRACKING.md has the gap.
# ---------------------------------------------------------------------------------------------------------------------------------
def tracking():
    fail('tracking is not available: the relative-tracking change (PainterSourceData, Ninja_Painter, NinjaLiveCore) is a Niagara graph '
         'edit this toolchain cannot reproduce yet; Tools/CorePatch/TRACKING.md has what it consists of, which helpers cover which part '
         'and what is missing. Use the pass3 files instead: unapply --from=FluidNinjaLive_2.0.0.56_RelativeTracking_UE5.8.zip')


# ---------------------------------------------------------------------------------------------------------------------------------
# unapply: a file step
# ---------------------------------------------------------------------------------------------------------------------------------
def source_bytes(source, filename):
    """The bytes of the file named filename inside source (a zip at any depth, or a folder searched recursively) and where it was found;
    (None, reason) when there is not exactly one. A source that is a file itself is taken as that file whatever its name (a backup such as
    DevHost_MF_NinjaOutput_BaseFunction_SURFACE.uasset): the hash check that follows judges its contents."""
    if not source:
        return None, 'no source given'
    if os.path.isfile(source) and source.lower().endswith('.zip'):
        with zipfile.ZipFile(source) as archive:
            names = [n for n in archive.namelist() if n.replace('\\', '/').rsplit('/', 1)[-1] == filename]
            if len(names) != 1:
                return None, 'found %d %s in %s' % (len(names), filename, source)
            return archive.read(names[0]), source + '!' + names[0]
    if os.path.isfile(source):
        with open(source, 'rb') as f:
            return f.read(), source
    if os.path.isdir(source):
        hits = [os.path.join(root, f) for root, _dirs, files in os.walk(source) for f in files if f == filename]
        if len(hits) != 1:
            return None, 'found %d %s under %s' % (len(hits), filename, source)
        with open(hits[0], 'rb') as f:
            return f.read(), hits[0]
    return None, 'no such zip, folder or file: ' + source


def unapply(options):
    src = options.get('from')
    if not src:
        fail('unapply needs --from=<zip or folder> with the pass3 NinjaLiveCore, PainterSourceData and Ninja_Painter '
             '(FluidNinjaLive_2.0.0.56_RelativeTracking_UE5.8.zip), and --surface=<zip, folder or .uasset> with the vendor SURFACE function '
             'unless --from has it')
    surface_src = options.get('surface') or src
    allow_unknown = 'allow-unknown' in options
    if unreal is not None and unreal.find_object(None, CORE + '.' + CORE.rsplit('/', 1)[1]) is not None:
        fail('NinjaLiveCore is already loaded in this process: run unapply on its own (or first), or under plain Python')
    log('UNAPPLY from', src, '| surface from', surface_src)
    staged = []
    for role, rel, table, wanted in UNAPPLY_FILES:
        filename = rel.rsplit('/', 1)[-1]
        data, where = source_bytes(surface_src if role == 'surface' else src, filename)
        if data is None:
            fail('%s: %s' % (filename, where))
        digest = hashlib.sha256(data).hexdigest()
        kind, desc = known(table, digest)
        log('UNAPPLY source', filename, 'sha256', digest, '| known:', kind, desc, '| from', where)
        if kind != wanted:
            if allow_unknown:
                log('WARNING %s is %s, not the %s file; copied anyway (--allow-unknown)' % (filename, kind, wanted))
            else:
                fail('%s is %s, not the %s file this step puts back (--allow-unknown copies it anyway)' % (filename, kind, wanted))
        staged.append((rel, data))
    live = os.path.join(CONTENT_DIR, 'FluidNinjaLive')
    if not os.path.isdir(live):
        fail('no FluidNinja LIVE content at ' + live)
    backup = os.path.join(PROJECT_DIR, 'Saved', 'FluidNinjaWaves_unapply_backup_' + datetime.datetime.now().strftime('%Y%m%d_%H%M%S'))
    for rel, data in staged:
        dst = os.path.join(live, rel.replace('/', os.sep))
        if os.path.exists(dst):
            kept = os.path.join(backup, rel.replace('/', os.sep))
            os.makedirs(os.path.dirname(kept), exist_ok=True)
            shutil.copy2(dst, kept)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            f.write(data)
        log('UNAPPLY wrote', dst, 'sha8', hashlib.sha256(data).hexdigest()[:8])
    waves_dir = os.path.join(live, 'Core', 'Modules', 'Waves')
    if os.path.isdir(waves_dir):
        shutil.copytree(waves_dir, os.path.join(backup, 'Core', 'Modules', 'Waves'), dirs_exist_ok=True)
        count = sum(len(files) for _root, _dirs, files in os.walk(waves_dir))
        shutil.rmtree(waves_dir)
        log('UNAPPLY deleted', waves_dir, '(%d files)' % count)
    else:
        log('UNAPPLY no', waves_dir, 'to delete')
    log('UNAPPLY backup of the replaced files:', backup)


# ---------------------------------------------------------------------------------------------------------------------------------
def parse_args():
    """Steps (bare words) and options (--name or --name=value) from the command line, plus FNB_WAVES_STEPS / FNB_WAVES_ARGS."""
    args = list(sys.argv[1:])
    if os.environ.get('FNB_WAVES_ARGS'):
        args += os.environ['FNB_WAVES_ARGS'].split()
    steps = [a for a in args if a and not a.startswith('-')]
    if not steps and os.environ.get('FNB_WAVES_STEPS'):
        steps = os.environ['FNB_WAVES_STEPS'].replace(',', ' ').split()
    options = {}
    for a in args:
        if a.startswith('--'):
            name, _sep, value = a[2:].partition('=')
            options[name] = value
    return steps or ['core', 'material'], options


def resolve_project(options):
    if options.get('project'):
        return os.path.abspath(options['project'])
    if unreal is not None:
        return unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
    if os.environ.get('FNB_PROJECT_DIR'):
        return os.path.abspath(os.environ['FNB_PROJECT_DIR'])
    raise SystemExit('outside the editor the project is needed: --project=<dir> (or FNB_PROJECT_DIR)')


def core_tag_lines():
    """The installed core's identity, the last lines before RESULT: the full sha256, the sha8 and the release tag it names."""
    path = package_file(CORE)
    if not os.path.exists(path):
        log('CORE', CORE, 'MISSING')
        return
    digest = file_sha(path)
    log('CORE', CORE, 'sha256', digest, '| known:', known(KNOWN_CORES, digest)[0])
    log('CORE sha8', digest[:8], 'tag', 'core-' + digest[:8])


ORDER = ['unapply', 'tracking', 'core', 'windsea', 'material', 'describe', 'check', 'inspect', 'verify']


def main():
    global PROJECT_DIR, CONTENT_DIR, REPORT, INSPECT_REPORT
    steps, options = parse_args()
    PROJECT_DIR = resolve_project(options)
    CONTENT_DIR = os.path.join(PROJECT_DIR, 'Content')
    REPORT = os.path.join(PROJECT_DIR, 'Saved', 'FluidNinjaWaves_apply.txt')
    INSPECT_REPORT = os.path.join(PROJECT_DIR, 'Saved', 'FluidNinjaWaves_inspect.txt')
    log('FluidNinja Waves apply, steps', steps, 'project', PROJECT_DIR)
    ok = False
    try:
        unknown = [s for s in steps if s not in ORDER]
        if unknown:
            fail('unknown steps %s (known: %s)' % (unknown, ORDER))
        for step in [s for s in ORDER if s in steps]:
            if step == 'unapply':
                unapply(options)
            elif step == 'tracking':
                tracking()
            elif step == 'core':
                need_editor(step)
                apply_core()
            elif step == 'windsea':
                need_editor(step)
                apply_windsea()
            elif step == 'material':
                need_editor(step)
                apply_material()
            elif step == 'describe':
                need_editor(step)
                describe()
            elif step == 'check':
                need_editor(step)
                system = unreal.load_asset(CORE)
                summary = DEV.describe_niagara(system, '', '')
                log('core has waves:', core_has_waves(system, summary), '| base', core_base(summary),
                    '| interface version', user_int(summary, 'User.Wave.InterfaceVersion'), '| core build', user_int(summary, 'User.Wave.CoreBuild'),
                    '| stages', stage_names(system))
            elif step == 'inspect':
                need_editor(step)
                inspect_graphs()
            elif step == 'verify':
                need_editor(step)
                verify()
        ok = not failures
    except Exception:
        log('ERROR ' + traceback.format_exc().strip())
    finally:
        core_tag_lines()
        log('RESULT', 'PASS' if ok else 'FAIL')
        flush()
    if not ok:
        # A SystemExit with a non-zero code reaches the python script commandlet as a script error (a zero one is swallowed by the
        # engine), so UnrealEditor-Cmd exits non-zero; under plain Python it is the exit code itself.
        sys.exit(1)


if __name__ == '__main__':
    main()
