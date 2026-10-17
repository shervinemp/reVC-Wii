# Wii performance work (October 2026)

What was changed to make the port faster, what has and has not been checked, and
how to measure it on the console. **None of this has run on hardware yet.** It
builds, and the pieces that could be checked off-console were checked on Dolphin
with small standalone test programs (the game itself does not boot in Dolphin).

## The changes

Each is its own commit, so a regression can be bisected or reverted alone.

| Commit subject | What it does | Why it is faster |
|---|---|---|
| wii: square roots on frsqrte instead of newlib's integer loop | `sqrtf` and the game's `Sqrt`/`RecipSqrt` use Broadway's `frsqrte` plus three Newton-Raphson steps | Broadway has no `fsqrt`; newlib's fallback was a 25-step bit loop, a few hundred cycles, on ~800 call sites (every distance, length and normalise) |
| gx: present without waiting for the GPU, and free GPU memory once it is done | The present queues the display copy and returns; interrupt handlers flip it at the next retrace; GPU memory freed during the update is held until the GPU is done with it | The game's update for frame N+1 runs while the GPU draws frame N; the CPU no longer sits idle through the GPU's tail and the vsync wait. A frame a little over two retraces long is no longer shown for three |
| gx: program only the GX state a mesh changes | TEV, texture object, colour channels, lights, vertex format and normal matrix are sent only when they differ from the last mesh | Most of the world needs identical state mesh after mesh; thousands of redundant GX calls a frame are gone |
| gx: skin peds faster, and give a second clone of a model its own vertices | Normal renormalisation via `frsqrte`; repeat clones of one model skin into a 512KB ring | Removes a software sqrt and divide per vertex; fixes peds of the same model drawing half in each other's pose, without the GPU drain couch-coop's fix used |
| build: no errno from math calls and no exception tables on the Wii | `-fno-math-errno -fno-exceptions -fno-asynchronous-unwind-tables` | Pure math calls can be shared and hoisted; about 370KB of MEM1 back |
| wii: a frame-rate line in debug.log every ten seconds | `WII_PERF_LOG` in `WiiTrace.h` | Measurement, see below |
| gx: skin in paired singles | Skinning blends each vertex's bone matrices, then transforms once, in Broadway's paired-single instructions | About 40 instructions per two-bone vertex instead of about 100 |
| gx: free a streamed model's own copy of its data once it is instanced | After the first instancing of a model read from a DFF, frees RenderWare's triangles, prelight, UVs and mesh indices, its bind pose unless skinned, and the instance's index copy | Memory, not speed: every streamed model was held twice. About half of a model's bytes go (44KB of ~80KB for a 1000-vertex, 1500-triangle prelit building), which is heap headroom under the same streaming budget |

On `coop-4-perf` (the same commits on top of `coop-4`) the skin commit replaces
`aa7070e5`, which drained the GPU once per repeat draw of a shared model: up to
three drains a frame with four players. One more commit there adds the player count
to the frame-rate line.

## What was checked, and how

- **Builds** at every commit (each intermediate state was compiled).
- **Square root** (Dolphin, bit-exact `frsqrte` emulation): identical to newlib's
  exact `sqrtf`, bit for bit, on ~400,000 values including denormals, zero,
  infinity, NaN and negatives. `RecipSqrt` is within 1 ulp of `x/sqrtf(y)`.
- **GX backend** (Dolphin, real librw code): coloured quads covering textured and
  untextured, material colour, texture switches, lit (ambient + directional) and
  unlit, Im2D and back, read back from the EFB after each draw. 30 checks, all pass,
  with values identical to the pre-change librw. 240 frames ran at exactly 60fps
  while textures and geometry were destroyed and rebuilt under frames in flight
  (120 rebuilds). A deliberately broken texture cache fails 8+ of the checks, so
  the test does catch that class of bug.
- **Paired-single skinning** (Dolphin): matches the scalar blend to within 5e-7
  on 2000 random vertices with one to four bones, including unaligned pairs and
  a normal cancelled to zero.
- **Freeing a streamed model's data** (Dolphin, real librw code): quads written to a
  memory stream and read back draw identically before and after the release, lit
  ones included, and so does a clone; a quad built in code keeps its data; every
  released geometry was destroyed again, some with their last frame in flight, and
  none leaked. 37 checks, all pass. Skinned models (which keep their bind pose)
  were not part of it.
- **Not checked anywhere:** real frame rates, real game scenes, the frame pacing
  on a real TV, and anything only the console shows.

## Measuring on the console

With `CREATE_LOG 1` and `WII_PERF_LOG 1` (both the default), `debug.log` on the SD
card gets one line every ten seconds, for example (the numbers are made up):

    WII perf: 300 frames 30.0 fps | frame 33.3ms worst 41ms | update 20.1 render 9.8
      (skin 2.4, 9100 verts) gpu-wait 0.6 vsync-wait 2.8 | skin repeats 3 stalls 0 | players 4-4 state=N

- `update` -- the game's simulation, streaming, audio, input.
- `render` -- submitting the frame; `skin` is its skinning share.
- `gpu-wait` -- the CPU waiting on the GPU. Large means GPU-bound.
- `vsync-wait` -- waiting for the retrace. Large means there is headroom.
- `skin repeats` -- draws per frame of a model already skinned that frame;
  `stalls` -- how many of those had to drain the GPU (should stay 0).
- `players` (coop-4-perf only) -- fewest and most players in the world.

## Four-player co-op test

`dist/coop4-perf-ab/` (not in git) holds two builds of the same co-op code:

- `reVC-coop4-before.dol` -- `coop-4` as it is, plus a frame-rate-only line
  (`WII perf (baseline)`), so it can be compared.
- `reVC-coop4-after.dol` -- `coop-4-perf`: every change above.

Deploy each as `apps/reVC/boot.dol` in turn (what `scripts/deploy-wii.bat` does
with `build-wii/src/reVC.dol`), and run the same scenes for about 30 seconds each,
which gives three lines per scene:

1. one player on foot on a busy street (Ocean Drive);
2. two players together there;
3. four players on foot together, all on the default skin (one shared model:
   the case the skin ring is for);
4. four players in two cars, driving fast through the city (streaming load).

Compare `fps` and `frame` line for line. On the after build, `gpu-wait` against
`render` says whether the scene is CPU- or GPU-bound, and `stalls` should stay 0.

## If something is wrong on hardware

| Symptom | Most likely commit |
|---|---|
| Hang at a present, black screen after a while, tearing, or frames out of order | present without waiting for the GPU |
| Wrong colour, texture or lighting on some meshes | program only the GX state a mesh changes |
| Peds with stretched or torn vertices | skin in paired singles, then skin peds faster |
| Physics or AI oddities (NaN, things flying off) | square roots on frsqrte |
| A crash that names exceptions or `__cxa` | no exception tables |
| A crash (DSI near address 0) when a model loads, changes LOD or is destroyed, or a model drawn wrong after its first frame | free a streamed model's own copy of its data |
