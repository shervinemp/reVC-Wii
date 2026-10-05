# TXD eviction — the texture memory leak

Branch: `txd-eviction` (from `definitive-qol`)
Status: **planned, not implemented.** Nothing in this branch changes behaviour yet.

---

## The defect, in one paragraph

GX texture memory is allocated per `Raster` by librw and freed **only in `~GxRaster`**.
Textures are reference counted, and `Texture::destroy()` at refcount zero destroys its
`Raster` — so freeing texture memory requires the owning TXD to be deleted. The game
never deletes TXDs during gameplay: every place it drops a TXD reference calls
`CTxdStore::RemoveRefWithoutDelete()`, which decrements and **never deletes**. The one
real `CTxdStore::RemoveRef()` in the codebase is in `BaseModelInfo.cpp:75`, a
destructor that only runs at shutdown. Therefore `nativeTextureMemory` only rises.

## The measurement

```
MEM2 free   textures
 47647K        112    ← boot
 25423K       1766
 12607K       1960
 10579K       1977    ← peak
  6715K       1858    ← freeze, every session
```

librw exports `nativeTextureMemory` (`rw::gx::nativeTextureMemory`, `rwgx.h:182`) and
maintains it in `gxraster.cpp:319` / `:67`. It is now on the arena log line.

## Consequence: the freeze and the mesh bug are this bug

1. Texture memory climbs ~30MB over a session
2. Arena2 drains 47.6MB → ~6.7MB
3. Below roughly that level the render cannot allocate what it is drawing
4. `scene draw` blocks forever — frozen heap, no allocation, no log

**Every session freezes at the same level**, which is a threshold and not a
coincidence. The enemy mesh flattened onto the ground plane is what a failed
allocation mid-frame looks like, and it resolved when the ped was killed (freeing its
textures), which fits.

## What is already correct

**The reference counting balances.** Every `AddRef` has a matching
`RemoveRefWithoutDelete`:

| take | release |
|---|---|
| `Streaming.cpp:604` model load | `Streaming.cpp:633` load complete, TXD no longer needed |
| `Streaming.cpp:828` model finalize | `Streaming.cpp:833` |
| `Streaming.cpp:850` TXD load | `Streaming.cpp:854` |

So **there is no counting bug to find.** The design is right and only its final step
was removed. No audit of the 17 `AddRef` sites is needed.

## Why `WithoutDelete` is in use, and why it must stay

All three call sites are *immediately after* a load completes, mid-streaming-operation.
Deleting there would free textures belonging to a model that was just loaded, and
`RemoveTxd` → `RemoveModel` re-enters the streaming system. **Deferral is not a
convenience here, it is required.**

---

## The fix

A deferred deletion queue. Three parts.

### 1. `CTxdStore` — queue instead of delete

`TxdStore.h` / `TxdStore.cpp`:

```cpp
// TXDs whose last reference went away during a streaming operation.  They cannot be
// torn down there -- RemoveTxd re-enters CStreaming, and the model that was just
// loaded may still reference their textures -- so they wait for the next frame's
// streaming update.
static void QueueForDeletion(int slot);
static void DrainDeletionQueue(void);
static int GetPendingDeletionCount(void);   // for the log
```

`RemoveRef()` becomes: decrement; **queue** at zero instead of calling
`CStreaming::RemoveTxd` directly.

Implementation notes:
- Fixed array of `COLSTORESIZE` entries (31), not a heap allocation — this is on a
  console that has already leaked 46MB.
- **Ignore duplicates.** `CStreaming::RemoveModel` early-returns when the streaming
  entry is not `STREAMSTATE_LOADED`, so a duplicate is harmless, but dedupe on insert
  to keep the array from filling.
- Overflow behaviour: if full, drop the entry and log. Losing a deletion is the same
  as today's behaviour, not worse.

### 2. The drain point

`CStreaming::Update()`, at the **top**, before `LoadRequestedModels()`:

```cpp
CTxdStore::DrainDeletionQueue();
```

That is the earliest point in a frame where nothing is mid-load and
`CStreaming::RemoveTxd` is safe — `TexRead.cpp:499`, `Radar.cpp:169` and
`Game.cpp:571` already call `RemoveTxd` from comparable contexts, so this is not a
new calling convention.

### 3. Safety at drain time

For each queued slot, re-validate before deleting:

- **Re-check the refcount.** A model may have been loaded between queueing and
  draining and re-referenced the TXD. Skip if `GetNumRefs(slot) > 0`.
- **Re-check the slot is valid.** `CTxdStore::RemoveTxdSlot` frees a slot; a queued
  index could be reused. Verify via `FindTxdSlot(GetColName(slot)) == slot` — or
  simply that `GetSlot(slot)` is non-nil and its name still matches what was queued.
  **This is the one real hazard and it needs care.**
- **Do not drain during `LoadAllRequestedModels`.** Guard with the same flag
  `CStreaming::ms_disableStreaming` uses, or a dedicated one.

---

## What must be measured to call it done

The arena line already carries what is needed. Success is:

```
tex <N>K    ← must rise during play and FALL when the player moves away
```

Today `nativeTextureMemory` climbs 112 → 1977 textures and never drops. After the
fix it should be **sawtooth**, not monotonic. That single number is the acceptance
test — no new instrumentation required.

Secondary: the freeze should stop recurring, and `MEM2 free` should plateau rather
than walking to 6.7MB.

## Risk

- **Re-entrancy** is the whole risk, and it is why the drain is a separate frame from
  the decrement. If a crash appears during a load, the drain point is wrong — move it
  later in `CStreaming::Update`, not earlier.
- **Slot reuse** is the other. If the game removes and re-adds a TXD slot, a stale
  queue entry could delete the wrong one. The re-validation above is mandatory, not
  defensive.
- The `~Raster` path frees `nativeRaster->pixels` and `nativeAllocation` and asserts
  `nativeSize <= nativeTextureMemory`. Double-free would trip that assert — but
  asserts are compiled out in Release, so **verify by measurement, not by the assert
  passing.**

## Explicitly not doing

- **`RemoveRef` on the three streaming sites directly.** This is the trap: it restores
  designed behaviour but reintroduces the mid-stream deletion the escape hatch exists
  to prevent.
- **A texture-memory budget with forced eviction.** It bounds the symptom while
  fighting the refcount design. Worth doing *later* as a safety net if the sawtooth
  turns out too generous, not instead of this.
- **libogc's GX texture cache.** The purpose-built answer, but a rewrite of librw's
  raster path on hardware that cannot be tested here.
- **Making texture upload fail soft.** Cheap insurance against the freeze specifically,
  and worth adding eventually — but it hides the leak rather than removing it, and the
  user asked for the proper fix.

## Verification discipline

This leak has been misdiagnosed twice this session: textures were "exonerated" from a
window where the count happened to be flat, when the real curve runs 112 → 1977. So:

- **Measure the curve across a whole session**, not a window.
- **`nativeTextureMemory` is the number**, not the raster count — the count plateaus
  at the top and reads as flat while the bytes climb.
- Every "exonerated" in this project has come from generalising a slice.