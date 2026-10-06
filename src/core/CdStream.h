#ifndef __GTA_CDSTREAM_H__
#define __GTA_CDSTREAM_H__

// IMPORTANT: this file has to be includable from C,
// so no custom types like int32 &c.

#define CDSTREAM_SECTOR_SIZE 2048

#define _GET_INDEX(a) (a >> 24)
#define _GET_OFFSET(a)  (a & 0xFFFFFF)

#ifdef    __cplusplus
extern "C"
{
#endif

// These have to match IOP and also some SCE defines
enum
{
	STREAM_NONE            = 0,
	STREAM_SUCCESS         = 1,
	STREAM_READING         = 0xFF,
	STREAM_ERROR           = 0xFE,	// SCECdErREADCFR
	STREAM_ERROR_NOCD      = 0xFD,	// SCECdErREADCF
	STREAM_ERROR_WRONGCD   = 0xFC,
	STREAM_ERROR_OPENCD    = 0xFB,
	STREAM_WAITING         = 0xFA
};

void CdStreamInit(int numChannels);
int CdStreamRead(int channel, void *buffer, unsigned int offset, unsigned int size);
int CdStreamGetStatus(int channel);
int CdStreamGetLastPosn(void);
int CdStreamSync(int channel);

#ifdef NINTENDO_WII
// The longest single wait, in milliseconds, that CdStreamSync spent blocked since
// the last CdStreamResetWaitStats().  On this port CdStreamSync is a condition
// variable wait with no timeout: it returns when the reader thread signals, and
// if that thread is stuck inside a read() there is nothing to time it out.  So a
// long wait here is not slowness, it is the game thread parked on storage with no
// way to notice -- which is the one thing a stall report otherwise cannot tell us
// apart from a slow frame.  Costs one gettime() pair per call.
extern uint32 g_cdStreamLongestWaitMs;
void CdStreamResetWaitStats(void);
#endif

#ifndef GTA_PS2
typedef struct Queue Queue;
struct Queue
{
	int32 *items;
	int32 head;
	int32 tail;
	int32 size;
};

VALIDATE_SIZE(Queue, 0x10);

void CdStreamShutdown(void);
// on IOP on PS2
void CdStreamInitThread(void);
void AddToQueue(Queue *queue, int item);
int32 GetFirstInQueue(Queue *queue);
void RemoveFirstInQueue(Queue *queue);

// not on PS2 at all
uint32 GetGTA3ImgSize(void);
bool CdStreamAddImage(char const *path);
char *CdStreamGetImageName(int cd);
void CdStreamRemoveImages(void);
int32 CdStreamGetNumImages(void);
#endif

#ifdef FLUSHABLE_STREAMING
extern bool flushStream[MAX_CDCHANNELS];
#endif

#ifdef    __cplusplus
}
#endif

#endif // __GTA_CDSTREAM_H__
