# The `tracking` step: scripting the relative-tracking patch

Status (2026-10-17): **not available**. `apply_fluidninja_waves.py tracking` reports that and fails. Relative tracking still comes from the hand-made pass3 files (core `3bee517c`, PainterSourceData `9986466c`, Ninja_Painter `9f2c488c`; `unapply --from=FluidNinjaLive_2.0.0.56_RelativeTracking_UE5.8.zip` puts them back). This page says exactly what the hand-made change is, which part the toolchain's helpers already cover, what is missing, and what could not be verified without an editor.

## What the hand-made change consists of

Read from the pass3 assets and the pass3 core's compiled GPU HLSL with the editor closed (a strings-level comparison of the vendor files and the pass3 files; the node topology is to be confirmed with the `inspect` step, below). It matches `Docs/RelativeTrackingProposal.md`, "The code".

### 1. `Core/Functions/PainterSourceData` (a Niagara function script)

The compiled function gained two inputs at the end of its signature:

```
void Ninja_Painter_PainterSourceData_Emitter_Func(int In_IterationCount, float2 In_UnitCoords, int In_SelectivelyKillSources,
    float In_ArrayBrushVelocity, float In_ParticleBrushVelocity, float In_RelativeTracking, bool In_CameraFacing, inout FSimulationContext Context)
```

so the function graph has two new **function input nodes** (`UNiagaraNodeInput`, usage Parameter): `RelativeTracking` (float) and `CameraFacing` (bool).

New **parameter-map reads** (map-get outputs; the default-pin tooltips are in the saved asset): `Emitter.CompositeGrid` (Grid2DCollection), `Emitter.GridCellSizeInUnitSpace` (vec2), `Emitter.SimSpeedCoefficient` (vec2), `System.WorldGridExtents` (vec2), `System.WorldOffsetDelta` (vec3), `Engine.System.NumTimeSteps` (int), `Engine.InverseDeltaTime` (float).

Three new nodes, wired into the ArrayReader branch:

| Node | Inputs | Output | Code |
|---|---|---|---|
| Custom HLSL `FNB_WaterUV` | `Pos` float3 (the ArrayReader's `UnitPosition`: a Particle Read *Get Vector By Index* call on `Emitter.ArrayReader`, attribute `UnitPosition`; the compiled name `…UnitPositionOutput_Value2` says it is a second call of that read), `WorldOffsetDelta` float3 | `UV` float2 | `UV = Pos.xy + WorldOffsetDelta.xy;` |
| Data-interface call `SamplePreviousGridVector3Value` on `Emitter.CompositeGrid`, function specifier `Attribute = Composite` | `UV` from `FNB_WaterUV` | `Value` float3 (the water's velocity at the pivot, cells per tick) | the engine's `NiagaraDataInterfaceGridColl2D_SampleVector3` |
| Custom HLSL `FNB_RelativeVelocity` | `UV` float3 (the ArrayReader's `UnitVelocity`: *Get Vector By Index*, attribute `UnitVelocity`), `Water` float3, `IsScreen` bool (*Get Bool By Index*, attribute `IsScreenPosition`), `InverseDeltaTime` float, `GridCellSizeInUnitSpace` float2, `SimSpeedCoefficient` float2, `WorldGridExtents` float2, `RelativeTracking` float, `CameraFacing` bool, `NumTimeSteps` int | `Velocity` float3 | the proposal's code (`Steps`, `WaterUnit`, `SpeedCmS`, `Gate`, `lerp`) |

The multiply that scales the brush velocity, `Result = f2 * In_ArrayBrushVelocity * <velocity>` (a `NiagaraNodeOp` Multiply), takes `FNB_RelativeVelocity.Velocity` where it took `UnitVelocity`: one existing input link re-wired.

### 2. `Core/Modules/Ninja_Painter` (the module that calls the function)

Two new module inputs on its parameter-map get: `Module.RelativeTracking` (float, default 0) and `Module.CameraFacing` (bool, default false), each linked to the matching new input pin of the `PainterSourceData` function call node. (That node shows the new pins only after it is refreshed from the changed function.)

### 3. `NinjaLiveCore`

- A user parameter `User.TrackVelocityRelativeToWater` (float, default 0): this is how the script tells a pass3 core from a stock one.
- In the **TrackPoint Painter** simulation stage, module `Ninja_Painter`: input `RelativeTracking` linked to `User.TrackVelocityRelativeToWater`, input `CameraFacing` linked to `System.TransformMatrices2D.CameraFacing`. The compiled stage reads
  `Ninja_Painter.RelativeTracking = User.TrackVelocityRelativeToWater;` and `Ninja_Painter.CameraFacing = System.TransformMatrices2D.CameraFacing;`.
- The pass3 core also carries a stale rapid-iteration constant `Constants.PainterAndSim.Ninja_Painter.RelativeGain` from the earlier "pass2" prototype (a gain input that was removed again). Nothing reads it; a scripted patch would not make it.

## What the helpers cover

| Edit | Helper (`unreal.FNBDevLibrary`) | State |
|---|---|---|
| Map-get outputs in PainterSourceData and Ninja_Painter | `AddMapGetOutput` | exists, used by the `core` step |
| The two custom HLSL nodes and their pins | `AddCustomHlslNode`, `AddCustomHlslPins` | exist, used by the `core` step |
| Links, including re-wiring the multiply's input that already has a link | `LinkPins` | exists; `UEdGraphSchema_Niagara::TryCreateConnection` answers `CONNECT_RESPONSE_BREAK_OTHERS` for an input pin, so the old link is broken |
| The user parameter | `AddUserVariable` | exists |
| Linking `Ninja_Painter`'s inputs inside the TrackPoint Painter stage | `LinkNiagaraModuleInput` (walks the graph: `Script = "Stage:TrackPoint Painter"`) | **added with this toolchain move, not yet built or run**: `SetNiagaraInputLinked` could not reach a simulation stage (the external-edit API addresses stacks by script usage only) |
| Refreshing the function call node in Ninja_Painter after the function changed | `RefreshNiagaraNode` | **added, not yet built or run** |
| Finding the vendor nodes an edit hangs on (the Get Vector By Index calls, the multiply) by what they are, not by their object names | `DescribeNiagaraNode` + `DescribePins`, through the `inspect` step | **added, not yet built or run** |
| A function input node (`RelativeTracking`, `CameraFacing` in PainterSourceData) | none | **missing**. The editor's "Add Input" action is `FNiagaraEditorUtilities::InitializeParameterInputNode`, which is not exported from NiagaraEditor; a helper has to do what it does itself: a `UNiagaraNodeInput` with `Input` = (type, name), `Usage = Parameter`, `CallSortPriority` after the existing inputs, `ExposureOptions.bExposed`, added to the graph like `AddCustomHlslNode` adds its node, and the graph's script variable for it (`UNiagaraGraph::AddParameter`) so the function's signature and the calling module's inputs update. |
| A data-interface function call node (`SamplePreviousGridVector3Value` on `Emitter.CompositeGrid` with `Attribute = Composite`; the second Particle Read `Get Vector By Index` on `Emitter.ArrayReader`) | none | **missing**. A `UNiagaraNodeFunctionCall` with no `FunctionScript` and `Signature` taken from `UNiagaraDataInterface::GetFunctionSignatures()` of the data interface's class (the one named `SamplePreviousGridVector3Value`), `Signature.FunctionSpecifiers = {Attribute: Composite}`, placed like the graph palette's data-interface actions do (`UEdGraphSchema_Niagara::GetGraphContextActions`), its data-interface input pin linked to the map-get's `Emitter.CompositeGrid` output. |

The three new helpers compile against the UE 5.8 headers on paper (every call is a public member or an exported function: `FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin`, `SetLinkedParameterValueForFunctionInput`, `UNiagaraNode::RefreshFromExternalChanges`), but no build or run of them has happened: they were written while the editor and the compiler were off limits.

## What `tracking` would do, once the two helpers exist

1. Check the installed files are the vendor ones (`KNOWN_CORES` / `KNOWN_PROTECTED`: `7b9ed72f`, `79a2eab0`, `eb48a5d0`), else stop (an `unapply` to the vendor files would need a vendor zip; none is known to this toolchain yet).
2. PainterSourceData: the map-get outputs; the two input nodes; the three nodes above (the ArrayReader reads found by signature name + specifier through `DescribeNiagaraNode`, the multiply by its `op=Multiply` and its link from the `UnitVelocity` read); the links; the re-wire.
3. Ninja_Painter: the two map-get outputs; `RefreshNiagaraNode` on the PainterSourceData call; the two links.
4. NinjaLiveCore: `AddUserVariable("TrackVelocityRelativeToWater", "float", "0")`; `LinkNiagaraModuleInput(system, "PainterAndSim", "Stage:TrackPoint Painter", "Ninja_Painter", "RelativeTracking", "User.TrackVelocityRelativeToWater")` and the same for `CameraFacing` → `System.TransformMatrices2D.CameraFacing`; `CompileNiagaraSystem`; save the three assets.
5. Verify, semantically: `core_base` says pass3; the GPU HLSL of PainterAndSim contains an `FNB_RelativeVelocity…_Func(` call and `Ninja_Painter.RelativeTracking = …User.TrackVelocityRelativeToWater`; the GPU constants added are exactly `User_TrackVelocityRelativeToWater` (and its `PREV_`); the protected hashes change (expected: PainterSourceData and Ninja_Painter are the patch). Then the proposal's river test numbers (13.7 m, 1.9 m/s at fixed 60 fps with density off).

**A byte-identical result is not possible.** A re-made asset carries fresh node and pin GUIDs, change ids and compile ids, and a different save order, so the result can never hash to `3bee517c` / `9986466c` / `9f2c488c`. The verification above (structure, compiled HLSL, measurements) is what "the same change" can mean; the hashes of a scripted pass3 would be new rows in VERSIONS.md.

## What could not be verified without the editor

- The exact node topology of the vendor `PainterSourceData` graph: which nodes carry `UnitVelocity`, `UnitPosition` and `IsScreenPosition` (the compiled names say Particle Read calls on `Emitter.ArrayReader`), and which `Multiply` is the one to re-wire. The `inspect` step is there to answer this: run it on the pass3 assets (a host project after `unapply`) and on the vendor assets, and diff the two `Saved/FluidNinjaWaves_inspect.txt`.
- Whether the function's new inputs were made as input nodes (what the compiled signature says) or read from the map as `Module.*` (what the saved strings also contain, from the compile metadata).
- That the three added helpers build and behave as described.

## Next steps

1. Build the plugin's editor module; run `inspect` in a host project (pass3 + waves installed) and read `Saved/FluidNinjaWaves_inspect.txt`.
2. Write the two missing helpers (`AddFunctionInputNode`, `AddDataInterfaceFunctionNode`) in `FNBDevLibrary`, after the editor's own actions.
3. Implement `tracking` as above, run it on a vendor copy, verify, measure.
4. Record the new hashes in VERSIONS.md and the script's tables; the zip for a FluidNinja update then needs no hand-made files at all.
