# TXD eviction — the texture memory leak

Branch: `txd-eviction` (from `definitive-qol`)
Status: **implemented, not yet measured.** The fix is in and builds clean. Whether it
works is a question only hardware can answer.

---

## The defect

GX texture memory is allocated per `Raster` by librw and freed **only in `~GxRaster`**.
The chain that reaches it is:

```
CTxdStore::RemoveTxd(slot)
  -> RwTexDictionaryDestroy(texDict)        (src/fakerw/fake.cpp:338)
  -> TexDictionary::destroy()               (vendor/librw/src/texture.cpp:131)
  -> for each texture: Texture::destroy()   -> refCount-- -> at 0: raster->destroy()
  -> ~GxRaster                              -> frees nativeAllocation
```

All of that verified by reading, not assumed. So **freeing texture memory requires
deleting a TXD**, and the game never deletes one during gameplay.

## The measurement

```
MEM2 free   textures
 47647K        112    <- boot
 25423K       1766
 12607K       1960
 10579K       1977    <- peak
  6715K       1858    <- freeze, every session
```

librw exports `rw::gx::nativeTextureMemory` (`rwgx.h`) and maintains it in
`gxraster.cpp`. It is on the arena log line as `tex <N>K`.

## Consequence

Texture memory climbs ~30MB over a session; arena2 drains 47.6MB -> ~6.7MB; below
roughly that level `scene draw` cannot allocate what it is drawing and blocks
forever. **Every session freezes at the same level**, which is a threshold and not a
coincidence. The enemy mesh flattened onto the ground plane is most likely the same
cause -- a failed allocation mid-frame -- and it resolved when the ped died, freeing
its textures, which fits.

---

## The correction: refcount is not liveness

The first version of this plan said the fix was "make deletion happen, deferred" —
queue a TXD when its refcount reaches zero, drain it next frame. **That would have
destroyed the textures of every loaded model.** It is wrong, and the reason matters:

`Streaming.cpp:604` is the **only** place in the codebase that AddRefs a gameplay
model's TXD, and `Streaming.cpp:633` drops it the moment the load completes. So:

> a world model sitting fully loaded on screen holds a **refcount of zero**.

The refcount covers *loads in flight*, not *models resident*. `refCount == 0` means
"nothing is reading it at this instant", which is not the same as "nobody wants it".
The escape hatch (`RemoveRefWithoutDelete`) is therefore not merely a re-entrancy
workaround -- dropping to zero is a normal state that the original code also passes
through.

Every AddRef/RemoveRef pair balances perfectly (604/633, 828/833, 850/854), and that
balance is exactly what makes the naive fix dangerous rather than merely wrong.

## The real relationship, and the missing wiring

A TXD must outlive the models that draw with it. Nothing in the game expresses that,
and `CStreaming::CanRemoveTxd()` -- the game's own "is this safe to remove" predicate
-- **had zero callers**, while `CanRemoveModel` had eight and `CanRemoveCol` had one.

Not because it was forgotten. Because there was no TXD eviction pass for it to gate.
That is the whole defect in one line: the gate was written, correctly, and nothing was
ever built to pass through it.

Same shape as the rest of this project's findings: a mechanism that exists, correctly
designed, routed around by omission.

---

## The fix

`CTxdStore::EvictUnusedTxds()` in `src/rw/TxdStore.cpp`. State-derived, no bookkeeping.

**Pass one** marks every TXD slot named by a model that is *in play* -- any streaming
state except `NOTLOADED` -- using a static bit-per-slot bitmap
(`(TXDSTORESIZE+7)/8` = 174 bytes).

The state test is `!= STREAMSTATE_NOTLOADED`, **not** `== STREAMSTATE_LOADED`, and the
distinction is load bearing. `ConvertBufferToObject` takes the TXD refcount at
`Streaming.cpp:618`, which runs *after* the CD read has already completed, and drops
it as soon as the conversion finishes. So a model that is still **queued or mid-read**
has its dictionary loaded and its refcount back at zero. Testing for `LOADED` alone
frees the dictionary out from under it; `ConvertBufferToObject:605` then re-requests
it, and the model ping-pongs between loading a dictionary and having it pulled, which
reads as a hang rather than as churn. `GetModelInfo` is also nil-checked here: it is
an unchecked array read and this loop walks every model index.

**Pass two** tears down every loaded TXD that passes all three gates:

| gate | protects |
|---|---|
| `CStreaming::CanRemoveTxd(slot)` | radar tiles, male ped, script-owned -- anything pinned with `STREAMFLAGS_CANT_REMOVE` |
| `GetNumRefs(slot) <= 0` | a conversion reading it right now (the crash `RemoveRefWithoutDelete` exists to avoid) |
| bitmap bit clear | any model in play that still names it |

Each gate was checked against the real permanent TXDs rather than assumed:

- `generic` (`Game.cpp:441`) and `particle` (`Game.cpp:454`) take an `AddRef` at init
  and never release it -> refcount > 0 -> protected by gate two
- splash TXDs (`main.cpp:604`) AddRef per splash -> protected by gate two
- radar tiles (`Radar.cpp:163`) and male ped (`Game.cpp:572`) request with
  `STREAMFLAGS_DONT_REMOVE` -> protected by gate one
- script TXDs (`Script4.cpp:1358`) AddRef -> protected by gate two

Every permanent case is covered by one of the two liveness gates. That consistency is
the main reason to believe the design.

## The trigger: the escalation that already existed

GX texture memory is not the only thing that accumulates, and reclaiming it on a timer
is the wrong shape. The game already has a memory-pressure escalation:
`CGame::DrasticTidyUpMemory`, restored for Wii, which fires when arena2 free drops below
`kWiiLowMemoryBytes` (16MB, comfortably above the 6.7MB freeze point) and then waits
`kTidyCooldownMs` (15s) between attempts. Its comment already records the reasoning that
governs this whole area: the steps delete geometry, so a burst of them "leaves cars and
peds on geometry that is no longer loaded", and it measures with `SYS_GetArena2Size`
rather than `mallinfo` because the latter under-reports free memory here by tens of
megabytes.

So the reclaim is wired into that escalation rather than invented:

- **`CTxdStore::ReclaimUnusedTxds()` is a step in `DrasticTidyUpMemory`**, ahead of the
  building removals because it is the step that recovers the tens of megabytes. Safe in
  that position: it only frees dictionaries no model in play names, so buildings that
  are about to be unloaded keep their textures either way.
- **`CGame::IsMemoryTight()`** exposes the single threshold, so the escalation and the
  reclaim cannot each pick their own number and disagree about when memory is a problem.
- **The same reclaim also runs from `CStreaming::Update`, behind that same threshold.**
  The escalation only fires on loads and cutscene boundaries, and the freeze happened
  while *driving*, where no load happens for minutes. Throttled to every 30th frame;
  `SYS_GetArena2Size()` is two register reads, so the per-frame gate is free.

**Why not on a timer unconditionally.** The first cut of this ran the reclaim every 30
frames regardless of pressure. That is a cache holding exactly its working set, so every
dictionary reloads the moment the player looks away from it -- pop-in on every area
transition, paid on every frame, as a regression introduced by the fix for the freeze.
With the threshold in front of it, a session with memory to spare never takes the branch
and every dictionary stays resident exactly as the stock game left it. The cost is a
constant that has to be right; `kWiiLowMemoryBytes` already existed and already sits
above the observed failure point, so nothing new had to be guessed.

Gated `#ifdef NINTENDO_WII`, including the `IsMemoryTight` definition, because it reads a
libogc arena register that no other platform has. The logic is platform-independent and
correct, but this branch can only be tested on hardware I have, and it should not be able
to regress builds I cannot test.

### Why derived state rather than a counter

A per-TXD loaded-model counter would be O(1) per event instead of O(7900) per sweep,
and is the obvious optimisation. It was rejected because **model loads complete in
five separate places** (`Streaming.cpp:760, 868, 2227, 2572, 2754`) and hooking all of
them correctly is a worse way to be wrong than reading the state that already exists.
A missed hook silently under-counts and untextures the world; a sweep cannot miss.

If the sweep ever shows up as a frame cost, that is the moment to add the counter --
with the sweep left in as a cross-check that the two agree.

---

## What must be measured to call it done

Two lines now, because they answer different questions:

```
WII tidy: escalated at <N>K free arena, freed <M> unused TXDs
WII arena: ... tex <N>K in <count>, ..., txd free <M>
```

Success is **`tex <N>K` falling after each escalation**, and `freed <M>` being
non-zero — a reclaim that frees nothing while reporting pressure means it is refusing
everything, which is the failure mode to look for first.

Today `tex` goes 112 -> 1977 and never falls. It should now fall whenever arena2 drops
below 16MB, which is what keeps it from ever reaching the 6.7MB freeze.

Read `tex` and `freed` together. `tex` still climbing with `freed > 0` means eviction
is running but not reclaiming -- which would mean the leak is not (only) in TXDs.

Secondary: the freeze should stop recurring, and MEM2 free should sawtooth around the
threshold rather than walk to 6.7MB.

## Risks

- **Too aggressive** is the live risk: freeing a TXD a model still draws with. The
  three gates are the defence, and the bitmap is the one that matters. If the world
  goes untextured, the bitmap is wrong -- check `GetTxdSlot()` on in-play models
  before anything else.
- **Ping-pong** is the second risk, and the reason the bitmap tests for "in play"
  rather than "loaded". If a dictionary is freed and immediately re-requested, the
  model re-requests forever. The fix is a residency grace period (do not free a TXD
  freed within the last N sweeps), not a different gate.
- **The threshold could be wrong in the aggressive direction.** `kWiiLowMemoryBytes`
  predates this work and was chosen for geometry reclaim. If `tex` now sawtooths but
  loads visibly crawl, that is the symptom, and the answer is to raise the floor
  rather than to add gates.
- **Slot reuse.** Unlike the queued design this plan started with, the reclaim holds
  no entries across frames, so there is no stale-slot hazard and no generation counter
  is needed. It re-reads live state every time.
- Asserts are compiled out in Release, so `~Raster`'s `assert(nativeSize <=
  nativeTextureMemory)` proving nothing is not evidence. **Verify by measurement.**
- `DrasticTidyUpMemory` is labelled `"drastic tidy [NO-OP]"` in `GameLogic.cpp` and
  that label was wrong -- the Wii branch is restored and the function does run.
  Relabelled. Worth remembering that the stale label would have survived a dozen
  correct fixes and still been misleading.

## Explicitly not doing

- **Deferred queue on refcount zero** -- the design this plan started with, and wrong.
  Recorded above because the reasoning error is the useful part.
- **Budget with forced eviction** -- bounds the symptom while fighting the design.
  Worth adding later as a safety net if the sawtooth proves too generous.
- **libogc's GX texture cache** -- the purpose-built answer, but a rewrite of librw's
  raster path on hardware that cannot be tested here.
- **Fail-soft texture upload** -- cheap insurance against the freeze specifically.
  Genuinely worth adding eventually, but it hides a leak rather than removing one.

## Verification discipline

This leak was misdiagnosed twice: textures were "exonerated" from a window where the
count happened to be flat, when the real curve runs 112 -> 1977. So:

- **Measure the curve across a whole session**, not a window.
- **`nativeTextureMemory` is the number**, not the raster count -- the count plateaus
  at the top and reads as flat while the bytes climb.
- Every "exonerated" in this project has come from generalising a slice.
- Check what a predicate *does* before trusting its name. `CanRemoveTxd` sounds like
  it governs TXD removal; for a whole session it governed nothing at all.
