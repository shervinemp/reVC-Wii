#ifndef WIITRACE_H
#define WIITRACE_H

// Loading diagnostics for the Wii port.
//
// The question a plain step log cannot answer is whether a load that shows no
// progress is stuck or merely slow, because the thread that would print the
// answer is the one that stopped.  So the step is recorded by the game thread
// and reported by a separate one.

// The one switch for diagnostics on this port.  At 1 every line goes into
// debug.log beside the ELF; at 0 NOTHING below runs at all.
//
// The console copy is deliberately absent in a normal build: on the Wii both
// printf through the console device and SYS_Report draw onto the framebuffer,
// which scrolled the menus and the game image around, so screen output only
// exists while the boot is too early to read a log card.  debug.log beside the
// ELF is the one medium, which makes a freeze on real hardware legible without
// touching what is on screen.
//
// Turning it off is worth real time rather than just a quieter screen.  The
// expensive part was never the screen itself: it is the work done to reach it.
// wiiLog formats into a 512 byte buffer for every model, texture and stream the
// game touches, and streaming touches thousands while driving; WiiTraceNote
// copies each of those strings again; WiiTraceHeap walks the whole heap through
// mallinfo; and the watchdog is a thread with an 8KB stack waking once a
// second.  At 0 none of that is compiled in, and the watchdog is never started.
#define CREATE_LOG 1

// Opens debug.log inside the given directory, and does nothing if one is already
// open, so it can be called again later with a better guess.  Does nothing at
// all unless CREATE_LOG is 1.
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

// Starts the reporting thread.  It names the current step and reports the heap
// whenever that step has not changed for a while, so a main thread stuck in a
// loop or blocked on storage still gets described.
void WiiTraceStartWatchdog(void);

// One tagged heap line.  Free bytes alone cannot separate the cases that
// matter, so the split is reported too: a large free total spread over many
// blocks with a small top chunk is fragmentation, and allocating less will not
// help, whereas a small free total with a large top chunk is plain exhaustion.
void WiiTraceHeap(const char *tag);

#endif
