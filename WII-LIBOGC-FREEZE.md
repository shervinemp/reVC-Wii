# The frame-present freeze, and why libogc must be >= 3.1.0

## Symptom

The game would freeze after a few minutes of play. The watchdog reported

    WII watchdog: no frame for 5s, stuck in [e:present] ...

and the console had to be switched off. `e:present` is set immediately before
`RsCameraShowRaster()`, and by that point the frame had issued every draw call -- so
the GPU had been handed the whole frame and then never retired it. No crash, no
exception, nothing on the CPU side.

## Root cause

It was not our code. It was libogc 3.0.4's GX command-processor FIFO interrupt
handler, `__GXCPInterruptHandler()` in `libogc/gx.c`. That handler suspends the render
thread when the GP FIFO overflows and resumes it when it underflows:

    __GXOverflowHandler:  LWP_SuspendThread(_gxcurrentlwp);
    __GXUnderflowHandler: LWP_ResumeThread(_gxcurrentlwp);

In 3.0.4 the handler read the CP status register and acted on it without clearing
spurious pending underflow/overflow IRQs first, and it tested the two conditions with
separate `if`s. A spurious overflow could therefore suspend the render thread with no
matching underflow ever arriving to resume it, leaving the thread asleep forever.

That is exactly the observed signature: a hang with no CPU fault and the last frame
fully recorded, at whatever point the stray interrupt happened to fire -- which is why
it looked random and depended on where you were and what was streaming.

## The fix

**libogc 3.1.0.** Its tag commit is precisely this change:

    c70bdf2f  gx: revise __GXCPInterruptHandler to avoid spurious underflow/overflow IRQs

which clears the spurious IRQs before reading the status register and uses `else if`
so that only one of the two can be handled per interrupt.

### Verified in the binary, not inferred

Disassembling `__GXCPInterruptHandler` out of `gx.o` in each archive shows the old and
new logic directly:

    3.0.4   lhz    r8,2(r6)     ; r8 = _cpCRreg (cached enable bits)
            ...
            andi.  r5,r8,8      ; gate the decision on cpCRreg & 0x08
            ...

    3.1.0   ori    r9,r9,3
            sth    r9,0(r10)    ; _cpReg[2] = WriteFifoIntReset(GX_TRUE,GX_TRUE) -- clear first
            ...
            andi.  r6,r9,2      ; test the live status only, then else-if

## Requirement

**Build with libogc >= 3.1.0.** Building against libogc < 3.1.0 reintroduces the
freeze.

This is a toolchain change, not a repository change: nothing in git records it, which
is why it is written down here.

### Updating

devkitPPC r49.2 shipped with libogc 3.0.4, which has the bug. libogc 3.1.0 declares no
dependencies, so it upgrades on its own without touching the compiler:

    pacman -S libogc        # in the devkitPro MSYS2 shell; installs 3.1.0-1

Confirm with `pacman -Q libogc` (expect `3.1.0-1` or newer). No other package needs to
move -- devkitPPC stays at 15.2.0 and gcc does not change.

## What this was NOT

The freeze was misattributed several times before this. Recording the dead ends so they
are not re-investigated:

- **Not memory exhaustion.** The streaming budget was cut 24MB -> 16MB; the freeze
  persisted with 18MB of MEM2 free, and persisted again after the cut was reverted
  with only 6MB free. Memory pressure was never the cause. The revert stands.
- **Not the TXD reclaim path.** The tidier has never freed a single TXD on any
  escalation.
- **Not a null-geometry dereference in the LOD fade path.** That was a real bug -- a
  missing lower clamp on `fadefactor` let a nil `lodatm` reach `RpAtomicSetGeometry`,
  corrupting the atomic -- and it was fixed. It was not this freeze. Both fixes are
  kept: the clamp is correct regardless.
- **Not the cached GX display-list path.** An A/B build that disabled display lists
  was prepared to test it. The libogc fix landed first, so the A/B was never run and
  the path was restored. It has not been cleared as a suspect, only not implicated.
