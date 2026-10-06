#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <unistd.h>

#include <gccore.h>
#include <ogc/lwp.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/mutex.h>

#include "WiiTrace.h"

namespace
{

// How long a step has to stay unchanged before it is reported, and how often it
// is repeated afterwards.  Long enough that a slow but progressing load stays
// quiet, short enough to notice a stall while watching.
constexpr unsigned int kStallSeconds = 5;

// Thresholds for the livelock check -- frames completing while the streaming
// request count refuses to fall.  kStallPendingFloor is deliberately far above
// anything real hardware has shown: logs from actual play put the pending count
// between 0 and 10, so 64 cannot fire on ordinary streaming churn.  The window is
// long because the condition is a count that is not decreasing, and a busy street
// produces bursts that dip and recover well inside ten seconds.
constexpr int kStallPendingFloor = 64;
constexpr unsigned int kStallPendingSeconds = 10;

// How often the heap is reported while the game is simply running.  The failure
// this is here to catch takes minutes of play to show up -- streaming in new
// blocks while driving is what grows the heap -- and it is a TREND rather than
// any single number, so it has to be sampled while nothing is going wrong.  Once
// a run has ended, whether the last few lines climb steadily is the difference
// between memory exhaustion and something else entirely.
constexpr unsigned int kHeapReportSeconds = 15;

// The step is written by the game thread and read by the watchdog with no lock.
// A torn read costs one garbled log line and nothing else, which is a better
// trade than putting a mutex on the path every wiiLog() call takes.  The serial
// is what progress is actually judged by, so it is written last.
char s_step[192] = "boot";
volatile unsigned int s_stepSerial;
volatile bool s_watchdogRunning;

// Which phase of the frame the game thread is in, set by the frame loop and by the
// engine's own update passes.
//
// s_step above could not answer this, and the reason is worth stating because it is
// the whole defect.  s_step is only written when something LOGS, so during actual
// play -- when the log is quiet by design -- it never moves.  Every freeze report
// therefore said "last step [boot]", which is true and useless: it named the last
// rare event, not what the thread was doing when it stopped.
//
// A pointer store, not a copy, and not a log call.  Every name is a string literal
// with static storage, so there is nothing to own or free and nothing to allocate;
// the cost on the game thread is one store per phase per frame.  A torn read by the
// watchdog can only produce a garbled name, which is the same trade s_step already
// makes, and a pointer is no more tearable than a length-prefixed string is.
volatile const char *s_zone = "none";
// Bumped on every change.  Distinguishes "stuck in one place" from "moving between
// places but never finishing a frame", which are different faults and the zone name
// alone can be ambiguous about: a phase entered and left every frame looks
// identical to a phase entered once and never left.
volatile unsigned int s_zoneSerial;
volatile int s_streamPending = -1;
volatile int s_streamLoaded;
volatile int s_textures = -1;
volatile int s_rasters = -1;
volatile int s_colBytes;
volatile int s_texBytes;
volatile int s_txdEvicted = -1;

// Per-MEMID allocation deltas for the last interval.  Fixed size rather than
// malloc'd, and only the largest few are printed, because the point is to rank where
// allocation is happening and the long tail is noise.
//
// Six, not five, and the reason is specific: main.cpp pushes MEMID_GAME and never pops
// it, so MEMID_GAME is the ambient category for the entire game and every allocation
// outside an inner scope lands in it.  It will very likely be the largest bucket, and
// at five printed lines that would leave only four categories visible -- enough to
// truncate exactly the one being looked for.  Six keeps a full set of streaming
// categories visible alongside it.
enum { kMemIdSlots = 32, kMemIdPrinted = 6 };
static int s_memidGrowth[kMemIdSlots];
static int s_memidSlotCount;

// Names for the categories that can plausibly own a large share of the arena.  A MEMID
// with no name here is still counted, it just is not worth a column of log.
//
// Indexed by MEMID, and the trailing "?" entries are padding to the slot count.  The
// static assert is not decoration: this table was written twice with 33 entries and a
// silent off-by-one is exactly what a counted list of placeholders invites.
static const char *const kMemIdNames[] = {
	"free", "game", "world", "anim", "pools", "defmodels", "stream", "strmodels",	// 0-7
	"strlods", "strtex", "strcol", "stranim", "textures", "collision", "prealloc",	// 8-15
	"gameproc", "script", "cars", "render", "pedattr",								// 16-19
	"?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?",						// 20-31
};
static_assert(sizeof(kMemIdNames) / sizeof(kMemIdNames[0]) == kMemIdSlots,
              "kMemIdNames must be indexed by MEMID up to kMemIdSlots");

// Whether the watchdog also samples the heap on a timer.  Off, and the reason is
// that it is the only expensive thing the watchdog does -- walking the heap from a
// second thread while the game runs -- and it sits OUTSIDE the stall check, so it
// used to fire every fifteen seconds whether or not anything was wrong.  That is
// what made this whole facility look unplayable and get switched off.  The stall
// report itself is nearly free: one comparison a second against a serial the game
// thread already bumps, plus the log commit it would have had to do anyway.
static bool s_heapSampling = false;

lwp_t s_watchdogThread = LWP_THREAD_NULL;
u8 s_watchdogStack[8192] ATTRIBUTE_ALIGN(8);

#if CREATE_LOG

// The game thread appends and the watchdog commits, so the two share a lock.
// newlib's own stdio locking would cover the writes on its own, but not the
// fsync, and holding one lock across both is simpler than reasoning about which
// half is already covered.
FILE *s_logFile;
mutex_t s_logMutex = LWP_MUTEX_NULL;
volatile bool s_logDirty;

// Pushes what has been written out of libfat's cache and onto the card.  fflush
// alone only moves a line out of newlib's buffer into that cache, where a run
// that ends in a freeze leaves it: the console has to be reset, and the cache
// goes with it.  Committing costs far too much to do per line, so the watchdog
// does it once a second, which is also the bound on how much of the tail a
// freeze can take with it.
void
commitLog(void)
{
	if(s_logFile == nullptr || !s_logDirty)
		return;
	LWP_MutexLock(s_logMutex);
	fsync(fileno(s_logFile));
	s_logDirty = false;
	LWP_MutexUnlock(s_logMutex);
}

#endif // CREATE_LOG

void*
watchdogMain(void*)
{
	unsigned int lastSerial = 0;
	// Self-limiting by design: it only prints while memory is FALLING, so a healthy
	// session that holds steady produces nothing at all, and a drain produces a line
	// every five seconds until it stops.  No run-time switch, because unlike the heap
	// walk this is two register reads -- SYS_GetArena1Size and SYS_GetArena2Size are
	// not mallinfo and do not walk anything.
constexpr unsigned int kArenaSampleSeconds = 5;

unsigned int stalledSeconds = 0;
	unsigned int secondsSinceHeap = 0;
	// For the livelock check, which is on the frame-turning path and so is
	// independent of the no-frame one above.
	int lastPending = 0;
	unsigned int pendingStalledSeconds = 0;
	unsigned int secondsSinceArena = 0;
	unsigned int lastArena2 = 0xFFFFFFFFu;

	while(s_watchdogRunning){
		usleep(1000*1000);

		// Before the progress check, so a run that stops making progress still
		// gets everything it had already written onto the card.
#if CREATE_LOG
		commitLog();
#endif

		// Outside the progress check on purpose, but opt-in: a game running
		// perfectly well is when a heap TREND would be worth having, and that is
		// also the one thing in this file that costs real frame time.
		if(s_heapSampling && ++secondsSinceHeap >= kHeapReportSeconds){
			secondsSinceHeap = 0;
			WiiTraceHeap("running");
		}

		// Arena drain.  The one measurement with no game-affecting downside, and the
		// one thing that would settle where the memory went: streaming holds its 24MB
		// budget every time, so the ~30MB that disappears is somewhere the streaming
		// eviction loop cannot see, and the heap report only shows the total after the
		// fact rather than when it went.
		//
		// Only logs while the arena is falling, so it is quiet on a healthy session and
		// self-limiting during a drain: no threshold to tune, no flood, and it stops
		// on its own the moment memory stops moving.
		if(++secondsSinceArena >= kArenaSampleSeconds){
			secondsSinceArena = 0;
			const unsigned int arena1 = (unsigned int)SYS_GetArena1Size();
			const unsigned int arena2 = (unsigned int)SYS_GetArena2Size();
			if(lastArena2 != 0xFFFFFFFFu && arena2 < lastArena2)
				// The pending request count rides along because the arena drain and the
				// request list may or may not be the same curve, and that difference is
				// the whole question.  If both fall together, something is queued that
				// nothing will satisfy.  If the arena falls while the count stays flat,
				// it is a plain leak and nothing is waiting on it.
				//
				// Free to read: s_streamPending is written by the frame loop already,
				// for the watchdog's stall line, so this costs nothing and adds no
				// dependency on the streaming engine to this file.
				WiiTraceReport("WII arena: MEM1 %uK free, MEM2 %uK free (down %uK),"
				               " streaming %d pending, tex %dK in %d, col %dK, txd free %d\n",
				               (unsigned int)(arena1 / 1024u),
				               (unsigned int)(arena2 / 1024u),
				               (unsigned int)((lastArena2 - arena2) / 1024u),
				               s_streamPending, s_texBytes / 1024, s_textures,
				               s_colBytes / 1024, s_txdEvicted);

				// Where the allocation went, by category, for the interval just ended.
				//
				// This rides the same line and the same 5-second gate as the numbers
				// above, so it costs no extra logging: the arena sampler already only
				// reports while memory is falling, which is exactly when the answer is
				// wanted.  Printed as a second line rather than packed into the first
				// because the set of categories varies and a fixed-width column would
				// either truncate the interesting one or waste the width on the tail.
				//
				// Cumulative allocation, not live bytes -- see PUSH_MEMID in
				// MemoryHeap.h.  Read it beside "down %uK": a category allocating far
				// more than the arena actually fell is churning, and one tracking it is
				// a candidate for the growth.
				{
					// Repeatedly take the largest entry not already taken.  Five picks
					// over a handful of candidates, so selection beats sorting and there
					// is no allocation.
					//
					// Chosen by INDEX and not by value: two categories can easily report
					// the same delta, and comparing values would silently drop one of
					// them -- which is the sort of error that makes a diagnostic lie.
					int chosen[kMemIdPrinted];
					for(int slot = 0; slot < kMemIdPrinted; slot++){
						chosen[slot] = -1;
						int best = -1;
						for(int i = 0; i < s_memidSlotCount; i++){
							bool taken = false;
							for(int j = 0; j < slot; j++)
								if(chosen[j] == i) { taken = true; break; }
							if(taken)
								continue;
							if(best < 0 || s_memidGrowth[i] > s_memidGrowth[best])
								best = i;
						}
						if(best < 0 || s_memidGrowth[best] <= 0)
							break;
						chosen[slot] = best;
						WiiTraceReport("WII arena: alloc %s %dK\n",
						               kMemIdNames[best], s_memidGrowth[best] / 1024);
					}
				}
			lastArena2 = arena2;
		}

		if(s_stepSerial != lastSerial){
			lastSerial = s_stepSerial;
			stalledSeconds = 0;
			// Frames turning is necessary but not sufficient.  A livelock inside the
			// streaming update can keep completing frames while the request count
			// sits unchanged, and every other check in this thread is blind to that
			// by construction: the serial moves, so nothing is reported at all.
			//
			// Only reached on a frame boundary, which is what makes it safe.  A slow
			// load legitimately parks here for twenty seconds with a high pending
			// count, and that case never enters this branch because no frame is
			// completing to trigger it.
			//
			// The threshold is far above anything observed on real hardware.  Logs
			// from actual play show a pending count of 0 to 10, so a floor of 64 is
			// an order of magnitude clear of normal streaming churn and will not fire
			// on a busy street.  Ten seconds of not falling is long enough that
			// ordinary bursts, where the count dips and recovers, cannot accumulate
			// into it.
			if(s_streamPending >= kStallPendingFloor){
				if(s_streamPending >= lastPending){
					if(++pendingStalledSeconds >= kStallPendingSeconds){
						pendingStalledSeconds = 0;
						WiiTraceReport("WII streaming: %d models pending and not"
						               " falling for %ds while frames kept completing"
						               " (zone [%s]); the request set is not being"
						               " satisfied\n",
						               s_streamPending, kStallPendingSeconds,
						               s_zone);
					}
				}else
					pendingStalledSeconds = 0;
				lastPending = s_streamPending;
			}else{
				pendingStalledSeconds = 0;
				lastPending = 0;
			}
			continue;
		}

		if(++stalledSeconds < kStallSeconds)
			continue;
		stalledSeconds = 0;
		WiiTraceReport("WII watchdog: no frame for %us, stuck in [%s] zone#%u,"
		               " streaming %d pending / %d loaded, last log [%s]\n",
		               kStallSeconds, s_zone, s_zoneSerial,
		               s_streamPending, s_streamLoaded, s_step);
		// Sampled here, on the way out, rather than on a timer while healthy.  A
		// hang is exactly when the heap is worth having, and this is the one moment
		WiiTraceHeap("watchdog");
	}
	return nullptr;
}

} // namespace

// Outside the anonymous namespace: called from the engine's own update passes, in
// several translation units.
void
WiiTraceSetStep(const char *zone)
{
	s_zone = zone;
	s_zoneSerial++;
}

void
WiiTraceSetStreamingState(int pending, int loaded)
{
	s_streamLoaded = loaded;
	s_streamPending = pending;
}

void
WiiTraceSetResourceCounts(int textures, int rasters, int colBytes, int texBytes)
{
	s_textures = textures;
	s_rasters = rasters;
	s_colBytes = colBytes;
	s_texBytes = texBytes;
}

void
WiiTraceSetTxdEvictions(int evicted)
{
	s_txdEvicted = evicted;
}

void
WiiTraceSetMemIdGrowth(const int *bytesById, int numIds)
{
	if(numIds > kMemIdSlots)
		numIds = kMemIdSlots;
	for(int i = 0; i < numIds; i++)
		s_memidGrowth[i] = bytesById[i];
	s_memidSlotCount = numIds;
}

void
WiiTraceOpenLog(const char *directory)
{
#if CREATE_LOG
	if(s_logFile != nullptr || directory == nullptr)
		return;

	char path[192];
	std::snprintf(path, sizeof(path), "%s/debug.log", directory);

	if(LWP_MutexInit(&s_logMutex, false) != 0){
		SYS_Report("WII log: mutex create failed, %s not opened\n", path);
		return;
	}

	// Truncated rather than appended.  This is read after a run that did not
	// finish, and a previous run's tail sitting above this one is the quickest
	// way to misread where the current one stopped.
	s_logFile = std::fopen(path, "w");
	if(s_logFile == nullptr){
		LWP_MutexDestroy(s_logMutex);
		s_logMutex = LWP_MUTEX_NULL;
		SYS_Report("WII log: could not open %s\n", path);
		return;
	}

	WiiTraceReport("WII log: writing to %s\n", path);
#else
	(void)directory;
#endif
}

void
WiiTraceCloseLog(void)
{
#if CREATE_LOG
	if(s_logFile == nullptr)
		return;
	LWP_MutexLock(s_logMutex);
	std::fclose(s_logFile);
	s_logFile = nullptr;
	s_logDirty = false;
	LWP_MutexUnlock(s_logMutex);
	LWP_MutexDestroy(s_logMutex);
	s_logMutex = LWP_MUTEX_NULL;
#endif
}

void
WiiTraceTick(void)
{
#if CREATE_LOG
	s_stepSerial++;
#endif
}

int
WiiTraceLogFd(void)
{
#if CREATE_LOG
	// Deliberately takes no mutex.  The one caller is a signal handler, where
	// taking the log mutex could deadlock against the thread that faulted while
	// holding it -- which is the most likely way to have faulted at all.
	return s_logFile != nullptr ? fileno(s_logFile) : -1;
#else
	return -1;
#endif
}

void
WiiTraceLogLine(const char *message)
{
#if CREATE_LOG
	if(s_logFile == nullptr || message == nullptr)
		return;

	// Trailing newlines are trimmed and one is written back, because the callers
	// disagree: most SYS_Report format strings end in \n and some messages do
	// not, and a log with occasional blank lines and occasional run-on ones is
	// harder to read than either convention on its own.
	size_t length = std::strlen(message);
	while(length > 0 && (message[length - 1] == '\n' || message[length - 1] == '\r'))
		length--;

	// Milliseconds since boot, in front of every line.  For a hang the useful
	// question is not only which line came last but how long it sat there, and a
	// bare step list cannot answer that.
	const unsigned int elapsed = (unsigned int)ticks_to_millisecs(gettime());

	LWP_MutexLock(s_logMutex);
	std::fprintf(s_logFile, "[%8u] %.*s\n", elapsed, (int)length, message);
	// Only out of newlib's buffer; commitLog is what reaches the card.
	std::fflush(s_logFile);
	s_logDirty = true;
	LWP_MutexUnlock(s_logMutex);
#else
	(void)message;
#endif
}

void
WiiTraceReport(const char *format, ...)
{
#if CREATE_LOG
	char message[512];
	va_list arguments;
	va_start(arguments, format);
	std::vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);

	WiiTraceLogLine(message);
#ifdef WII_CONSOLE_REPORT
	// Off for the console device: on the Wii SYS_Report draws every byte onto
	// the framebuffer, which scrolled the menus and the game image around.
	// The card copy below is the log; a cable reader can have its output back
	// by defining WII_CONSOLE_REPORT.
	SYS_Report("%s", message);
#endif
#else
	(void)format;
#endif
}

void
WiiTraceService(void)
{
#if CREATE_LOG == 1
	// Events are rare, so this is almost always a single flag test.  When one has
	// been written it is committed on a later frame than the one it happened in,
	// and no more than once a second.
	static u64 s_lastCommit;
	if(!s_logDirty)
		return;
	const u64 now = gettime();
	if(ticks_to_millisecs(now - s_lastCommit) < 1000)
		return;
	s_lastCommit = now;
	commitLog();
#endif
}

void
WiiTraceNote(const char *message)
{
#if CREATE_LOG < 2
	// Nothing reads s_step with the watchdog gone, so this would be a strlen and
	// a copy per model streamed, written into a buffer no one looks at.
	(void)message;
#else
	size_t length = std::strlen(message);
	while(length > 0 &&
	      (message[length - 1] == '\n' || message[length - 1] == '\r'))
		length--;
	if(length >= sizeof(s_step))
		length = sizeof(s_step) - 1;
	std::memcpy(s_step, message, length);
	s_step[length] = '\0';
	s_stepSerial++;
#endif
}

void
WiiTraceHeap(const char *tag)
{
#if CREATE_LOG < 1
	// The timer that used to call this every fifteen seconds is gone (s_heapSampling,
	// off by default), so the only caller left is the watchdog's stall report.  That
	// makes the cost argument that justified the level-2 gate irrelevant: it is
	// reached only when the game has already stopped turning, where nothing about the
	// heap walk can cost anything.  At level 1 this used to compile to nothing, which
	// meant the watchdog reported a freeze with no state attached to it at all.
	(void)tag;
#else
	struct mallinfo info = mallinfo();

	WiiTraceReport("WII heap %s: used=%uK free=%uK blocks=%d top=%uK "
	           "arena1=%uK arena2=%uK\n",
	           tag,
	           (unsigned int)info.uordblks/1024u,
	           (unsigned int)info.fordblks/1024u,
	           info.ordblks,
	           (unsigned int)info.keepcost/1024u,
	           (unsigned int)SYS_GetArena1Size()/1024u,
	           (unsigned int)SYS_GetArena2Size()/1024u);
#endif
}

void
WiiTraceStartWatchdog(void)
{
#if CREATE_LOG < 1
	// Nothing to report to, so no thread to report from.
#else
	if(s_watchdogRunning)
		return;
	s_watchdogRunning = true;
	// Above the game thread on purpose.  A genuine hang is a loop that never
	// yields, and a watchdog that only runs when the main thread lets it would
	// stay silent for exactly the case it exists to report.  It sleeps between
	// checks, so the priority costs nothing.
	if(LWP_CreateThread(&s_watchdogThread, watchdogMain, nullptr,
	                    s_watchdogStack, sizeof(s_watchdogStack), 100) != 0){
		s_watchdogRunning = false;
		WiiTraceReport("WII watchdog: thread create failed, loading stalls will "
		               "not be reported, and debug.log will only reach the card "
		               "when the game exits\n");
	}
#endif
}
