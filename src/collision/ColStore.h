#ifndef __GTA_COLSTORE_H__
#define __GTA_COLSTORE_H__

#include "templates.h"

struct ColDef {	// made up name
	int32 unused;
	bool isLoaded;
	CRect bounds;
	char name[20];
	int16 minIndex;
	int16 maxIndex;
	// Bytes of collision data currently held for this slot, zero when not loaded.
	// Kept because nothing else on the streaming path counts collision: the 24MB
	// streaming budget covers models only, so a district's collision load is invisible
	// to every memory check the game has.  With COLSTORESIZE slots that is a real
	// amount of memory with nothing measuring it.
	int32 size;
};

class CColStore
{
	static CPool<ColDef,ColDef> *ms_pColPool;

public:
	static void Initialise(void);
	static void Shutdown(void);
	static int AddColSlot(const char *name);
	static void RemoveColSlot(int32 slot);
	static int FindColSlot(const char *name);
	static char *GetColName(int32 slot);
	static CRect &GetBoundingBox(int32 slot);
	static void IncludeModelIndex(int32 slot, int32 modelIndex);
	static bool LoadCol(int32 storeID, uint8 *buffer, int32 bufsize);
	static void RemoveCol(int32 slot);
	static void AddCollisionNeededAtPosn(const CVector2D &pos);
	static void LoadAllCollision(void);
	static void RemoveAllCollision(void);
	static void LoadCollision(const CVector2D &pos);
	static void RequestCollision(const CVector2D &pos);
	static void EnsureCollisionIsInMemory(const CVector2D &pos);
	static bool HasCollisionLoaded(const CVector2D &pos);

	// Bytes of collision currently resident across all slots.  Walked on demand rather
	// than tracked, because it is read about once a second by a log line and 31 slots
	// is nothing to walk.
	static int GetLoadedColBytes() {
		int bytes = 0;
		for(int i = 1; i < COLSTORESIZE; i++){
			const ColDef *def = GetSlot(i);
			if(def != nil && def->isLoaded && def->size > 0)
				bytes += def->size;
		}
		return bytes;
	}

	static ColDef *GetSlot(int slot) {
		assert(slot >= 0);
		assert(ms_pColPool);
		assert(slot < ms_pColPool->GetSize());
		return ms_pColPool->GetSlot(slot);
	}
};

#endif // __GTA_COLSTORE_H__
