#include "Audio.h"
#include "play_memory.h"
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
    if (mtProject.instrument[instrIdx].getGranularScan() != 0)
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

// Heads 2 and 3: extra grains read from the same sample and mixed into the main grain.
// They follow the main grain's position, length, shape and pitch; Spread offsets each head
// by up to (head / heads) of the whole sample. Loop type Backward or the Reverse FX play
// them backwards; PingPong plays them forwards. Direction can reverse a head: it then scans
// the mirror image of the main grain's travel and plays its grains the other way round.
AUDIO_CORE_FLASH void AudioPlayMemory::mixExtraGranularHeads(audio_block_t* block)
{
    const strInstrument& instr = mtProject.instrument[currentInstrIdx];
    const uint8_t heads = instr.getGranularHeads();

    // Streamed samples are not fully in RAM, so extra heads cannot read them directly
    if (heads < 2 || currentIsStreamed || currentStartAddress == nullptr ||
        currentSampleLength < 4 || !playing)
        return;

    const int16_t* data = (const int16_t*)currentStartAddress;
    const uint32_t sampleLength = currentSampleLength;

    uint32_t grainLength = (currentGranularLength >= MIN_GRANULAR_LENGTH)
                             ? currentGranularLength
                             : instr.granular.grainLength;
    if (grainLength > sampleLength - 1)
        grainLength = sampleLength - 1;
    if (grainLength < 2)
        return;

    const bool backwards = (reverseDirectionFlag != 0) != (granularLoopType == granularLoopBackward);
    const bool isSquareEnv = (granularEnvelopeTab == squareTab);
    const uint32_t spread = instr.getGranularSpread();

    const uint32_t pitchInt = (uint32_t)pitchControl;
    const uint32_t pitchFrac = (uint32_t)((pitchControl - (float)pitchInt) * 65536.0f);

    // Keep the total level close to one head: 1/sqrt(heads) in Q15
    const int32_t mixGain = (heads == 2) ? 23170 : 18919;

    int32_t mix[AUDIO_BLOCK_SAMPLES];
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        mix[i] = block->data[i];

    // Main grain position, and its mirror around where the scan started (for reversed heads)
    const uint32_t mainPosition = (uint32_t)currentGranularPosition << 16;
    const uint32_t mirrorPosition = (instr.getGranularScan() != 0)
                                      ? granularScanBase - (granularScanAccum - granularScanBase)
                                      : mainPosition;

    for (uint8_t h = 0; h < heads - 1; h++)
    {
        strExtraGranularHead& head = extraGranularHeads[h];
        const bool reversed = instr.isGranularHeadReversed(h);
        const bool headBackwards = backwards != reversed;

        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        {
            if (head.grainLength == 0 || head.counter >= head.grainLength)
            {
                // New grain around the main position, offset by Spread for this head
                const uint32_t offset =
                  (uint32_t)(((uint64_t)spread * (h + 1) << 32) / (100u * heads));
                const uint32_t position = (reversed ? mirrorPosition : mainPosition) + offset;
                const uint32_t center = (uint32_t)(((uint64_t)position * sampleLength) >> 32);
                head.position = (uint16_t)(position >> 16);

                uint32_t start = (center > grainLength / 2) ? center - grainLength / 2 : 0;
                if (start + grainLength >= sampleLength)
                    start = sampleLength - 1 - grainLength;

                const uint32_t overshoot =
                  (head.grainLength != 0) ? head.counter - head.grainLength : 0;

                head.grainStart = start;
                head.grainLength = grainLength;
                head.counter = (head.startPhase != 0)
                                 ? (uint32_t)(((uint64_t)head.startPhase * grainLength) >> 16)
                                 : overshoot;
                head.startPhase = 0;
                if (head.counter >= grainLength)
                    head.counter = 0;
                head.envStep = ((uint32_t)(GRANULAR_LOOKUP_SIZE - 1) << 16) / grainLength;
            }

            const uint32_t idx =
              headBackwards ? (head.grainLength - 1 - head.counter) : head.counter;
            const int16_t* p = data + head.grainStart + idx;
            int32_t s = p[0];
            const int32_t next = headBackwards ? ((idx > 0) ? p[-1] : s) : p[1];
            s += (int32_t)((((int32_t)(head.fraction >> 2)) * (next - s)) >> 14);

            if (!isSquareEnv)
            {
                uint32_t envIdx = (uint32_t)(((uint64_t)head.counter * head.envStep) >> 16);
                if (envIdx > GRANULAR_LOOKUP_SIZE - 1)
                    envIdx = GRANULAR_LOOKUP_SIZE - 1;
                s = (s * granularEnvelopeTab[envIdx]) >> 16;
            }

            mix[i] += s;

            head.fraction += pitchFrac;
            head.counter += pitchInt + (head.fraction >> 16);
            head.fraction &= 0xFFFF;
        }

        // Read position for the on-screen playhead of this head
        uint32_t counter = (head.counter < head.grainLength) ? head.counter : head.grainLength - 1;
        const uint32_t readIdx =
          head.grainStart + (headBackwards ? (head.grainLength - 1 - counter) : counter);
        head.playPosition = (uint16_t)(((uint64_t)readIdx * 65535u) / sampleLength);
    }

    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
    {
        int32_t v = (mix[i] * mixGain) >> 15;
        if (v > 32767)
            v = 32767;
        else if (v < -32768)
            v = -32768;
        block->data[i] = (int16_t)v;
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
                *out++ = (interpSample * 65535) >> 16;
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
                *out++ = (interpSample * 65535) >> 16;
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
                *out++ = (interpSample * 65535) >> 16;
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
                *out++ = (interpSample * 65535) >> 16;
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
                *out++ = (interpSample * 65535) >> 16;
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
                *out++ = (interpSample * 65535) >> 16;
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
