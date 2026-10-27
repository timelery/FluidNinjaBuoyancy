# Wave Follow: things that ride the waves without being bodies

Wave Follow lets floating decoration move with FluidNinja Waves:

- lily pads, water lettuce and pennywort;
- leaves, litter and foam mats;
- anything placed as a plain mesh or as foliage instances.

Each vertex moves up and down with the drawn wave height where it stands. A pad on a passing wake rises, tilts across the crest and settles again, like the water around it. Wakes, ripples, splashes and the wind sea all count.

These things are not buoyancy bodies. They cost no physics, no readback and no gather, and thousands of instances cost a few vertex-shader instructions each. They do not drift with the current or push the water. For that, use a FluidNinja Buoyancy component.

## How it works

FluidNinja makes each simulation's pressure buffer at run time, so a material cannot name it. The pressure buffer is `RT_PressureDivergence`; its B channel is the drawn wave height, η + P.

Every frame, after the waves' frame, the FluidNinja Buoyancy subsystem does three things:

1. **Picks one simulation.** It takes the one under the camera, or else the nearest. The simulation's pressure buffer must be RGBA16F, which the modified core makes; a stock RG16F buffer carries no wave height. It must also be larger than FluidNinja's 1 × 1 placeholder.
2. **Copies that buffer** on the GPU into `RT_FNB_WaveFollow`, a render target in the plugin's `Content/Materials`. The target takes the buffer's size the first time and whenever the simulation changes. Nothing marks the asset dirty, so it is not saved at that size.
3. **Publishes the square** in `MPC_FNB_WaveFollow`:
   - **Window**: the centre X and Y and the size, in cm, with W 1 while the copy is valid, else 0.
   - **Shape**: the surface's WaveHeightScale, Wave Follow Max Height Cm and Wave Follow Edge Fade Cm.

`MF_FNB_WaveFollow` reads both in the vertex shader and returns a World Position Offset of (0, 0, height). The height is B × WaveHeightScale where the vertex stands, clamped to ± Max Height. It eases to 0 over Edge Fade at the square's edge, and is 0 while Window W is 0.

## Using it in a material

1. Add **MF_FNB_WaveFollow** (palette category FluidNinja Buoyancy) to the material.
2. Add its output to whatever already drives World Position Offset, such as the material's wind. If nothing does, connect it straight in.
3. Leave **Position** unconnected: each vertex follows the water under it, so flat leaves tilt with the waves. Connect **Object Position** instead to lift each mesh, or each foliage instance, rigidly by the waves at its pivot.
4. Turn on **Evaluate World Position Offset** on the mesh components or foliage instances. It is on by default.
5. Put the mesh's pivot at its waterline and place it at the water's still level. The function adds only the waves.

A procedural foliage tool can do this for its floating species by itself: it makes wave-following copies of each material it places when it builds its graph.

## Settings

Project Settings > FluidNinja Buoyancy > Waves:

| Setting | Default | Range | What it does |
|---|---|---|---|
| Wave Follow | on | on / off | Copies and publishes the wave field every frame. Off: Window W stays 0 and following materials stand still. |
| Wave Follow Target | `RT_FNB_WaveFollow` | a render target | The target the copy goes into. A following material must sample the same one. |
| Wave Follow Collection | `MPC_FNB_WaveFollow` | a parameter collection with vector parameters Window and Shape | Where the square and the shape go. |
| Wave Follow Edge Fade Cm | 300 | 0–5000 | How far in from the square's edge the following eases from full to none. 0: no easing. |
| Wave Follow Max Height Cm | 60 | 1–500 | The most a vertex moves up or down. A bow wave higher than this flattens on the plants. |

## Limits

- **One simulation at a time.** Things on other simulations' water, or outside the followed square, stand still.
- **In the editor**, outside Play, everything stands still: Window W is 0 in the editor world.
- **One frame behind.** The copy is made before that frame's Niagara GPU step, so it trails the drawn surface by a frame. That is about 2 cm on a fast wake.
- **Only FluidNinja Waves.** FluidNinja's own mesh distortion, a few cm of tile-map and density wobble, is not followed.
- **Vertical only.** Vertices move straight up and down. Upright parts shift without leaning, and the shading normals keep their rest pose.
- **Bounds.** World Position Offset beyond a mesh's bounds can be culled early at the screen's edge. Allow Max Height Cm in the material's Max World Position Offset Displacement if that shows. Leave it at 0 when the material has other large offsets, such as a billboard's turn to the camera.
- **Precision.** The square's centre goes through the parameter collection as floats: about 0.06 cm at 10 km from the origin.

## Cost

- **GPU copy:** one copy of the pressure buffer a frame, 8 MB at 1024 × 1024. That is a few hundredths of a millisecond.
- **Vertex shader:** one texture read per vertex, about 20 instructions.
- **Game thread:** a few microseconds.

## Testing

In Play, with a wave simulation running, read the copy back and compare it with the simulation's own buffer. Pick a point inside the published Window, then:

1. Read the copy at that point. Use `RenderingLibrary.read_render_target_raw_pixel(world, RT_FNB_WaveFollow, x, y, False)`; reads with `read_render_target_raw_uv` clamp floats to 0–1.
2. Read the same texel of the simulation's pressure buffer, B channel, the same way.
3. Optionally, read **Get Water At Location**'s WaveHeight there.

The first two match exactly. WaveHeight is B − A, the free wave without the hulls' static head, so it matches B only away from floating bodies. The log says `FluidNinja Buoyancy Wave Follow: following <NinjaLive> (cells, size)` whenever the followed simulation changes.
