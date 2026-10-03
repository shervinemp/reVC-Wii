#include <cmath>
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
//    while the alarm is still running would arm it twice.  So there is exactly one
//    clip in flight at a time, each in its own static buffer.
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
constexpr u32 kSampleRate = 6000;

// The alarm cancels itself one tick after the last packet, so a new stream may
// only start a couple of packets past the nominal end.
constexpr u32 kGuardPackets = 2;

// How long the speaker stays powered after a clip.  A gunfight or a flick through
// stations asks for sound several times a second, and powering it up each time
// costs a noticeable gap before the first sound.
constexpr u32 kLingerMs = 4000;

// Enabling normally takes a fraction of a second; past this something is wrong
// (remote lost, queue full) and the attempt is dropped.
constexpr u32 kWarmupTimeoutMs = 2000;

// The clips, in priority order: a later one replaces an earlier one that is still
// waiting its turn.  Each has a length (a whole number of packets) and an age past
// which waiting made it pointless, because a gunshot a second late is not a
// gunshot.
enum Clip
{
	CLIP_SHOT,
	CLIP_STATIC,
	CLIP_RING,
	CLIP_COUNT,
	CLIP_NONE = CLIP_COUNT
};

constexpr u32 kShotBytes = 15*kPacketBytes;		// 100 ms
constexpr u32 kStaticBytes = 60*kPacketBytes;	// 400 ms
constexpr u32 kRingBytes = 150*kPacketBytes;	// 1 s

constexpr u32 kClipBytes[CLIP_COUNT] = { kShotBytes, kStaticBytes, kRingBytes };
constexpr u32 kClipMaxAgeMs[CLIP_COUNT] = { 100, 400, 900 };

u8 s_shot[kShotBytes];
u8 s_static[kStaticBytes];
u8 s_ring[kRingBytes];
u8 *const s_clipData[CLIP_COUNT] = { s_shot, s_static, s_ring };
bool s_built[CLIP_COUNT];

enum State
{
	STATE_OFF,
	STATE_WARMING,
	STATE_ON
};

State s_state = STATE_OFF;
Clip s_pending = CLIP_NONE;
u64 s_pendingTime = 0;
u64 s_clipEnd = 0;
u64 s_deadline = 0;	// warm-up timeout while warming, power-down time while on

u32 s_noise = 0x2545F491u;

// White noise in -1..1.
float
noise(void)
{
	s_noise ^= s_noise << 13;
	s_noise ^= s_noise >> 17;
	s_noise ^= s_noise << 5;
	return (float)(s16)(s_noise >> 16)/32768.0f;
}

s16
toSample(float value)
{
	if(value > 1.0f)
		value = 1.0f;
	else if(value < -1.0f)
		value = -1.0f;
	return (s16)(value*32000.0f);
}

void
encode(const s16 *pcm, u32 samples, u8 *out)
{
	WPADEncStatus encoder;
	std::memset(&encoder, 0, sizeof(encoder));
	WPAD_EncodeData(&encoder, 0, pcm, (s32)samples, out);
}

// Tuning static: white noise with a short attack and a fade-out, so it starts and
// ends without a click.
void
buildStatic(void)
{
	constexpr u32 samples = kStaticBytes*2;
	s16 pcm[samples];
	const u32 attack = samples/20;
	const u32 release = samples*2/5;
	for(u32 i = 0; i < samples; i++){
		float gain = 1.0f;
		if(i < attack)
			gain = (float)i/(float)attack;
		else if(i >= samples - release)
			gain = (float)(samples - i)/(float)release;
		pcm[i] = toSample(noise()*0.55f*gain);
	}
	encode(pcm, samples, s_static);
}

// A gunshot: a sharp crack of noise that dies in a few milliseconds over a low
// thump, which is most of what a small speaker can say about a gun.
void
buildShot(void)
{
	constexpr u32 samples = kShotBytes*2;
	s16 pcm[samples];
	for(u32 i = 0; i < samples; i++){
		const float t = (float)i/(float)kSampleRate;
		const float crack = noise()*std::exp(-t/0.012f);
		const float thump = std::sin(6.2831853f*170.0f*t)*std::exp(-t/0.035f);
		pcm[i] = toSample(0.7f*crack + 0.45f*thump);
	}
	encode(pcm, samples, s_shot);
}

// A telephone: two bursts of a bell-like pair of tones, each chopped at 24 Hz into
// the "brrr" of an old ringer, with a pause between.
void
buildRing(void)
{
	constexpr u32 samples = kRingBytes*2;
	s16 pcm[samples];
	for(u32 i = 0; i < samples; i++){
		const float t = (float)i/(float)kSampleRate;
		// Two bursts: 0.00-0.38 s and 0.48-0.86 s.
		float local = -1.0f;
		if(t < 0.38f)
			local = t;
		else if(t >= 0.48f && t < 0.86f)
			local = t - 0.48f;
		if(local < 0.0f){
			pcm[i] = 0;
			continue;
		}
		// Short fades keep the edges from clicking.
		float edge = local < 0.01f ? local/0.01f : (local > 0.37f ? (0.38f - local)/0.01f : 1.0f);
		const float chop = 0.5f*(1.0f + std::sin(6.2831853f*24.0f*t));
		const float tones = 0.5f*std::sin(6.2831853f*1350.0f*t) + 0.5f*std::sin(6.2831853f*1700.0f*t);
		pcm[i] = toSample(0.6f*edge*chop*tones);
	}
	encode(pcm, samples, s_ring);
}

void
build(Clip clip)
{
	switch(clip){
	case CLIP_SHOT:
		buildShot();
		break;
	case CLIP_STATIC:
		buildStatic();
		break;
	default:
		buildRing();
		break;
	}
	s_built[clip] = true;
}

void
powerDown(void)
{
	WPAD_ControlSpeaker(kChannel, 0);
	s_state = STATE_OFF;
	s_pending = CLIP_NONE;
}

// Asks for a clip, powering the speaker up if it is off.  A request that is still
// waiting when a better one arrives is replaced rather than queued: there is room
// for one, and the better one is the one worth hearing.
void
request(Clip clip)
{
	if(!WiiRemoteSpeakerEnabled)
		return;

	u32 expansion;
	if(WPAD_Probe(kChannel, &expansion) != WPAD_ERR_NONE)
		return;

	if(s_pending == CLIP_NONE || clip >= s_pending){
		s_pending = clip;
		s_pendingTime = gettime();
	}
	if(s_state == STATE_OFF){
		WPAD_ControlSpeaker(kChannel, 1);
		s_state = STATE_WARMING;
		s_deadline = gettime() + millisecs_to_ticks(kWarmupTimeoutMs);
	}
}

} // namespace

void
WiiSpeakerPlayTuneStatic(void)
{
	request(CLIP_STATIC);
}

void
WiiSpeakerPlayShot(void)
{
	request(CLIP_SHOT);
}

void
WiiSpeakerPlayRing(void)
{
	request(CLIP_RING);
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

	// Whatever waited too long, through warm-up or behind another clip, is dropped.
	if(s_pending != CLIP_NONE && now - s_pendingTime > millisecs_to_ticks(kClipMaxAgeMs[s_pending]))
		s_pending = CLIP_NONE;

	if(s_pending != CLIP_NONE && now >= s_clipEnd){
		const Clip clip = s_pending;
		if(!s_built[clip])
			build(clip);
		WPAD_SendStreamData(kChannel, s_clipData[clip], kClipBytes[clip]);
		s_pending = CLIP_NONE;
		s_clipEnd = now + microsecs_to_ticks((kClipBytes[clip]/kPacketBytes + kGuardPackets)*kPacketMicros);
		s_deadline = s_clipEnd + millisecs_to_ticks(kLingerMs);
	}else if(s_pending == CLIP_NONE && now > s_deadline){
		powerDown();
	}
}
