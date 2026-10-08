#include "common.h"

#include "FileMgr.h"
#ifdef MORE_LANGUAGES
#include "Game.h"
#endif
#include "Frontend.h"
#include "Messages.h"
#include "Text.h"
#include "Timer.h"
#ifdef NINTENDO_WII
#include "Font.h"
#endif

#ifdef NINTENDO_WII
#include "wii-port/WiiLog.h"
#endif

wchar WideErrorString[25];

CText TheText;

CText::CText(void)
{
	encoding = 'e';
	bHasMissionTextOffsets = false;
	bIsMissionTextLoaded = false;
	memset(szMissionTableName, 0, sizeof(szMissionTableName));
	memset(WideErrorString, 0, sizeof(WideErrorString));
}

void
CText::Load(void)
{
	char filename[32];
	size_t offset;
	int file;
	bool tkey_loaded = false, tdat_loaded = false;
	ChunkHeader m_ChunkHeader;

	bIsMissionTextLoaded = false;
	bHasMissionTextOffsets = false;

	Unload();
#ifdef NINTENDO_WII
	wiiLog("WII text: unloaded previous text\n");
#endif

	CFileMgr::SetDir("TEXT");
#ifdef NINTENDO_WII
	wiiLog("WII text: directory=TEXT\n");
#endif
	switch(FrontEndMenuManager.m_PrefsLanguage){
	case CMenuManager::LANGUAGE_AMERICAN:
#ifdef GTA_PS2
#ifdef GTA_PAL
		sprintf(filename, "ENGLISH.GXT");
#else
		sprintf(filename, "AMERICAN.GXT");
#endif
#else
		sprintf(filename, "AMERICAN.GXT");
#endif
		break;
	case CMenuManager::LANGUAGE_FRENCH:
		sprintf(filename, "FRENCH.GXT");
		break;
	case CMenuManager::LANGUAGE_GERMAN:
		sprintf(filename, "GERMAN.GXT");
		break;
	case CMenuManager::LANGUAGE_ITALIAN:
		sprintf(filename, "ITALIAN.GXT");
		break;
	case CMenuManager::LANGUAGE_SPANISH:
		sprintf(filename, "SPANISH.GXT");
		break;
#ifdef MORE_LANGUAGES
	case CMenuManager::LANGUAGE_POLISH:
		sprintf(filename, "POLISH.GXT");
		break;
	case CMenuManager::LANGUAGE_RUSSIAN:
		sprintf(filename, "RUSSIAN.GXT");
		break;
	case CMenuManager::LANGUAGE_JAPANESE:
		sprintf(filename, "JAPANESE.GXT");
		break;
#endif
	}

	file = CFileMgr::OpenFile(filename, "rb");
	// The open failure convention is -1 in FileMgr (fd 0 was the old 0-return
	// protocol); a -1 here used to fall to fread and crash the boot.
	if(file <= 0){
		CFileMgr::SetDir("");
		return;
	}

	offset = 0;
	while (!tkey_loaded || !tdat_loaded) {
		if(!ReadChunkHeader(&m_ChunkHeader, file, &offset)){
#ifdef NINTENDO_WII
			wiiLog("WII text: incomplete GXT at offset=%u\n", (unsigned)offset);
#endif
			CFileMgr::CloseFile(file);
			CFileMgr::SetDir("");
			return;
		}
#ifdef NINTENDO_WII
		wiiLog("WII text: chunk=%.4s size=%d offset=%u\n",
		       m_ChunkHeader.magic, m_ChunkHeader.size, (unsigned)offset);
#endif
		if (m_ChunkHeader.size != 0) {
			if (strncmp(m_ChunkHeader.magic, "TABL", 4) == 0) {
				MissionTextOffsets.Load(m_ChunkHeader.size, file, &offset, 0x58000);
				bHasMissionTextOffsets = true;
			} else if (strncmp(m_ChunkHeader.magic, "TKEY", 4) == 0) {
				this->keyArray.Load(m_ChunkHeader.size, file, &offset);
				tkey_loaded = true;
			} else if (strncmp(m_ChunkHeader.magic, "TDAT", 4) == 0) {
				this->data.Load(m_ChunkHeader.size, file, &offset);
				tdat_loaded = true;
			} else {
				CFileMgr::Seek(file, m_ChunkHeader.size, SEEK_CUR);
				offset += m_ChunkHeader.size;
			}
		}
	}

	keyArray.Update(data.chars);
	CFileMgr::CloseFile(file);
	CFileMgr::SetDir("");
}

void
CText::Unload(void)
{
	CMessages::ClearAllMessagesDisplayedByGame();
	keyArray.Unload();
	data.Unload();
	mission_keyArray.Unload();
	mission_data.Unload();
	bIsMissionTextLoaded = false;
	memset(szMissionTableName, 0, sizeof(szMissionTableName));
}

#ifdef NINTENDO_WII
// The labels this port added live in the GXT files built from utils/gxt, and a
// language with no source file there (Russian) cannot have them compiled in; a
// missing key would show as "WII_AIM missing".  English is the better answer.
static wchar *
WiiFallbackText(const char *key)
{
	static const struct { const char *key; const char *text; } fallbacks[] = {
		{ "WII_RMK", "REMOTE SPEAKER" },
		{ "WII_AIM", "AIM ASSIST" },
		{ "WII_IRA", "POINTER AIM" },
		{ "WII_PHN", "PHONE CALLS" },
		{ "WII_RMT", "REMOTE" },
		{ "WII_BTH", "BOTH" },
		{ "WII_BOX", "POINTER BOX" },
		{ "WII_ENH", "ENHANCEMENTS" },
		{ "WII_COP", "CO-OP" },
		{ "WII_COS", "PARTNER SKIN" },
		{ "WII_SK0", "SAME AS PLAYER 1" },
		{ "WII_SK1", "CANDY" },
		{ "WII_SK2", "KEN" },
		{ "WII_SK3", "LANCE" },
		{ "WII_SK4", "PHIL" },
		{ "WII_SK5", "DIAZ" },
		{ "WII_SK6", "MERCEDES" },
		{ "WII_SK7", "SONNY" },
		{ "WII_SK8", "COLONEL" },
		{ "WII_SK9", "JEZZ" },
		{ "WII_SK10", "HILARY" },
		{ "WII_SK11", "GONZALEZ" },
		{ "WII_FF", "FRIENDLY FIRE" },
		{ "WII_SHW", "SHARED WANTED" },
		{ "WII_MG", "MINIGAMES" },
		{ "WII_CSH", "COP SHIFT" },
		{ "WII_CSR", "SMUGGLING RUN" },
		{ "WII_P2D", "Player 2 is down - reach them to bring them back." },
		{ "WII_P3D", "Player 3 is down - reach them to bring them back." },
		{ "WII_P4D", "Player 4 is down - reach them to bring them back." },
		{ "WII_P2R", "Player 2 is back on their feet." },
		{ "WII_P3R", "Player 3 is back on their feet." },
		{ "WII_P4R", "Player 4 is back on their feet." },
		{ "WII_CC", "COUCH CO-OP" },
		{ "WII_P2J", "Player 2: press any button to join." },
		{ "WII_P3J", "Player 3: press any button to join." },
		{ "WII_P4J", "Player 4: press any button to join." },
		{ "WII_P2I", "Player 2 has joined." },
		{ "WII_P3I", "Player 3 has joined." },
		{ "WII_P4I", "Player 4 has joined." },
		{ "WII_P2O", "Player 2 has left." },
		{ "WII_P3O", "Player 3 has left." },
		{ "WII_P4O", "Player 4 has left." },
		{ "WII_CCM", "Co-op is paused until the mission is over." },
		{ "WII_TTH", "You cannot get any further apart on foot." },
		{ "WII_CF0", "Shared camera: low." },
		{ "WII_CF1", "Shared camera: middle." },
		{ "WII_CF2", "Shared camera: high." },
		{ "WII_CF3", "Shared camera: overhead." },
		{ "WII_CFP", "Shared camera: following player 2's car." },
		{ "WII_CFL", "Shared camera: following player 1's car." },
		{ "WII_SML", "SMALL" },
		{ "WII_MED", "MEDIUM" },
		{ "WII_LRG", "LARGE" },
		{ "WII_SDF", "Save failed! Check the SD card: it must be inserted, unlocked and not full." },
		// The Load screen's Quick Save row.  Found by auditing every label this port
		// references against this table: the card gets the USER'S untouched .gxt
		// (deploy-wii.bat defaults to "Assets: untouched"), so any label added to
		// utils/gxt/american.txt is absent at runtime and this table is the only
		// thing that renders it.  This one was missing, so the row came out blank on
		// an untouched install while looking fine on a card whose .gxt had been
		// replaced.
		{ "WII_QSV", "Quick Save" },
		{ "WII_AIC", "AIM IN CAR" },
		{ "WII_DBW", "DRIVE-BY WEAPONS" },

		// The cheat menu.  Audited the same way as the rows above: every key referenced
		// by the MENUPAGE_CHEATS pages has an entry here, because on an untouched card
		// none of them exist in the runtime .gxt and an absent key renders as the key
		// name followed by "missing".
		//
		// Each row is "<official cheat word> - <what it does>".  The words are the ones
		// the desktop build types -- taken from the matcher in Pad.cpp, which records
		// them as comments beside each handler ("ASPIRINE", "THUGSTOOLS", ...) -- so the
		// menu says something the player can match against any written cheat list.
		//
		// Page titles.  WII_CPL/WII_CWH/WII_CVH/WII_CDB are used twice on purpose, as
		// the hub's row label and as the sub-page's own title, which is how the options
		// pages already do it.  WII_CHP is the Change Player row and is deliberately NOT
		// WII_CPL: that key belongs to the Player page, and sharing it made the row
		// render as the word "PLAYER".
		{ "WII_CHE", "CHEATS" },
		{ "WII_CHT", "CHEATS" },
		{ "WII_CPL", "PLAYER" },
		{ "WII_CWH", "WORLD" },
		{ "WII_CVH", "VEHICLES" },
		{ "WII_CDB", "DEBUG" },

		// Each row names the word the engine's own cheat matcher uses for that handler,
		// so typing the word does the same thing, and then the official effect from the
		// game's cheat list -- not a paraphrase.  Money has no typed word in the desktop
		// matcher, so it names the effect only rather than inventing a code that does
		// not exist.
		//
		// Player: Tommy's own health, armour, money, guns, skin, jump and heat.
		{ "WII_HLT", "ASPIRINE - Full Health" },
		{ "WII_ARO", "PRECIOUSPROTECTION - Full Armour" },
		{ "WII_MON", "MONEY - +$250,000" },
		{ "WII_WPN", "THUGSTOOLS - Weapon Set 1" },
		{ "WII_CHP", "STILLLIKEDRESSINGUP - Change Character" },
		{ "WII_KAN", "KANGAROO - High Jump" },
		{ "WII_WUU", "YOUWONTTAKEMEALIVE - Raise Wanted Level" },
		{ "WII_WDN", "LEAVEMEALONE - Lower Wanted Level" },

		// World: weather and clock, the pedestrian cheats, then the one that levels the
		// traffic.  OURGODGIVENRIGHTTOBEARARMS arms the pedestrians, it is not a weapon
		// give, and GRIPISEVERYTHING is a vehicle change -- both used to sit under Player.
		{ "WII_SUN", "APLEASANTDAY - Sunny" },
		{ "WII_CLD", "ABITDRIEG - Cloudy" },
		{ "WII_RAI", "CATSANDDOGS - Rainy" },
		{ "WII_FOG", "CANTSEEATHING - Foggy" },
		{ "WII_FWX", "LIFEISPASSINGMEBY - Speed Up Clock" },
		{ "WII_FTM", "ONSPEED - Fast Motion" },
		{ "WII_STM", "BOOOOOORING - Slow Motion" },
		{ "WII_MAY", "FIGHTFIGHTFIGHT - Peds Riot" },
		{ "WII_ATK", "NOBODYLIKESME - Peds Attack You" },
		{ "WII_AWP", "OURGODGIVENRIGHTTOBEARARMS - Peds Carry Weapons" },
		{ "WII_BUP", "BIGBANG - Blow Up All Cars" },

		// Vehicles: the ten spawns the original cheat words reach, then the ones
		// that change how a car drives.
		{ "WII_RHI", "PANZER - Rhino Tank" },
		{ "WII_BLD", "TRAVELINSTYLE - Bloodring Banger" },
		{ "WII_ROM", "THELASTRIDE - Romero's Hearse" },
		{ "WII_LOV", "ROCKANDROLLCAR - Love Fist Limo" },
		{ "WII_TRS", "RUBBISHCAR - Trashmaster" },
		{ "WII_BLB", "GETTHEREQUICKLY - Bloodring Banger B" },
		{ "WII_SBT", "GETTHEREFAST - Sabre Turbo" },
		{ "WII_CAD", "BETTERTHANWALKING - Golf Caddy" },
		{ "WII_HTA", "GETTHEREFASTINDEED - Hotring Racer A" },
		{ "WII_HTB", "GETTHEREAMAZINGLYFAST - Hotring Racer B" },
		{ "WII_CCB", "COMEFLYWITHME - Flying Cars" },
		{ "WII_GRP", "GRIPISEVERYTHING - Better Handling" },
		{ "WII_WCL", "SPIDERCAR - Cars Climb Walls" },
		{ "WII_HEL", "CARSAREHELI - Cars Fly" },

		// Debug: the render and debug toggles, including the water ones.  Only
		// WHEELSAREALLINEED is an original code; the other three are the port's own.
		{ "WII_WHL", "WHEELSAREALLINEED - Invisible Cars" },
		{ "WII_NSB", "SEABEDCHEAT - Hide Sea Bed" },
		{ "WII_WLY", "WATERLAYERSCHEAT - Water Layers" },
		{ "WII_DBG", "PEDDEBUG - Debug Display" },
	};
	static wchar converted[ARRAY_SIZE(fallbacks)][96];

	for (int i = 0; i < ARRAY_SIZE(fallbacks); i++) {
		if (strcmp(key, fallbacks[i].key) != 0)
			continue;
		if (converted[i][0] == '\0')
			AsciiToUnicode(fallbacks[i].text, converted[i]);
		return converted[i];
	}
	return nil;
}
#endif

wchar*
CText::Get(const char *key)
{
	uint8 result = false;
#if defined (FIX_BUGS) || defined(FIX_BUGS_64)
	wchar *outstr = keyArray.Search(key, data.chars, &result);
#else
	wchar *outstr = keyArray.Search(key, &result);
#endif

	if (!result && bHasMissionTextOffsets && bIsMissionTextLoaded)
#if defined (FIX_BUGS) || defined(FIX_BUGS_64)
		outstr = mission_keyArray.Search(key, mission_data.chars, &result);
#else
		outstr = mission_keyArray.Search(key, &result);
#endif
#ifdef NINTENDO_WII
	if (!result) {
		wchar *fallback = WiiFallbackText(key);
		if (fallback)
			return fallback;
	}
#endif
	return outstr;
}

wchar UpperCaseTable[128] = {
	128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138,
	139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149,
	150, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137,
	138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148,
	149, 173, 173, 175, 176, 177, 178, 179, 180, 181, 182,
	183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193,
	194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204,
	205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215,
	216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226,
	227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237,
	238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248,
	249, 250, 251, 252, 253, 254, 255
};

wchar FrenchUpperCaseTable[128] = {
	128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138,
	139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149,
	150, 65, 65, 65, 65, 132, 133, 69, 69, 69, 69, 73, 73,
	73, 73, 79, 79, 79, 79, 85, 85, 85, 85, 173, 173, 175,
	176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186,
	187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197,
	198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208,
	209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219,
	220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230,
	231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241,
	242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252,
	253, 254, 255
};

wchar
CText::GetUpperCase(wchar c)
{
	switch (encoding)
	{
	case 'e':
		if (c >= 'a' && c <= 'z')
			return c - 32;
		break;
	case 'f':
		if (c >= 'a' && c <= 'z')
			return c - 32;

		if (c >= 128 && c <= 255)
			return FrenchUpperCaseTable[c-128];
		break;
	case 'g':
	case 'i':
	case 's':
		if (c >= 'a' && c <= 'z')
			return c - 32;

		if (c >= 128 && c <= 255)
			return UpperCaseTable[c-128];
		break;
	default:
		break;
	}
	return c;
}

void
CText::UpperCase(wchar *s)
{
	while(*s){
		*s = GetUpperCase(*s);
		s++;
	}
}

void
CText::GetNameOfLoadedMissionText(char *outName)
{
	strcpy(outName, szMissionTableName);
}

bool
CText::ReadChunkHeader(ChunkHeader *buf, int32 file, size_t *offset)
{
#ifdef THIS_IS_STUPID
	char *_buf = (char*)buf;
	for (int i = 0; i < sizeof(ChunkHeader); i++) {
		if(CFileMgr::Read(file, &_buf[i], 1) != 1)
			return false;
		(*offset)++;
	}
#else
	// original code loops 8 times to read 1 byte with CFileMgr::Read, that's retarded
	if(CFileMgr::Read(file, (char*)buf, sizeof(ChunkHeader)) != sizeof(ChunkHeader))
		return false;
	*offset += sizeof(ChunkHeader);
#endif
#ifdef BIGENDIAN
	buf->size = BSWAP_I32(buf->size);
#endif
	return true;
}

void
CText::LoadMissionText(char *MissionTableName)
{
	char filename[32];
	CMessages::ClearAllMessagesDisplayedByGame();

	mission_keyArray.Unload();
	mission_data.Unload();

	bool search_result = false;
	int missionTableId = 0;

	for (missionTableId = 0; missionTableId < MissionTextOffsets.size; missionTableId++) {
		if (strncmp(MissionTextOffsets.data[missionTableId].szMissionName, MissionTableName, strlen(MissionTextOffsets.data[missionTableId].szMissionName)) == 0) {
			search_result = true;
			break;
		}
	}

	if (!search_result) {
		printf("CText::LoadMissionText - couldn't find %s", MissionTableName);
		return;
	}

	CFileMgr::SetDir("TEXT");
	switch (FrontEndMenuManager.m_PrefsLanguage) {
	case CMenuManager::LANGUAGE_AMERICAN:
		sprintf(filename, "AMERICAN.GXT");
		break;
	case CMenuManager::LANGUAGE_FRENCH:
		sprintf(filename, "FRENCH.GXT");
		break;
	case CMenuManager::LANGUAGE_GERMAN:
		sprintf(filename, "GERMAN.GXT");
		break;
	case CMenuManager::LANGUAGE_ITALIAN:
		sprintf(filename, "ITALIAN.GXT");
		break;
	case CMenuManager::LANGUAGE_SPANISH:
		sprintf(filename, "SPANISH.GXT");
		break;
#ifdef MORE_LANGUAGES
	case CMenuManager::LANGUAGE_POLISH:
		sprintf(filename, "POLISH.GXT");
		break;
	case CMenuManager::LANGUAGE_RUSSIAN:
		sprintf(filename, "RUSSIAN.GXT");
		break;
	case CMenuManager::LANGUAGE_JAPANESE:
		sprintf(filename, "JAPANESE.GXT");
		break;
#endif
	}
	CTimer::Suspend();
	int file = CFileMgr::OpenFile(filename, "rb");
	if(file == 0){
		CFileMgr::SetDir("");
		CTimer::Resume();
		return;
	}
	CFileMgr::Seek(file, MissionTextOffsets.data[missionTableId].offset, SEEK_SET);

	char TableCheck[8];
	CFileMgr::Read(file, TableCheck, 8);
	if (strncmp(TableCheck, MissionTableName, 8) != 0)
		printf("CText::LoadMissionText - expected to find %s in the text file", MissionTableName);

	bool tkey_loaded = false, tdat_loaded = false;
	ChunkHeader m_ChunkHeader;
	while (!tkey_loaded || !tdat_loaded) {
		size_t bytes_read = 0;
		if(!ReadChunkHeader(&m_ChunkHeader, file, &bytes_read)){
			CFileMgr::CloseFile(file);
			CFileMgr::SetDir("");
			return;
		}
		if (m_ChunkHeader.size != 0) {
			if (strncmp(m_ChunkHeader.magic, "TKEY", 4) == 0) {
				size_t bytes_read = 0;
				mission_keyArray.Load(m_ChunkHeader.size, file, &bytes_read);
				tkey_loaded = true;
			} else if (strncmp(m_ChunkHeader.magic, "TDAT", 4) == 0) {
				size_t bytes_read = 0;
				mission_data.Load(m_ChunkHeader.size, file, &bytes_read);
				tdat_loaded = true;
			} else
				CFileMgr::Seek(file, m_ChunkHeader.size, SEEK_CUR);
		}
	}

	mission_keyArray.Update(mission_data.chars);
	CFileMgr::CloseFile(file);
	CTimer::Resume();
	CFileMgr::SetDir("");
	strcpy(szMissionTableName, MissionTableName);
	bIsMissionTextLoaded = true;
}


void
CKeyArray::Load(size_t length, int file, size_t* offset)
{
	char *rawbytes;

	// You can make numEntries size_t if you want to exceed 32-bit boundaries, everything else should be ready.
	numEntries = (int)(length / sizeof(CKeyEntry));
	entries = new CKeyEntry[numEntries];
	rawbytes = (char*)entries;

#ifdef THIS_IS_STUPID
	for (uint32 i = 0; i < length; i++) {
		CFileMgr::Read(file, &rawbytes[i], 1);
		(*offset)++;
	}
#else
	CFileMgr::Read(file, rawbytes, length);
	*offset += length;
#endif
#ifdef BIGENDIAN
	for(int i = 0; i < numEntries; i++)
		entries[i].valueOffset = BSWAP_U32(entries[i].valueOffset);
#endif
}

void
CKeyArray::Unload(void)
{
	delete[] entries;
	entries = nil;
	numEntries = 0;
}

void
CKeyArray::Update(wchar *chars)
{
#if !defined(FIX_BUGS) && !defined(FIX_BUGS_64)
	int i;
	for(i = 0; i < numEntries; i++)
		entries[i].value = (wchar*)((uint8*)chars + (uintptr)entries[i].value);
#endif
}

CKeyEntry*
CKeyArray::BinarySearch(const char *key, CKeyEntry *entries, int16 low, int16 high)
{
	int mid;
	int diff;

	if(low > high)
		return nil;

	mid = (low + high)/2;
	diff = strcmp(key, entries[mid].key);
	if(diff == 0)
		return &entries[mid];
	if(diff < 0)
		return BinarySearch(key, entries, low, mid-1);
	if(diff > 0)
		return BinarySearch(key, entries, mid+1, high);
	return nil;
}

wchar*
#if defined (FIX_BUGS) || defined(FIX_BUGS_64)
CKeyArray::Search(const char *key, wchar *data, uint8 *result)
#else
CKeyArray::Search(const char *key, uint8 *result)
#endif
{
	CKeyEntry *found;
	char errstr[25];
	int i;

#if defined (FIX_BUGS) || defined(FIX_BUGS_64)
	found = BinarySearch(key, entries, 0, numEntries-1);
	if (found) {
		*result = true;
		return (wchar*)((uint8*)data + found->valueOffset);
	}
#else
	found = BinarySearch(key, entries, 0, numEntries-1);
	if (found) {
		*result = true;
		return found->value;
	}
#endif
	*result = false;
#ifdef MASTER
	sprintf(errstr, "");
#else
	sprintf(errstr, "%s missing", key);
#endif // MASTER
	for(i = 0; i < 25; i++)
		WideErrorString[i] = errstr[i];
	return WideErrorString;
}

void
CData::Load(size_t length, int file, size_t * offset)
{
	char *rawbytes;

	// You can make numChars size_t if you want to exceed 32-bit boundaries, everything else should be ready.
	numChars = (int)(length / sizeof(wchar));
	chars = new wchar[numChars];
	rawbytes = (char*)chars;

#ifdef THIS_IS_STUPID
	for(uint32 i = 0; i < length; i++){
		CFileMgr::Read(file, &rawbytes[i], 1);
		(*offset)++;
	}
#else
	CFileMgr::Read(file, rawbytes, length);
	*offset += length;
#endif
#ifdef BIGENDIAN
	for(int i = 0; i < numChars; i++)
		chars[i] = static_cast<wchar>(BSWAP_U16(static_cast<uint16>(chars[i])));
#endif
}

void
CData::Unload(void)
{
	delete[] chars;
	chars = nil;
	numChars = 0;
}

void
CMissionTextOffsets::Load(size_t table_size, int file, size_t *offset, int)
{
#ifdef THIS_IS_STUPID
	size_t num_of_entries = table_size / sizeof(CMissionTextOffsets::Entry);
	for (size_t mi = 0; mi < num_of_entries; mi++) {
		for (uint32 i = 0; i < sizeof(data[mi].szMissionName); i++) {
			CFileMgr::Read(file, &data[i].szMissionName[i], 1);
			(*offset)++;
		}
		char* _buf = (char*)&data[mi].offset;
		for (uint32 i = 0; i < sizeof(data[mi].offset); i++) {
			CFileMgr::Read(file, &_buf[i], 1);
			(*offset)++;
		}
	}
	size = (uint16)num_of_entries;
#else
	// not exact VC code but smaller and better :P

	// You can make this size_t if you want to exceed 32-bit boundaries, everything else should be ready.
	size = (uint16) (table_size / sizeof(CMissionTextOffsets::Entry));
	CFileMgr::Read(file, (char*)data, sizeof(CMissionTextOffsets::Entry) * size);
	*offset += sizeof(CMissionTextOffsets::Entry) * size;
#endif
#ifdef BIGENDIAN
	for(uint16 i = 0; i < size; i++)
		data[i].offset = BSWAP_U32(data[i].offset);
#endif
}

char*
UnicodeToAscii(wchar *src)
{
	static char aStr[256];
	int len;
	for(len = 0; *src != '\0' && len < 256-1; len++, src++)
#ifdef MORE_LANGUAGES
		if(*src < 128 || ((CGame::russianGame || CGame::japaneseGame) && *src < 256))
#else
		if(*src < 128)
#endif
			aStr[len] = *src;
		// convert to CP1252
		else if(*src <= 131)
			aStr[len] = *src + 64;
		else if (*src <= 141)
			aStr[len] = *src + 66;
		else if (*src <= 145)
			aStr[len] = *src + 68;
		else if (*src <= 149)
			aStr[len] = *src + 71;
		else if (*src <= 154)
			aStr[len] = *src + 73;
		else if (*src <= 164)
			aStr[len] = *src + 75;
		else if (*src <= 168)
			aStr[len] = *src + 77;
		else if (*src <= 204)
			aStr[len] = *src + 80;
		else switch (*src) {
		case 205: aStr[len] = 209; break;
		case 206: aStr[len] = 241; break;
		case 207: aStr[len] = 191; break;
		default: aStr[len] = '#'; break;
		}
	aStr[len] = '\0';
	return aStr;
}

char*
UnicodeToAsciiForSaveLoad(wchar *src)
{
	static char aStr[256];
	int len;
	for(len = 0; *src != '\0' && len < 256; len++, src++)
		if(*src < 256)
			aStr[len] = *src;
		else
			aStr[len] = '#';
	aStr[len] = '\0';
	return aStr;
}

char*
UnicodeToAsciiForMemoryCard(wchar *src)
{
	static char aStr[256];
	int len;
	for(len = 0; *src != '\0' && len < 256; len++, src++)
		if(*src < 256)
			aStr[len] = *src;
		else
			aStr[len] = '#';
	aStr[len] = '\0';
	return aStr;
}

void
TextCopy(wchar *dst, const wchar *src)
{
	while((*dst++ = *src++) != '\0');
}
