#include <cstring>

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>

#include "WiiSpeaker.h"

int8_t WiiRemoteSpeakerEnabled = 1;

// How libogc streams to the remote, which is what the rest of this file is shaped
// around (read off libwiiuse's disassembly, because none of it is documented):
//
//  - WPAD_SendStreamData does not send anything itself.  It stores the pointer and
//    length it was given and arms a 150 Hz alarm that pushes the buffer to the
//    remote 20 bytes at a time, reading from that pointer AFTER the call has
//    returned.  The buffer therefore has to outlive the call, and a second call
//    while the alarm is still running would arm it twice.
//  - The remote's speaker is configured for Yamaha 4-bit ADPCM at 6 kHz, so the
//    stream is 3000 bytes a second of ADPCM, not PCM.  WPAD_EncodeData is the
//    supported encoder.
//  - Data sent before the speaker has finished enabling is silently dropped, and
//    enabling is a handful of asynchronous writes, so it has to be waited out.
namespace
{

constexpr s32 kChannel = WPAD_CHAN_0;

constexpr u32 kPacketBytes = 20;
constexpr u32 kPacketMicros = 6667;
constexpr u32 kBurstPackets = 60;                          // 400 ms of static
constexpr u32 kBurstBytes = kPacketBytes * kBurstPackets;
constexpr u32 kBurstSamples = kBurstBytes * 2;             // two samples a byte

// The alarm cancels itself one tick after the last packet, so a new stream may
// only start a couple of packets past the nominal end.
constexpr u32 kBurstGuardPackets = 2;

// How long the speaker stays powered after a burst.  Flicking through stations
// retunes several times a second, and powering it up each time costs a
// noticeable gap before the first sound.
constexpr u32 kLingerMs = 4000;

// Enabling normally takes a fraction of a second; past this something is wrong
// (remote lost, queue full) and the attempt is dropped.
constexpr u32 kWarmupTimeoutMs = 2000;

enum State
{
	STATE_OFF,
	STATE_WARMING,
	STATE_ON
};

State s_state = STATE_OFF;
bool s_pending = false;
bool s_burstBuilt = false;
u64 s_burstEnd = 0;
u64 s_deadline = 0;	// warm-up timeout while warming, power-down time while on
u8 s_burst[kBurstBytes];

// White noise with a short attack and a fade-out, so the burst starts and ends
// without a click, encoded once to the remote's ADPCM.
void
buildBurst(void)
{
	s16 pcm[kBurstSamples];
	const u32 attack = kBurstSamples / 20;
	const u32 release = kBurstSamples * 2 / 5;
	u32 lfsr = 0x2545F491u;
	for(u32 i = 0; i < kBurstSamples; i++){
		lfsr ^= lfsr << 13;
		lfsr ^= lfsr >> 17;
		lfsr ^= lfsr << 5;
		float gain = 1.0f;
		if(i < attack)
			gain = (float)i / (float)attack;
		else if(i >= kBurstSamples - release)
			gain = (float)(kBurstSamples - i) / (float)release;
		pcm[i] = (s16)((float)(s16)(lfsr >> 16) * 0.55f * gain);
	}

	WPADEncStatus encoder;
	std::memset(&encoder, 0, sizeof(encoder));
	WPAD_EncodeData(&encoder, 0, pcm, (s32)kBurstSamples, s_burst);
	s_burstBuilt = true;
}

void
powerDown(void)
{
	WPAD_ControlSpeaker(kChannel, 0);
	s_state = STATE_OFF;
	s_pending = false;
}

} // namespace

void
WiiSpeakerPlayTuneStatic(void)
{
	if(!WiiRemoteSpeakerEnabled)
		return;

	u32 expansion;
	if(WPAD_Probe(kChannel, &expansion) != WPAD_ERR_NONE)
		return;

	s_pending = true;
	if(s_state == STATE_OFF){
		WPAD_ControlSpeaker(kChannel, 1);
		s_state = STATE_WARMING;
		s_deadline = gettime() + millisecs_to_ticks(kWarmupTimeoutMs);
	}
}

void
WiiSpeakerService(void)
{
	if(s_state == STATE_OFF)
		return;

	u32 expansion;
	if(!WiiRemoteSpeakerEnabled || WPAD_Probe(kChannel, &expansion) != WPAD_ERR_NONE){
		powerDown();
		return;
	}

	const u64 now = gettime();
	if(s_state == STATE_WARMING){
		if(WPAD_IsSpeakerEnabled(kChannel) <= 0){
			if(now > s_deadline)
				powerDown();
			return;
		}
		s_state = STATE_ON;
		s_deadline = now + millisecs_to_ticks(kLingerMs);
	}

	if(s_pending && now >= s_burstEnd){
		if(!s_burstBuilt)
			buildBurst();
		WPAD_SendStreamData(kChannel, s_burst, kBurstBytes);
		s_pending = false;
		s_burstEnd = now + microsecs_to_ticks((kBurstPackets + kBurstGuardPackets) * kPacketMicros);
		s_deadline = s_burstEnd + millisecs_to_ticks(kLingerMs);
	}else if(!s_pending && now > s_deadline){
		powerDown();
	}
}
