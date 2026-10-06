#include <cmath>
#include <cstdlib>
#include <cstring>

#include <malloc.h>

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>

#include "WiiSpeaker.h"

int8_t WiiRemoteSpeakerEnabled = 1;
int8_t WiiPhoneRemoteMode = 2;

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

// --- phone calls ----------------------------------------------------------------
// A phone call is not a clip: it is a line of speech from the mission audio, as long
// as it is, and it arrives as the game decodes it.  So it gets a buffer of its own,
// sized for the whole line and pre-filled with ADPCM silence, handed to libogc ONCE
// and then filled in from the front as the audio is decoded.  The decoder runs
// ahead of what the TV is playing and the remote starts reading when the first of it
// arrives, so the write position stays ahead of the read position and nothing has to
// be re-armed mid-line.  The allocation is exactly the line's length plus a little
// slack, which is also what ends the stream: libogc stops when it runs off the end.
constexpr u32 kCallBytesPerMs = 3;			// 3000 bytes a second
constexpr u32 kCallSlackBytes = 20*kPacketBytes;	// about 130 ms past the stated length
constexpr u32 kCallMaxBytes = 120*1000*kCallBytesPerMs;
constexpr u8 kAdpcmSilence = 0x08;			// +1/8 step then -1/8 step, around zero
constexpr u32 kCallTvDuckPercent = 25;			// TV level in the "both" mode
constexpr u32 kWakeLingerMs = 8000;			// keeps the speaker up for a call about to start

struct Call
{
	u8 *buffer;
	u32 capacity;
	u32 written;
	u32 sourceRate;
	u32 phase;		// decimator: accumulates the output rate, emits at the input rate
	float accumulator;
	u32 accumulated;
	s16 carry;
	bool hasCarry;
	bool encoderFresh;
	bool active;
	bool started;
	WPADEncStatus encoder;
};
Call s_call;
u64 s_callFreeTime = 0;	// when the buffer libogc may still be reading can be freed
u64 s_wakeUntil = 0;

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
	// A call routed to the remote is over with it; the TV takes the rest of the line
	// (the stream asks WiiSpeakerCallActive every frame).
	s_call.active = false;
}

// Frees the call buffer once libogc cannot still be reading it.
void
releaseCallBuffer(void)
{
	if(s_call.buffer != nullptr && !s_call.active && gettime() > s_callFreeTime){
		std::free(s_call.buffer);
		s_call.buffer = nullptr;
	}
}

// Encodes whatever has been resampled so far into the call buffer.  ADPCM packs
// two samples a byte, so an odd one out is carried to the next call.
void
encodeCall(const s16 *samples, u32 count)
{
	s16 work[1026];
	u32 total = 0;
	if(s_call.hasCarry){
		work[total++] = s_call.carry;
		s_call.hasCarry = false;
	}
	for(u32 i = 0; i < count && total < 1025; i++)
		work[total++] = samples[i];
	const u32 pairs = total & ~1u;
	if(total != pairs){
		s_call.carry = work[total - 1];
		s_call.hasCarry = true;
	}
	if(pairs == 0 || s_call.written + pairs/2 > s_call.capacity - kCallSlackBytes/2)
		return;

	WPAD_EncodeData(&s_call.encoder, s_call.encoderFresh ? 0 : 1, work, (s32)pairs,
		s_call.buffer + s_call.written);
	s_call.encoderFresh = false;
	s_call.written += pairs/2;

	if(!s_call.started && s_state == STATE_ON){
		// The first of the line is in place: let libogc start reading.
		const u64 now = gettime();
		WPAD_SendStreamData(kChannel, s_call.buffer, s_call.capacity);
		s_call.started = true;
		s_clipEnd = now + microsecs_to_ticks((s_call.capacity/kPacketBytes + kGuardPackets)*kPacketMicros);
		s_callFreeTime = s_clipEnd + millisecs_to_ticks(200);
		s_deadline = s_clipEnd + millisecs_to_ticks(kLingerMs);
	}
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
WiiSpeakerWake(void)
{
	if(!WiiRemoteSpeakerEnabled || !WiiPhoneRemoteMode)
		return;
	u32 expansion;
	if(WPAD_Probe(kChannel, &expansion) != WPAD_ERR_NONE)
		return;
	s_wakeUntil = gettime() + millisecs_to_ticks(kWakeLingerMs);
	if(s_state == STATE_OFF){
		WPAD_ControlSpeaker(kChannel, 1);
		s_state = STATE_WARMING;
		s_deadline = gettime() + millisecs_to_ticks(kWarmupTimeoutMs);
	}
}

bool
WiiSpeakerBeginCall(u32 lengthMs, u32 sampleRate)
{
	if(!WiiRemoteSpeakerEnabled || !WiiPhoneRemoteMode || sampleRate < kSampleRate)
		return false;
	releaseCallBuffer();
	// Only a speaker that is already up, and nothing else still streaming: a line
	// that cannot start at its first word is better left on the TV than joined late.
	// Waiting for the speaker to finish powering up is allowed: the line is buffered and
	// starts streaming the moment it is up (see encodeCall).  Refusing a warming speaker
	// here is what a call arriving during power-up used to hit.
	if(s_call.active || s_call.buffer != nullptr || s_state == STATE_OFF || gettime() < s_clipEnd)
		return false;
	u32 expansion;
	if(WPAD_Probe(kChannel, &expansion) != WPAD_ERR_NONE)
		return false;

	u32 bytes = lengthMs*kCallBytesPerMs + kCallSlackBytes;
	if(bytes > kCallMaxBytes)
		bytes = kCallMaxBytes;
	bytes = (bytes + kPacketBytes - 1)/kPacketBytes*kPacketBytes;
	u8 *buffer = (u8*)memalign(32, bytes);
	if(buffer == nullptr)
		return false;
	std::memset(buffer, kAdpcmSilence, bytes);

	std::memset(&s_call, 0, sizeof(s_call));
	s_call.buffer = buffer;
	s_call.capacity = bytes;
	s_call.sourceRate = sampleRate;
	s_call.encoderFresh = true;
	s_call.active = true;
	s_pending = CLIP_NONE;
	return true;
}

void
WiiSpeakerFeedCall(const s16 *pcm, u32 frames, u32 channels)
{
	if(!s_call.active || pcm == nullptr || channels == 0)
		return;

	// Down to the remote's 6 kHz mono by averaging: every input sample adds the
	// output rate to a running total, and each time that passes the input rate one
	// output sample leaves, the mean of the input since the last.  Averaging is also
	// the low pass that keeps the rest from folding back down as hiss.
	s16 out[1024];
	u32 n = 0;
	for(u32 frame = 0; frame < frames; frame++){
		float sample = 0.0f;
		for(u32 channel = 0; channel < channels; channel++)
			sample += (float)pcm[frame*channels + channel];
		s_call.accumulator += sample/(float)channels;
		s_call.accumulated++;
		s_call.phase += kSampleRate;
		if(s_call.phase >= s_call.sourceRate){
			s_call.phase -= s_call.sourceRate;
			out[n++] = toSample(s_call.accumulator/(float)s_call.accumulated/32000.0f);
			s_call.accumulator = 0.0f;
			s_call.accumulated = 0;
			if(n == 1024){
				encodeCall(out, n);
				n = 0;
			}
		}
	}
	if(n != 0)
		encodeCall(out, n);
}

void
WiiSpeakerEndCall(void)
{
	// The line is over (or stopped): stop writing.  Whatever was written plays out on
	// the remote, and the buffer is freed once libogc is done with it.
	s_call.active = false;
}

void
WiiSpeakerAbortCall(void)
{
	if(!s_call.active)
		return;
	// The TV stream was paused, which the remote cannot follow: it would run on and
	// finish early.  Silence it and let the TV carry the rest.
	powerDown();
}

bool
WiiSpeakerCallActive(void)
{
	return s_call.active;
}

uint32_t
WiiSpeakerCallTvPercent(void)
{
	return WiiPhoneRemoteMode == 1 ? 0 : kCallTvDuckPercent;
}

void
WiiSpeakerService(void)
{
	releaseCallBuffer();
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
		// A clip that waited out this power-up starts its own age clock now, rather than
		// being judged against the moment it was asked for.  Enabling the speaker is
		// allowed to take kWarmupTimeoutMs, which is longer than the ring's whole
		// tolerance, so a lone ring was always discarded before it could ever play.
		s_pendingTime = now;
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
	}else if(s_pending == CLIP_NONE && !s_call.active && now > s_deadline && now > s_wakeUntil){
		powerDown();
	}
}
