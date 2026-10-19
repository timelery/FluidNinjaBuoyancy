# FluidNinja Buoyancy performance

What the plugin costs, measured. Every number here comes from a CSV profiler run (`-csvGpuStats`, `CsvProfile Frames=N`) on a fixed frame
step (`-UseFixedTimeStep -FPS=60` or `30`, `-RenderOffscreen`), p50 / p95 in milliseconds, after a 5 s skip. Hardware: one machine, an RTX
5090 desktop (16 cores / 32 threads). The demo pool runs sync physics; the project levels run async physics at 120 Hz.

**Read the dates.** The tables below were measured on 2026-10-18 at the Batch B3 / Phase 3 state, with the then-default quality tier, which
is today's **Cine** (`sg.WaterQuality 4`): today's default **Epic** (3) is the calibrated set and runs lighter (a 1024 px capture instead
of 2048, a smaller own-field reach). The later changes (core interface 5, the plunging source, the mean-relax split) add a few operations
per wave cell and per sinking body; they were verified for correctness, not re-profiled. A full re-run of the matrix on the final code is
listed under [Not measured](#not-measured).

## How to measure

```
UnrealEditor.exe <project> <map> -game -windowed -ResX=1280 -ResY=720 -RenderOffscreen -UseFixedTimeStep -FPS=60 -csvGpuStats
    -ExecCmds="fnb.Dev.SpawnCrates 50, CsvProfile ExitOnCompletion, CsvProfile Frames=7200"
```

- `-ExecCmds` separates commands with commas (a `;` list arrives as one command).
- Do not put `-csvCaptureFrames` on the command line: it stops a `-game` launch from writing its log. Start the capture from the console.
- Gate every run on an idle CPU. A run taken while other work loaded the CPU (74 %) read the game thread at 27 ms for 150 bodies against
  10 ms idle; it was thrown away.
- The plugin's own columns: `FNB/TickMs`, `FNB/PatchMs`, `FNB/PhysicsMs`, `FNB/PreTickMs`, `FNB/GatherPoints`, `FNB/SelfFields`,
  `FNB/SelfCells`, `FNB/ReadbackAgeFrames`, `FNB/BodiesLod0..2`, `FNB/WaveSimsActive`, and the GPU passes `GPU/FNB_*`
  (Raster, Assemble, SelfPrepare, SelfMip, SelfStep, SelfExport, Splat, Gather). `GPU/NiagaraGPUSimulation` holds all of Niagara's GPU
  simulation, FluidNinja and the wave stages together: the waves' Niagara cost is that column with waves on minus waves off.

## Demo pool (FNB_DemoPool: an 800² pool and a 1024² river)

### 60 fps

| bodies | waves | frame | game | render | GPU | FNB tick | patches | physics | pre-tick | awake | gather pts |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | on | 5.77 / 6.96 | 3.75 / 5.14 | 5.77 / 6.85 | 4.88 / 5.33 | 1.54 / 2.60 | 1.13 / 2.05 | 0.15 / 0.21 | 0.04 / 0.05 | 15 | 118,495 |
| 1 | off | 4.78 / 5.59 | 3.25 / 4.17 | 4.78 / 5.57 | 3.45 / 3.90 | 1.22 / 1.98 | 1.00 / 1.71 | 0.12 / 0.16 | 0.03 / 0.04 | 15 | 118,304 |
| 14 | on | 6.46 / 7.26 | 4.63 / 6.47 | 6.45 / 7.18 | 5.57 / 6.18 | 2.02 / 3.72 | 1.50 / 1.87 | 0.19 / 0.27 | 0.07 / 0.08 | 28 | 224,068 |
| 14 | off | 6.23 / 7.29 | 4.44 / 5.54 | 6.22 / 7.34 | 3.53 / 4.01 | 1.72 / 2.32 | 1.42 / 2.01 | 0.19 / 0.26 | 0.06 / 0.07 | 28 | 223,754 |
| 50 | on | 8.49 / 10.43 | 6.37 / 10.22 | 8.44 / 9.67 | 7.70 / 8.65 | 2.99 / 6.54 | 2.24 / 4.31 | 0.30 / 0.46 | 0.15 / 0.20 | 64 | 514,828 |
| 50 | off | 6.75 / 8.23 | 5.63 / 7.92 | 6.78 / 7.96 | 3.68 / 4.08 | 2.43 / 4.34 | 2.10 / 3.94 | 0.27 / 0.46 | 0.11 / 0.16 | 64 | 512,986 |
| 150 | on | 10.36 / 13.29 | 10.33 / 13.28 | 9.70 / 10.93 | 8.84 / 9.48 | 4.67 / 7.09 | 3.18 / 3.52 | 0.56 / 0.70 | 0.38 / 0.48 | 164 | 1,325,926 |
| 150 | off | 8.83 / 11.33 | 8.09 / 10.88 | 8.75 / 10.98 | 4.09 / 4.50 | 3.61 / 5.88 | 3.13 / 5.39 | 0.52 / 0.80 | 0.25 / 0.54 | 164 | 1,323,798 |

### 30 fps

| bodies | waves | frame | game | render | GPU | FNB tick | patches | physics | pre-tick | awake | gather pts |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | on | 7.19 / 8.60 | 4.14 / 6.06 | 7.23 / 8.62 | 6.42 / 7.01 | 1.90 / 3.48 | 1.53 / 2.99 | 0.15 / 0.20 | 0.03 / 0.05 | 15 | 118,628 |
| 1 | off | 5.84 / 7.08 | 3.72 / 5.22 | 5.83 / 7.03 | 4.20 / 4.63 | 1.51 / 2.77 | 1.31 / 2.53 | 0.14 / 0.20 | 0.02 / 0.05 | 15 | 118,528 |
| 14 | on | 8.28 / 9.00 | 5.26 / 7.02 | 8.28 / 8.84 | 7.60 / 8.04 | 1.97 / 3.55 | 1.44 / 1.79 | 0.18 / 0.22 | 0.07 / 0.09 | 28 | 224,106 |
| 14 | off | 6.11 / 6.78 | 4.66 / 5.48 | 6.10 / 6.80 | 4.24 / 4.62 | 1.66 / 2.04 | 1.39 / 1.61 | 0.16 / 0.22 | 0.05 / 0.07 | 28 | 223,559 |
| 50 | on | 12.07 / 13.13 | 6.87 / 9.38 | 12.06 / 13.13 | 11.33 / 12.28 | 2.97 / 5.32 | 2.23 / 4.07 | 0.28 / 0.35 | 0.14 / 0.19 | 64 | 516,138 |
| 50 | off | 6.70 / 8.61 | 6.24 / 8.52 | 6.76 / 8.06 | 4.50 / 4.91 | 2.51 / 4.28 | 2.15 / 3.85 | 0.27 / 0.37 | 0.11 / 0.18 | 64 | 513,613 |
| 150 | on | 13.74 / 14.72 | 9.26 / 12.72 | 13.75 / 14.67 | 12.99 / 13.73 | 4.02 / 7.04 | 2.85 / 5.49 | 0.50 / 0.62 | 0.37 / 0.49 | 164 | 1,326,229 |
| 150 | off | 9.92 / 12.96 | 9.85 / 12.38 | 9.61 / 12.53 | 4.93 / 5.40 | 3.79 / 4.99 | 3.11 / 3.93 | 0.55 / 0.75 | 0.28 / 0.47 | 164 | 1,322,850 |

What it says:
- **The waves are a GPU cost.** At 60 fps they add 1.4 ms of GPU for 1 body, 2.0 ms for 14, 4.0 ms for 50 and 4.8 ms for 150. At 30 fps
  they add 2.2 to 8.1 ms, because the core steps at a fixed 60 ticks a second: a 30 fps frame carries two ticks.
- **The bodies are a game-thread cost.** The plugin's tick grows from 1.2 ms (1 body) to 3.6 ms (150) with waves off; physics itself stays
  under 0.6 ms at p50.
- At 150 bodies and 60 fps the frame is game-thread bound (10.3 ms game against 8.8 ms GPU).

## Project levels (UE 5.8.3, async physics at 120 Hz)

| level | run | frame | game | render | GPU | FNB tick | patches | physics | awake | gather pts | LOD0 / 1 / 2 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Open-world level | 60 fps, waves on | 13.72 / 17.89 | 13.37 / 17.51 | 12.46 / 15.66 | 8.40 / 9.13 | 1.37 / 2.05 | 0.54 / 1.11 | 0.29 / 0.38 | 4 | 42,427 | 4 / 0 / 0 |
| Open-world level | 60 fps, waves off | 14.20 / 24.88 | 10.07 / 22.39 | 13.57 / 20.53 | 8.95 / 12.18 | 1.21 / 2.36 | 0.60 / 1.52 | 0.29 / 0.48 | 4 | 43,204 | 4 / 0 / 0 |
| Open-world level | 30 fps, waves on | 16.77 / 28.30 | 11.17 / 22.62 | 16.33 / 22.10 | 12.26 / 14.77 | 1.58 / 2.95 | 0.91 / 2.00 | 0.57 / 0.90 | 4 | 42,155 | 4 / 0 / 0 |
| Open-world level | 30 fps, waves off | 15.73 / 21.60 | 10.24 / 19.41 | 15.36 / 19.15 | 11.94 / 14.15 | 1.27 / 2.12 | 0.86 / 1.52 | 0.51 / 0.69 | 4 | 43,593 | 4 / 0 / 0 |
| Open-world level | 60 fps, Low (`sg.WaterQuality 0`) | 12.33 / 15.67 | 5.90 / 13.60 | 12.10 / 15.09 | 9.14 / 11.87 | 0.19 / 0.67 | 0.02 / 0.06 | 0.13 / 0.19 | 4 | 218 | 1 / 3 / 0 |
| Open-world level | 60 fps, Epic (`sg.WaterQuality 3`) | 14.49 / 21.21 | 8.74 / 18.35 | 14.20 / 19.61 | 10.28 / 13.04 | 1.16 / 2.39 | 0.55 / 1.29 | 0.30 / 0.51 | 4 | 43,018 | 4 / 0 / 0 |
| 11-tank test level | 60 fps, waves on | 9.11 / 14.90 | 7.06 / 13.37 | 9.10 / 13.76 | 5.12 / 7.20 | 2.29 / 4.18 | 1.44 / 2.94 | 0.53 / 0.82 | 28 | 119,371 | 17 / 19 / 0 |
| 11-tank test level | 60 fps, waves off | 8.00 / 16.48 | 6.06 / 13.06 | 7.86 / 15.41 | 3.74 / 5.72 | 1.87 / 3.68 | 1.30 / 2.83 | 0.49 / 0.85 | 28 | 118,286 | 17 / 19 / 0 |
| 11-tank test level | 30 fps, waves on | 10.17 / 13.46 | 9.04 / 12.88 | 9.65 / 12.04 | 6.48 / 9.50 | 2.50 / 4.41 | 1.47 / 3.14 | 1.02 / 1.32 | 28 | 119,206 | 17 / 19 / 0 |
| 11-tank test level | 30 fps, waves off | 7.88 / 11.23 | 7.08 / 10.76 | 7.72 / 9.68 | 4.50 / 6.24 | 1.95 / 3.45 | 1.35 / 2.77 | 0.90 / 1.14 | 28 | 118,211 | 17 / 19 / 0 |
| 11-tank test level | 60 fps, Low | 6.23 / 7.81 | 4.88 / 6.88 | 6.19 / 7.72 | 3.19 / 3.52 | 0.56 / 0.98 | 0.04 / 0.06 | 0.29 / 0.38 | 26 | 345 | 2 / 11 / 23 |
| 11-tank test level | 60 fps, Epic | 7.46 / 9.01 | 6.25 / 8.79 | 7.46 / 8.53 | 4.61 / 4.95 | 2.16 / 2.76 | 1.38 / 1.73 | 0.50 / 0.63 | 28 | 119,415 | 17 / 19 / 0 |

- **The open-world level is not bound by FluidNinja Buoyancy:** its 13-14 ms frame is the level; the plugin's tick is 1.2-1.6 ms and the waves add
  well under a millisecond of GPU there (4 floaters).
- **The 11-tank test level** (28 bodies over many simulations): the waves add 1.4 ms of GPU at 60 fps and 2.0 ms at 30.
- **The Low tier** cuts the plugin's tick to 0.2-0.6 ms and the gather from ~43,000-119,000 points to a few hundred.

## The readback, the scale-down and the Low tier

**The readback** (Batch B1, `fnb.Gather.FastReadback`): the water the physics reads is 2.39 frames old on average (61 % of frames 2 old,
39 % 3 old) against 3.0 frames when the gather was consumed after the physics step: 0.6 of a frame younger. 14 bodies, 60 fps.

**The wave grid's scale-down** (`fnb.Waves.ScaleDown`, the core at 1/S² of its cells), accuracy against the full grid:

| test | S = 1 | S = 2 | S = 4 |
|---|---|---|---|
| ripple-dispersion, the core's own discrete dispersion, normalised RMS r 1 / 2 / 4 m | 1.8 / 1.8 / 2.6 % | about 4.1 / 3.8 / 3.6 % (port) | about 3.8 / 2.4 / 2.9 % (port) |
| ripple-kelvin, measured wake against the exact linear wake | 1.01× (RMS 12 %) | 0.97× (RMS 10-15 %) | not run |
| ripple-weight, a 20 kg crate's static head against m / ρ | within 2 % | +9.6 % at rest (S = 2) | within 1 % |

The scale-down keeps wave arrival and dispersion within a few per cent; its cost falls as 1 / S² (see the tier table in the user guide).

**The Low tier** (`sg.WaterQuality 0`): every bench test passes (82 / 82). It allows one wave simulation (`fnb.Waves.MaxActiveSims 1`),
and since 2026-10-18 that is the simulation whose nearest floating body or swimmer in the water is nearest the camera; before, a simulation
that follows the player always won, and a pool full of floaters 15 m away never ran waves.

## Not measured

- **The full matrix on the final code.** The demo matrix above is the Batch B3 / Phase 3 state at today's Cine tier; it was not re-run
  on core interface 5 and the default Epic tier (a re-run on the final code was started and discarded because the CPU was loaded). Re-run it
  with an idle CPU before quoting costs for the final build.
- **Sync against async physics** on the same level (the demo runs sync, the project levels async; no paired run).
- **Per-stage Niagara GPU times** (Wave Prepare, mips, Step, Aux, Export): the CSV carries Niagara's GPU total only; the per-stage split
  needs Unreal Insights with the Niagara channel (`fx.Niagara.GpuProfiling.Enabled 1`).
- **A second, mid-range GPU.**
- **120 s captures and the max column:** these runs captured 25-35 s at p50 / p95.
- **The bench's own frame rate.** Uncapped bench runs of the Phase 4 build held about 49 fps where earlier builds held 60; whether that
  was the build or other load on the machine was not settled. The bench verdicts are taken on a fixed frame step, which makes them
  independent of it.
