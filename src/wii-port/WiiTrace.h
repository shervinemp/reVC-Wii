#ifndef WIITRACE_H
#define WIITRACE_H

// Loading diagnostics for the Wii port.
//
// The question a plain step log cannot answer is whether a load that shows no
// progress is stuck or merely slow, because the thread that would print the
// answer is the one that stopped.  So the step is recorded by the game thread
// and reported by a separate one.

// The one switch for diagnostics on this port.
//
//   0  nothing runs at all.
//   1  an EVENT log: debug.log beside the ELF (or on the SD card when the ELF is on a
//      read-only stick) gets the rare lines -- boot, saves, HOME, errors, and the
//      debounced stall line -- and nothing else.  Nothing is done per asset, the arena
//      is not sampled, and the file is committed to the card at most once a second, so
//      it costs nothing while playing.  This is the setting to leave on.  The watchdog
//      thread still runs here: it costs a one-second sleep and two register reads, and
//      it is what reports a freeze, so it is deliberately kept.
//   2  full diagnostics: also the arena drain sample and the per-MEMID allocation
//      breakdown, a line for every model, texture and stream the game touches, and the
//      renderer's frame trace.  For chasing a freeze or a leak, and expensive: every
//      asset line is formatted, written and flushed, and the arena is sampled every few
//      seconds.  It is what made the game hitch mid-play.
//
// The console copy is deliberately absent in a normal build: on the Wii both
// printf through the console device and SYS_Report draw onto the framebuffer, which
// scrolled the menus and the game image around, so screen output only exists while
// the boot is too early to read a log card.  debug.log is the one medium, which
// makes a freeze on real hardware legible without touching what is on screen.
#define CREATE_LOG 1

// Opens debug.log inside the given directory, and does nothing if one is already
// open, so it can be called again later with a better guess.  Does nothing at
// all unless CREATE_LOG is 1 or more.
void WiiTraceOpenLog(const char *directory);

// Commits the log and closes it.  Only matters for a clean exit: the watchdog
// commits once a second on its own, which is what a run that never reaches an
// exit relies on.
void WiiTraceCloseLog(void);

// One line into the log file and nowhere else.  The only file sink both
// wiiLog() and the (now silent) console printf carry reach, so anything a load
// prints lands here exactly once.
void WiiTraceLogLine(const char *message);

// One printf-style line into the log file.  The Wii port calls this everywhere
// it used to call SYS_Report directly.
void WiiTraceReport(const char *format, ...) __attribute__((format(printf, 1, 2)));

// Records the step the game is in.  Called for every wiiLog() line so the
// watchdog sees fine grained progress without the log carrying it.
void WiiTraceNote(const char *message);

// Commits what the event log has written to the card, at most once a second.  Called
// once a frame; only does anything at CREATE_LOG 1 (at 2 the watchdog does it).
void WiiTraceService(void);

// Starts the reporting thread (off only at CREATE_LOG 0).  It names the current step and
// reports the heap whenever that step has not changed for a while, so a main thread stuck
// in a loop or blocked on storage still gets described.  It also carries the arena drain
// sample, which is gated to CREATE_LOG 2.
void WiiTraceStartWatchdog(void);

// One increment per frame, from the game loop.  The watchdog judges "is the game
// still turning" by this serial, and it used to move only when something was
// logged -- which was an accident of how much the engine used to print, and is
// exactly backwards now that the log is quiet.  Ticking it from the loop makes a
// hang INSIDE a frame visible: the frame never completes, so the stall timer at
// the end of the frame is never reached, but this stops moving and the watchdog
// says so from its own thread.
void WiiTraceTick(void);

// Names the phase the game thread is currently in, for the watchdog to report when
// a frame never finishes.  A pointer store of a string literal -- no copy, no
// allocation, no logging -- so it is free enough to call from the engine's own
// update passes rather than only once per frame.  Wii-only; a no-op elsewhere.
void WiiTraceSetStep(const char *zone);

// Publishes the streaming system's state for the watchdog to report: how many
// models are queued and how many are loaded.  Passed in rather than read from
// CStreaming so that this file keeps no dependency on the engine -- the frame loop
// already has both numbers for the stall line, so it costs nothing to hand them over.
//
// This is the number that decides between the two live theories about the freeze.
// A hang with a large pending count means the request set cannot be satisfied and
// something is spinning on it; a hang with a pending count of zero means the
// request set is not involved at all.
void WiiTraceSetStreamingState(int pending, int loaded);

// Bytes the streaming system counts against its own budget -- CStreaming::
// ms_memoryUsed, which is CD bytes rather than resident memory.
//
// On the watchdog line and not only on the stall line, because the stall line needs a
// frame that completes slowly and a hard freeze completes no frame at all.  In the run
// that produced the 6.6MB freeze the last stall line was 69 seconds before the freeze,
// so the budget figure at the moment it mattered was never written down.
// unsigned int rather than uint32 because this header deliberately does not include
// common.h, to stay independent of the game; the rest of the file casts to unsigned int
// for the same reason.
void WiiTraceSetResidentBytes(unsigned int bytes);

// Publishes librw's live texture and raster counts for the watchdog to report.
// Passed in for the same reason as the streaming state: this file deliberately has
// no librw dependency, and the frame loop can read both counters for free.
void WiiTraceSetResourceCounts(int textures, int rasters, int colBytes, int texBytes);

// How many TXDs the last eviction sweep actually tore down.  Diagnostic only: a
// sweep that always reports 0 is not running or is refusing everything, and both
// look identical from the texture byte count alone.
void WiiTraceSetTxdEvictions(int evicted);

// Running totals of bytes allocated per MEMID, indexed by MEMID.  Totals rather than
// per-interval figures because the interval is not known here -- the arena sampler
// decides when to print, and it differences these against its own previous snapshot so
// the number spans exactly the interval it is reporting.
//
// See the comment on PUSH_MEMID in MemoryHeap.h for why these are cumulative bytes
// requested rather than live bytes.
void WiiTraceSetMemIdTotals(const int *cumulativeById, int numIds);

// One tagged heap line.  Free bytes alone cannot separate the cases that
// matter, so the split is reported too: a large free total spread over many
// blocks with a small top chunk is fragmentation, and allocating less will not
// help, whereas a small free total with a large top chunk is plain exhaustion.
void WiiTraceHeap(const char *tag);

#endif
