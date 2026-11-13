#include "Audio.h"
#include "play_memory.h"
#include <cmath>
#include <cstring>

// Tape play mode: the sample plays like a loop of tape.
//
// - Speed is varispeed: pitch and speed move together, on top of the played note. Negative
//   Speed (or the Reverse FX) runs the tape backwards.
// - Wow (slow) and Flutter (fast) wobble the speed.
// - The loop splice (Loop Start / Loop End) is crossfaded so it never clicks.
// - Drive is soft tape saturation, Age rolls off the highs and adds hiss, and Wear ages the
//   sound a little more on every pass round the loop.
// - The Tape Stop FX slows the motor to a halt and starts it again when the FX ends.
//
// The read position, LFOs and hiss are deterministic from the note start, so the two channels
// of a stereo sample stay identical apart from their audio.

namespace
{
constexpr float kSampleRate = AUDIO_SAMPLE_RATE_EXACT;
constexpr float kTwoPi = 6.28318531f;
constexpr int32_t kMaxSpliceFade = 882; // about 20 ms

// 4-point Hermite interpolation: smooth enough for slow varispeed without dulling the sound
inline float hermite(float xm1, float x0, float x1, float x2, float t)
{
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

// Soft saturation, close to tanh for |x| < 3
inline float tapeSaturate(float x)
{
    if (x > 3.0f)
        return 1.0f;
    if (x < -3.0f)
        return -1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// Speed factor from wow and flutter at time t (seconds)
inline float tapeWobble(float t, float wow, float flutter)
{
    const float w = 0.7f * sinf(kTwoPi * 0.55f * t) + 0.3f * sinf(kTwoPi * 0.93f * t + 1.3f);
    const float f = 0.7f * sinf(kTwoPi * 6.3f * t) + 0.3f * sinf(kTwoPi * 11.7f * t + 0.7f);
    return 1.0f + wow * 0.012f * w + flutter * 0.004f * f;
}
} // namespace

AUDIO_CORE_FLASH void AudioPlayMemory::playTape(uint8_t instrIdx, int8_t note)
{
    // Start, loop points, pitch and glide are set up exactly as for Forward Loop
    playLoopForward(instrIdx, note);

    AudioNoInterrupts();

    const strInstrument& instr = mtProject.instrument[currentInstrIdx];
    const bool backwards =
      (instr.getTapeParam(strInstrument::tapeSpeed) < 0) != (reverseDirectionFlag != 0);

    const float spin = tape.spin; // keep a stopped motor stopped across notes
    tape = strTapeState();
    tape.spin = (tapeStopFx > 0) ? spin : 1.0f;
    setTapeStop(tapeStopFx);

    if (backwards)
    {
        // Backwards the tape starts at the loop end and runs round the loop
        const uint32_t loopEnd =
          (pointsInSamples.loop2 > 1) ? pointsInSamples.loop2 - 1 : pointsInSamples.loop2;
        tape.position = (int64_t)loopEnd << 16;
        tape.inLoop = true;
    }
    else
    {
        tape.position = (int64_t)pointsInSamples.start << 16;
    }

    AudioInterrupts();
}

void AudioPlayMemory::setTapeStop(int16_t value)
{
    tapeStopFx = value;
    if (value > 0)
    {
        const float seconds = 0.05f + (value / 100.0f) * 2.95f;
        tape.spinTarget = 0.0f;
        tape.spinStep = 1.0f / (seconds * kSampleRate);
    }
    else
    {
        // Start again at the same rate it stopped (or quickly if it never stopped)
        tape.spinTarget = 1.0f;
        if (tape.spinStep <= 0.0f)
            tape.spinStep = 1.0f / (0.15f * kSampleRate);
    }
}

AUDIO_CORE_FLASH audio_block_t* AudioPlayMemory::updateTape()
{
    audio_block_t* block = allocate();
    if (!block)
        return nullptr;

    if (!playing)
    {
        release(block);
        return nullptr;
    }

    const int16_t* data = (const int16_t*)currentStartAddress;
    const int32_t sampleLength = (int32_t)currentSampleLength;
    if (data == nullptr || sampleLength < 8)
    {
        memset(block->data, 0, sizeof(block->data));
        return block;
    }

    const strInstrument& instr = mtProject.instrument[currentInstrIdx];

    // Settings, with Wear adding age and wow on every pass round the loop
    const float speedSetting = instr.getTapeParam(strInstrument::tapeSpeed) * 0.02f; // -2..2
    const float wear = instr.getTapeParam(strInstrument::tapeWear) / 100.0f;
    const float worn = std::min(1.0f, tape.passes * wear * 0.05f);
    const float wowSetting = instr.getTapeParam(strInstrument::tapeWow) / 100.0f;
    const float wow = wowSetting + (1.0f - wowSetting) * worn * 0.5f;
    const float flutter = instr.getTapeParam(strInstrument::tapeFlutter) / 100.0f;
    const float drive = instr.getTapeParam(strInstrument::tapeDrive) / 100.0f;
    const float ageSetting = instr.getTapeParam(strInstrument::tapeAge) / 100.0f;
    const float age = ageSetting + (1.0f - ageSetting) * worn;

    const bool backwards = (speedSetting < 0.0f) != (reverseDirectionFlag != 0);
    const float tapeSpeed = fabsf(speedSetting);

    // Loop region in sample frames; a loop too short to splice plays the whole sample
    int32_t loop1 = (int32_t)pointsInSamples.loop1;
    int32_t loop2 = (int32_t)pointsInSamples.loop2;
    if (loop2 > sampleLength - 2)
        loop2 = sampleLength - 2;
    if (loop2 - loop1 < 64)
    {
        loop1 = 0;
        loop2 = sampleLength - 2;
    }
    const int32_t loopLength = loop2 - loop1;
    const int32_t splice = std::min(kMaxSpliceFade, loopLength / 4);
    const int64_t loop1Q = (int64_t)loop1 << 16;
    const int64_t loop2Q = (int64_t)loop2 << 16;
    const int64_t spliceQ = (int64_t)splice << 16;
    const int64_t wrapQ = (int64_t)(loopLength - splice) << 16;

    auto read = [&](int64_t pos) -> float
    {
        int32_t i = (int32_t)(pos >> 16);
        const float t = (float)(pos & 0xFFFF) * (1.0f / 65536.0f);
        auto at = [&](int32_t n) -> float
        { return (float)data[(n < 0) ? 0 : ((n >= sampleLength) ? sampleLength - 1 : n)]; };
        return hermite(at(i - 1), at(i), at(i + 1), at(i + 2), t);
    };

    // Wow and flutter: speed factor ramps across the block from the last block's value
    const float blockSeconds = AUDIO_BLOCK_SAMPLES / kSampleRate;
    const float modEnd = tapeWobble(tape.lfoTime + blockSeconds, wow, flutter);
    const float modStart = tape.lastMod;
    tape.lfoTime += blockSeconds;
    if (tape.lfoTime > 3600.0f)
        tape.lfoTime -= 3600.0f;
    tape.lastMod = modEnd;

    // Age: two one-pole low-passes (12 dB/oct) from about 18 kHz down to 2.5 kHz, lower still as
    // a stopping tape slows down; hiss up to about -44 dB
    const float cutoff = (18000.0f - 15500.0f * age) * (0.25f + 0.75f * tape.spin);
    const float toneCoef = 1.0f - expf(-kTwoPi * cutoff / kSampleRate);
    const float hiss = age * age * 200.0f;

    // Drive: soft saturation with up to x4 into it; the level is matched at about -10 dB so
    // Drive squashes peaks (like tape) instead of just making it louder
    const float driveGain = 1.0f + 3.0f * drive;
    const float driveOut = 0.3f / tapeSaturate(0.3f * driveGain);

    int16_t* out = block->data;
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
    {
        // Glide, as in the other play modes
        if (constrainsInSamples.pitchSmoothing &&
            pitchSmoothingControlCounter <= constrainsInSamples.pitchSmoothing)
        {
            pitchControl += pitchSmoothingControl;
            if (pitchControl > notes[MAX_NOTE - 1])
            {
                pitchControl = notes[MAX_NOTE - 1];
                pitchSmoothingControlCounter = constrainsInSamples.pitchSmoothing;
            }
            else if (pitchControl < notes[MIN_NOTE])
            {
                pitchControl = notes[MIN_NOTE];
                pitchSmoothingControlCounter = constrainsInSamples.pitchSmoothing;
            }
            pitchSmoothingControlCounter++;
        }

        // Tape motor (Tape Stop FX)
        if (tape.spin < tape.spinTarget)
            tape.spin = std::min(tape.spinTarget, tape.spin + tape.spinStep);
        else if (tape.spin > tape.spinTarget)
            tape.spin = std::max(tape.spinTarget, tape.spin - tape.spinStep);

        const float mod = modStart + (modEnd - modStart) * ((i + 1) * (1.0f / AUDIO_BLOCK_SAMPLES));
        float speed = pitchControl * tapeSpeed * mod * tape.spin;
        if (speed > 16.0f)
            speed = 16.0f;
        const int64_t step = (int64_t)(speed * 65536.0f);

        float s;
        if (!backwards)
        {
            if (!tape.inLoop && tape.position >= loop1Q)
                tape.inLoop = true;

            s = read(tape.position);
            if (tape.inLoop && splice > 0 && tape.position >= loop2Q - spliceQ)
            {
                // Splice: fade into the loop start while leaving the loop end
                const float t = (float)(tape.position - (loop2Q - spliceQ)) / (float)spliceQ;
                s += (read(tape.position - wrapQ) - s) * std::min(t, 1.0f);
            }

            tape.position += step;
            if (tape.inLoop && tape.position >= loop2Q)
            {
                tape.position -= (splice > 0) ? wrapQ : ((int64_t)loopLength << 16);
                if (tape.passes < 0xFFFF)
                    tape.passes++;
            }
        }
        else
        {
            tape.inLoop = true;
            if (tape.position > loop2Q)
                tape.position = loop2Q;

            s = read(tape.position);
            if (splice > 0 && tape.position < loop1Q + spliceQ)
            {
                const float t = (float)((loop1Q + spliceQ) - tape.position) / (float)spliceQ;
                s += (read(tape.position + wrapQ) - s) * std::min(t, 1.0f);
            }

            tape.position -= step;
            if (tape.position < loop1Q)
            {
                tape.position += (splice > 0) ? wrapQ : ((int64_t)loopLength << 16);
                if (tape.passes < 0xFFFF)
                    tape.passes++;
            }
        }

        // Keep the read inside the sample whatever happens to the loop points
        if (tape.position < 0)
            tape.position = 0;
        else if (tape.position > ((int64_t)(sampleLength - 2) << 16))
            tape.position = (int64_t)(sampleLength - 2) << 16;

        // Hiss, tone, saturation
        if (hiss > 0.0f)
        {
            tape.noise ^= tape.noise << 13;
            tape.noise ^= tape.noise >> 17;
            tape.noise ^= tape.noise << 5;
            s += ((int32_t)tape.noise >> 16) * (hiss / 32768.0f);
        }
        tape.tone1 += toneCoef * (s - tape.tone1);
        tape.tone2 += toneCoef * (tape.tone1 - tape.tone2);
        s = tape.tone2;

        if (drive > 0.0f)
            s = tapeSaturate(s * (driveGain / 32768.0f)) * (driveOut * 32768.0f);

        // A nearly stopped tape fades out
        if (tape.spin < 0.5f)
            s *= tape.spin * 2.0f;

        out[i] = granularSoftLimit((int32_t)s);
    }

    return block;
}
