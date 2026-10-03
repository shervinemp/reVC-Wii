#include "common.h"

#include "AimAssist.h"

#ifdef AIM_ASSIST

#include "Camera.h"
#include "Draw.h"
#include "General.h"
#include "Pad.h"
#include "PedIK.h"
#include "PlayerPed.h"
#include "Pools.h"
#include "Timer.h"
#include "Vehicle.h"
#include "WeaponInfo.h"
#include "World.h"

int8 CAimAssist::bEnabled = true;

namespace
{

// Angles are radians, measured from the crosshair ray.  A target only assists
// inside a cone that follows how big it looks: wide up close, narrow far away.
const float kMaxCone = DEGTORAD(7.0f);
const float kMinCone = DEGTORAD(2.5f);
const float kTargetRadius = 1.2f;	// metres

// Look speed while the aim button is held, target or not.
const float kAimLookScale = 0.7f;

// Further look speed with the crosshair dead on a target (on top of the above);
// 1.0 at the edge of the cone.
const float kFrictionMin = 0.6f;

// Share of the remaining error toward the chest that is closed per second at the
// centre of the cone, fading to nothing at its edge.
const float kPullPerSecond = 1.5f;

// Look input smaller than this per frame is stick noise, not the player asking to
// leave the target.
const float kBreakAwayInput = 0.002f;

// A new target has to be this much nearer its own cone's centre to take over.
const float kSwitchRatio = 0.7f;

// How often the held target's line of sight is rechecked; it is a world ray cast.
const uint32 kSightIntervalMs = 250;

int32 s_targetHandle = -1;
bool s_targetVisible = false;
uint32 s_sightCheckTime = 0;

bool
CanAssist(CPlayerPed *player, CPad *pad)
{
	if(!CAimAssist::bEnabled || player == nil || player->bInVehicle)
		return false;
	if(!pad->GetTarget() || pad->GetLookBehindForPed() || pad->ArePlayerControlsDisabled())
		return false;
	if(player->m_nSelectedWepSlot != player->m_currentWeapon || player->m_nMoveState == PEDMOVE_SPRINT)
		return false;
	// Not fists, melee or thrown weapons.
	CWeaponInfo *info = CWeaponInfo::GetWeaponInfo(player->GetWeapon()->m_eWeaponType);
	return info->m_nWeaponSlot > 2 && info->IsFlagSet(WEAPONFLAG_CANAIM);
}

bool
IsTargetable(CPed *ped, CPlayerPed *player)
{
	// The same rules as the Classic lock-on (CPlayerPed::FindWeaponLockOnTarget).
	if(ped == nil || ped == player || ped->DyingOrDead() || ped->m_leader == player || ped->bNeverEverTargetThisPed)
		return false;
	return !ped->bInVehicle || (ped->m_pMyVehicle && ped->m_pMyVehicle->IsBike());
}

CVector
ChestOf(CPed *ped)
{
	CVector chest;
	ped->m_pedIK.GetComponentPosition(chest, PED_MID);
	return chest;
}

// Camera Alpha/Beta for a direction, the inverse of how CCam builds its Front.
void
ToAngles(const CVector &direction, float &alpha, float &beta)
{
	alpha = Asin(Clamp(direction.z, -1.0f, 1.0f));
	beta = Atan2(-direction.y, -direction.x);
}

}

void
CAimAssist::Process(const CVector &source, const CVector &front, const CVector &up, float fov,
                    float &alphaOffset, float &betaOffset)
{
	CPlayerPed *player = FindPlayerPed();
	CPad *pad = CPad::GetPad(0);
	if(front.MagnitudeSqr() < 0.5f || !CanAssist(player, pad)){
		s_targetHandle = -1;
		return;
	}

	// What the player asked for, before any of the below touches it.
	float inputAlpha = alphaOffset, inputBeta = betaOffset;
	alphaOffset *= kAimLookScale;
	betaOffset *= kAimLookScale;

	// The ray shots leave along, as CCamera::Find3rdPersonCamTargetVector builds it.
	float angleX = DEGTORAD((CCamera::m_f3rdPersonCHairMultX-0.5f) * 1.8f * 0.5f * fov * CDraw::GetAspectRatio());
	float angleY = DEGTORAD((0.5f-CCamera::m_f3rdPersonCHairMultY) * 1.8f * 0.5f * fov);
	CVector aim = front + up*Tan(angleY) + CrossProduct(front, up)*Tan(angleX);
	aim.Normalise();

	float range = CWeaponInfo::GetWeaponInfo(player->GetWeapon()->m_eWeaponType)->m_fRange;

	// Where the held target stands now, as a share of its own cone (0 centre, 1 rim).
	CPed *held = s_targetHandle >= 0 ? CPools::GetPedPool()->GetAt(s_targetHandle) : nil;
	float heldRatio = 2.0f;
	CPed *best = nil;
	float bestRatio = 1.0f;
	CVector bestChest;

	for(int i = CPools::GetPedPool()->GetSize() - 1; i >= 0; i--){
		CPed *ped = CPools::GetPedPool()->GetSlot(i);
		if(!IsTargetable(ped, player))
			continue;

		// Cheap reject on the ped's origin before asking for its chest.
		CVector rough = ped->GetPosition() - source;
		float roughDist = rough.Magnitude();
		if(roughDist > range + 2.0f || DotProduct(rough, aim) < 0.9f*roughDist)
			continue;

		CVector chest = ChestOf(ped);
		CVector toTarget = chest - source;
		float dist = toTarget.Magnitude();
		if(dist < 0.5f || dist > range)
			continue;
		float cosAngle = DotProduct(toTarget, aim) / dist;
		if(cosAngle <= 0.0f)
			continue;
		float cone = Clamp(Atan2(kTargetRadius, dist), kMinCone, kMaxCone);
		float ratio = Acos(Min(cosAngle, 1.0f)) / cone;
		if(ratio >= 1.0f)
			continue;

		if(ped == held)
			heldRatio = ratio;
		if(ratio < bestRatio){
			best = ped;
			bestRatio = ratio;
			bestChest = chest;
		}
	}

	// Keep the held target unless another is clearly nearer the crosshair.
	if(held && best != held && heldRatio < 1.0f && bestRatio >= heldRatio*kSwitchRatio){
		best = held;
		bestRatio = heldRatio;
		bestChest = ChestOf(held);
	}
	if(best == nil){
		s_targetHandle = -1;
		return;
	}

	// Only assist what a shot could reach, rechecked now and then.
	int32 handle = CPools::GetPedPool()->GetIndex(best);
	if(handle != s_targetHandle || CTimer::GetTimeInMilliseconds() - s_sightCheckTime > kSightIntervalMs){
		s_targetVisible = player->OurPedCanSeeThisOne(best, true);
		s_sightCheckTime = CTimer::GetTimeInMilliseconds();
		s_targetHandle = handle;
	}
	if(!s_targetVisible)
		return;

	// 1 on the target, 0 at the rim of its cone.
	float closeness = 1.0f - bestRatio;

	float slow = 1.0f - (1.0f - kFrictionMin)*closeness;
	alphaOffset *= slow;
	betaOffset *= slow;

	// Ease the crosshair onto the chest, unless the player is pushing away.
	CVector toChest = bestChest - source;
	toChest.Normalise();
	float aimAlpha, aimBeta, chestAlpha, chestBeta;
	ToAngles(aim, aimAlpha, aimBeta);
	ToAngles(toChest, chestAlpha, chestBeta);
	float errorAlpha = chestAlpha - aimAlpha;
	float errorBeta = chestBeta - aimBeta;
	while(errorBeta > PI) errorBeta -= 2*PI;
	while(errorBeta < -PI) errorBeta += 2*PI;

	bool pushingAway = inputAlpha*errorAlpha + inputBeta*errorBeta < 0.0f &&
		Abs(inputAlpha) + Abs(inputBeta) > kBreakAwayInput;
	if(pushingAway)
		return;

	float pull = Min(1.0f, kPullPerSecond * CTimer::GetTimeStep()/50.0f) * closeness;
	alphaOffset += errorAlpha*pull;
	betaOffset += errorBeta*pull;
}

#endif
