#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <malloc.h>

#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>

#include <fat.h>
#include <gccore.h>
#include <ntfs.h>
#include <ogc/console.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/usb.h>
#include <ogc/usbstorage.h>
#include <wiiuse/wpad.h>

#include "common.h"
#include "crossplatform.h"
#include "audio_enums.h"
#include "DMAudio.h"
#include "FileMgr.h"
#include "Font.h"
#include "Frontend.h"
#include "Game.h"
#include "main.h"
#include "Pad.h"
#include "PCSave.h"
#include "platform.h"
#include "skeleton.h"
#include "Streaming.h"
#include "CdStream.h"
#include "WiiLog.h"
#include "WiiPad.h"
#include "WiiSpeaker.h"
#include "WiiTrace.h"
#include "WiiStdout.h"
// Per-build stamp: refreshed on every link so the banner never lies about the
// revision that is actually running (wii_build_stamp.h lives in the build dir).
#include "wii_build_stamp.h"

extern const char* g_GIT_SHA1;

extern volatile int32 frameCount;

// Declared here rather than by including librw's private gx header, the same way
// TexRead.cpp reaches its sibling setNativeTextureTrace.
namespace rw { namespace gx { void setFrameTrace(bool32 enabled); } }

namespace
{

// On-screen + log echo for boot-critical events; the video initializer owns
// the implementation defined later in the file.
void bootPrintf(const char *format, ...);

// MUY BIEN HARDCODEANDO COSAS 
constexpr unsigned int kFifoSize = 256 * 1024;
constexpr const char *kInstallDirectories[] = {
	"sd:/apps/reVC", "usb:/apps/reVC", "usb2:/apps/reVC",
	"usb3:/apps/reVC", "usb4:/apps/reVC"
};

GXRModeObj *s_renderMode;
void *s_frameBuffers[2];
char s_installDirectory[128] = "sd:/apps/reVC";
char s_userFilesDirectory[128] = "sd:/apps/reVC";

void
onVerticalRetrace(u32)
{
	frameCount++;
}

bool
fileExists(const char *path)
{
	FILE *file = std::fopen(path, "rb");
	if(file == nullptr)
		return false;
	std::fclose(file);
	return true;
}

// Everything up to the last separator of argv[0], which a Wii loader fills in
// with the ELF's own path (usb:/apps/gtavc/boot.dol and the like).
bool
elfDirectory(int argc, char **argv, char *out, size_t size)
{
	if(argc <= 0 || argv == nullptr || argv[0] == nullptr)
		return false;
	const char *lastSeparator = std::strrchr(argv[0], '/');
	if(lastSeparator == nullptr)
		return false;
	const size_t length = (size_t)(lastSeparator - argv[0]);
	if(length == 0 || length >= size)
		return false;
	std::memcpy(out, argv[0], length);
	out[length] = '\0';
	return true;
}

// The failure this guards against is a correct install under a name the list
// below cannot know; only the outcome reaches the log, not each probe.
bool
tryInstallDirectory(const char *directory)
{
	char path[192];
	std::snprintf(path, sizeof(path), "%s/DATA/GTA_VC.DAT", directory);
	if(!fileExists(path))
		return false;
	if(chdir(directory) != 0){
		bootPrintf("WII game boot: chdir failed for %s\n", directory);
		return false;
	}

	std::snprintf(s_installDirectory, sizeof(s_installDirectory), "%s", directory);
	bootPrintf("WII game boot: data found at %s\n", directory);
	return true;
}

bool
selectInstallDirectory(const char *launchDirectory)
{
	// Where the ELF was launched from comes first.  The data sits beside it for
	// anyone who did not name the folder reVC, and the list below cannot know
	// what they did name it -- it only covers a loader that passed no argv for
	// that directory to be derived from.
	if(launchDirectory != nullptr && tryInstallDirectory(launchDirectory))
		return true;

	for(const char *directory : kInstallDirectories)
		if(tryInstallDirectory(directory))
			return true;
	
	// deberia tirar algun mensaje en vez de un logging para la gente gaga en el dolphin!!
	bootPrintf("WII game boot: DATA/GTA_VC.DAT not found\n");
	return false;
}

// The one reliable test of whether a volume takes writes: a read-only NTFS
// mount and an absent SD card both simply fail the create.
bool
directoryAcceptsWrites(const char *directory)
{
	char path[192];
	std::snprintf(path, sizeof(path), "%s/writetest.tmp", directory);
	FILE *file = std::fopen(path, "wb");
	if(file == nullptr)
		return false;
	std::fclose(file);
	std::remove(path);
	return true;
}

// mkdir -p for a "device:/a/b" path; whatever already exists is left alone.
void
makeDirectories(const char *path)
{
	char partial[128];
	std::snprintf(partial, sizeof(partial), "%s", path);
	char *scan = std::strchr(partial, ':');
	scan = scan != nullptr ? scan + 1 : partial;
	while(*scan == '/')
		scan++;
	for(; *scan != '\0'; scan++){
		if(*scan != '/')
			continue;
		*scan = '\0';
		mkdir(partial, 0777);
		*scan = '/';
	}
	mkdir(partial, 0777);
}

// Saves and settings live beside the game when the game is on the SD card, as
// they always have.  A game on USB keeps them on the SD card too, which is the
// only place they can go when the stick is NTFS (mounted read-only), so the
// folder is created if the card does not have it yet: nothing else would, and
// every save would fail on the missing directory.  Only when the card does not
// take writes do they follow the game onto the stick, if that is writable.
// When nothing does, the SD default stays and the failure is the plain "cannot
// save" rather than a path nobody expected.
void
selectUserFilesDirectory()
{
	if(std::strncmp(s_installDirectory, "sd:", 3) == 0){
		std::snprintf(s_userFilesDirectory, sizeof(s_userFilesDirectory),
		              "%s", s_installDirectory);
		return;
	}
	makeDirectories(s_userFilesDirectory);
	if(!directoryAcceptsWrites(s_userFilesDirectory) &&
	   directoryAcceptsWrites(s_installDirectory))
		std::snprintf(s_userFilesDirectory, sizeof(s_userFilesDirectory),
		              "%s", s_installDirectory);
}

bool
initializeVideo()
{
	VIDEO_Init();
	s_renderMode = VIDEO_GetPreferredMode(nullptr);
	for(void *&frameBuffer : s_frameBuffers){
		frameBuffer = MEM_K0_TO_K1(SYS_AllocateFramebuffer(s_renderMode));
		if(frameBuffer == nullptr)
			return false;

		// ESTA MIERDA SE COLOCA PORQUE SI NO SE BUGEAN TODOS LOS GRAFICOS por el framebuffer que viene ya garchado de los menus de la wii
		// en dolphin no pasa
		VIDEO_ClearFrameBuffer(s_renderMode, frameBuffer, COLOR_BLACK);
	}

	VIDEO_Configure(s_renderMode);
	VIDEO_SetNextFramebuffer(s_frameBuffers[0]);
	VIDEO_SetBlack(FALSE);
	VIDEO_SetPostRetraceCallback(onVerticalRetrace);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if(s_renderMode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();

	// Mirror every boot-critical trace onto the screen: debug.log lives next
	// to the DOL and is harmless to copy, but an empty log cannot tell apart a
	// wedged device call from one that never ran.  The console draws into
	// buffer 0 before the game fills both buffers, so the last visible line
	// under a black screen is exactly the stage reached.  It comes up before
	// GX_Init so even a GX-phase failure leaves its banner behind.
	console_init(s_frameBuffers[0], 10, 10,
	             s_renderMode->fbWidth, s_renderMode->efbHeight,
	             s_renderMode->fbWidth * VI_DISPLAY_PIX_SZ);
	std::printf("[boot] video ok\n");

	void *fifo = memalign(32, kFifoSize);
	if(fifo == nullptr){
		std::printf("[boot] DevExpress fifo alloc failed\n");
		return false;
	}
	std::memset(fifo, 0, kFifoSize);
	std::printf("[boot] GX init\n");
	GX_Init(fifo, kFifoSize);
	std::printf("[boot] GX ok\n");

	// para el background bien
	GXColor background = { 0, 0, 0, 255 };
	GX_SetCopyClear(background, GX_MAX_Z24);
	GX_SetViewport(0.0f, 0.0f, s_renderMode->fbWidth,
	               s_renderMode->efbHeight, 0.0f, 1.0f);
	GX_SetDispCopyYScale((f32)s_renderMode->xfbHeight/(f32)s_renderMode->efbHeight);
	GX_SetScissor(0, 0, s_renderMode->fbWidth, s_renderMode->efbHeight);
	GX_SetDispCopySrc(0, 0, s_renderMode->fbWidth, s_renderMode->efbHeight);
	GX_SetDispCopyDst(s_renderMode->fbWidth, s_renderMode->xfbHeight);
	GX_SetCopyFilter(s_renderMode->aa, s_renderMode->sample_pattern,
	                 GX_TRUE, s_renderMode->vfilter);
	GX_SetFieldMode(s_renderMode->field_rendering,
	                s_renderMode->viHeight == 2*s_renderMode->xfbHeight ?
	                GX_ENABLE : GX_DISABLE);
	GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetDispCopyGamma(GX_GM_1_0);
	GX_CopyDisp(s_frameBuffers[1], GX_TRUE);
	std::printf("[boot] GX framebuffer ready\n");

	return true;
}

// The boot console: under CREATE_LOG 0 the report half is a no-op and this
// moves printf bytes through the (silent) console device, so a normal boot
// logs and screens nothing.  It stays for the failure paths -- haltBoot only
// has the television -- where the stage name shows live instead of hiding a
// wedged device behind a silent black frame.
void
bootPrintf(const char *format, ...)
{
	char message[512];
	va_list arguments;
	va_start(arguments, format);
	std::vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);

	WiiTraceReport("%s", message);
	std::printf("[boot] %s", message);
}

[[noreturn]] void
haltBoot(const char *stage)
{
	WiiTraceReport("WII game boot: halted at %s\n", stage);
	// Nothing after this ever runs, and the watchdog that would otherwise commit
	// the log may not have been started yet at this point in the boot, so the
	// reason for the halt is written out here or not at all.  The console is
	// the output the player can actually watch while the box sits black below.
	std::printf("[boot] HALTED at %s\n", stage);
	WiiTraceCloseLog();
	while(true)
		VIDEO_WaitVSync();
}

} // namespace

// Overrides libogc's weak symbol to make Arena2 the one and only sbrk arena.
// At its default of 0 the whole heap is what is left of MEM1 once the ELF, the
// two framebuffers and the GX FIFO are paid for, and the watchdog's heap line
// shows that running out: arena1 at zero with MEM2 untouched beside it.
//
// The cost is latency, because MEM2 is GDDR3 and everything malloc'd moves
// there.  If that ever measures badly the answer is a small MEM1 allocator for
// chosen buffers, not flipping this back.
//
// It has to be initialised data rather than something a constructor assigns:
// allocations happen during libc and static init, before any constructor of
// ours could run.  The extern "C" block is what keeps it from becoming a
// mangled C++ symbol that would not override anything.
extern "C" {
u32 MALLOC_MEM2 = 1;
}

long _dwOperatingSystemVersion = 0;
size_t _dwMemAvailPhys = 48 * 1024 * 1024;
RwUInt32 gGameState = 0;

extern "C" void
wiiLog(const char *format, ...)
{
#if CREATE_LOG < 2
	// The one that actually costs something.  The engine calls this for every
	// model, texture, collision file and audio stream it touches, and streaming
	// touches thousands of them while driving -- each one formatting into the
	// 512 byte buffer below before anything decides whether it is wanted.
	// Leaving early is what makes CREATE_LOG 0 worth switching to.
	(void)format;
#else
	char message[512];
	va_list arguments;
	va_start(arguments, format);
	std::vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	WiiTraceNote(message);
	WiiTraceLogLine(message);
#endif
}

double
psTimer(void)
{
	return ticks_to_millisecs(gettime());
}

RwBool
psInitialize(void)
{
	RsGlobal.ps = nullptr;
	RsGlobal.maximumWidth = s_renderMode->fbWidth;
	RsGlobal.maximumHeight = s_renderMode->efbHeight;
	RsGlobal.width = s_renderMode->fbWidth;
	RsGlobal.height = s_renderMode->efbHeight;
	CFileMgr::Initialise();

	// librw is built as its own target and cannot see CREATE_LOG, so the switch
	// is carried across here rather than compiled in over there.
	rw::gx::setFrameTrace(CREATE_LOG >= 2);

// Saves land beside the game data, which is where the desktop skeletons put
// them too -- glfw.cpp, sdl2.cpp and win.cpp all make this same call from
// their own initialisation.  Without it DefaultPCSaveFileName stays empty and
// every slot is written to a bare "1.b" in whatever the current directory
// happens to be.  The directory is read straight off the variable rather than
// through _psGetUserFilesFolder, which returns it but is defined further down
// with the rest of the platform hooks.
//
// The user files (saves, audio cache) cannot live where the assets live when
// that volume is read-only (an NTFS stick); keep them beside the DOL's usual
// spot on sd: instead.  The pre-run default matches the historical behavior
// for the common whole-install-on-SD layout.
C_PcSave::SetSaveDirectory(s_userFilesDirectory);
	return TRUE;
}

void psTerminate(void) {}

void
psCameraShowRaster(RwCamera *camera)
{
	RwCameraShowRaster(camera, nullptr, rwRASTERFLIPWAITVSYNC);
}

RwBool
psCameraBeginUpdate(RwCamera *camera)
{
	return RwCameraBeginUpdate(camera) != nullptr;
}

RwImage*
psGrabScreen(RwCamera *camera)
{
	rw::Image *image = RwCameraGetRaster(camera)->toImage();
	if(image)
		image->removeMask();
	return image;
}

void psMouseSetPos(RwV2d *) {}
RwBool psSelectDevice() { return TRUE; }
RwMemoryFunctions *psGetMemoryFunctions(void) { return nullptr; }
RwBool psInstallFileSystem(void) { return TRUE; }
RwBool psNativeTextureSupport() { return TRUE; }
const char *_psGetUserFilesFolder() { return s_userFilesDirectory; }

// Write artifacts (audio length cache etc.) have to stay out of the install
// tree when that volume is read-only NTFS; flatten such files into the
// user-files directory on sd: instead, while a whole-install-on-SD layout
// keeps taking the historical relative path.
const char *
WiiUserCachePath(const char *relative)
{
	static char path[192];
	if(std::strcmp(s_installDirectory, s_userFilesDirectory) == 0)
		return relative;
	const char *leaf = relative;
	for(const char *scan = relative; (scan = strchr(scan, '/')) != nullptr; scan++)
		leaf = scan + 1;
	std::snprintf(path, sizeof(path), "%s/%s", s_userFilesDirectory, leaf);
	return path;
}
void _InputTranslateShiftKeyUpDown(RsKeyCodes *) {}
long _InputInitialiseMouse(bool) { return 0; }
void _InputShutdownMouse() {}
bool _InputMouseNeedsExclusive() { return false; }
void _InputInitialiseJoys() {}
void InitialiseLanguage() {}
void _psSelectScreenVM(RwInt32) {}
RwBool _psSetVideoMode(RwInt32, RwInt32) { return TRUE; }

// The PC frontend lists its video modes through this pointer and derefs it
// blindly inside MENUACTION_SCREENRES drawing (Frontend.cpp), so a nullptr
// stub crashed the Display/Graphics page the first time it drew the row.  The
// Wii has exactly one hardware video mode, which is what the list says.
const char *wiiVideoModes[] = { "480i", nullptr };
RwChar **_psGetVideoModeList() { return const_cast<RwChar **>(wiiVideoModes); }
RwInt32 _psGetNumVideModes() { return 1; }

void
CapturePad(RwInt32 padID)
{
	if(padID < 0 || padID >= MAX_PADS)
		return;

	CPad *pad = CPad::GetPad(padID);
	CControllerState &state = pad->PCTempJoyState;
	state.Clear();
	WiiPadCapture(padID, state);
}

// The single exit door, and the reason it is worth a log line of its own: a run
// that ends here wrote this line, and a run that ended any other way -- an
// unhandled exception, a failed allocation, a stack that ran off the end -- did
// not.  So whether debug.log ends with this or simply stops mid-file is what
// separates "something asked the game to quit" from "the game died", which are
// entirely different investigations.
void
HandleExit()
{
	if(!RsGlobal.quit)
		WiiTraceReport("WII game boot: exit requested\n");
	RsGlobal.quit = TRUE;
}

namespace
{

// The console's own buttons, and the Wiimote's power button.  They are
// registered rather than polled because the reset button and both power buttons
// are delivered as callbacks and never appear in any pad state, so there is
// nothing to poll for.
//
// All three go through HandleExit rather than exiting on the spot: it raises the
// same RsGlobal.quit the frontend's Quit option raises, so the main loop below
// unwinds and shuts the game down the one way it already knows how.  Calling
// SYS_ResetSystem from inside the callback would cut the frame in half instead.
// Each names itself first, because HandleExit cannot tell who called it and
// these three are indistinguishable from a menu Quit once the flag is set.
void
onResetButton(u32, void*)
{
	WiiTraceReport("WII system: reset button\n");
	HandleExit();
}

void
onPowerButton(void)
{
	WiiTraceReport("WII system: power button\n");
	HandleExit();
}

void
onWiimotePowerButton(s32)
{
	WiiTraceReport("WII system: wiimote power button\n");
	HandleExit();
}

// fatInitDefault() covers whichever device booted us, but leaves the other
// side unmounted; a DOL launched from usb:/apps/reVC with an SD card also
// inserted (or assets parked on usb: with the DOL on SD) otherwise fails the
// data lookup and bounces HBC straight back to its SD listing.

// The NTFS-3G port reports what actually refused the mount through its own
// logging channel, not errno (which the Wii port leaves at zero across that
// path).  Its handler is routed into bootPrintf, which reaches debug.log --
// the exact failing NTFS call is on the card instead of only somewhere under
// haltBoot.  These five come from libntfs's logging.h, which is not a public
// header for consumers.
typedef int (ntfs_log_handler)(const char *function, const char *file, int line,
	unsigned int level, void *data, const char *format, va_list arguments);
extern "C" void ntfs_log_set_handler(ntfs_log_handler *handler);
extern "C" unsigned int ntfs_log_set_levels(unsigned int levels);
#define REVC_NTFS_LOG_QUIET  (1u << 2)
#define REVC_NTFS_LOG_INFO   (1u << 3)
#define REVC_NTFS_LOG_VERBOSE (1u << 4)
#define REVC_NTFS_LOG_PROGRESS (1u << 5)
#define REVC_NTFS_LOG_WARNING (1u << 6)
#define REVC_NTFS_LOG_ERROR  (1u << 7)
#define REVC_NTFS_LOG_PERROR (1u << 8)
#define REVC_NTFS_LOG_CRITICAL (1u << 9)

int
ntfsWiiLogHandler(const char *function, const char *file, int line,
                  unsigned int level, void *data, const char *format,
                  va_list arguments)
{
	char message[256];
	(void)file;
	(void)data;
	(void)line;
	std::vsnprintf(message, sizeof(message), format, arguments);
	bootPrintf("NTFS(%s) %s: %s\n",
	           level & REVC_NTFS_LOG_CRITICAL ? "critical" :
	           level & REVC_NTFS_LOG_ERROR ? "error" :
	           level & REVC_NTFS_LOG_PERROR ? "perror" :
	           level & REVC_NTFS_LOG_WARNING ? "warning" : "info",
	           function, message);
	return 1;
}

static bool
mountUsbStorage()
{
	// USB_Initialize under HBC has no timeout of its own; report the return so
	// a wedged USB stack is distinguishable from a wedged device.
	s32 usbInit = USB_Initialize();
	bootPrintf("WII storage: USB_Initialize=%d\n", usbInit);

	// USB mass storage registers a beat or two behind the SD on real hardware,
	// especially after a launch straight out of HBC.  Retry tight: 50 checks at
	// 100 ms keeps the worst case near five seconds without the coarse 1 s
	// steps that made a slow stick look like a hang.  Only the outcomes reach
	// the log, so a mount that works is one line and a total failure lists the
	// two attempts that got furthest.
	for(int attempt = 0; attempt < 50; attempt++){
		if(attempt > 0)
			usleep(100000);
		if(fatMountSimple("usb", &__io_usbstorage)){
			bootPrintf("WII storage: usb: mounted as FAT on attempt %d\n", attempt + 1);
			return true;
		}

		// libogc's FAT driver only speaks FAT: an NTFS stick (the most common
		// thing to plug in) stays invisible to every fallback path below.  The
		// vendored read-only NTFS-3G port picks it up instead.  Find the first
		// partition explicitly, because ntfsMount needs a start sector; most
		// sticks carry exactly one NTFS volume, so that is what gets used.
		if((attempt & 3) == 3){
			sec_t *ntfsPartitions = nullptr;
			int partitionCount =
				ntfsFindPartitions(&__io_usbstorage, &ntfsPartitions);
			bool ntfsMounted = false;
			if(partitionCount > 0 && ntfsPartitions != nullptr) {
				// ntfsInit has run by now (behind ntfsFindPartitions), so the
				// default null handler is in place; the hook takes it over
				// before the mount's internal logging does anything.
				// Subscribe to problems only: INFO/VERBOSE/PROGRESS chatter
				// (open/close/updating-times, several lines per file) made a
				// busy boot write thousands of console+card lines and was
				// itself a big boot-time cost.  Step ladders in the vendor
				// now emit DBG-level messages and stay silent here.
				ntfs_log_set_handler(ntfsWiiLogHandler);
				ntfs_log_set_levels(REVC_NTFS_LOG_WARNING |
				                    REVC_NTFS_LOG_ERROR | REVC_NTFS_LOG_PERROR |
				                    REVC_NTFS_LOG_CRITICAL);
				// Removable sticks routinely carry the dirty/hibernated marks
				// Windows fast-startup and un-ejected removals leave; the
				// plain NTFS_DEFAULT mount refuses those on sight, so recover
				// explicitly.  The driver is mounted read-only, so running the
				// recovery loop stays safe.  Case handling stays case-
				// insensitive: on-stick names arrive from xcopy in whatever
				// case Windows stored them ("data/gta_vc.dat") while the game
				// walks "DATA/GTA_VC.DAT"; libntfs's IGNORE_CASE is what makes
				// either form resolve.  (fcaseopen/casepath complements it for
				// exact-case callers.)
				ntfsMounted = ntfsMount("usb", &__io_usbstorage, ntfsPartitions[0],
				                        CACHE_DEFAULT_PAGE_COUNT,
				                        CACHE_DEFAULT_PAGE_SIZE,
				                        NTFS_FORCE | NTFS_READ_ONLY | NTFS_IGNORE_CASE);
			}
			std::free(ntfsPartitions);
			if(ntfsMounted){
				bootPrintf("WII storage: usb: mounted as NTFS on attempt %d\n", attempt + 1);
				return true;
			}
		}
	}
	bootPrintf("WII storage: usb: could not be mounted\n");
	return false;
}

} // namespace

// The writer swap lives outside the anonymous namespace only because the
// devoptab symbol is a plain C name; nothing here reaches the engine.
#include <reent.h>
#include <sys/iosupport.h>

namespace {

devoptab_t *s_stdoutWrapper;
const devoptab_t *s_consoleDevoptab;	// libogc's own, which stdout's cookie points at

ssize_t
stdoutSwallow(struct _reent *r, void *fd, const char *ptr, size_t len)
{
	// On the Wii the console device draws every byte onto the game's
	// framebuffer, so printf scrolling pushed the menus and the game image
	// around.  Upstream reads fine on PC, so instead of hunting every printf
	// call the console device itself gets a silent writer: a swallow that
	// counts the bytes, draws nothing and logs nothing.  The log goes through
	// wiiLog/WiiTraceReport only, which is the line the diagnostics use.
	(void)r; (void)fd; (void)ptr;
	return len;
}

} // namespace

#ifdef NINTENDO_WII
// --- a crash has to be caught at the moment it happens ------------------------
// A line-oriented log cannot tell you where a hard fault was.  Everything it
// holds was written by frames that already finished; the faulting frame writes
// nothing at all, because the process is gone before it gets to.  That is
// exactly what a real log showed: no stall line before the end, and a tail of
// unrelated streaming chatter that correlated with nothing.
//
// So the fault gets its own writer.  Three rules make it safe inside a signal
// handler: no printf and no snprintf (both allocate), no mutex (the log mutex
// may well be the thing that was held when it faulted), and the message is
// assembled with a hand-rolled hex writer into a stack buffer.  write() is
// async-signal-safe, so one call to it is all it takes to put the faulting
// address on the card.
//
// After writing it hands the signal back to the default disposition and
// re-raises, so the console still does whatever it would normally have done
// rather than this swallowing the crash.
static int s_crashFd = -1;

static char *
putHex(char *p, u32 v)
{
	static const char digits[] = "0123456789ABCDEF";
	for(int shift = 28; shift >= 0; shift -= 4)
		*p++ = digits[(v >> shift) & 0xF];
	return p;
}

static void
crashHandler(int sig)
{
	char line[64];
	char *p = line;

	const char *tag = "WII CRASH sig=";
	while(*tag)
		*p++ = *tag++;
	p = putHex(p, (u32)sig);
	*p++ = ' ';
	tag = "t=";
	while(*tag)
		*p++ = *tag++;
	p = putHex(p, (u32)ticks_to_millisecs(gettime()));
	*p++ = '\n';

	if(s_crashFd >= 0){
		// The one async-signal-safe call here, and the whole point of the handler.
		ssize_t written = write(s_crashFd, line, (size_t)(p - line));
		(void)written;
	}

	// Back to the default disposition and re-raise, so the crash still happens
	// rather than being swallowed -- and so libogc's own handler, if it installs
	// one, gets its turn afterwards.
	signal(sig, SIG_DFL);
	raise(sig);
}

// Installed once the log has a file to write to, since the handler needs its
// descriptor.
//
// Deliberately signal() and not sigaction(): devkitPro's bare-metal newlib has
// no SA_SIGINFO, no si_addr and no sa_sigaction, so the POSIX route does not
// exist here and the faulting context is not available through the standard
// handler.  The consequence is that this records THAT and WHEN a fault happened
// but not the faulting PC -- that needs libogc's own exception API, which is a
// separate piece of work.
static void
installCrashHandler(void)
{
	const int signals[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE };
	for(unsigned i = 0; i < sizeof(signals)/sizeof(signals[0]); i++){
		if(signal(signals[i], crashHandler) == SIG_ERR)
			WiiTraceReport("WII crash: cannot handle signal %d\n", signals[i]);
	}
	WiiTraceReport("WII crash: handler installed\n");
}
#endif

void
WiiStdoutHookInstall(void)
{
	if(s_stdoutWrapper != nullptr)
		return;
	const devoptab_t *original = devoptab_list[1];
	if(original == nullptr)
		return;

	// Patched in place, which is the part that actually matters.  console_init
	// opens "CON:" into the stdout FILE, and newlib writes through the cookie
	// that fopen stored -- a pointer to libogc's devoptab, captured then and
	// never looked up again.  Swapping the entry in devoptab_list changes what
	// the table says slot 1 is, which nothing consults, so a copy installed here
	// sits there inert while every printf goes straight to the framebuffer.  The
	// console devoptab is ordinary data in the linked image, so overwriting its
	// write_r does reach the FILE, and that is what silences the ~250 printf
	// calls the engine makes.
	devoptab_t *patched = (devoptab_t*)original;
	patched->write_r = stdoutSwallow;

	// Still installed in the table as well, and still worth it: it covers
	// anything that resolves the devoptab by fd rather than by cookie, which is
	// how the game's own file access reaches stdio.
	devoptab_t *wrapper = new (std::nothrow) devoptab_t;
	if(wrapper == nullptr)
		return;
	*wrapper = *patched;
	wrapper->name = "consoleQuiet";
	wrapper->write_r = stdoutSwallow;
	s_stdoutWrapper = wrapper;
	s_consoleDevoptab = original;
	devoptab_list[1] = wrapper;
}

// The port requires a Nunchuk: the stick does all walking and steering, so
// without one there is nothing to move with.  Rather than drop the player into a
// game they cannot control, block here with a centered notice -- the same idea
// as a Wii game that needs an accessory -- and re-check every frame, so plugging
// a Nunchuk in walks straight past it with no keypress.  Returns true once one is
// present.  The screen is drawn with the game's own font on the dark background,
// matching how a console title shows such a notice.
static bool
waitForNunchuk(void)
{
	static bool logged;
	if(!logged){
		logged = true;
		WiiTraceReport("WII pad: waiting for a Nunchuk controller\n");
	}

	// The frontend textures carry the font, and this runs before the menu would
	// have loaded them, so pull them in first.
	FrontEndMenuManager.LoadAllTextures();

	static wchar title[64];
	static wchar body[128];
	static bool stringsBuilt = false;
	if(!stringsBuilt){
		const char *a = "This game requires a Nii Remote(TM) Nunchuk Controller";
		const char *b = "Please connect a Nunchuk to your Wii Remote, then continue.";
		for(int i = 0; a[i] && i < 63; i++) title[i] = (wchar)a[i];
		title[63] = 0;
		for(int i = 0; b[i] && i < 127; i++) body[i] = (wchar)b[i];
		body[127] = 0;
		stringsBuilt = true;
	}

	while(!RsGlobal.quit && !WiiPadNunchukConnected()){
		DoRWStuffStartOfFrame(0, 0, 0, 0, 0, 0, 255);

		CFont::SetBackgroundOff();
		CFont::SetScale(SCREEN_SCALE_X(1.0f), SCREEN_SCALE_Y(1.35f));
		CFont::SetJustifyOn();
		CFont::SetFontStyle(FONT_HEADING);
		CFont::SetColor(CRGBA(255, 255, 255, 255));
		CFont::SetDropShadowPosition(2);
		CFont::PrintString(SCREEN_WIDTH / 2 - SCREEN_SCALE_X(1.0f),
		                   SCREEN_SCALE_Y(32.0f) + SCREEN_SCALE_Y(2.0f), title);
		CFont::SetColor(CRGBA(0, 0, 0, 255));
		CFont::PrintString(SCREEN_WIDTH / 2 + SCREEN_SCALE_X(1.0f),
		                   SCREEN_SCALE_Y(32.0f) + SCREEN_SCALE_Y(2.0f), title);

		CFont::SetScale(SCREEN_SCALE_X(0.8f), SCREEN_SCALE_Y(1.35f));
		CFont::SetFontStyle(FONT_STANDARD);
		CFont::SetColor(CRGBA(0, 0, 0, 255));
		CFont::PrintString(SCREEN_WIDTH / 2 - SCREEN_SCALE_X(1.0f), SCREEN_SCALE_Y(22.0f), body);
		CFont::SetColor(CRGBA(255, 255, 255, 255));
		CFont::PrintString(SCREEN_WIDTH / 2 + SCREEN_SCALE_X(1.0f), SCREEN_SCALE_Y(22.0f), body);

		CFont::DrawFonts();
		DoRWStuffEndOfFrame();
		VIDEO_WaitVSync();

		// Keep the input stack ticking so the Nunchuk is seen the instant it is
		// plugged in, and so HOME/quit still work while we wait.
		WiiPadScan();
		WiiSpeakerService();
		WiiTraceService();
	}

	if(!RsGlobal.quit){
		WiiTraceReport("WII pad: Nunchuk connected, continuing\n");
		return true;
	}
	return false;
}

// A frame that takes this long is not a frame, it is a freeze.  Far above
// anything this port manages on an empty street and far below anything a player
// would put up with as "a slow patch", which is the whole point: the number only
// has to separate normal from not-normal.
static const unsigned int kStallFrameMs = 400;
// And once a stall has been reported, this long before another one is.  A single
// freeze then costs exactly one line.
static const unsigned int kStallReportGapMs = 5000;

// Both of the ways a game can start, factored out because a running game has to be
// able to start another one and that is the case the boot path never exercises.
//
// `teardownFirst` is what separates them.  InitialiseGame is CGame::Initialise and
// nothing else -- it builds the pools and the world and does not clear anything --
// so run over a live world it builds a second game on top of the first.  Coming
// here from GS_PLAYING_GAME means there IS a live world, so it has to come down
// first; coming from the boot frontend there is nothing to bring down.
static void
startSavedGame(bool teardownFirst)
{
	if(teardownFirst)
		CGame::ShutDownForRestart();

	WiiTraceReport("WII game: loading saved game\n");
	WiiTraceHeap("pre-load");

	// Three steps and not one, and the order is the whole thing.  InitialiseGame
	// builds the pools, streaming and world that the save is restored into, so it
	// has to run first; ShutDownForRestart then clears what it placed; and
	// InitialiseWhenRestarting is the one that actually reads the slot -- it is
	// what consumes m_bWantToLoad and calls GenericLoad.  Same order as glfw.cpp
	// and sdl2.cpp.  InitialiseGame on its own never reaches GenericLoad, which is
	// why picking a slot used to start the story over from the beginning.
	InitialiseGame();
	CGame::ShutDownForRestart();
	CGame::InitialiseWhenRestarting();
	DMAudio.ChangeMusicMode(MUSICMODE_GAME);

	FrontEndMenuManager.m_bGameNotLoaded = false;
	// Both cleared here as well as inside InitialiseWhenRestarting, which does it
	// itself on the success path but NOT on the failure path (Game.cpp leaves it
	// false after its own error handling).  A stale true here is not cosmetic: it
	// is the condition the next InitialiseWhenRestarting tests to decide whether
	// to load at all, so leaving it set makes a later unrelated start take the
	// load branch at a moment nothing asked for one.
	FrontEndMenuManager.m_bWantToLoad = false;
	FrontEndMenuManager.m_bWantToRestart = false;

	// DoSettingsBeforeStartingAGame fades both to zero on its way out, and the only
	// code that puts them back is InitialiseOnceAfterRW, which does not run again
	// after boot.  Harmless on the boot path (they are already at 127), essential on
	// the in-game one, which is otherwise a silent game.
	DMAudio.SetEffectsFadeVol(127);
	DMAudio.SetMusicFadeVol(127);

	gGameState = GS_PLAYING_GAME;
	WiiTraceHeap("post-load");
}

static void
startFreshGame(bool teardownFirst)
{
	if(teardownFirst)
		CGame::ShutDownForRestart();

	WiiTraceReport("WII game: starting new game\n");
	WiiTraceHeap("pre-load");
	InitialiseGame();

	FrontEndMenuManager.m_bGameNotLoaded = false;
	FrontEndMenuManager.m_bWantToLoad = false;
	FrontEndMenuManager.m_bWantToRestart = false;
	DMAudio.SetEffectsFadeVol(127);
	DMAudio.SetMusicFadeVol(127);

	gGameState = GS_PLAYING_GAME;
	WiiTraceHeap("post-load");
}

int
main(int argc, char **argv)
{
	// Print the banner as the very first area on the television once the
	// console exists: a build with the wrong devkitPro/libogc pairing then
	// announces itself (GCC rev mirrors the toolchain install) instead of
	// hiding behind a silent black screen.
	//
	// Video stays ahead of storage.  initializeVideo ends in console_init, which
	// installs libogc's console as stdout and so overwrites the silent writer
	// WiiStdoutHookInstall puts there; with the hook installed first, every
	// engine printf lands back on the screen.  And a haltBoot before VIDEO_Init
	// would wait on a VSync nothing has set up.
	if(!initializeVideo()){
		WiiTraceReport("WII game boot: video=failed\n");
		haltBoot("video");
	}

	if(!fatInitDefault())
		haltBoot("storage mount");

	char launchPath[128];
	const char *launchDirectory =
		elfDirectory(argc, argv, launchPath, sizeof(launchPath)) ? launchPath : nullptr;

	// As early as storage allows, so a boot that never reaches the frontend still
	// leaves the reason behind.  The log has to be up before the USB mount runs:
	// its attempts are the detail most likely to explain a failed data lookup,
	// and lines written before open go nowhere.  The second call covers a loader
	// that passed no argv, and is a no-op once the first has already opened the file.
	WiiTraceOpenLog(launchDirectory);
	// A DOL launched from a read-only stick cannot keep its log beside itself;
	// the SD card's app folder is the fallback, when it exists yet.
	WiiTraceOpenLog(s_userFilesDirectory);

	// The stamp is on the screen AND in the log: whichever medium reaches the
	// user carries the exact build that ran, so a stale DOL can no longer pose.
	bootPrintf("build " REVC_WII_PLATFORM " gcc " __VERSION__ "\n");
	bootPrintf("dol %s   built %s\n", g_GIT_SHA1, WII_BUILD_STAMP);
	bootPrintf("WII game boot: launch dir=%s\n", launchDirectory ? launchDirectory : "(none)");

	// Best effort: without usb: the data lookup can still find SD installs.
	if(!mountUsbStorage())
		bootPrintf("WII storage: continuing without usb:\n");

	if(!selectInstallDirectory(launchDirectory))
		haltBoot("game data lookup");
	WiiTraceOpenLog(s_installDirectory);

	selectUserFilesDirectory();
	WiiTraceOpenLog(s_userFilesDirectory);
	bootPrintf("WII game boot: user files dir=%s\n", s_userFilesDirectory);

	// Now that there is a log to write to, catch a hard fault where it happens.
	// Placed here rather than earlier because the handler needs the descriptor.
	s_crashFd = WiiTraceLogFd();
	installCrashHandler();

	// Now that the boot banners are on the screen, stop the console device from
	// taking any more of it.  Installed here rather than before the banners for
	// the obvious reason, and installed over the console devoptab in place rather
	// than over the table slot, because the table slot was never the path: see
	// WiiStdoutHookInstall.
	WiiStdoutHookInstall();
	WiiTraceReport("WII log: console devoptab %s, stdout slot %s\n",
	               s_consoleDevoptab ? s_consoleDevoptab->name : "(none)",
	               devoptab_list[1] ? devoptab_list[1]->name : "(none)");

	// psInitialize stores exactly these two into RsGlobal, and the pointer has to
	// report against the same pair, so both are taken from the render mode here
	// rather than from RsGlobal, which is still empty this early.
	WiiPadInitialise(s_renderMode->fbWidth, s_renderMode->efbHeight);
	SYS_SetResetCallback(onResetButton);
	SYS_SetPowerCallback(onPowerButton);
	WPAD_SetPowerButtonCallback(onWiimotePowerButton);
	WiiTraceStartWatchdog();
	WiiTraceHeap("boot");

	bootPrintf("WII game boot: rsINITIALIZE\n");
	if(RsEventHandler(rsINITIALIZE, nullptr) == rsEVENTERROR)
		haltBoot("rsINITIALIZE");

	rw::EngineOpenParams openParameters = {
		{ s_frameBuffers[0], s_frameBuffers[1] },
		s_renderMode->fbWidth,
		s_renderMode->efbHeight
	};
	bootPrintf("WII game boot: rsRWINITIALIZE\n");
	if(RsEventHandler(rsRWINITIALIZE, &openParameters) == rsEVENTERROR){
		bootPrintf("WII game boot: rsRWINITIALIZE=failed\n");
		haltBoot("rsRWINITIALIZE");
	}

	bootPrintf("WII game boot: RenderWare initialized\n");

	// Read the settings back, here and not after InitialiseOnceAfterRW below.
	//
	// On PC this is the OS skeleton's job -- skel/glfw.cpp does it long before
	// anything else -- and the Wii has no skeleton, so without it nothing ever
	// loads reVC.ini.  Every option the player set was written correctly and then
	// silently ignored, and came back as its default on the next launch.
	//
	// It has to sit above InitialiseOnceAfterRW rather than below it, because that
	// function CONSUMES the audio preferences (it pushes the volumes, the speaker
	// config and the 3D provider into DMAudio).  Loading them afterwards would mean
	// a saved volume only took effect on the launch after next.  It also pulls in
	// the stored bindings and then re-asserts this port's own defaults over them
	// (WiiPadApplyControlDefaults at the end of LoadSettings), which is the order
	// we want.
	bootPrintf("WII game boot: loading settings\n");
	FrontEndMenuManager.LoadSettings();

	bootPrintf("WII game boot: InitialiseOnceAfterRW\n");
	if(!CGame::InitialiseOnceAfterRW())
		haltBoot("InitialiseOnceAfterRW");
	bootPrintf("WII game boot: core services initialized\n");

	// The game state machine the PC skeleton drives from its message loop.
	// The movie and PS2 memory card states have no counterpart here, so boot
	// straight into the frontend, which loads the game data only once a game
	// is actually started.
	FrontEndMenuManager.m_bGameNotLoaded = true;
	FrontEndMenuManager.m_bStartUpFrontEndRequested = true;
	gGameState = GS_FRONTEND;
	WiiTraceReport("WII game boot: entering frontend\n");

	// No Nunchuk, no game.  This blocks on a centered notice and returns as soon
	// as one is plugged in; RsGlobal.quit (power/reset/HOME-to-exit) still breaks.
	if(!waitForNunchuk())
		return 0;

	u64 lastStallReport = 0;

	while(!RsGlobal.quit){
		const u64 frameStart = gettime();
		WiiTraceSetStep("frame begin");
		// Also cleared here so a stall reported on the frame after a reset cannot
		// inherit the previous one's worst wait.  Redundant with the clear below
		// on any frame that runs to completion, and this is the one that cannot.
		CdStreamResetWaitStats();
		switch(gGameState){
		case GS_FRONTEND:
			WiiTraceSetStep("frontend idle");
			RsEventHandler(rsFRONTENDIDLE, nullptr);
			if(FrontEndMenuManager.m_bWantToLoad){
				startSavedGame(false);
				WiiTraceReport("WII game boot: save loaded, entering game\n");
			}else if(!FrontEndMenuManager.m_bMenuActive)
				gGameState = GS_INIT_PLAYING_GAME;
			break;

		case GS_INIT_PLAYING_GAME:
			startFreshGame(false);
			WiiTraceReport("WII game boot: game data initialized\n");
			break;

		case GS_PLAYING_GAME:
			WiiTraceSetStep("playing idle");
			RsEventHandler(rsIDLE, (void*)TRUE);
			// Load Game or New Game, chosen from the pause menu.
			//
			// The desktop skeletons run this same switch inside
			//     while(!quit && !m_bWantToRestart && !SDL_QuitRequested())
			// so m_bWantToRestart is what tears the running game down and starts it
			// again -- and DoSettingsBeforeStartingAGame sets it on BOTH of those
			// choices, so it is the one signal that covers them.  This port had no
			// such loop and GS_PLAYING_GAME was terminal, so the request was never
			// consumed at all:
			//
			//   - New Game left m_bWantToRestart set with the frontend shut down and
			//     both audio fades at zero, so the game carried on in a half torn-down
			//     state instead of starting again.
			//   - Load Game additionally left m_bWantToLoad set, and that flag is what
			//     the NEXT InitialiseWhenRestarting tests to decide whether to load
			//     (Game.cpp:930), so the load never happened where it was asked for
			//     and the next unrelated start took the load branch against a world
			//     that had never been shut down for it.
			//
			// Both halves of that are the "load crashed when already in a game"
			// report.  m_bWantToLoad picks which kind of restart this is; it is set
			// for a load and left clear for a new game.
			if(FrontEndMenuManager.m_bWantToRestart){
				WiiTraceReport("WII game: restart requested from the pause menu "
				               "(%s)\n",
				               FrontEndMenuManager.m_bWantToLoad ? "load" : "new game");
				if(FrontEndMenuManager.m_bWantToLoad)
					startSavedGame(true);
				else
					startFreshGame(true);
			}
			break;

		default:
			break;
		}

		// Commits the log to the card, at most once a second.  This was only
		// being called from the wait-for-Nunchuk loop above, so for the whole of
		// actual play the log sat in its buffer: a run that froze, or that was
		// powered off mid-game, lost precisely the lines that would have said
		// what it was doing.  The cost is one fsync per second and it happens on
		// a frame that has already gone past its budget anyway.
		// One increment per completed frame, for the watchdog to judge liveness against.
		// At the very end of the iteration on purpose: a hang INSIDE a frame never
		// reaches this line, and that is exactly the case the end-of-frame stall
		// timer cannot see.  It is the whole reason the watchdog exists.
		WiiTraceTick();
		WiiTraceService();
		// Handed over here rather than read inside the watchdog, so that the trace
		// code stays independent of the streaming engine.  A hang that reports a
		// large pending count is spinning on a request set it cannot satisfy; one
		// that reports zero has nothing to do with streaming at all.
		WiiTraceSetStreamingState(CStreaming::ms_numModelsRequested,
		                          CStreaming::ms_numPedsLoaded);

		// One line for a frame that took longer than any frame should, and then
		// silence for a few seconds.  A freeze during play is otherwise entirely
		// invisible: nothing is written during a frame, so a run that hung looks
		// exactly like a run that ended.  A per-frame trace is the wrong answer to
		// that, because it fills the card and costs the very frame time it is
		// trying to measure -- so this is deliberately one debounced line rather
		// than a trace, and it is measured by the thread that stalled, which needs
		// no watchdog and no second thread to notice.
		//
		// The context is here because the obvious explanations disagree on it.
		// ms_numModelsRequested separates "outran the streamer" from "the streamer
		// was idle", and the resident and per-type loaded counts say how much was
		// in memory when it happened.  The heap is deliberately NOT sampled: that
		// walk is expensive enough to be the thing being measured.
		//
		// cdwait is the one that settles the storage question.  CdStreamSync on this
		// port is a condition variable wait with no timeout -- it returns when the
		// reader thread signals, and a thread stuck inside a read() never signals --
		// so the game thread can end a frame parked on the card with nothing to
		// show for it.  A cdwait close to the frame time means exactly that, and
		// means the frame was not slow, it was blocked; a cdwait near zero means
		// storage is not involved and the frame was slow for some other reason.
		const u64 frameEnd = gettime();
		const unsigned int frameMs = ticks_to_millisecs(frameEnd - frameStart);
		const unsigned int cdWaitMs = g_cdStreamLongestWaitMs;
		// Cleared every frame rather than only when reporting, so the number is
		// always this frame's and not whichever frame happened to notice.
		CdStreamResetWaitStats();
		if(frameMs >= kStallFrameMs &&
		   ticks_to_millisecs(frameEnd - lastStallReport) >= kStallReportGapMs){
			lastStallReport = frameEnd;
			WiiTraceReport("WII stall: frame=%ums cdwait=%ums state=%d streamReq=%d "
			               "resident=%uKB veh=%d ped=%d\n",
			               frameMs, cdWaitMs, (int)gGameState,
			               CStreaming::ms_numModelsRequested,
			               (unsigned)(CStreaming::ms_memoryUsed / 1024),
			               CStreaming::ms_numVehiclesLoaded, CStreaming::ms_numPedsLoaded);
		}
	}

	WiiTraceReport("WII game boot: exiting\n");
	WiiTraceCloseLog();
	return 0;
}
