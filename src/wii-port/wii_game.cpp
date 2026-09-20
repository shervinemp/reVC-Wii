#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <malloc.h>

#include <unistd.h>

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
#include "Frontend.h"
#include "Game.h"
#include "main.h"
#include "Pad.h"
#include "PCSave.h"
#include "platform.h"
#include "skeleton.h"
#include "WiiLog.h"
#include "WiiPad.h"
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
	bootPrintf("WII fileexists: fopen %s\n", path);
	FILE *file = std::fopen(path, "rb");
	bootPrintf("WII fileexists: fopen=%p\n", (void *)file);
	if(file == nullptr)
		return false;
	bootPrintf("WII fileexists: fclose %p\n", (void *)file);
	std::fclose(file);
	bootPrintf("WII fileexists: closed\n");
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

// Each miss is reported rather than counted, because the failure this guards
// against is a correct install under a name the list below cannot know, and the
// paths actually tried are the only thing that distinguishes that from data that
// is genuinely absent.
bool
tryInstallDirectory(const char *directory)
{
	char path[192];
	bootPrintf("WII game boot: probing %s\n", directory);
	std::snprintf(path, sizeof(path), "%s/DATA/GTA_VC.DAT", directory);
	bootPrintf("WII game boot: testing %s\n", path);
	if(!fileExists(path)){
		bootPrintf("WII game boot: no data at %s\n", path);
		return false;
	}
	bootPrintf("WII game boot: data found at %s, chdir\n", directory);
	if(chdir(directory) != 0){
		bootPrintf("WII game boot: chdir failed for %s\n", directory);
		return false;
	}

	std::snprintf(s_installDirectory, sizeof(s_installDirectory), "%s", directory);
	bootPrintf("WII game boot: install=%s\n", directory);
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

// The on-screen counterpart for the boot trace: the log still gets the full
// text, and where the boot sits still is shown live on the television.
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
#if !CREATE_LOG
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
	rw::gx::setFrameTrace(CREATE_LOG != 0);

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
	if(strncmp(s_installDirectory, "sd:", 3) == 0)
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
RwChar **_psGetVideoModeList() { return nullptr; }
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
	// steps that made a slow stick look like a hang.  Every cycle is logged:
	// the lines flush out per write, so wherever a device call spins to a halt,
	// the last line on the card names it.
	for(int attempt = 0; attempt < 50; attempt++){
		if(attempt > 0)
			usleep(100000);
		if(fatMountSimple("usb", &__io_usbstorage)){
			bootPrintf("WII storage: usb: mounted as FAT on attempt %d\n", attempt + 1);
			return true;
		}
		bootPrintf("WII storage: usb: FAT attempt %d failed\n", attempt + 1);

		// libogc's FAT driver only speaks FAT: an NTFS stick (the most common
		// thing to plug in) stays invisible to every fallback path below.  The
		// vendored read-only NTFS-3G port picks it up instead.  Find the first
		// partition explicitly, because ntfsMount needs a start sector; most
		// sticks carry exactly one NTFS volume, so that is what gets used.
		if((attempt & 3) == 3){
			bootPrintf("WII storage: usb: NTFS probe attempt %d\n", attempt + 1);
			sec_t *ntfsPartitions = nullptr;
			int partitionCount =
				ntfsFindPartitions(&__io_usbstorage, &ntfsPartitions);
			bootPrintf("WII storage: usb: ntfsFindPartitions=%d\n", partitionCount);
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
				if(!ntfsMounted)
					bootPrintf("WII storage: usb: NTFS errno=%d\n", errno);
			}
			std::free(ntfsPartitions);
			if(ntfsMounted){
				bootPrintf("WII storage: usb: mounted as NTFS on attempt %d\n", attempt + 1);
				return true;
			}
			bootPrintf("WII storage: usb: NTFS mount attempt %d failed\n", attempt + 1);
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

void
WiiStdoutHookInstall(void)
{
	if(s_stdoutWrapper != nullptr)
		return;
	const devoptab_t *original = devoptab_list[1];
	if(original == nullptr)
		return;
	devoptab_t *wrapper = new (std::nothrow) devoptab_t;
	if(wrapper == nullptr)
		return;
	*wrapper = *original;
	wrapper->name = "consoleQuiet";
	wrapper->write_r = stdoutSwallow;
	s_stdoutWrapper = wrapper;
	devoptab_list[1] = wrapper;
}

int
main(int argc, char **argv)
{
	// Print the banner as the very first area on the television once the
	// console exists: a build with the wrong devkitPro/libogc pairing then
	// announces itself (GCC rev mirrors the toolchain install) instead of
	// hiding behind a silent black screen.
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
	// Engine printf()/debug() lines land in the log from here on, so the
	// console and the file narrate the same boot.
	WiiStdoutHookInstall();
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

	// Whole-install-on-SD layouts keep saving inside the install tree as
	// always; a read-only USB assets mount relocates the user files onto the
	// SD card (already the s_userFilesDirectory default).
	if(strncmp(s_installDirectory, "sd:", 3) == 0)
		std::snprintf(s_userFilesDirectory, sizeof(s_userFilesDirectory),
		              "%s", s_installDirectory);
	bootPrintf("WII game boot: user files dir=%s\n", s_userFilesDirectory);

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

	while(!RsGlobal.quit){
		switch(gGameState){
		case GS_FRONTEND:
			RsEventHandler(rsFRONTENDIDLE, nullptr);
			if(FrontEndMenuManager.m_bWantToLoad){
				WiiTraceReport("WII game boot: loading saved game\n");
				WiiTraceHeap("pre-load");

				// Three steps and not one, which is what this used to be.
				// InitialiseGame alone is CGame::Initialise, and that is a NEW
				// game: it never reaches GenericLoad, so picking a save slot
				// started the story over from the beginning.
				//
				// The save is restored into the pools, streaming and world that
				// CGame::Initialise builds, so that still has to run first -- on
				// the desktop skeletons it already has by this point, because they
				// initialise before showing the menu, while this one boots
				// straight into the frontend and loads nothing until asked.
				// ShutDownForRestart then clears what Initialise placed in the
				// world, and InitialiseWhenRestarting is the one that actually
				// reads the slot.  Same order as glfw.cpp and sdl2.cpp.
				InitialiseGame();
				CGame::ShutDownForRestart();
				CGame::InitialiseWhenRestarting();
				DMAudio.ChangeMusicMode(MUSICMODE_GAME);

				FrontEndMenuManager.m_bGameNotLoaded = false;
				// Cleared here as well.  Leaving it set makes every later restart
				// look like another load request.
				FrontEndMenuManager.m_bWantToLoad = false;
				FrontEndMenuManager.m_bWantToRestart = false;
				gGameState = GS_PLAYING_GAME;

				WiiTraceHeap("post-load");
				WiiTraceReport("WII game boot: save loaded, entering game\n");
			}else if(!FrontEndMenuManager.m_bMenuActive)
				gGameState = GS_INIT_PLAYING_GAME;
			break;

		case GS_INIT_PLAYING_GAME:
			WiiTraceReport("WII game boot: loading DATA/GTA_VC.DAT\n");
			// Brackets the load, so what the frontend left behind and what the
			// world costs can be read off against the boot line.
			WiiTraceHeap("pre-load");
			InitialiseGame();
			FrontEndMenuManager.m_bGameNotLoaded = false;
			// Starting a game leaves this set so the desktop skeleton can run its
			// outer restart loop.  The Wii skeleton completes that transition here.
			FrontEndMenuManager.m_bWantToRestart = false;
			gGameState = GS_PLAYING_GAME;
			WiiTraceHeap("post-load");
			WiiTraceReport("WII game boot: game data initialized\n");
			break;

		case GS_PLAYING_GAME:
			RsEventHandler(rsIDLE, (void*)TRUE);
			break;

		default:
			break;
		}
	}

	WiiTraceReport("WII game boot: exiting\n");
	WiiTraceCloseLog();
	return 0;
}
