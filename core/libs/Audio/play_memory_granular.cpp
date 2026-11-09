#include "Audio.h"
#include "play_memory.h"
#include "mtSequencer.h"
#include "utility/dspinst.h"
#include <cmath>
#include <cr_section_macros.h>

uint16_t AudioPlayMemory::gaussTab[GRANULAR_LOOKUP_SIZE];
uint16_t AudioPlayMemory::squareTab[GRANULAR_LOOKUP_SIZE];
uint16_t AudioPlayMemory::triangleTab[GRANULAR_LOOKUP_SIZE];

AUDIO_CORE_FLASH void AudioPlayMemory::initGranularShapeLookup()
{
    for (int idx = 0; idx < GRANULAR_LOOKUP_SIZE; idx++)
    {
        squareTab[idx] = std::numeric_limits<uint16_t>::max();
        gaussTab[idx] = std::numeric_limits<uint16_t>::max() *
                        sinf((float)M_PI * (idx / (float)GRANULAR_LOOKUP_SIZE));

        if (idx < GRANULAR_LOOKUP_SIZE / 2)
        {
            uint16_t linearValue =
              std::numeric_limits<uint16_t>::max() * (idx / ((float)GRANULAR_LOOKUP_SIZE / 2));

            triangleTab[idx] = linearValue;
            triangleTab[GRANULAR_LOOKUP_SIZE - 1 - idx] = linearValue;
        }
    }
}

AUDIO_CORE_FLASH void AudioPlayMemory::playGranular(uint8_t instrIdx, int8_t note)
{
    //	__disable_irq();
    AudioNoInterrupts();

    if (instrIdx > kSampleInstrumentsCount - 1)
        instrIdx = kSampleInstrumentsCount - 1;
    if (note > MAX_NOTE)
        note = MAX_NOTE;

    lastInstrIdxInPlay = currentInstrIdxInPlay;
    currentInstrIdxInPlay = instrIdx;
    currentInstrIdx = instrIdx;

    refreshStartParameters();

    constrainCurrentTune(note);

    setStartPitch(note);

    applyFinetuneOnPitch(note);

    calculateGlidePitch(note);

    lastNote = note;

    granularLoopType = mtProject.instrument[instrIdx].granular.type;

    switch (mtProject.instrument[instrIdx].granular.shape)
    {
        case granularShapeSquare: granularEnvelopeTab = squareTab; break;
        case granularShapeGauss: granularEnvelopeTab = gaussTab; break;
        case granularShapeTriangle: granularEnvelopeTab = triangleTab; break;
        default: break;
    }

    currentGranularPosition = granularForcedFlag
                                ? forcedGranularPosition
                                : mtProject.instrument[instrIdx].granular.currentPosition;
    currentGranularLength = granularLengthForcedFlag
                              ? forcedGranularLength
                              : mtProject.instrument[instrIdx].granular.grainLength;

    auto [startGranular, loopPoint1Granular, loopPoint2Granular, endGranular] =
      calculateGranularPoints(currentGranularPosition,
                              currentGranularLength,
                              mtProject.instrument[currentInstrIdx].sample.length);

    pointsInSamples.start =
      (uint32_t)((float)startGranular * ((float)currentSampleLength / MAX_16BIT));
    pointsInSamples.end = (uint32_t)((float)endGranular * ((float)currentSampleLength / MAX_16BIT));

    pointsInSamples.loop1 =
      (uint32_t)((float)loopPoint1Granular * ((float)currentSampleLength / MAX_16BIT));
    pointsInSamples.loop2 =
      (uint32_t)((float)loopPoint2Granular * ((float)currentSampleLength / MAX_16BIT));

    constrainsInSamples.loopPoint1 = pointsInSamples.loop1 - pointsInSamples.start;
    constrainsInSamples.loopPoint2 = pointsInSamples.loop2 - pointsInSamples.start;
    constrainsInSamples.loopLength = pointsInSamples.loop2 - pointsInSamples.loop1;
    constrainsInSamples.endPoint = pointsInSamples.end - pointsInSamples.start;

    next = currentStartAddress + pointsInSamples.start;
    length = currentSampleLength - pointsInSamples.start;
    iPitchCounter = reverseDirectionFlag ? constrainsInSamples.endPoint - 1 : 0;

    granularPositionRefreshFlag = 0;

    // Scan starts from the grain's position and moves it on at each grain boundary
    granularScanAccum = (uint32_t)currentGranularPosition << 16;
    granularScanBase = granularScanAccum;
    if (effectiveGranularScan(mtProject.instrument[instrIdx]) != 0)
        granularPositionRefreshFlag = 1;

    // Extra heads start their first grain part-way through, so grains overlap evenly
    {
        const uint8_t heads = mtProject.instrument[instrIdx].getGranularHeads();
        for (uint8_t h = 0; h < kMaxExtraGranularHeads; h++)
        {
            extraGranularHeads[h] = strExtraGranularHead();
            extraGranularHeads[h].startPhase = ((uint32_t)(h + 1) << 16) / heads;
            extraGranularHeads[h].position = currentGranularPosition;
        }
        granularMixGain = (heads >= 3) ? 18919 : ((heads == 2) ? 23170 : 32767);
        granularOverlapActive = false; // the first block starts the overlap grains fresh
        granularSyncActive = false;
        granularFreshNote = true; // nothing was playing: no crossfade from the original player
    }

    playing = 1;
    //	__enable_irq();
    AudioInterrupts();
}

AUDIO_CORE_FLASH audio_block_t* AudioPlayMemory::updateGranular()
{
    audio_block_t* block = nullptr;
    switch (granularLoopType)
    {
        case granularLoopForward: block = updateGranularLoopForward(); break;
        case granularLoopBackward: block = updateGranularLoopBackward(); break;
        case granularLoopPingPong: block = updateGranularLoopPingPong(); break;
        default: return nullptr; break;
    }

    if (block != nullptr)
        mixExtraGranularHeads(block);

    return block;
}

// One sample from a grain stream. A stream starts its next grain around ctx.position when the
// current one ends; a twin instead waits until its head asks for it (pending).
inline int32_t AudioPlayMemory::nextGrainSample(strExtraGranularHead& head,
                                                          const strGrainContext& ctx,
                                                          bool twin)
{
    if (head.delay != 0)
    {
        head.delay--;
        return 0;
    }

    if (head.grainLength == 0 || head.counter >= head.grainLength)
    {
        if (twin && !head.pending)
        {
            head.grainLength = 0; // idle until its head reaches half way
            return 0;
        }

        const uint32_t grainLength = ctx.grainLength;

        // Jitter: start this grain at a random offset around the head's position
        uint32_t position = ctx.position;
        if (ctx.jitter != 0)
        {
            granularRandom ^= granularRandom << 13;
            granularRandom ^= granularRandom >> 17;
            granularRandom ^= granularRandom << 5;
            position += (uint32_t)(((uint64_t)granularRandom * ctx.jitter) >> 32) - ctx.jitter / 2;
        }

        const uint32_t center = (uint32_t)(((uint64_t)position * ctx.sampleLength) >> 32);
        head.position = (uint16_t)(position >> 16);

        uint32_t start = (center > grainLength / 2) ? center - grainLength / 2 : 0;
        if (start + grainLength >= ctx.sampleLength)
            start = ctx.sampleLength - 1 - grainLength;

        // A free-running stream carries over what it overshot; a twin starts on time
        const uint32_t overshoot =
          (!twin && head.grainLength != 0) ? head.counter - head.grainLength : 0;

        head.pending = false;
        head.flip = !head.flip;
        head.grainStart = start;
        head.grainLength = grainLength;
        head.counter = (overshoot < grainLength) ? overshoot : 0;
        head.envStep = ((uint32_t)(GRANULAR_LOOKUP_SIZE - 1) << 16) / grainLength;

        // The first grain waits its turn instead of starting half-way through (which
        // would start at full level and pop), so heads still overlap evenly
        if (head.startPhase != 0)
        {
            head.delay = (uint32_t)(((uint64_t)head.startPhase * grainLength) >> 16);
            head.startPhase = 0;
            return 0;
        }
    }

    const bool backwards = ctx.backwards != (ctx.pingPong && !head.flip); // PingPong: 1st grain forwards
    const uint32_t idx = backwards ? (head.grainLength - 1 - head.counter) : head.counter;
    const int16_t* p = ctx.data + head.grainStart + idx;
    int32_t s = p[0];
    const int32_t next = backwards ? ((idx > 0) ? p[-1] : s) : p[1];
    s += (int32_t)((((int32_t)(head.fraction >> 2)) * (next - s)) >> 14);

    if (ctx.square)
    {
        s = (s * granularSquareGain(
                   (int32_t)head.counter, 0, (int32_t)head.grainLength - 1, ctx.squareFade)) >>
            16;
    }
    else
    {
        uint32_t envIdx = (uint32_t)(((uint64_t)head.counter * head.envStep) >> 16);
        if (envIdx > GRANULAR_LOOKUP_SIZE - 1)
            envIdx = GRANULAR_LOOKUP_SIZE - 1;
        s = (s * granularEnvelopeTab[envIdx]) >> 16;
    }

    head.fraction += ctx.pitchFrac;
    head.counter += ctx.pitchInt + (head.fraction >> 16);
    head.fraction &= 0xFFFF;
    return s;
}

// Where a stream is reading now, for its on-screen playhead
AUDIO_CORE_FLASH void AudioPlayMemory::updateGrainPlayPosition(strExtraGranularHead& head,
                                                               const strGrainContext& ctx)
{
    if (head.grainLength == 0)
        return;
    const bool backwards = ctx.backwards != (ctx.pingPong && !head.flip); // PingPong: 1st grain forwards
    const uint32_t counter =
      (head.counter < head.grainLength) ? head.counter : head.grainLength - 1;
    const uint32_t readIdx =
      head.grainStart + (backwards ? (head.grainLength - 1 - counter) : counter);
    head.playPosition = (uint16_t)(((uint64_t)readIdx * 65535u) / ctx.sampleLength);
}

// Heads 2 and 3: extra grains read from the same sample and mixed into the main grain.
// They follow the main grain's position, length, shape and pitch; Spread offsets each head
// by up to (head / heads) of the whole sample. Loop type Backward or the Reverse FX play
// them backwards; PingPong flips every grain. Direction can reverse a head: it then scans
// the mirror image of the main grain's travel and plays its grains the other way round.
// Density adds overlapping twin grains to every head (Triangle on Auto: 2), and Jitter starts
// each grain at a random offset; with either, the main head's grains are played here too.
AUDIO_CORE_FLASH void AudioPlayMemory::mixExtraGranularHeads(audio_block_t* block)
{
    const strInstrument& instr = mtProject.instrument[currentInstrIdx];
    const uint8_t heads = instr.getGranularHeads();
    const bool isTriangle = (granularEnvelopeTab == triangleTab);
    const bool isSquare = (granularEnvelopeTab == squareTab);

    // Density: grains of each head playing at once. Auto = 2 for Triangle, otherwise 1.
    // A note value instead starts each head's grains in time with the song (Sync).
    const uint8_t densitySetting = effectiveGranularDensity(instr);
    const bool sync = strInstrument::isGranularSync(densitySetting);
    uint8_t density;
    switch (densitySetting)
    {
        case strInstrument::granularDensity1: density = 1; break;
        case strInstrument::granularDensity2: density = 2; break;
        case strInstrument::granularDensity3: density = 3; break;
        default: density = isTriangle ? 2 : 1; break;
    }
    if (sync)
        density = kMaxGranularDensity; // head and twins form a pool for the synced grains
    const uint8_t jitter = effectiveGranularJitter(instr);

    // Square edge fades last about 2 ms of output whatever the pitch
    granularSquareFadeLength = (int32_t)(kGranularSquareFade * std::max(pitchControl, 1.0f));

    // Streamed samples are not fully in RAM, so the grain streams cannot read them
    const bool canRead =
      !currentIsStreamed && currentStartAddress != nullptr && currentSampleLength >= 4 && playing;
    // With overlap, Jitter or Sync the main head's grains are played by the grain streams too
    const bool ownMain = canRead && (sync || density > 1 || jitter > 0);
    if (!ownMain)
        density = 1;

    // When the main head switches between the original player and the grain streams (e.g. a
    // Grain FX starts or ends), crossfade over one block instead of jumping
    const bool wasOwnMain = granularOverlapActive;
    const bool switchingOn = ownMain && !wasOwnMain;
    const bool crossfadeIn = switchingOn && !granularFreshNote;
    granularFreshNote = false;
    const bool switchingOff = wasOwnMain && !ownMain && canRead;
    const bool renderMain = ownMain || switchingOff;

    if (switchingOn)
    {
        // Just switched on: start the main head's own grains fresh
        mainGranularStream = strExtraGranularHead();
        for (auto& twins : granularTwins)
            for (auto& twin : twins)
                twin = strExtraGranularHead();
    }
    granularOverlapActive = ownMain;

    if (!canRead || (heads < 2 && !renderMain))
    {
        // Heads was turned down to 1: bring the level back up smoothly instead of jumping
        if (granularMixGain != 32767)
        {
            const int32_t from = granularMixGain;
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
            {
                const int32_t g = from + ((32767 - from) * (i + 1)) / AUDIO_BLOCK_SAMPLES;
                block->data[i] = (int16_t)((block->data[i] * g) >> 15);
            }
            granularMixGain = 32767;
        }
        return;
    }

    const uint32_t sampleLength = currentSampleLength;
    uint32_t grainLength = (currentGranularLength >= MIN_GRANULAR_LENGTH)
                             ? currentGranularLength
                             : instr.granular.grainLength;
    if (grainLength > sampleLength - 1)
        grainLength = sampleLength - 1;
    if (grainLength < 2)
        return;

    const bool backwards = (reverseDirectionFlag != 0) != (granularLoopType == granularLoopBackward);
    const uint32_t spread = effectiveGranularSpread(instr);

    // Sync: grain spacing in output samples from the song tempo
    float syncInterval = 0.0f;
    if (sync)
    {
        float tempo = sequencer.getCurrentTempo();
        if (tempo < 10.0f)
            tempo = 10.0f;
        syncInterval = (AUDIO_SAMPLE_RATE_EXACT * 60.0f / tempo) *
                       strInstrument::granularSyncBeats(densitySetting);
        if (syncInterval < 32.0f)
            syncInterval = 32.0f;

        if (!granularSyncActive)
        {
            // Sync just started (or a new note): heads fire spread evenly across one interval
            for (uint8_t h = 0; h <= kMaxExtraGranularHeads; h++)
                granularSyncCountdown[h] = (h < heads) ? (syncInterval * h) / heads : 0.0f;
            for (auto& head : extraGranularHeads)
            {
                head.startPhase = 0;
                head.delay = 0;
            }
        }
    }
    granularSyncActive = sync;

    // Keep the total level close to one head: 1/sqrt(heads) in Q15
    int32_t mixGain = (heads >= 3) ? 18919 : ((heads == 2) ? 23170 : 32767);
    // and overlapping grains close to one grain's peak: 1 / (overlap x average envelope level)
    float overlap = density;
    if (sync)
    {
        // How many synced grains overlap: grain duration over spacing
        const float pitch = std::max(pitchControl, 0.01f);
        overlap = std::min((grainLength / pitch) / syncInterval, (float)kMaxGranularDensity);
    }
    if (overlap > 1.0f)
    {
        const float average = isSquare ? 1.0f : (isTriangle ? 0.5f : 0.637f);
        float densityGain = 1.0f / (overlap * average);
        if (densityGain > 1.0f)
            densityGain = 1.0f;
        mixGain = (int32_t)(mixGain * densityGain);
    }

    // Main grain position, and its mirror around where the scan started (for reversed heads)
    const uint32_t mainPosition = (uint32_t)currentGranularPosition << 16;
    const uint32_t mirrorPosition = (effectiveGranularScan(instr) != 0)
                                      ? granularScanBase - (granularScanAccum - granularScanBase)
                                      : mainPosition;

    strGrainContext ctx;
    ctx.data = (const int16_t*)currentStartAddress;
    ctx.sampleLength = sampleLength;
    ctx.grainLength = grainLength;
    ctx.pingPong = (granularLoopType == granularLoopPingPong);
    ctx.square = isSquare;
    ctx.jitter = (uint32_t)(((uint64_t)jitter << 32) / 100u); // +/- jitter/2 % of the sample

    // Twin k of a head starts when the head is k/density of the way through its grain
    uint32_t twinPoint[kMaxGranularDensity - 1];
    for (uint8_t k = 0; k + 1 < kMaxGranularDensity; k++)
        twinPoint[k] = (grainLength * (k + 1)) / density;

    // One head (its own grain plus density - 1 twins) into mix
    auto playHead = [&](strExtraGranularHead& head,
                        strExtraGranularHead* twins,
                        int32_t* mix,
                        bool add,
                        float& countdown)
    {
        if (sync)
        {
            // Sync: at every tick start a grain on whichever of the head's streams is free
            strExtraGranularHead* pool[kMaxGranularDensity] = { &head, &twins[0], &twins[1] };
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
            {
                countdown -= 1.0f;
                if (countdown <= 0.0f)
                {
                    countdown += syncInterval;
                    for (auto* stream : pool)
                    {
                        if (!stream->pending &&
                            (stream->grainLength == 0 || stream->counter >= stream->grainLength))
                        {
                            stream->pending = true;
                            break;
                        }
                    }
                    // All busy (grains longer than 3 ticks): this tick is skipped
                }
                int32_t s = 0;
                for (auto* stream : pool)
                    if (stream->grainLength != 0 || stream->pending)
                        s += nextGrainSample(*stream, ctx, true);
                mix[i] = add ? mix[i] + s : s;
            }
            updateGrainPlayPosition(head, ctx);
            return;
        }

        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        {
            const uint32_t before = head.counter;
            const bool started = (head.delay == 0);
            int32_t s = nextGrainSample(head, ctx, false);
            // Twins beyond Density only finish the grain they are playing, so lowering it is smooth
            for (uint8_t k = 0; k + 1 < kMaxGranularDensity; k++)
            {
                if (k + 1 < density && started && before < twinPoint[k] &&
                    head.counter >= twinPoint[k])
                    twins[k].pending = true;
                strExtraGranularHead& twin = twins[k];
                if (twin.grainLength != 0 || twin.pending) // skip idle twins
                    s += nextGrainSample(twin, ctx, true);
            }
            mix[i] = add ? mix[i] + s : s;
        }
        updateGrainPlayPosition(head, ctx);
    };

    int32_t mix[AUDIO_BLOCK_SAMPLES];

    if (renderMain)
    {
        // Main head: its grains are played here (the original single grain is replaced)
        ctx.position = mainPosition;
        ctx.backwards = backwards;
        float mainPitch = pitchControl;
        if (mainPitch > 64.0f)
            mainPitch = 64.0f;
        ctx.pitchInt = (uint32_t)mainPitch;
        ctx.pitchFrac = (uint32_t)((mainPitch - (float)ctx.pitchInt) * 65536.0f);
        ctx.squareFade = (int32_t)(kGranularSquareFade * std::max(mainPitch, 1.0f));
        playHead(mainGranularStream, granularTwins[0], mix, false, granularSyncCountdown[0]);

        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        {
            const int32_t in = i + 1; // original player fades in over the block
            const int32_t out = AUDIO_BLOCK_SAMPLES - in;
            if (switchingOff)
                mix[i] = (mix[i] * out + block->data[i] * in) / AUDIO_BLOCK_SAMPLES;
            else if (crossfadeIn)
                mix[i] += (block->data[i] * (out)) / AUDIO_BLOCK_SAMPLES;
        }
    }
    else
    {
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
            mix[i] = block->data[i];
    }

    for (uint8_t h = 0; h + 1 < heads; h++)
    {
        strExtraGranularHead& head = extraGranularHeads[h];
        const bool reversed = instr.isGranularHeadReversed(h);

        // New grains centre on the main position, offset by Spread and Rate for this head
        const uint32_t offset = (uint32_t)(((uint64_t)spread * (h + 1) << 32) / (100u * heads));
        ctx.position = (reversed ? mirrorPosition : mainPosition) + offset + head.drift;
        ctx.backwards = backwards != reversed;

        // Pitch: each head can play at its own interval from the note
        float headPitch = pitchControl * instr.getGranularHeadPitchRatio(h);
        if (headPitch > 64.0f)
            headPitch = 64.0f;
        ctx.pitchInt = (uint32_t)headPitch;
        ctx.pitchFrac = (uint32_t)((headPitch - (float)ctx.pitchInt) * 65536.0f);
        ctx.squareFade = (int32_t)(kGranularSquareFade * std::max(headPitch, 1.0f));

        playHead(head, granularTwins[h + 1], mix, true, granularSyncCountdown[h + 1]);
    }

    // Ramp the level across the block when Heads or Density changes, so it doesn't jump
    const int32_t fromGain = granularMixGain;
    granularMixGain = mixGain;
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
    {
        const int32_t g = fromGain + ((mixGain - fromGain) * (i + 1)) / AUDIO_BLOCK_SAMPLES;
        int32_t v = (int32_t)(((int64_t)mix[i] * g) >> 15);
        block->data[i] = granularSoftLimit(v);
    }
}

AUDIO_CORE_FLASH audio_block_t* AudioPlayMemory::updateGranularLoopForward()
{
    if (reverseDirectionFlag)
        return updateGranularLoopForwardReverse();
    else
        return updateGranularLoopForwardNormal();
}
AUDIO_CORE_FLASH audio_block_t* AudioPlayMemory::updateGranularLoopBackward()
{
    loopBackwardFlag = 1;
    if (reverseDirectionFlag)
        return updateGranularLoopBackwardReverse();
    else
        return updateGranularLoopBackwardNormal();
}
AUDIO_CORE_FLASH audio_block_t* AudioPlayMemory::updateGranularLoopPingPong()
{
    if (reverseDirectionFlag)
        return updateGranularLoopPingPongReverse();
    else
        return updateGranularLoopPingPongNormal();
}

AUDIO_CORE_FLASH audio_block_t* AudioPlayMemory::updateGranularLoopForwardNormal()
{
    audio_block_t* block = nullptr;
    int16_t* in = nullptr;
    int16_t* out = nullptr;
    int32_t castPitchControl;
    float pitchFraction;
    int32_t loopEndPoint = std::min((int32_t)constrainsInSamples.loopPoint2,
                                    (int32_t)std::min<int>(length, constrainsInSamples.endPoint));

    block = allocate();
    if (!block)
        return nullptr;

    if (!playing)
    {
        release(block);
        return nullptr;
    }
    else if (playing == 1)
    {
        out = block->data;
        in = (int16_t*)next;

        castPitchControl = (int32_t)pitchControl;
        pitchFraction = pitchControl - (int32_t)pitchControl;

        if (enableInterpolation)
            interpolationCondition = ((iPitchCounter + 128 * pitchControl) < length) ? 0 : 1;
        else
            interpolationCondition = 1;

        int16_t* in_interpolation = in + 1;

        int32_t currentFractionPitchCounter = fPitchCounter * MAX_16BIT;
        int32_t currentFractionPitchControl = pitchFraction * MAX_16BIT;
        const bool isSquareEnv = (granularEnvelopeTab == squareTab);

        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        {
            //*********************************** GLIDE HANDLE

            if (constrainsInSamples.pitchSmoothing)
            {
                if (pitchSmoothingControlCounter <= constrainsInSamples.pitchSmoothing)
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
                    castPitchControl = (int32_t)pitchControl;

                    pitchFraction = pitchControl - (int32_t)pitchControl;

                    currentFractionPitchControl = pitchFraction * MAX_16BIT;

                    pitchSmoothingControlCounter++;
                }
            }
            //***********************************************

            //***************************************************************************************
            // poczatek przetwarzania pitchCountera
            currentSampelValue = *(in + iPitchCounter);

            if (interpolationCondition)
                interpolationDif = 0;
            else
                interpolationDif = (*(in_interpolation + iPitchCounter) - currentSampelValue);

            //**************************************************************************
            int32_t interpSample =
              currentSampelValue +
              (int32_t)(((currentFractionPitchCounter >> 2) * interpolationDif) >> 14);
            if (isSquareEnv)
            {
                *out++ = (interpSample * granularSquareGain(iPitchCounter,
                                                            constrainsInSamples.loopPoint1,
                                                            constrainsInSamples.loopPoint2,
                                                            granularSquareFadeLength)) >>
                         16;
            }
            else
            {
                uint16_t volIndex = mapClamped<int>(iPitchCounter,
                                                    constrainsInSamples.loopPoint1,
                                                    constrainsInSamples.loopPoint2,
                                                    0,
                                                    GRANULAR_LOOKUP_SIZE - 1);
                *out++ = (interpSample * granularEnvelopeTab[volIndex]) >> 16;
            }

            iPitchCounter += castPitchControl;

            currentFractionPitchCounter += currentFractionPitchControl;
            if (currentFractionPitchCounter >= MAX_16BIT)
            {
                currentFractionPitchCounter -= MAX_16BIT;
                iPitchCounter++;
            }
            //**************************************************************************

            //***************************************************************************************
            // koniec przetwarzania pitchCountera

            //***************************************************************************************
            // warunki pętli
            if (iPitchCounter >= loopEndPoint)
            {
                uint32_t exceedConstrainValue = iPitchCounter - loopEndPoint;
                if (granularPositionRefreshFlag)
                {
                    refreshGranularPosition();
                    // aktualizacja po zmianie pozycji
                    loopEndPoint =
                      std::min((int32_t)constrainsInSamples.loopPoint2,
                               (int32_t)std::min<int>(length, constrainsInSamples.endPoint));

                    if (enableInterpolation)
                        interpolationCondition =
                          ((iPitchCounter + 128 * pitchControl) < length) ? 0 : 1;
                    else
                        interpolationCondition = 1;

                    in = (int16_t*)currentStartAddress + pointsInSamples.start;
                    in_interpolation = in + 1;
                }

                iPitchCounter = constrainsInSamples.loopPoint1 + exceedConstrainValue;
                iPitchCounter =
                  keepInLoop(iPitchCounter, (int32_t)constrainsInSamples.loopPoint1, loopEndPoint);
            }
            //***************************************************************************************
            // warunki pętli
        }
        next = currentStartAddress + pointsInSamples.start;
        fPitchCounter = (float)currentFractionPitchCounter / MAX_16BIT;

        return block;
    }

    return nullptr;
}
AUDIO_CORE_FLASH audio_block_t* AudioPlayMemory::updateGranularLoopForwardReverse()
{
    audio_block_t* block = nullptr;
    int16_t* in = nullptr;
    int16_t* out = nullptr;
    int32_t castPitchControl;
    float pitchFraction;
    int32_t loopStartPoint = std::max<int>((int32_t)constrainsInSamples.loopPoint1, 0);

    block = allocate();
    if (!block)
        return nullptr;

    if (!playing)
    {
        release(block);
        return nullptr;
    }
    else if (playing == 1)
    {
        out = block->data;
        in = (int16_t*)next;

        castPitchControl = (int32_t)-pitchControl;
        pitchFraction = -(pitchControl - (int32_t)pitchControl);

        if (enableInterpolation)
            interpolationCondition = ((int)(iPitchCounter - 128 * pitchControl) > 0) ? 0 : 1;
        else
            interpolationCondition = 1;

        int16_t* in_interpolation = in - 1;

        int32_t currentFractionPitchCounter = fPitchCounter * MAX_16BIT;
        int32_t currentFractionPitchControl = pitchFraction * MAX_16BIT;
        const bool isSquareEnv = (granularEnvelopeTab == squareTab);

        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        {
            //*********************************** GLIDE HANDLE

            if (constrainsInSamples.pitchSmoothing)
            {
                if (pitchSmoothingControlCounter <= constrainsInSamples.pitchSmoothing)
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
                    castPitchControl = (int32_t)-pitchControl;
                    pitchFraction = -(pitchControl - (int32_t)pitchControl);

                    currentFractionPitchControl = pitchFraction * MAX_16BIT;

                    pitchSmoothingControlCounter++;
                }
            }
            //***********************************************

            //***************************************************************************************
            // poczatek przetwarzania pitchCountera
            currentSampelValue = *(in + iPitchCounter);

            if (interpolationCondition)
                interpolationDif = 0;
            else
                interpolationDif = (*(in_interpolation + iPitchCounter) - currentSampelValue);

            //**************************************************************************
            int32_t interpSample =
              currentSampelValue +
              (int32_t)((((-currentFractionPitchCounter) >> 2) * interpolationDif) >> 14);
            if (isSquareEnv)
            {
                *out++ = (interpSample * granularSquareGain(iPitchCounter,
                                                            constrainsInSamples.loopPoint1,
                                                            constrainsInSamples.loopPoint2,
                                                            granularSquareFadeLength)) >>
                         16;
            }
            else
            {
                uint16_t volIndex = mapClamped<int>(iPitchCounter,
                                                    constrainsInSamples.loopPoint1,
                                                    constrainsInSamples.loopPoint2,
                                                    0,
                                                    GRANULAR_LOOKUP_SIZE - 1);
                *out++ = (interpSample * granularEnvelopeTab[volIndex]) >> 16;
            }

            iPitchCounter += castPitchControl;
            currentFractionPitchCounter += currentFractionPitchControl;

            if (currentFractionPitchCounter <= -MAX_16BIT)
            {
                currentFractionPitchCounter += MAX_16BIT;
                iPitchCounter--;
            }
            //**************************************************************************

            //***************************************************************************************
            // koniec przetwarzania pitchCountera

            //***************************************************************************************
            // warunki pętli
            if (iPitchCounter <= loopStartPoint)
            {
                uint32_t exceedConstrainValue = loopStartPoint - iPitchCounter;
                if (granularPositionRefreshFlag)
                {
                    refreshGranularPosition();

                    // aktualizacja po zmianie pozycji
                    loopStartPoint = std::max<int>((int32_t)constrainsInSamples.loopPoint1, 0);

                    if (enableInterpolation)
                        interpolationCondition =
                          ((int)(iPitchCounter - 128 * pitchControl) > 0) ? 0 : 1;
                    else
                        interpolationCondition = 1;

                    in = (int16_t*)currentStartAddress + pointsInSamples.start;
                    in_interpolation = in - 1;
                }
                iPitchCounter = constrainsInSamples.loopPoint2 - exceedConstrainValue;
                iPitchCounter = keepInLoop(
                  iPitchCounter, loopStartPoint, (int32_t)constrainsInSamples.loopPoint2);
            }
            //***************************************************************************************
            // warunki pętli
        }

        next = currentStartAddress + pointsInSamples.start;
        fPitchCounter = (float)currentFractionPitchCounter / MAX_16BIT;

        return block;
    }

    return nullptr;
}
AUDIO_CORE_FLASH audio_block_t* AudioPlayMemory::updateGranularLoopBackwardNormal()
{
    audio_block_t* block = nullptr;
    int16_t* in = nullptr;
    int16_t* out = nullptr;
    int32_t castPitchControl;
    float pitchFraction;
    int32_t loopStartPoint = std::max<int>((int32_t)constrainsInSamples.loopPoint1, 0);
    int32_t loopEndPoint = std::min((int32_t)constrainsInSamples.loopPoint2,
                                    (int32_t)std::min<int>(length, constrainsInSamples.endPoint));

    block = allocate();
    if (!block)
        return nullptr;

    if (!playing)
    {
        release(block);
        return nullptr;
    }
    else if (playing == 1)
    {
        out = block->data;
        in = (int16_t*)next;

        castPitchControl = (int32_t)pitchControl;
        pitchFraction = pitchControl - (int32_t)pitchControl;

        int32_t currentFractionPitchCounter = fPitchCounter * MAX_16BIT;
        int32_t currentFractionPitchControl = pitchFraction * MAX_16BIT;

        if (enableInterpolation)
            interpolationCondition = (((iPitchCounter + 128 * pitchControl) < length)) ? 0 : 1;
        else
            interpolationCondition = 1;

        int16_t* in_interpolation = loopBackwardFlag ? in - 1 : in + 1;
        int32_t currentInterpolationFraction;
        const bool isSquareEnv = (granularEnvelopeTab == squareTab);
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        {
            //*********************************** GLIDE HANDLE

            if (constrainsInSamples.pitchSmoothing)
            {
                if (pitchSmoothingControlCounter <= constrainsInSamples.pitchSmoothing)
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
                    castPitchControl = (int32_t)pitchControl;
                    pitchFraction = pitchControl - (int32_t)pitchControl;

                    currentFractionPitchControl = pitchFraction * MAX_16BIT;

                    pitchSmoothingControlCounter++;
                }
            }
            //**********************************************
            //***************************************************************************************
            // poczatek przetwarzania pitchCountera
            currentSampelValue = *(in + iPitchCounter);

            if (interpolationCondition)
                interpolationDif = 0;
            else
                interpolationDif = (*(in_interpolation + iPitchCounter) - currentSampelValue);

            //**************************************************************************
            currentInterpolationFraction = (currentFractionPitchCounter < 0)
                                             ? -currentFractionPitchCounter
                                             : currentFractionPitchCounter;
            int32_t interpSample =
              currentSampelValue +
              (int32_t)(((currentInterpolationFraction >> 2) * interpolationDif) >> 14);
            if (isSquareEnv)
            {
                *out++ = (interpSample * granularSquareGain(iPitchCounter,
                                                            constrainsInSamples.loopPoint1,
                                                            constrainsInSamples.loopPoint2,
                                                            granularSquareFadeLength)) >>
                         16;
            }
            else
            {
                uint16_t volIndex = mapClamped<int>(iPitchCounter,
                                                    constrainsInSamples.loopPoint1,
                                                    constrainsInSamples.loopPoint2,
                                                    0,
                                                    GRANULAR_LOOKUP_SIZE - 1);
                *out++ = (interpSample * granularEnvelopeTab[volIndex]) >> 16;
            }

            if (!loopBackwardFlag)
            {
                iPitchCounter += castPitchControl;
                currentFractionPitchCounter += currentFractionPitchControl;
                if (currentFractionPitchCounter >= MAX_16BIT)
                {
                    currentFractionPitchCounter -= MAX_16BIT;
                    iPitchCounter++;
                }
            }
            else
            {
                iPitchCounter -= castPitchControl;

                currentFractionPitchCounter -= currentFractionPitchControl;
                if (currentFractionPitchCounter <= -MAX_16BIT)
                {
                    currentFractionPitchCounter += MAX_16BIT;
                    iPitchCounter--;
                }
            }
            //**************************************************************************

            //***************************************************************************************
            // koniec przetwarzania pitchCountera

            //***************************************************************************************
            // warunki pętli
            if (iPitchCounter <= loopStartPoint)
            {
                uint32_t exceedConstrainValue = loopStartPoint - iPitchCounter;
                if (granularPositionRefreshFlag)
                {
                    refreshGranularPosition();

                    // aktualizacja po zmianie pozycji
                    loopStartPoint = std::max<int>((int32_t)constrainsInSamples.loopPoint1, 0);
                    loopEndPoint =
                      std::min((int32_t)constrainsInSamples.loopPoint2,
                               (int32_t)std::min<int>(length, constrainsInSamples.endPoint));

                    if (enableInterpolation)
                        interpolationCondition =
                          (((iPitchCounter + 128 * pitchControl) < length)) ? 0 : 1;
                    else
                        interpolationCondition = 1;

                    in = (int16_t*)currentStartAddress + pointsInSamples.start;
                    in_interpolation = loopBackwardFlag ? in - 1 : in + 1;
                }
                iPitchCounter = constrainsInSamples.loopPoint2 - exceedConstrainValue;
                iPitchCounter = keepInLoop(iPitchCounter, loopStartPoint, loopEndPoint);
            }
            else if (iPitchCounter >= loopEndPoint)
            {
                uint32_t exceedConstrainValue = iPitchCounter - loopEndPoint;
                if (granularPositionRefreshFlag)
                {
                    refreshGranularPosition();

                    // aktualizacja po zmianie pozycji
                    loopStartPoint = std::max<int>((int32_t)constrainsInSamples.loopPoint1, 0);
                    loopEndPoint =
                      std::min((int32_t)constrainsInSamples.loopPoint2,
                               (int32_t)std::min<int>(length, constrainsInSamples.endPoint));

                    if (enableInterpolation)
                        interpolationCondition =
                          (((iPitchCounter + 128 * pitchControl) < length)) ? 0 : 1;
                    else
                        interpolationCondition = 1;

                    in = (int16_t*)currentStartAddress + pointsInSamples.start;
                    in_interpolation = loopBackwardFlag ? in - 1 : in + 1;
                }
                iPitchCounter = constrainsInSamples.loopPoint2 - 1 - exceedConstrainValue;
                iPitchCounter = keepInLoop(iPitchCounter, loopStartPoint, loopEndPoint);
                loopBackwardFlag = 1;
                in_interpolation = in + 1;
                currentFractionPitchCounter = -currentFractionPitchCounter;
            }
            //***************************************************************************************
            // warunki pętli
        }

        next = currentStartAddress + pointsInSamples.start;
        fPitchCounter = (float)currentFractionPitchCounter / MAX_16BIT;

        return block;
    }
    return nullptr;
}

AUDIO_CORE_FLASH audio_block_t* AudioPlayMemory::updateGranularLoopBackwardReverse()
{
    audio_block_t* block = nullptr;
    int16_t* in = nullptr;
    int16_t* out = nullptr;
    int32_t castPitchControl;
    float pitchFraction;
    int32_t loopStartPoint = std::max<int>((int32_t)constrainsInSamples.loopPoint1, 0);
    int32_t loopEndPoint = std::min((int32_t)constrainsInSamples.loopPoint2,
                                    (int32_t)std::min<int>(length, constrainsInSamples.endPoint));

    block = allocate();
    if (!block)
        return nullptr;

    if (!playing)
    {
        release(block);
        return nullptr;
    }
    else if (playing == 1)
    {
        out = block->data;
        in = (int16_t*)next;

        castPitchControl = (int32_t)-pitchControl;
        pitchFraction = -(pitchControl - (int32_t)pitchControl);

        int32_t currentFractionPitchCounter = fPitchCounter * MAX_16BIT;
        int32_t currentFractionPitchControl = pitchFraction * MAX_16BIT;

        if (enableInterpolation)
            interpolationCondition = (((int)(iPitchCounter - 128 * pitchControl) > 0)) ? 0 : 1;
        else
            interpolationCondition = 1;

        int16_t* in_interpolation = loopBackwardFlag ? in + 1 : in - 1;
        int32_t currentInterpolationFraction;
        const bool isSquareEnv = (granularEnvelopeTab == squareTab);
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        {
            //*********************************** GLIDE HANDLE

            if (constrainsInSamples.pitchSmoothing)
            {
                if (pitchSmoothingControlCounter <= constrainsInSamples.pitchSmoothing)
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
                    castPitchControl = (int32_t)-pitchControl;
                    pitchFraction = -(pitchControl - (int32_t)pitchControl);
                    currentFractionPitchControl = pitchFraction * MAX_16BIT;

                    pitchSmoothingControlCounter++;
                }
            }
            //**********************************************
            //***************************************************************************************
            // poczatek przetwarzania pitchCountera
            currentSampelValue = *(in + iPitchCounter);

            if (interpolationCondition)
                interpolationDif = 0;
            else
                interpolationDif = (*(in_interpolation + iPitchCounter) - currentSampelValue);

            //**************************************************************************

            currentInterpolationFraction = (currentFractionPitchCounter < 0)
                                             ? -currentFractionPitchCounter
                                             : currentFractionPitchCounter;
            int32_t interpSample =
              currentSampelValue +
              (int32_t)(((currentInterpolationFraction >> 2) * interpolationDif) >> 14);
            if (isSquareEnv)
            {
                *out++ = (interpSample * granularSquareGain(iPitchCounter,
                                                            constrainsInSamples.loopPoint1,
                                                            constrainsInSamples.loopPoint2,
                                                            granularSquareFadeLength)) >>
                         16;
            }
            else
            {
                uint16_t volIndex = mapClamped<int>(iPitchCounter,
                                                    constrainsInSamples.loopPoint1,
                                                    constrainsInSamples.loopPoint2,
                                                    0,
                                                    GRANULAR_LOOKUP_SIZE - 1);
                *out++ = (interpSample * granularEnvelopeTab[volIndex]) >> 16;
            }

            if (!loopBackwardFlag)
            {
                iPitchCounter += castPitchControl;
                currentFractionPitchCounter += currentFractionPitchControl;
                if (currentFractionPitchCounter <= -MAX_16BIT)
                {
                    currentFractionPitchCounter += MAX_16BIT;
                    iPitchCounter--;
                }
            }
            else
            {
                iPitchCounter -= castPitchControl;

                currentFractionPitchCounter -= currentFractionPitchControl;
                if (currentFractionPitchCounter >= MAX_16BIT)
                {
                    currentFractionPitchCounter -= MAX_16BIT;
                    iPitchCounter++;
                }
            }
            //**************************************************************************

            //***************************************************************************************
            // koniec przetwarzania pitchCountera
            if (iPitchCounter >= loopEndPoint)
            {
                uint32_t exceedConstrainValue = iPitchCounter - loopEndPoint;
                if (granularPositionRefreshFlag)
                {
                    refreshGranularPosition();

                    // aktualizacja po zmianie pozycji
                    loopStartPoint = std::max<int>((int32_t)constrainsInSamples.loopPoint1, 0);
                    loopEndPoint =
                      std::min((int32_t)constrainsInSamples.loopPoint2,
                               (int32_t)std::min<int>(length, constrainsInSamples.endPoint));

                    if (enableInterpolation)
                        interpolationCondition =
                          (((int)(iPitchCounter - 128 * pitchControl) > 0)) ? 0 : 1;
                    else
                        interpolationCondition = 1;

                    in = (int16_t*)currentStartAddress + pointsInSamples.start;
                    in_interpolation = loopBackwardFlag ? in + 1 : in - 1;
                }
                iPitchCounter = constrainsInSamples.loopPoint1 ? constrainsInSamples.loopPoint1 : 1;
                iPitchCounter += exceedConstrainValue;
                iPitchCounter = keepInLoop(iPitchCounter, loopStartPoint, loopEndPoint);
            }
            else if (iPitchCounter <= loopStartPoint)
            {
                uint32_t exceedConstrainValue = loopStartPoint - iPitchCounter;
                if (granularPositionRefreshFlag)
                {
                    refreshGranularPosition();

                    // aktualizacja po zmianie pozycji
                    loopStartPoint = std::max<int>((int32_t)constrainsInSamples.loopPoint1, 0);
                    loopEndPoint =
                      std::min((int32_t)constrainsInSamples.loopPoint2,
                               (int32_t)std::min<int>(length, constrainsInSamples.endPoint));

                    if (enableInterpolation)
                        interpolationCondition =
                          (((int)(iPitchCounter - 128 * pitchControl) > 0)) ? 0 : 1;
                    else
                        interpolationCondition = 1;

                    in = (int16_t*)currentStartAddress + pointsInSamples.start;
                    in_interpolation = loopBackwardFlag ? in + 1 : in - 1;
                }
                iPitchCounter =
                  constrainsInSamples.loopPoint1 ? constrainsInSamples.loopPoint1 + 1 : 1;
                iPitchCounter += exceedConstrainValue;
                iPitchCounter = keepInLoop(iPitchCounter, loopStartPoint, loopEndPoint);
                loopBackwardFlag = 1;
                in_interpolation = in + 1;
                currentFractionPitchCounter = -currentFractionPitchCounter;
            }
        }

        next = currentStartAddress + pointsInSamples.start;
        fPitchCounter = (float)currentFractionPitchCounter / MAX_16BIT;

        return block;
    }

    return nullptr;
}
AUDIO_CORE_FLASH audio_block_t* AudioPlayMemory::updateGranularLoopPingPongNormal()
{
    audio_block_t* block = nullptr;
    int16_t* in = nullptr;
    int16_t* out = nullptr;
    int32_t castPitchControl;
    float pitchFraction;
    int32_t loopStartPoint = std::max<int>((int32_t)constrainsInSamples.loopPoint1, 0);
    int32_t loopEndPoint = std::min((int32_t)constrainsInSamples.loopPoint2,
                                    (int32_t)std::min<int>(length, constrainsInSamples.endPoint));

    block = allocate();
    if (!block)
        return nullptr;

    if (!playing)
    {
        release(block);
        return nullptr;
    }
    else if (playing == 1)
    {
        out = block->data;
        in = (int16_t*)next;

        castPitchControl = (int32_t)pitchControl;
        pitchFraction = pitchControl - (int32_t)pitchControl;

        int32_t currentFractionPitchCounter = fPitchCounter * MAX_16BIT;
        int32_t currentFractionPitchControl = pitchFraction * MAX_16BIT;

        if (enableInterpolation)
            interpolationCondition = (((iPitchCounter + 128 * pitchControl) < length)) ? 0 : 1;
        else
            interpolationCondition = 1;

        int16_t* in_interpolation = loopBackwardFlag ? in - 1 : in + 1;
        int32_t currentInterpolationFraction;
        const bool isSquareEnv = (granularEnvelopeTab == squareTab);

        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        {
            //*********************************** GLIDE HANDLE

            if (constrainsInSamples.pitchSmoothing)
            {
                if (pitchSmoothingControlCounter <= constrainsInSamples.pitchSmoothing)
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
                    castPitchControl = (int32_t)pitchControl;
                    pitchFraction = pitchControl - (int32_t)pitchControl;

                    currentFractionPitchControl = pitchFraction * MAX_16BIT;

                    pitchSmoothingControlCounter++;
                }
            }
            //***********************************************
            //***************************************************************************************
            // poczatek przetwarzania pitchCountera
            currentSampelValue = *(in + iPitchCounter);

            if (interpolationCondition)
                interpolationDif = 0;
            else
                interpolationDif = (*(in_interpolation + iPitchCounter) - currentSampelValue);

            currentInterpolationFraction = (currentFractionPitchCounter < 0)
                                             ? -currentFractionPitchCounter
                                             : currentFractionPitchCounter;
            //**************************************************************************
            int32_t interpSample =
              currentSampelValue +
              (int32_t)(((currentInterpolationFraction >> 2) * interpolationDif) >> 14);
            if (isSquareEnv)
            {
                *out++ = (interpSample * granularSquareGain(iPitchCounter,
                                                            constrainsInSamples.loopPoint1,
                                                            constrainsInSamples.loopPoint2,
                                                            granularSquareFadeLength)) >>
                         16;
            }
            else
            {
                uint16_t volIndex = mapClamped<int>(iPitchCounter,
                                                    constrainsInSamples.loopPoint1,
                                                    constrainsInSamples.loopPoint2,
                                                    0,
                                                    GRANULAR_LOOKUP_SIZE - 1);
                *out++ = (interpSample * granularEnvelopeTab[volIndex]) >> 16;
            }

            if (!loopBackwardFlag)
            {
                iPitchCounter += castPitchControl;
                currentFractionPitchCounter += currentFractionPitchControl;
                if (currentFractionPitchCounter >= MAX_16BIT)
                {
                    currentFractionPitchCounter -= MAX_16BIT;
                    iPitchCounter++;
                }
            }
            else
            {
                iPitchCounter -= castPitchControl;
                currentFractionPitchCounter -= currentFractionPitchControl;
                if (currentFractionPitchCounter <= -MAX_16BIT)
                {
                    currentFractionPitchCounter += MAX_16BIT;
                    iPitchCounter--;
                }
            }
            //**************************************************************************

            //***************************************************************************************
            // koniec przetwarzania pitchCountera
            if (iPitchCounter <= loopStartPoint)
            {
                uint32_t exceedConstrainValue = loopStartPoint - iPitchCounter;
                if (granularPositionRefreshFlag)
                {
                    refreshGranularPosition();

                    // aktualizacja po zmianie pozycji
                    loopStartPoint = std::max<int>((int32_t)constrainsInSamples.loopPoint1, 0);
                    loopEndPoint =
                      std::min((int32_t)constrainsInSamples.loopPoint2,
                               (int32_t)std::min<int>(length, constrainsInSamples.endPoint));

                    if (enableInterpolation)
                        interpolationCondition =
                          (((iPitchCounter + 128 * pitchControl) < length)) ? 0 : 1;
                    else
                        interpolationCondition = 1;

                    in = (int16_t*)currentStartAddress + pointsInSamples.start;
                    in_interpolation = loopBackwardFlag ? in - 1 : in + 1;
                }
                iPitchCounter =
                  constrainsInSamples.loopPoint1 ? constrainsInSamples.loopPoint1 + 1 : 1;
                iPitchCounter += exceedConstrainValue;
                iPitchCounter = keepInLoop(iPitchCounter, loopStartPoint, loopEndPoint);
                loopBackwardFlag = 0;
                in_interpolation = in + 1;
                currentFractionPitchCounter = -currentFractionPitchCounter;
            }
            else if (iPitchCounter >= loopEndPoint)
            {
                uint32_t exceedConstrainValue = iPitchCounter - loopEndPoint;
                if (granularPositionRefreshFlag)
                {
                    refreshGranularPosition();

                    // aktualizacja po zmianie pozycji
                    loopStartPoint = std::max<int>((int32_t)constrainsInSamples.loopPoint1, 0);
                    loopEndPoint =
                      std::min((int32_t)constrainsInSamples.loopPoint2,
                               (int32_t)std::min<int>(length, constrainsInSamples.endPoint));

                    if (enableInterpolation)
                        interpolationCondition =
                          (((iPitchCounter + 128 * pitchControl) < length)) ? 0 : 1;
                    else
                        interpolationCondition = 1;

                    in = (int16_t*)currentStartAddress + pointsInSamples.start;
                    in_interpolation = loopBackwardFlag ? in - 1 : in + 1;
                }
                iPitchCounter = constrainsInSamples.loopPoint2 - 1 - exceedConstrainValue;
                iPitchCounter = keepInLoop(iPitchCounter, loopStartPoint, loopEndPoint);
                loopBackwardFlag = 1;
                in_interpolation = in - 1;
                currentFractionPitchCounter = -currentFractionPitchCounter;
            }
        }

        next = currentStartAddress + pointsInSamples.start;
        fPitchCounter = (float)currentFractionPitchCounter / MAX_16BIT;

        return block;
    }

    return nullptr;
}
AUDIO_CORE_FLASH audio_block_t* AudioPlayMemory::updateGranularLoopPingPongReverse()
{
    audio_block_t* block = nullptr;
    int16_t* in = nullptr;
    int16_t* out = nullptr;
    int32_t castPitchControl;
    float pitchFraction;
    int32_t loopStartPoint = std::max<int>((int32_t)constrainsInSamples.loopPoint1, 0);
    int32_t loopEndPoint = std::min((int32_t)constrainsInSamples.loopPoint2,
                                    (int32_t)std::min<int>(length, constrainsInSamples.endPoint));

    block = allocate();
    if (!block)
        return nullptr;

    if (!playing)
    {
        release(block);
        return nullptr;
    }
    else if (playing == 1)
    {
        out = block->data;
        in = (int16_t*)next;

        castPitchControl = (int32_t)-pitchControl;
        pitchFraction = -(pitchControl - (int32_t)pitchControl);

        int32_t currentFractionPitchCounter = fPitchCounter * MAX_16BIT;
        int32_t currentFractionPitchControl = pitchFraction * MAX_16BIT;

        if (enableInterpolation)
            interpolationCondition = (((int)(iPitchCounter - 128 * pitchControl) > 0)) ? 0 : 1;
        else
            interpolationCondition = 1;

        int16_t* in_interpolation = loopBackwardFlag ? in + 1 : in - 1;
        int32_t currentInterpolationFraction;
        const bool isSquareEnv = (granularEnvelopeTab == squareTab);

        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        {
            //*********************************** GLIDE HANDLE

            if (constrainsInSamples.pitchSmoothing)
            {
                if (pitchSmoothingControlCounter <= constrainsInSamples.pitchSmoothing)
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
                    castPitchControl = (int32_t)-pitchControl;
                    pitchFraction = -(pitchControl - (int32_t)pitchControl);
                    currentFractionPitchControl = pitchFraction * MAX_16BIT;
                    pitchSmoothingControlCounter++;
                }
            }
            //***********************************************
            //***************************************************************************************
            // poczatek przetwarzania pitchCountera
            currentSampelValue = *(in + iPitchCounter);

            if (interpolationCondition)
                interpolationDif = 0;
            else
                interpolationDif = (*(in_interpolation + iPitchCounter) - currentSampelValue);

            //**************************************************************************

            currentInterpolationFraction = (currentFractionPitchCounter < 0)
                                             ? -currentFractionPitchCounter
                                             : currentFractionPitchCounter;
            int32_t interpSample =
              currentSampelValue +
              (int32_t)(((currentInterpolationFraction >> 2) * interpolationDif) >> 14);
            if (isSquareEnv)
            {
                *out++ = (interpSample * granularSquareGain(iPitchCounter,
                                                            constrainsInSamples.loopPoint1,
                                                            constrainsInSamples.loopPoint2,
                                                            granularSquareFadeLength)) >>
                         16;
            }
            else
            {
                uint16_t volIndex = mapClamped<int>(iPitchCounter,
                                                    constrainsInSamples.loopPoint1,
                                                    constrainsInSamples.loopPoint2,
                                                    0,
                                                    GRANULAR_LOOKUP_SIZE - 1);
                *out++ = (interpSample * granularEnvelopeTab[volIndex]) >> 16;
            }
            if (!loopBackwardFlag)
            {
                iPitchCounter += castPitchControl;
                currentFractionPitchCounter += currentFractionPitchControl;
                if (currentFractionPitchCounter <= -MAX_16BIT)
                {
                    currentFractionPitchCounter += MAX_16BIT;
                    iPitchCounter--;
                }
            }
            else
            {
                iPitchCounter -= castPitchControl;
                currentFractionPitchCounter -= currentFractionPitchControl;
                if (currentFractionPitchCounter >= MAX_16BIT)
                {
                    currentFractionPitchCounter -= MAX_16BIT;
                    iPitchCounter++;
                }
            }
            //**************************************************************************

            //***************************************************************************************
            // koniec przetwarzania pitchCountera

            //***************************************************************************************
            // warunki pointow

            if (iPitchCounter >= loopEndPoint)
            {
                uint32_t exceedConstrainValue = iPitchCounter - loopEndPoint;
                if (granularPositionRefreshFlag)
                {
                    refreshGranularPosition();

                    // aktualizacja po zmianie pozycji
                    loopStartPoint = std::max<int>((int32_t)constrainsInSamples.loopPoint1, 0);
                    loopEndPoint =
                      std::min((int32_t)constrainsInSamples.loopPoint2,
                               (int32_t)std::min<int>(length, constrainsInSamples.endPoint));

                    if (enableInterpolation)
                        interpolationCondition =
                          (((int)(iPitchCounter - 128 * pitchControl) > 0)) ? 0 : 1;
                    else
                        interpolationCondition = 1;

                    in = (int16_t*)currentStartAddress + pointsInSamples.start;
                    in_interpolation = loopBackwardFlag ? in + 1 : in - 1;
                }
                iPitchCounter = constrainsInSamples.loopPoint2 - 1 - exceedConstrainValue;
                loopBackwardFlag = 0;
                in_interpolation = in - 1;
                currentFractionPitchCounter = -currentFractionPitchCounter;
            }
            if (iPitchCounter <= loopStartPoint)
            {
                uint32_t exceedConstrainValue = loopStartPoint - iPitchCounter;

                if (granularPositionRefreshFlag)
                {
                    refreshGranularPosition();

                    // aktualizacja po zmianie pozycji
                    loopStartPoint = std::max<int>((int32_t)constrainsInSamples.loopPoint1, 0);
                    loopEndPoint =
                      std::min((int32_t)constrainsInSamples.loopPoint2,
                               (int32_t)std::min<int>(length, constrainsInSamples.endPoint));

                    if (enableInterpolation)
                        interpolationCondition =
                          (((int)(iPitchCounter - 128 * pitchControl) > 0)) ? 0 : 1;
                    else
                        interpolationCondition = 1;

                    in = (int16_t*)currentStartAddress + pointsInSamples.start;
                    in_interpolation = loopBackwardFlag ? in + 1 : in - 1;
                }
                iPitchCounter =
                  constrainsInSamples.loopPoint1 ? constrainsInSamples.loopPoint1 + 1 : 1;
                iPitchCounter += exceedConstrainValue;
                iPitchCounter = keepInLoop(iPitchCounter, loopStartPoint, loopEndPoint);
                loopBackwardFlag = 1;
                in_interpolation = in + 1;
                currentFractionPitchCounter = -currentFractionPitchCounter;
            }
        }

        next = currentStartAddress + pointsInSamples.start;
        fPitchCounter = (float)currentFractionPitchCounter / MAX_16BIT;

        return block;
    }

    return nullptr;
}
