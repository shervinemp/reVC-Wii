#include "common.h"

#include "main.h"
#include "WeaponEffects.h"
#include "TxdStore.h"
#include "Sprite.h"
#include "PlayerPed.h"
#include "World.h"
#include "WeaponType.h"

RwTexture *gpCrossHairTex;

CWeaponEffects gCrossHair;

CWeaponEffects::CWeaponEffects()
{
	
}

CWeaponEffects::~CWeaponEffects()
{
	
}

void
CWeaponEffects::Init(void)
{
	gCrossHair.m_bActive = false;
	gCrossHair.m_vecPos = CVector(0.0f, 0.0f, 0.0f);
	gCrossHair.m_nRed = 255;
	gCrossHair.m_nGreen = 0;
	gCrossHair.m_nBlue = 0;
	gCrossHair.m_nAlpha = 127;
	gCrossHair.m_fSize = 1.0f;
	gCrossHair.m_fRotation = 0.0f;
	
	
	CTxdStore::PushCurrentTxd();
	int32 slot = CTxdStore::FindTxdSlot("particle");
	CTxdStore::SetCurrentTxd(slot);
	
	gpCrossHairTex    = RwTextureRead("target256", "target256m");
	
	CTxdStore::PopCurrentTxd();
}

void
CWeaponEffects::Shutdown(void)
{
	RwTextureDestroy(gpCrossHairTex);
	gpCrossHairTex = nil;
}

void
CWeaponEffects::MarkTarget(CVector pos, uint8 red, uint8 green, uint8 blue, uint8 alpha, float size)
{
	gCrossHair.m_bActive = true;
	gCrossHair.m_vecPos = pos;
	gCrossHair.m_fSize = size;
}

void
CWeaponEffects::ClearCrossHair(void)
{
	gCrossHair.m_bActive = false;
}

// How much larger than stock to draw the crosshair.  See the use in Render().
//
// 3.0, from 1.6, which was not enough.  The crosshair is a world-space marker placed by
// CWeaponEffects::MarkTarget and sized by the view window at the target's depth, so it
// shrinks as the target gets further away -- the boost compounds at range rather than
// being a flat offset, which is why a factor that looked reasonable up close still read
// as too small.
static const float kCrossHairScale = 3.0f;

void
CWeaponEffects::Render(void)
{
	static float aCrossHairSize[WEAPONTYPE_TOTALWEAPONS] =
	{
		1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
		0.4f, 0.4f,
		0.5f,
		0.3f,
		0.9f, 0.9f, 0.9f,
		0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f,
		0.1f, 0.1f,
		1.0f,
		0.6f,
		0.7f,
		0.0f, 0.0f
	};



	if ( gCrossHair.m_bActive )
	{
		float size = aCrossHairSize[FindPlayerPed()->GetWeapon()->m_eWeaponType];
		
		RwRenderStateSet(rwRENDERSTATEZWRITEENABLE,      (void *)FALSE);
		RwRenderStateSet(rwRENDERSTATEZTESTENABLE,       (void *)FALSE);
		RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void *)TRUE);
		RwRenderStateSet(rwRENDERSTATESRCBLEND,          (void *)rwBLENDSRCALPHA);
#ifdef FIX_BUGS
		RwRenderStateSet(rwRENDERSTATEDESTBLEND,         (void *)rwBLENDINVSRCALPHA);
#else
		RwRenderStateSet(rwRENDERSTATEDESTBLEND,         (void *)rwBLENDINVDESTALPHA);
#endif
		RwRenderStateSet(rwRENDERSTATETEXTURERASTER,     (void *)RwTextureGetRaster(gpCrossHairTex));

		RwV3d pos;
		float w, h;
		if ( CSprite::CalcScreenCoors(gCrossHair.m_vecPos, &pos, &w, &h, true) )
		{
			PUSH_RENDERGROUP("CWeaponEffects::Render");

			// The stock size is a couple of pixels at 480i, which is not something you
			// can hold a TV at and aim with -- and on this port the crosshair is the
			// pointer's only aiming reference whenever the player is unarmed.
			//
			// Scaling w and h scales the circle as well, because it is drawn at
			// size*w, size*h, so one factor enlarges both and leaves the dot-to-circle
			// proportion exactly as it was.  The unarmed case is unaffected in the
			// other direction: its aCrossHairSize entry is 0.0f, so the circle stays
			// collapsed and only the dot grows.
			//
			// No resolution ceiling to worry about: the source is target256, a 256x256
			// texture, and the sprite is drawn far smaller than that, so even at 3x it
			// is magnifying well inside the source's own resolution.  Raising this much
			// further is what would eventually soften it -- the asset is not modified,
			// so there is no higher-resolution source to fall back on.
			w *= kCrossHairScale;
			h *= kCrossHairScale;

			float recipz = 1.0f / pos.z;
			CSprite::RenderOneXLUSprite_Rotate_Aspect(pos.x, pos.y, pos.z,
				w, h,
				255, 88, 100, 158,
				recipz, gCrossHair.m_fRotation, gCrossHair.m_nAlpha);
				
			float recipz2 = 1.0f / pos.z;
			
			CSprite::RenderOneXLUSprite_Rotate_Aspect(pos.x, pos.y, pos.z,
				size*w, size*h,
				107, 134, 247, 158,
				recipz2, TWOPI - gCrossHair.m_fRotation, gCrossHair.m_nAlpha);
						
			gCrossHair.m_fRotation += 0.02f;
			if ( gCrossHair.m_fRotation > TWOPI )
				gCrossHair.m_fRotation = 0.0;

			POP_RENDERGROUP();
		}
			
		RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void *)FALSE);
		RwRenderStateSet(rwRENDERSTATEZWRITEENABLE,      (void *)TRUE);
		RwRenderStateSet(rwRENDERSTATEZTESTENABLE,       (void *)TRUE);
	}
}
