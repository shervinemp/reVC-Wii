#ifndef __GTA_MEMORYHEAP_H__
#define __GTA_MEMORYHEAP_H__

// some windows shit
#ifdef MoveMemory
#undef MoveMemory
#endif

#ifdef USE_CUSTOM_ALLOCATOR
#define PUSH_MEMID(id) gMainHeap.PushMemId(id)
#define POP_MEMID() gMainHeap.PopMemId()
#define REGISTER_MEMPTR(ptr) gMainHeap.RegisterMemPointer(ptr)
#else
#define PUSH_MEMID(id)
#define POP_MEMID()
#define REGISTER_MEMPTR(ptr)
#endif

enum {
	MEMID_FREE,
	MEMID_GAME = 1,	// "Game"
	MEMID_WORLD = 2,	// "World"
	MEMID_ANIMATION = 3,	// "Animation"
	MEMID_POOLS = 4,	// "Pools"
	MEMID_DEF_MODELS = 5,	// "Default Models"
	MEMID_STREAM = 6,	// "Streaming"
	MEMID_STREAM_MODELS = 7,	// "Streamed Models"
	MEMID_STREAM_LODS = 8,	// "Streamed LODs"
	MEMID_STREAM_TEXUTRES = 9,	// "Streamed Textures"
	MEMID_STREAM_COLLISION = 10,	// "Streamed Collision"
	MEMID_STREAM_ANIMATION = 11,	// "Streamed Animation"
	MEMID_TEXTURES = 12,	// "Textures"
	MEMID_COLLISION = 13,	// "Collision"
	MEMID_PRE_ALLOC = 14,	// "PreAlloc"
	MEMID_GAME_PROCESS = 15,	// "Game Process"
	MEMID_SCRIPT = 16,	// "Script"
	MEMID_CARS = 17,	// "Cars"
	MEMID_RENDER = 18,	// "Render"
	MEMID_PED_ATTR = 19,	// "Ped Attr"
	NUM_MEMIDS,

	NUM_FIXED_MEMBLOCKS = 6
};

// Per-MEMID allocation attribution for the Wii port.  A diagnostic, compiled in only at
// CREATE_LOG 2; a normal build keeps the empty PUSH_MEMID and POP_MEMID above, on the
// Wii as everywhere else.
//
// With USE_CUSTOM_ALLOCATOR off, every one of the ~90 PUSH_MEMID(MEMID_STREAM_TEXUTRES)-
// style call sites in src/ does nothing, so streamed models, streamed collision,
// streamed textures, the render and the world all draw from one arena with nothing
// recording which was which.  Switched on, the labels that already exist are hooked up:
// the push and pop below keep a current id, and the allocator hooks in wii_game.cpp
// charge each allocation to it.
//
// Redefined here, below the MEMID enum, because the inline functions reference
// MEMID_FREE.  wiiMemIdSelfTest exists because an earlier version of this left the empty
// definitions in force while the machinery sat compiled in beside them: it built, ran and
// reported every allocation as MEMID_FREE, which reads as one small category rather than
// as a fault.
#if defined NINTENDO_WII && !defined USE_CUSTOM_ALLOCATOR
#include "wii-port/WiiTrace.h"	// CREATE_LOG
#if CREATE_LOG >= 2
#define WII_MEMID_ATTRIBUTION
#endif
#endif

#ifdef WII_MEMID_ATTRIBUTION
#undef PUSH_MEMID
#undef POP_MEMID
#define PUSH_MEMID(id) wiiMemIdPush(id)
#define POP_MEMID()    wiiMemIdPop()

enum { WII_MEMID_SLOTS = 32 };

extern uint32 g_wiiMemidBytes[WII_MEMID_SLOTS];
extern int32  g_wiiMemidCurrent;
extern int32  g_wiiMemidStack[16];
extern int32  g_wiiMemidDepth;

// Saturating rather than wrapping: a nest deeper than the stack would otherwise
// mislabel the rest of the frame's allocations as MEMID_FREE, which is the one label
// that means "unattributed" and so would hide the very thing being looked for.
static inline void wiiMemIdPush(int32 id)
{
	if(id < 0 || id >= WII_MEMID_SLOTS)
		id = MEMID_FREE;
	if(g_wiiMemidDepth < (int32)ARRAY_SIZE(g_wiiMemidStack))
		g_wiiMemidStack[g_wiiMemidDepth++] = g_wiiMemidCurrent;
	g_wiiMemidCurrent = id;
}

static inline void wiiMemIdPop(void)
{
	g_wiiMemidCurrent = g_wiiMemidDepth > 0 ? g_wiiMemidStack[--g_wiiMemidDepth] : MEMID_FREE;
}

// CUMULATIVE bytes requested, never decremented on free.  That is a weaker number than
// live bytes and is reported as what it is: a category that allocates and frees the
// same memory forever also shows up here, so this ranks where allocation activity is,
// it does not by itself prove a leak.  Read against the arena's own net drain, which
// WiiTrace already reports, the two together localise it.
//
// Deliberately not a live-bytes figure.  That needs a size header on every allocation,
// which changes the alignment of every allocation in the game, in a renderer that
// cannot be exercised from a desktop.  Not worth it for a diagnostic.
//
// On the allocation path, so inline rather than a call: this runs for every allocation
// librw makes, and on this console a call plus a bounds check per allocation is real
// frame time for a diagnostic.
//
// The nesting here is not a hypothetical.  main.cpp pushes MEMID_GAME and never pops
// it -- it is commented "NB: not popped" -- so the stack sits permanently one deep for
// the whole game, and the pairs at 550/558 and 2096/2113 are alternative exits (one
// breaks) rather than a double pop.  That is harmless for the inner scopes, because
// popping restores the enclosing id correctly, but it does mean depth never returns to
// zero and so the stack can never be used as a sanity check on balance.
static inline void wiiMemIdCharge(size_t sz)
{
	if(g_wiiMemidCurrent >= 0 && g_wiiMemidCurrent < WII_MEMID_SLOTS)
		g_wiiMemidBytes[g_wiiMemidCurrent] += (uint32)sz;
}

// Returns 1 if the nesting is working, 0 if it is not.
//
// Exists because of the mistake recorded where PUSH_MEMID is redefined above: the macros
// compiled, the build linked, the log looked plausible, and the counters were dead.
// Nothing about that failure is visible in the output -- it reads as one small category
// rather than as a fault, which is the worst way for an instrument to fail.  A self-test
// turns it into a line that says so.
//
// Checks the two things that can independently break: that a push changes the current
// id, and that the matching pop puts it back.
static inline int wiiMemIdSelfTest(void)
{
	int32 before = g_wiiMemidCurrent;

	wiiMemIdPush(MEMID_RENDER);
	int sawPush = (g_wiiMemidCurrent == MEMID_RENDER);
	wiiMemIdPop();

	// Depth is deliberately not required to come back to where it started: main.cpp
	// pushes MEMID_GAME and never pops it, so the stack may already sit one deep.  What
	// must hold is that the current id is back where it was.
	return (sawPush && g_wiiMemidCurrent == before) ? 1 : 0;
}
#endif

template<typename T, uint32 N>
class CStack
{
public:
	T values[N];
	uint32 sp;

	CStack() : sp(0) {}
	void push(const T& val) { values[sp++] = val; }
	T& pop() { return values[--sp]; }
};


struct HeapBlockDesc
{
	uint32 m_size;
	int16 m_memId;
	int16 m_ptrListIndex;
	HeapBlockDesc *m_next;
	HeapBlockDesc *m_prev;

	HeapBlockDesc *GetNextConsecutive(void)
	{
		return (HeapBlockDesc*)((uintptr)this + sizeof(HeapBlockDesc) + m_size);
	}

	void *GetDataPointer(void)
	{
		return (void*)((uintptr)this + sizeof(HeapBlockDesc));
	}

	void RemoveHeapFreeBlock(void)
	{
		m_next->m_prev = m_prev;
		m_prev->m_next = m_next;
	}

	// after node
	void InsertHeapFreeBlock(HeapBlockDesc *node)
	{
		m_next = node->m_next;
		node->m_next->m_prev = this;
		m_prev = node;
		node->m_next = this;
	}

	HeapBlockDesc *FindSmallestFreeBlock(uint32 size)
	{
		HeapBlockDesc *b;
		for(b = m_next; b->m_size < size; b = b->m_next);
		return b;
	}
};

#ifdef USE_CUSTOM_ALLOCATOR
// TODO: figure something out for 64 bit pointers
static_assert(sizeof(HeapBlockDesc) == 0x10, "HeapBlockDesc must have 0x10 size otherwise most of assumptions don't make sense");
#endif

struct HeapBlockList
{
	HeapBlockDesc m_first;
	HeapBlockDesc m_last;

	void Init(void)
	{
		m_first.m_next = &m_last;
		m_last.m_prev = &m_first;
	}

	void Insert(HeapBlockDesc *node)
	{
		node->InsertHeapFreeBlock(&m_first);
	}
};

struct CommonSize
{
	HeapBlockList m_freeList;
	uint32 m_size;
	uint32 m_failed;
	uint32 m_remaining;

	void Init(uint32 size);
	void Free(HeapBlockDesc *node)
	{
		m_freeList.Insert(node);
		m_remaining++;
	}
	HeapBlockDesc *Malloc(void)
	{
		if(m_freeList.m_first.m_next == &m_freeList.m_last){
			m_failed++;
			return nil;
		}
		HeapBlockDesc *block = m_freeList.m_first.m_next;
		m_remaining--;
		block->RemoveHeapFreeBlock();
		block->m_ptrListIndex = -1;
		return block;
	}
};

class CMemoryHeap
{
public:
	HeapBlockDesc *m_start;
	HeapBlockDesc *m_end;
	HeapBlockList m_freeList;
	CommonSize m_fixedSize[NUM_FIXED_MEMBLOCKS];
	uint32 m_totalMemUsed;
	CStack<int32, 16> m_idStack;
	uint32 m_currentMemID;
	uint32 *m_memUsed;
	uint32 m_totalBlocksUsed;
	uint32 *m_blocksUsed;
	uint32 m_unkMemId;

	CMemoryHeap(void) : m_start(nil) {}
	void Init(uint32 total);
	void RegisterMalloc(HeapBlockDesc *block);
	void RegisterFree(HeapBlockDesc *block);
	void *Malloc(uint32 size);
	void *Realloc(void *ptr, uint32 size);
	void Free(void *ptr);
	void FillInBlockData(HeapBlockDesc *block, HeapBlockDesc *end, uint32 size);
	uint32 CombineFreeBlocks(HeapBlockDesc *block);
	void *MoveMemory(void *ptr);
	HeapBlockDesc *WhereShouldMemoryMove(void *ptr);
	void *MoveHeapBlock(HeapBlockDesc *dst, HeapBlockDesc *src);
	void PopMemId(void);
	void PushMemId(int32 id);
	void RegisterMemPointer(void *ptr);
	void TidyHeap(void);
	uint32 GetMemoryUsed(int32 id);
	uint32 GetBlocksUsed(int32 id);
	int32 GetLargestFreeBlock(void) { return m_freeList.m_last.m_prev->m_size; }

	void ParseHeap(void);

	HeapBlockDesc *GetDescFromHeapPointer(void *block)
	{
		return (HeapBlockDesc*)((uintptr)block - sizeof(HeapBlockDesc));
	}
	uint32 GetSizeBetweenBlocks(HeapBlockDesc *first, HeapBlockDesc *second)
	{
		return (uintptr)second - (uintptr)first - sizeof(HeapBlockDesc);
	}
	void FreeBlock(HeapBlockDesc *block){
		for(int i = 0; i < NUM_FIXED_MEMBLOCKS; i++){
			if(m_fixedSize[i].m_size == block->m_size){
				m_fixedSize[i].Free(block);
				return;
			}
		}
		HeapBlockDesc *b = m_freeList.m_first.FindSmallestFreeBlock(block->m_size);
		block->InsertHeapFreeBlock(b->m_prev);
	}
};

extern CMemoryHeap gMainHeap;

#endif // __GTA_MEMORYHEAP_H__
