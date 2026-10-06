#ifndef __GTA_TXDSTORE_H__
#define __GTA_TXDSTORE_H__

#include "templates.h"

struct TxdDef {
	RwTexDictionary *texDict;
	int refCount;
	char name[20];

	// Reclaim grace period state.  Set by CTxdStore::ReclaimUnusedTxds, not by the
	// streaming engine.  "Has been reclaimable since" rather than "last used",
	// because the thing being prevented is a dictionary being freed and immediately
	// re-requested -- a question about how long ago it stopped being wanted, which is
	// the one thing an access-time ordering cannot answer.
	uint32 reclaimableSinceMs;
	bool reclaimCandidate;
};

class CTxdStore
{
	static CPool<TxdDef,TxdDef> *ms_pTxdPool;
	static RwTexDictionary *ms_pStoredTxd;
public:
	static void Initialise(void);
	static void Shutdown(void);
	static void GameShutdown(void);
	static int AddTxdSlot(const char *name);
	static void RemoveTxdSlot(int slot);
	static int FindTxdSlot(const char *name);
	static char *GetTxdName(int slot);
	static void PushCurrentTxd(void);
	static void PopCurrentTxd(void);
	static void SetCurrentTxd(int slot);
	static void Create(int slot);
	static int GetNumRefs(int slot);
	static void AddRef(int slot);
	static void RemoveRef(int slot);
	static void RemoveRefWithoutDelete(int slot);
	static bool LoadTxd(int slot, RwStream *stream);
	static bool LoadTxd(int slot, const char *filename);
	static bool StartLoadTxd(int slot, RwStream *stream);
	static bool FinishLoadTxd(int slot, RwStream *stream);
	static void RemoveTxd(int slot);
	// Free every TXD that no model in play names, and return how many went.  This is
	// the only thing in the game that gives GX texture memory back:
	// RwTexDictionaryDestroy walks the dictionary's textures, each one drops to
	// refcount zero and destroys its raster, and ~GxRaster is the only place librw
	// frees the allocation.
	//
	// Deliberately not called on a timer.  A cache that holds exactly its working
	// set reloads every dictionary the moment the player looks away from it, which
	// is pop-in on every area transition -- a worse symptom than the leak, and paid
	// for on every frame rather than only when memory is short.  Callers gate it on
	// CGame::IsMemoryTight() instead.
	static int ReclaimUnusedTxds(void);
	static int GetLastReclaimCount(void);

	static TxdDef *GetSlot(int slot) {
		assert(slot >= 0);
		assert(ms_pTxdPool);
		assert(slot < ms_pTxdPool->GetSize());
		return ms_pTxdPool->GetSlot(slot);
	}
	static bool isTxdLoaded(int slot);
};

#endif // __GTA_TXDSTORE_H__
