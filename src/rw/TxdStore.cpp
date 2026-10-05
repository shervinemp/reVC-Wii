#include "common.h"

#include "templates.h"
#include "General.h"
#include "ModelInfo.h"
#include "Streaming.h"
#include "RwHelper.h"
#include "TxdStore.h"

CPool<TxdDef,TxdDef> *CTxdStore::ms_pTxdPool;
RwTexDictionary *CTxdStore::ms_pStoredTxd;

void
CTxdStore::Initialise(void)
{
	if(ms_pTxdPool == nil)
		ms_pTxdPool = new CPool<TxdDef,TxdDef>(TXDSTORESIZE, "TexDictionary");
}

void
CTxdStore::Shutdown(void)
{
	if(ms_pTxdPool)
		delete ms_pTxdPool;
}

void
CTxdStore::GameShutdown(void)
{
	int i;

	for(i = 0; i < TXDSTORESIZE; i++){
		TxdDef *def = GetSlot(i);
		if(def && GetNumRefs(i) == 0)
			RemoveTxdSlot(i);
	}
}

int
CTxdStore::AddTxdSlot(const char *name)
{
	TxdDef *def = ms_pTxdPool->New();
	assert(def);
	def->texDict = nil;
	def->refCount = 0;
	strcpy(def->name, name);
	return ms_pTxdPool->GetJustIndex(def);
}

void
CTxdStore::RemoveTxdSlot(int slot)
{
	TxdDef *def = GetSlot(slot);
	if(def->texDict)
		RwTexDictionaryDestroy(def->texDict);
	ms_pTxdPool->Delete(def);
}

int
CTxdStore::FindTxdSlot(const char *name)
{
	int size = ms_pTxdPool->GetSize();
	for(int i = 0; i < size; i++){
		TxdDef *def = GetSlot(i);
		if(def && !CGeneral::faststricmp(def->name, name))
			return i;
	}
	return -1;
}

char*
CTxdStore::GetTxdName(int slot)
{
	return GetSlot(slot)->name;
}

void
CTxdStore::PushCurrentTxd(void)
{
	ms_pStoredTxd = RwTexDictionaryGetCurrent();
}

void
CTxdStore::PopCurrentTxd(void)
{
	RwTexDictionarySetCurrent(ms_pStoredTxd);
	ms_pStoredTxd = nil;
}

void
CTxdStore::SetCurrentTxd(int slot)
{
	RwTexDictionarySetCurrent(GetSlot(slot)->texDict);
}

void
CTxdStore::Create(int slot)
{
	GetSlot(slot)->texDict = RwTexDictionaryCreate();
}

int
CTxdStore::GetNumRefs(int slot)
{
	return GetSlot(slot)->refCount;
}

void
CTxdStore::AddRef(int slot)
{
	GetSlot(slot)->refCount++;
}

void
CTxdStore::RemoveRef(int slot)
{
	if(--GetSlot(slot)->refCount <= 0)
		CStreaming::RemoveTxd(slot);
}

void
CTxdStore::RemoveRefWithoutDelete(int slot)
{
	GetSlot(slot)->refCount--;
}

bool
CTxdStore::LoadTxd(int slot, RwStream *stream)
{
	TxdDef *def = GetSlot(slot);

	if(RwStreamFindChunk(stream, rwID_TEXDICTIONARY, nil, nil)){
#ifdef NINTENDO_WII
		SetWiiTxdReadTrace(false);
#endif
		def->texDict = RwTexDictionaryGtaStreamRead(stream);
#ifdef NINTENDO_WII
		SetWiiTxdReadTrace(false);
#endif
		return def->texDict != nil;
	}
	printf("Failed to load TXD\n");
	return false;
}

bool
CTxdStore::LoadTxd(int slot, const char *filename)
{
	RwStream *stream;
	bool ret;

	ret = false;
#ifdef GTA_PC
	_rwD3D8TexDictionaryEnableRasterFormatConversion(true);
#endif
	do
		stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, filename);
	while(stream == nil);
	ret = LoadTxd(slot, stream);
	RwStreamClose(stream, nil);
	return ret;
}

bool
CTxdStore::StartLoadTxd(int slot, RwStream *stream)
{
	TxdDef *def = GetSlot(slot);
	if(RwStreamFindChunk(stream, rwID_TEXDICTIONARY, nil, nil)){
		def->texDict = RwTexDictionaryGtaStreamRead1(stream);
		return def->texDict != nil;
	}else{
		printf("Failed to load TXD\n");
		return false;
	}
}

bool
CTxdStore::FinishLoadTxd(int slot, RwStream *stream)
{
	TxdDef *def = GetSlot(slot);
	def->texDict = RwTexDictionaryGtaStreamRead2(stream, def->texDict);
	return def->texDict != nil;
}

void
CTxdStore::RemoveTxd(int slot)
{
	TxdDef *def = GetSlot(slot);
	if(def->texDict)
		RwTexDictionaryDestroy(def->texDict);
	def->texDict = nil;
}

// Scratch for ReclaimUnusedTxds: one bit per TXD slot, recording whether any model
// in play still names that slot as its texture dictionary.  Static rather than
// allocated, because a function whose whole purpose is to return memory should not
// be holding a chunk of it.
static uint8 ms_aTxdInUse[(TXDSTORESIZE + 7) / 8];
static int ms_lastReclaim;

int
CTxdStore::GetLastReclaimCount(void)
{
	return ms_lastReclaim;
}

int
CTxdStore::ReclaimUnusedTxds(void)
{
	int i;

	ms_lastReclaim = 0;

	if(ms_pTxdPool == nil)
		return 0;

	memset(ms_aTxdInUse, 0, sizeof(ms_aTxdInUse));

	// A TXD has to outlive the models that draw with it, and the TXD reference count
	// does not track that.  The count is taken in ConvertBufferToObject, which runs
	// when a read has already completed, and dropped again the instant the conversion
	// finishes -- so a world model sitting fully loaded on screen holds a refcount of
	// zero.  Only the streaming state knows which dictionaries are still wanted.
	//
	// So derive it instead of maintaining a parallel count.  There are five separate
	// places a model load completes, and hooking all of them would be a worse way to
	// be wrong than reading the state that already exists.
	//
	// "Wanted" means anything short of NOTLOADED, not just LOADED.  A model still
	// queued or mid-read has its dictionary loaded and its refcount back at zero,
	// because the refcount is not taken until the read finishes.  Testing for LOADED
	// alone would free the dictionary out from under it; ConvertBufferToObject would
	// then re-request it on its next pass and the model would ping-pong between
	// loading a dictionary and having it pulled, which reads as a hang.
	for(i = 0; i < STREAM_OFFSET_TXD; i++){
		if(CStreaming::ms_aInfoForModel[i].m_loadState == STREAMSTATE_NOTLOADED)
			continue;
		// GetModelInfo is an unchecked array read, and this loop walks every model
		// index rather than only the ones known to be real.
		CBaseModelInfo *mi = CModelInfo::GetModelInfo(i);
		if(!mi)
			continue;
		int16 slot = mi->GetTxdSlot();
		if(slot >= 0 && slot < TXDSTORESIZE)
			ms_aTxdInUse[slot / 8] |= (uint8)(1 << (slot % 8));
	}

	for(i = 0; i < TXDSTORESIZE; i++){
		// Nothing resident, so nothing to give back.
		if(CStreaming::ms_aInfoForModel[STREAM_OFFSET_TXD + i].m_loadState != STREAMSTATE_LOADED)
			continue;
		// The pool slot can be empty for an index the streamer still knows about.
		if(!GetSlot(i))
			continue;
		// Pinned by whoever asked for it to stay resident: radar tiles, the male ped,
		// anything a script owns.  This is the game's own predicate, and it had no
		// callers at all until now -- CanRemoveModel has eight and CanRemoveCol has
		// one, but CanRemoveTxd had none, because there was never a TXD reclaim for
		// it to gate.
		if(!CStreaming::CanRemoveTxd(i))
			continue;
		// A conversion is reading it right now.  Tearing one down mid-read is the
		// crash that made every unload path reach for RemoveRefWithoutDelete.
		if(GetNumRefs(i) > 0)
			continue;
		// Some model in play still names it.
		if(ms_aTxdInUse[i / 8] & (1 << (i % 8)))
			continue;
		CStreaming::RemoveTxd(i);
		ms_lastReclaim++;
	}
	return ms_lastReclaim;
}
