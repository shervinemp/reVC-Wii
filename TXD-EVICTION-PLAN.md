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

**Pass one** marks every TXD slot named by a currently loaded model, using a static
bit-per-slot bitmap (`(TXDSTORESIZE+7)/8` = 174 bytes).

**Pass two** tears down every loaded TXD that passes all three gates:

| gate | protects |
|---|---|
| `CStreaming::CanRemoveTxd(slot)` | radar tiles, male ped, script-owned -- anything pinned with `STREAMFLAGS_CANT_REMOVE` |
| `GetNumRefs(slot) <= 0` | a load reading it right now (the crash `RemoveRefWithoutDelete` exists to avoid) |
| bitmap bit clear | anything still on screen drawing with it |

Each gate was checked against the real permanent TXDs rather than assumed:

- `generic` (`Game.cpp:441`) and `particle` (`Game.cpp:454`) take an `AddRef` at init
  and never release it -> refcount > 0 -> protected by gate two
- splash TXDs (`main.cpp:604`) AddRef per splash -> protected by gate two
- radar tiles (`Radar.cpp:163`) and male ped (`Game.cpp:572`) request with
  `STREAMFLAGS_DONT_REMOVE` -> protected by gate one
- script TXDs (`Script4.cpp:1358`) AddRef -> protected by gate two

Every permanent case is covered by one of the two liveness gates. That consistency is
the main reason to believe the design.

**Called from** `CStreaming::Update`, immediately after `LoadRequestedModels()` -- the
earliest point at which this frame's unloads have happened and a dictionary nobody
needs is knowable. Throttled to every 30th frame; the sweep is ~7900 cheap iterations
and what it acts on only changes when the frame above does something.

Gated `#ifdef NINTENDO_WII`. The logic is platform-independent and correct, but this
branch can only be tested on hardware I have, and it should not be able to regress
builds I cannot test.

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

The arena line already carries it. Success is:

```
tex <N>K    <- must become a sawtooth, not a monotonic climb
txd free N  <- must be non-zero regularly; if it is always 0 the sweep refuses everything
```

Today `tex` goes 112 -> 1977 and never falls. After the fix it should rise during play
and **fall when the player moves away**.

Read the two together. `tex` flat with `txd free 0` means nothing is being evicted;
`tex` flat with `txd free` non-zero means eviction is running but not reclaiming --
which would mean the leak is not (only) in TXDs.

Secondary: the freeze should stop recurring and MEM2 free should plateau rather than
walk to 6.7MB.

## Risks

- **Too aggressive** is the live risk: freeing a TXD a model still draws with. The
  three gates are the defence, and the bitmap is the one that matters. If the world
  goes untextured, the bitmap is wrong -- check `GetTxdSlot()` on loaded models before
  anything else.
- **Slot reuse.** Unlike the queued design this plan started with, the sweep holds no
  entries across frames, so there is no stale-slot hazard and no generation counter is
  needed. It re-reads live state every time.
- **Thrash.** A TXD freed and immediately re-requested reloads from disc each time.
  If `tex` sawtooths but the world stutters, that is this, and the fix is a residency
  grace period rather than a different gate.
- Asserts are compiled out in Release, so `~Raster`'s `assert(nativeSize <=
  nativeTextureMemory)` proving nothing is not evidence. **Verify by measurement.**

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
