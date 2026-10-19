<#
Packages a project's modified FluidNinja core as a zip named by the core's sha8 (Tools/CorePatch/VERSIONS.md):
  FluidNinjaLive_<Version>_Waves_RelativeTracking_<Engine>_core-<sha8>.zip
with Content/FluidNinjaLive/NinjaLiveCore.uasset, Core/Functions/PainterSourceData.uasset, Core/Modules/Ninja_Painter.uasset,
Core/Modules/Waves/* and OutputMaterials/Base/MF_NinjaOutput_BaseFunction_SURFACE.uasset, plus a README.txt (what it is, install, undo,
the plugin tag core-<sha8>, every file's SHA-256). The toolchain itself is not in the zip any more: it is the plugin's Tools/CorePatch.
Run apply_fluidninja_waves.py verify on the project first (RESULT PASS).

  .\package_core.ps1 -Project "C:\...\MyHostProject" -OutDir "C:\...\CoreZips" [-Version 2.0.0.56] [-Engine UE5.8]
#>
param(
    [Parameter(Mandatory = $true)][string]$Project,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$Version = '2.0.0.56',
    [string]$Engine = 'UE5.8'
)
$ErrorActionPreference = 'Stop'
$live = Join-Path $Project 'Content\FluidNinjaLive'
if (-not (Test-Path (Join-Path $live 'NinjaLiveCore.uasset'))) { throw "no FluidNinja LIVE content at $live" }
$files = @('NinjaLiveCore.uasset', 'Core\Functions\PainterSourceData.uasset', 'Core\Modules\Ninja_Painter.uasset',
           'OutputMaterials\Base\MF_NinjaOutput_BaseFunction_SURFACE.uasset')
$waves = Join-Path $live 'Core\Modules\Waves'
if (-not (Test-Path $waves)) { throw "no $waves (the waves are not applied; run apply_fluidninja_waves.py core material verify first)" }
$files += Get-ChildItem $waves -File | ForEach-Object { 'Core\Modules\Waves\' + $_.Name }
foreach ($f in $files) { if (-not (Test-Path (Join-Path $live $f))) { throw "missing $f" } }

$sha = (Get-FileHash -Algorithm SHA256 (Join-Path $live 'NinjaLiveCore.uasset')).Hash.ToLower()
$sha8 = $sha.Substring(0, 8)
$name = "FluidNinjaLive_${Version}_Waves_RelativeTracking_${Engine}_core-$sha8"
$stage = Join-Path ([IO.Path]::GetTempPath()) $name
if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
foreach ($f in $files) {
    $dst = Join-Path $stage "Content\FluidNinjaLive\$f"
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    Copy-Item (Join-Path $live $f) $dst
}
$hashLines = $files | ForEach-Object { '  {0}  Content/FluidNinjaLive/{1}' -f (Get-FileHash -Algorithm SHA256 (Join-Path $live $_)).Hash.ToLower(), ($_ -replace '\\', '/') }
$date = Get-Date -Format 'yyyy-MM-dd'
$readme = @"
FluidNinja LIVE $Version - FluidNinja Waves and relative tracking, for FluidNinja Buoyancy (core $sha8, $date)

What this is
  The FluidNinja core changes the FluidNinja Buoyancy plugin uses, in one set: relative tracking (the user parameter
  TrackVelocityRelativeToWater; PainterSourceData and Ninja_Painter changed) and FluidNinja Waves (an opt-in linear wave layer inside
  NinjaLiveCore: user parameters WaveSimEnabled, WaveSourceTexture, WaveAuxTarget and Wave.*, the Core/Modules/Waves modules, and the
  SURFACE material function drawing the waves). With nothing driving it FluidNinja behaves exactly as before: the wave stages are off,
  the surface function adds 0, nothing is logged.
  This core is named by its hash: sha8 $sha8. The FluidNinja Buoyancy build that requires it is tagged core-$sha8 in the plugin's
  repository; its Docs/FluidNinjaWaves.md describes the settings and the interface, Tools/CorePatch/VERSIONS.md every core.

Requirements
  FluidNinja LIVE $Version in the project, Unreal Engine $Engine (these files do not load in older engines), and the FluidNinja
  Buoyancy plugin at tag core-$sha8 or later to drive the waves.

Install
  1. Close Unreal Editor.
  2. Back up these files from your project, if it has them:
       Content/FluidNinjaLive/NinjaLiveCore.uasset
       Content/FluidNinjaLive/Core/Functions/PainterSourceData.uasset
       Content/FluidNinjaLive/Core/Modules/Ninja_Painter.uasset
       Content/FluidNinjaLive/OutputMaterials/Base/MF_NinjaOutput_BaseFunction_SURFACE.uasset
  3. Copy the Content folder from this zip into your project folder, overwriting those files. This also adds the folder
     Content/FluidNinjaLive/Core/Modules/Waves.
  4. Open the project. The first open recompiles NinjaLiveCore's shaders and the FluidNinja surface materials.
  5. Check NinjaLiveCore's User Parameters: WaveSimEnabled (false), WaveSourceTexture, the Wave.* parameters, WaveAuxTarget
     (RT_WaveAux_Unbound), TrackVelocityRelativeToWater (0), Wave.InterfaceVersion and Wave.CoreBuild. Or run the plugin's
     Tools/CorePatch/apply_fluidninja_waves.py verify (RESULT PASS, CORE sha8 $sha8).

To undo
  Put the backed-up files back and delete Content/FluidNinjaLive/Core/Modules/Waves (the plugin's
  apply_fluidninja_waves.py unapply does the same from the relative-tracking files).

Re-applying (another FluidNinja version, or after a FluidNinja update)
  The toolchain is the plugin's Tools/CorePatch (apply_fluidninja_waves.py, its HLSL, and the plugin's editor module with the helpers);
  its README.md has the steps. The relative-tracking change is still made by hand (Docs/RelativeTrackingProposal.md, The code).

SHA-256
$($hashLines -join "`n")
"@
[IO.File]::WriteAllText((Join-Path $stage 'README.txt'), ($readme -replace "`r`n", "`n"), [Text.UTF8Encoding]::new($false))

New-Item -ItemType Directory -Force $OutDir | Out-Null
$zip = Join-Path $OutDir "$name.zip"
if (Test-Path $zip) { Remove-Item -LiteralPath $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Remove-Item -LiteralPath $stage -Recurse -Force
Get-Item $zip | Select-Object FullName, Length
"core sha256 $sha"
"core sha8 $sha8  tag core-$sha8"
