#include "core/interfacePopups.h"
#include "fileManager.h"
#include "mtAudioEngine.h"
#include "samplePlayback.h"
#include "Profiler.h"

static const uint16_t framesPlaces[7][4] = {
    { 0 + 1, 424, 800 / 8 - 2, 55 },
    { (800 / 8) * 1 + 1, 424, 800 / 8 - 2, 55 },
    { (800 / 8) * 2 + 1, 424, 800 / 8 - 2, 55 },
    { (800 / 8) * 3 + 1, 424, 800 / 8 - 2, 55 },
    { (800 / 8) * 4 + 1, 424, 800 / 8 - 2, 55 },
    { (800 / 8) * 5 + 1, 424, 800 / 8 - 2, 55 },
    { (800 / 8) * 6 + 1, 31, 800 / 4 - 3, 391 },
};

void cSamplePlayback::initDisplayControls()
{
    strControlProperties prop;
    frameData.placesCount = 7;
    frameData.startPlace = 0;
    frameData.places[0] = &framesPlaces[0][0];
    frameData.places[1] = &framesPlaces[1][0];
    frameData.places[2] = &framesPlaces[2][0];
    frameData.places[3] = &framesPlaces[3][0];
    frameData.places[4] = &framesPlaces[4][0];
    frameData.places[5] = &framesPlaces[5][0];
    frameData.places[6] = &framesPlaces[6][0];
    prop.style = 0;
    prop.value = 0;
    prop.colors = nullptr;
    prop.data = &frameData;
    frameControl = display.createControl<cFrame>(&prop);

    labelArrow.bitmaps[0].bitmapIndex = displayArrowU;
    labelArrow.bitmaps[0].xValue = (800 / 8) * 6 + (800 / 16) - 10;
    labelArrow.bitmaps[0].yValue = 450;
    labelArrow.bitmaps[1].bitmapIndex = displayArrowD;
    labelArrow.bitmaps[1].xValue = (800 / 8) * 7 + (800 / 16) + 10;
    labelArrow.bitmaps[1].yValue = 450;

    labelDataMore.dotIndicator.count = 2;
    labelDataMore.dotIndicator.current = 0;

    strControlProperties prop2;
    for (uint8_t i = 0; i < 8; i++)
    {
        prop2.value = 1;
        prop2.colors = display.getTheme().activeLabelsColors;
        prop2.style = (controlStyleCenterX | controlStyleCenterY | controlStyleFont3);
        prop2.y = 424;
        prop2.h = 55;
        if (i == 6)
        {
            prop2.x = (800 / 8) * 6 + (800 / 8);
            prop2.w = 800 / 4 - 6;
        }
        else
        {
            prop2.x = (800 / 8) * i + (800 / 16);
            prop2.w = 800 / 8 - 6;
        }
        label[i] = display.createControl<cLabel>(&prop2);
    }

    prop2.text = nullptr;
    prop2.colors = nullptr;
    prop2.value = 255;
    prop2.style = controlStyleNoTransparency | controlStyleShow;
    prop2.x = 0;
    prop2.w = 800;
    prop2.y = 425;
    prop2.h = 54;
    bgLabel = display.createControl<cBgLabel>(&prop2);

    playModeList.linesCount = 8;
    playModeList.start = editorInstrument->playMode;
    playModeList.length = playModeCount;
    playModeList.data = playModeNames;
    prop.x = (800 / 8) * 6 + 1;
    prop.y = 29;
    prop.w = 800 / 4 - 3;
    prop.h = 394;
    prop.style = controlStyleBackground |
                 (display.getTheme().bottomScreenShadow > 0 ? controlStyleBottomShadow : 0);
    prop.data = &playModeList;
    playModeListControl = display.createControl<cList>(&prop);

    // spectrum + points
    prop.style = 0;
    prop.x = 0;
    prop.y = 75 + ((300 - (MAX_16BIT / 300)) / 2);
    prop.w = 600;
    prop.h = MAX_16BIT / 300;
    progressCursor = display.createControl<cProgressCursor>(&prop);

    lineIndicatorSelection.start = 0;
    lineIndicatorSelection.end = 0;
    prop.data = &lineIndicatorSelection;
    prop.colors = display.getTheme().granularProgressColors;
    granularCursor = display.createControl<cLineIndicator>(&prop);

    // Granular Heads 2 and 3 get their own cursors, orange and yellow
    granularHeadColors[0][0] = display.getTheme().orange;
    granularHeadColors[1][0] = display.getTheme().yellow;
    for (uint8_t h = 0; h < 2; h++)
    {
        granularHeadColors[h][1] = display.getTheme().granularProgressColors[1];
        granularHeadSelection[h].start = 0;
        granularHeadSelection[h].end = 0;
        prop.data = &granularHeadSelection[h];
        prop.colors = granularHeadColors[h];
        granularHeadCursor[h] = display.createControl<cLineIndicator>(&prop);
    }

    // Moving playheads of Heads 2 and 3, in the same colours as their cursors
    for (uint8_t h = 0; h < 2; h++)
    {
        granularHeadProgressColors[h][0] = granularHeadColors[h][0];
        granularHeadProgressColors[h][1] = granularHeadColors[h][0];
        prop.data = nullptr;
        prop.colors = granularHeadProgressColors[h];
        granularHeadProgress[h] = display.createControl<cProgressCursor>(&prop);
    }

    prop.data = &points;
    prop.colors = nullptr;
    pointsControl = display.createControl<cPoints>(&prop);

    prop.data = &slicePoints;
    slicePointsControl = display.createControl<cSlicePoints>(&prop);

    spectrum[0].init();
    spectrum[1].init();

    prop.x = 0;
    prop.y = 75 + ((300 - ((MAX_16BIT / 300))) / 2);
    prop.w = 600;
    prop.h = (MAX_16BIT / 300) / 2;
    prop.data = &spectrum[0];
    spectrumControl[0] = display.createControl<cSpectrum>(&prop);

    prop.x = 0;
    prop.y = 75 + ((300 - ((MAX_16BIT / 300))) / 2) + (MAX_16BIT / 300) / 2;
    prop.w = 600;
    prop.h = (MAX_16BIT / 300) / 2;
    prop.data = &spectrum[1];
    spectrumControl[1] = display.createControl<cSpectrum>(&prop);

    prop.x = 0;
    prop.y = 400;
    prop.w = 600;
    prop.h = 2;
    prop.data = &wtPosition;
    wtPositionCursor = display.createControl<cWtProgress>(&prop);
}

void cSamplePlayback::showTitleBar()
{
    titleBar->show(true);
    titleBar->setTitle("Sample Playback");
    titleBar->showCurrentInstrumentOnRightLabel();
}

void cSamplePlayback::showDefaultScreen()
{
    showTitleBar();
    resizeWaveforms();
    showPlayModeList();

    display.setControlPosition(label[6], (800 / 8) * 6 + (800 / 8), 424);
    display.setControlSize(label[6], 800 / 4 - 6, 55);
    display.setControlData(label[6], &labelArrow);
    display.setAddControlStyle(label[6], controlStyleShowBitmap | controlStyleCenterY);
    display.setRemoveControlStyle(label[5], controlStyleSpecialFx2);

    display.setControlText(label[6], "Play Mode");
    display.setControlHide(label[7]);

    display.setControlColors(label[0], display.getTheme().activeLabelsColors);
    display.setControlColors(label[1], display.getTheme().activeLabelsColors);

    for (uint8_t i = 0; i < 7; i++)
    {
        display.setControlValue(label[i], 1);
        display.setControlStyle2(label[i],
                                 controlStyleCenterX | controlStyleCenterY | controlStyleFont2);
        display.setControlText2(label[i], "");
        display.setControlShow(label[i]);
        display.refreshControl(label[i]);
    }
    display.setControlValue(label[6], 0);

    if (loadedInstrumentType == mtSampleTypeWaveFile)
    {
        display.setControlShow(progressCursor);
        display.refreshControl(progressCursor);
        for (uint8_t h = 0; h < 2; h++)
        {
            display.setControlShow(granularHeadProgress[h]);
            display.refreshControl(granularHeadProgress[h]);
        }

        if ((editorInstrument->playMode == playModeSlice) ||
            (editorInstrument->playMode == playModeBeatSlice))
        {
            display.setControlHide(pointsControl);
            display.setControlHide(granularCursor);
            hideGranularHeadCursors();
            display.setControlShow(slicePointsControl);
            display.refreshControl(slicePointsControl);

            display.setAddControlStyle(label[5], controlStyleSpecialFx2);
            display.setControlData(label[5], &labelDataMore);
            display.setControlColors(label[5], display.getTheme().moreButtonColors);

            // processWavetableCursor(editorInstrument->wavetableCurrentWindow);
            hideWavetablePositionCursor();

            if (!sliceScreenMode2)
            {
                labelDataMore.dotIndicator.count = 2;
                labelDataMore.dotIndicator.current = 0;

                display.setControlText(label[0], "Slice");
                display.setControlText(label[1], "Adjust");
                display.setControlText(label[2], "Add");
                display.setControlText(label[3], "Remove");
                display.setControlText(label[4], "Zoom");
                display.setControlText(label[5], "More");

                display.setControlValue(label[2], 0);
                display.setControlValue(label[3], 0);

                showZoomValue();
                showSlicesAdjustValue();
                showSlicesSelectValue();
            }
            else
            {
                labelDataMore.dotIndicator.count = 2;
                labelDataMore.dotIndicator.current = 1;

                display.setControlText(label[0], "Slice");
                display.setControlText(label[1], "Slice");
                display.setControlText(label[2], "No of Slices");
                display.setControlText(label[3], "Play Thru");
                display.setControlText(label[4], "");
                display.setControlText(label[5], "More");

                display.setControlText2(label[0], "Auto");
                display.setControlText2(label[1], "Evenly");

                display.setControlStyle2(
                  label[0], controlStyleCenterX | controlStyleCenterY | controlStyleFont3);
                display.setControlStyle2(
                  label[1], controlStyleCenterX | controlStyleCenterY | controlStyleFont3);
                display.setControlColors(label[0], display.getTheme().activeButtonLabelsColors);
                display.setControlColors(label[1], display.getTheme().activeButtonLabelsColors);

                showSlicesEqualNumberValue();
                showSlicesPlayThruValue();

                if (selectedPlace != 2 && selectedPlace != 3)
                {
                    selectedPlace = 2;
                }
            }
        }
        else if (editorInstrument->playMode == playModeGranular)
        {
            // label 5 is "More": page 1 = Position/Length/Shape/Loop, page 2 = Scan/Heads/Spread
            display.setAddControlStyle(label[5], controlStyleSpecialFx2);
            display.setControlData(label[5], &labelDataMore);
            display.setControlColors(label[5], display.getTheme().moreButtonColors);
            labelDataMore.dotIndicator.count = 3;
            labelDataMore.dotIndicator.current = granularScreenPage;

            display.setControlText(label[0], "Preview");
            if (granularScreenPage == 0)
            {
                display.setControlText(label[1], "Position");
                display.setControlText(label[2], "Length");
                display.setControlText(label[3], "Shape");
                display.setControlText(label[4], "Loop");
            }
            else if (granularScreenPage == 1)
            {
                display.setControlText(label[1], "Scan");
                display.setControlText(label[2], "Heads");
                display.setControlText(label[3], "Spread");
                display.setControlText(label[4], "Direction");
            }
            else
            {
                display.setControlText(label[1], "Rate");
                display.setControlText(label[2], "Pitch");
                display.setControlText(label[3], "Jitter");
                display.setControlText(label[4], "Density");
            }
            display.setControlText(label[5], "More");

            display.setControlShow(granularCursor);
            display.refreshControl(granularCursor);

            display.setControlHide(pointsControl);
            display.refreshControl(pointsControl);

            display.setControlHide(slicePointsControl);
            display.refreshControl(slicePointsControl);

            showGranularPositionValue(); // also draws the grain cursor
            if (granularScreenPage == 0)
            {
                showGrainLengthValue();
                showShapeText();
                showLoopTypeText();
            }
            else if (granularScreenPage == 1)
            {
                showGranularScanValue();
                showGranularHeadsValue();
                showGranularSpreadValue();
                showGranularDirectionText();
            }
            else
            {
                showGranularRateText();
                showGranularPitchText();
                showGranularJitterValue();
                showGranularDensityText();
            }
            showGranularCpuMeter();
        }
        else
        {
            display.setControlHide(slicePointsControl);
            display.refreshControl(slicePointsControl);

            display.setControlHide(granularCursor);
            display.refreshControl(granularCursor);
            hideGranularHeadCursors();

            display.setControlText(label[0], "Preview");
            display.setControlText(label[1], "Start");
            display.setControlText(label[2], "");
            display.setControlText(label[3], "");
            display.setControlText(label[4], "End");
            display.setControlText(label[5], "Zoom");

            display.setControlShow(pointsControl);
            display.refreshControl(pointsControl);

            if ((editorInstrument->playMode == playModeSingleShot) ||
                (editorInstrument->playMode == playModeWavetable))
            {
                hideLoopPoints();
            }
            else
            {
                showLoopPoints();
            }

            showStartPointValue();
            showEndPointValue();
            showLoopPoint1Value();
            showLoopPoint2Value();
            showZoomValue();
            processWavetableCursor(editorInstrument->wavetableCurrentWindow);
            hideWavetablePositionCursor();

            display.setControlText2(label[0], "-");
        }
    }
    else
    {
        display.setControlText(label[0], "Preview");
        display.setControlText(label[1], "Position");
        display.setControlText(label[2], "Window");
        display.setControlText(label[3], "");
        display.setControlText(label[4], "");
        display.setControlText(label[5], "");

        display.setControlValue(label[0], 0);

        // cursor
        display.setControlHide(progressCursor);
        display.refreshControl(progressCursor);
        for (uint8_t h = 0; h < 2; h++)
        {
            display.setControlHide(granularHeadProgress[h]);
            display.refreshControl(granularHeadProgress[h]);
        }

        display.setControlHide(granularCursor);
        display.refreshControl(granularCursor);
        hideGranularHeadCursors();
        // points
        display.setControlHide(pointsControl);
        display.refreshControl(pointsControl);

        display.setControlHide(slicePointsControl);
        display.refreshControl(slicePointsControl);

        showWavetablePosition();
        showWavetableWindowSize();
        refreshSpectrum = 1;

        processWavetableCursor(editorInstrument->wavetableCurrentWindow);
        showWavetablePositionCursor();
    }

    display.setControlValue(bgLabel, 127);
    display.setControlShow(bgLabel);
    display.refreshControl(bgLabel);

    mtPopups.hideInfoPopup();

    activateLabelsBorder();

    display.synchronizeRefresh();
}

//==============================================================================================================
void cSamplePlayback::hideUselessControls()
{
    display.setControlHide(pointsControl);
    display.setControlHide(frameControl);
    display.setControlHide(playModeListControl);
    display.setControlHide(slicePointsControl);
    display.setControlHide(granularCursor);
    display.setControlHide(granularHeadCursor[0]);
    display.setControlHide(granularHeadCursor[1]);
    display.setControlHide(progressCursor);
    display.setControlHide(granularHeadProgress[0]);
    display.setControlHide(granularHeadProgress[1]);
    display.setControlHide(wtPositionCursor);
    display.setControlHide(wtPositionCursor);
    display.setControlHide(spectrumControl[0]);
    display.setControlHide(spectrumControl[1]);
}

void cSamplePlayback::resizeWaveforms()
{
    if (newFileManager.isInstrumentStereo(mtProject.values.lastUsedInstrument) &&
        editorInstrument->isActive)
    {
        if ((editorInstrument->playMode == playModeSlice) ||
            (editorInstrument->playMode == playModeBeatSlice))
        {
            spectrum[0].pointsLimit = strTrackerSpectrum::k200;
            spectrum[1].pointsLimit = strTrackerSpectrum::k200;
        }
        else
        {
            spectrum[0].pointsLimit = strTrackerSpectrum::k300;
            spectrum[1].pointsLimit = strTrackerSpectrum::k300;
        }

        display.setControlSize(spectrumControl[0], 600, (MAX_16BIT / 300) / 2);

        display.setControlShow(spectrumControl[0]);
        display.setControlShow(spectrumControl[1]);

        display.refreshControl(spectrumControl[0]);
        display.refreshControl(spectrumControl[1]);
    }
    else
    {
        spectrum[0].pointsLimit = strTrackerSpectrum::k300;
        spectrum[1].pointsLimit = strTrackerSpectrum::k300;

        display.setControlSize(spectrumControl[0], 600, (MAX_16BIT / 300));

        display.setControlShow(spectrumControl[0]);
        display.setControlHide(spectrumControl[1]);

        display.refreshControl(spectrumControl[0]);
        display.refreshControl(spectrumControl[1]);
    }
}

//==============================================================================================================
void cSamplePlayback::activateLabelsBorder()
{
    if (selectedPlace > frameData.placesCount - 1)
        return;

    display.setControlValue(frameControl, selectedPlace);
    display.setControlShow(frameControl);
    display.refreshControl(frameControl);
}

//==============================================================================================================
void cSamplePlayback::showWavetablePosition()
{
    if (refreshWavetablePosition)
        sprintf(wavetablePositionText, "%d", (int)currentEnvelopeWtPos);
    else
        sprintf(wavetablePositionText, "%d", (int)editorInstrument->wavetableCurrentWindow);

    display.setControlText2(label[1], wavetablePositionText);
    display.setControlShow(label[1]);
    display.refreshControl(label[1]);
}

void cSamplePlayback::showWavetableWindowSize()
{
    sprintf(wavetableWindowSizeText, "%d", editorInstrument->sample.wavetable_window_size);

    display.setControlText2(label[2], wavetableWindowSizeText);
    display.setControlShow(label[2]);
    display.refreshControl(label[2]);
}

void cSamplePlayback::showZoomValue()
{
    sprintf(zoomTextValue, "%.2f", zoom.zoomValue);

    int labelIdx = 5;

    if ((editorInstrument->playMode == playModeSlice) ||
        (editorInstrument->playMode == playModeBeatSlice))
    {
        labelIdx = 4;
    }

    display.setControlText2(label[labelIdx], zoomTextValue);
    display.setControlShow(label[labelIdx]);
    display.refreshControl(label[labelIdx]);
}

void cSamplePlayback::showPlayModeList()
{
    playModeList.start = editorInstrument->playMode;
    playModeList.length = playModeCount;
    playModeList.linesCount = 9;
    playModeList.data = playModeNames;

    display.setControlData(playModeListControl, &playModeList);
    display.setControlShow(playModeListControl);
    display.refreshControl(playModeListControl);
}

void cSamplePlayback::showStartPointValue()
{

    if (playerEngine::isZeroInPointZone(editorInstrument->startPoint,
                                        editorInstrument->sample.length,
                                        editorInstrument->sample.address))
    {
        points.zerocrossed |= startZerocrossed;
    }
    else
    {
        points.zerocrossed &= (~startZerocrossed);
    }

    float recTimeValue = samplePosToTime(editorInstrument->sample.length);
    float localStartPoint = (recTimeValue * editorInstrument->startPoint) / MAX_16BIT;

    formatTimeSeconds(startPointValueText, sizeof(startPointValueText), localStartPoint);

    display.setControlText2(label[1], startPointValueText);
    display.setControlShow(label[1]);
    display.refreshControl(label[1]);
    display.refreshControl(pointsControl);
}

void cSamplePlayback::showEndPointValue()
{
    if (playerEngine::isZeroInPointZone(editorInstrument->endPoint,
                                        editorInstrument->sample.length,
                                        editorInstrument->sample.address))
    {
        points.zerocrossed |= endZerocrossed;
    }
    else
    {
        points.zerocrossed &= (~endZerocrossed);
    }

    float recTimeValue = samplePosToTime(editorInstrument->sample.length);
    float localEndPoint = (recTimeValue * editorInstrument->endPoint) / MAX_16BIT;
    formatTimeSeconds(endPointValueText, sizeof(endPointValueText), localEndPoint);

    display.setControlText2(label[4], endPointValueText);
    display.setControlShow(label[4]);
    display.refreshControl(label[4]);
    display.refreshControl(pointsControl);
}

void cSamplePlayback::showLoopPoint1Value()
{
    if (editorInstrument->playMode == playModeSingleShot)
    {
        return;
    }

    if (playerEngine::isZeroInPointZone(editorInstrument->loopPoint1,
                                        editorInstrument->sample.length,
                                        editorInstrument->sample.address))
    {
        points.zerocrossed |= loop1Zerocrossed;
    }
    else
    {
        points.zerocrossed &= (~loop1Zerocrossed);
    }

    float recTimeValue = samplePosToTime(editorInstrument->sample.length);
    float localLoopPoint1 = (recTimeValue * editorInstrument->loopPoint1) / MAX_16BIT;

    formatTimeSeconds(loopPoint1ValueText, sizeof(loopPoint1ValueText), localLoopPoint1);

    display.setControlText2(label[2], loopPoint1ValueText);
    display.setControlShow(label[2]);
    display.refreshControl(label[2]);
    display.refreshControl(pointsControl);
}

void cSamplePlayback::showLoopPoint2Value()
{
    if (editorInstrument->playMode == playModeSingleShot)
    {
        return;
    }

    if (playerEngine::isZeroInPointZone(editorInstrument->loopPoint2,
                                        editorInstrument->sample.length,
                                        editorInstrument->sample.address))
    {
        points.zerocrossed |= loop2Zerocrossed;
    }
    else
    {
        points.zerocrossed &= (~loop2Zerocrossed);
    }

    float recTimeValue = samplePosToTime(editorInstrument->sample.length);
    float localLoopPoint2 = (recTimeValue * editorInstrument->loopPoint2) / MAX_16BIT;

    formatTimeSeconds(loopPoint2ValueText, sizeof(loopPoint2ValueText), localLoopPoint2);

    display.setControlText2(label[3], loopPoint2ValueText);
    display.setControlShow(label[3]);
    display.refreshControl(label[3]);
    display.refreshControl(pointsControl);
}

void cSamplePlayback::showPreviewValue()
{
    if (editorInstrument->playMode == playModeGranular)
        return; // the Preview label shows the CPU meter instead

    float playTimeValue =
      playProgressValue * samplePosToTime(editorInstrument->sample.length) / MAX_16BIT;
    float localEndPoint =
      (editorInstrument->sample.length * editorInstrument->endPoint) / MAX_16BIT;
    if (playTimeValue >= (localEndPoint - 0.01))
        playTimeValue = localEndPoint;

    formatTimeSeconds(playTimeValueText, sizeof(playTimeValueText), playTimeValue);

    display.setControlText2(label[0], playTimeValueText);
    display.setControlShow(label[0]);
    display.refreshControl(label[0]);
}

void cSamplePlayback::hidePreviewValue()
{
    if (editorInstrument->playMode == playModeGranular)
    {
        showGranularCpuMeter();
        return;
    }

    display.setControlText2(label[0], "-");
    display.setControlShow(label[0]);
    display.refreshControl(label[0]);
}

void cSamplePlayback::hideLoopPoints()
{
    display.setControlText2(label[2], "");
    display.setControlShow(label[2]);
    display.refreshControl(label[2]);

    display.setControlText2(label[3], "");
    display.setControlShow(label[3]);
    display.refreshControl(label[3]);
}

void cSamplePlayback::showLoopPoints()
{
    display.setControlText(label[2], "Loop Start");
    display.setControlShow(label[2]);
    display.refreshControl(label[2]);

    display.setControlText(label[3], "Loop End");
    display.setControlShow(label[3]);
    display.refreshControl(label[3]);

    showLoopPoint1Value();
    showLoopPoint2Value();
}

void cSamplePlayback::showWavetablePositionCursor()
{
    display.setControlData(wtPositionCursor, &wtPosition);
    display.setControlShow(wtPositionCursor);
    display.refreshControl(wtPositionCursor);
}

void cSamplePlayback::hideWavetablePositionCursor()
{
    display.setControlHide(wtPositionCursor);
    display.refreshControl(wtPositionCursor);
}

void cSamplePlayback::showAutoSlicePopup()
{
    mtPopups.showInfoPopup("This action will rewrite the current slices.",
                           "Do you want to continue?");

    for (uint8_t i = 0; i < 6; i++)
    {
        display.setControlHide(label[i]);
    }

    display.setControlShow(label[6]);
    display.setControlShow(label[7]);
    display.setControlText(label[6], "Cancel");
    display.setControlText(label[7], "Confirm");
    display.refreshControl(label[6]);
    display.refreshControl(label[7]);
    display.setControlValue(label[6], 0);
    display.setControlValue(label[7], 0);

    display.setControlPosition(label[6], (800 / 8) * 6 + (800 / 16), -1);
    display.setControlSize(label[6], 800 / 8 - 6, -1);
    display.setRemoveControlStyle(label[5], controlStyleSpecialFx2);
    display.setRemoveControlStyle(label[6], controlStyleShowBitmap);

    display.setControlValue(bgLabel, 255);
    display.setControlShow(bgLabel);
    display.refreshControl(bgLabel);

    display.setControlHide(frameControl);
    display.refreshControl(frameControl);

    display.synchronizeRefresh();
}

void cSamplePlayback::showSlicesSelectValue()
{
    if (editorInstrument->sliceNumber > 0)
    {
        sprintf(sliceSelectTextValue,
                "%d of %d",
                editorInstrument->selectedSlice + 1,
                editorInstrument->sliceNumber < 1 ? 1 : editorInstrument->sliceNumber);

        display.setControlText2(label[0], sliceSelectTextValue);
    }
    else
    {
        display.setControlText2(label[0], "-");
    }

    display.setControlShow(label[0]);
    display.refreshControl(label[0]);

    lightUpPadBoard();
}

void cSamplePlayback::showSlicesAdjustValue()
{
    if (editorInstrument->sliceNumber > 0)
    {
        float timeValue = samplePosToTime(editorInstrument->sample.length);
        uint16_t slice = (editorInstrument->sliceNumber == 0)
                           ? 0
                           : editorInstrument->slices[editorInstrument->selectedSlice];
        float currentSlice = (timeValue * slice) / MAX_16BIT;

        sprintf(sliceAdjustTextValue, "%.3f s", currentSlice);

        display.setControlText2(label[1], sliceAdjustTextValue);
        display.setControlShow(label[1]);
    }
    else
    {
        display.setControlText2(label[1], "-");
    }

    display.setControlShow(label[1]);
    display.refreshControl(label[1]);
}

void cSamplePlayback::showSlicesEqualNumberValue()
{
    sprintf(equalSliceNumberText, "%d", mtConfig.common.equalSliceNumber);
    display.setControlText2(label[2], equalSliceNumberText);
    display.setControlShow(label[2]);
    display.refreshControl(label[2]);
}

void cSamplePlayback::showSlicesPlayThruValue()
{
    display.setControlText2(label[3], editorInstrument->slicePlayThru ? "On" : "Off");
    display.setControlShow(label[3]);
    display.refreshControl(label[3]);
}

//==============================================================================================================
void cSamplePlayback::showGranularPositionValue()
{
    float localPosition =
      currentEnvelopeGranPos * samplePosToTime(editorInstrument->sample.length) / MAX_16BIT;

    sprintf(granularPositionTextValue, "%0.3f s", localPosition);

    uint32_t effectiveGrainLength =
      (currentEnvelopeGranLen > 0) ? currentEnvelopeGranLen
                                   : static_cast<uint32_t>(editorInstrument->granular.grainLength);

    granularPositionInSpectrum = currentEnvelopeGranPos * (600.0 / MAX_16BIT);

    setGranularCursorSelection(lineIndicatorSelection, granularPositionInSpectrum, effectiveGrainLength);

    display.setControlValue(granularCursor, granularPositionInSpectrum);
    display.setControlShow(granularCursor);
    display.refreshControl(granularCursor);

    showGranularHeadCursors();

    if (granular2ndScreenActive())
        return;

    display.setControlText2(label[1], granularPositionTextValue);
    display.setControlShow(label[1]);
    display.refreshControl(label[1]);
}

// Grain zone around a cursor; near the sample edges the zone keeps its width
void cSamplePlayback::setGranularCursorSelection(strLineIndicatorSelection& selection,
                                                 uint16_t positionInSpectrum,
                                                 uint32_t grainLength)
{
    const int displayedLength = ((uint32_t)(grainLength * 600)) / editorInstrument->sample.length;
    const int half = displayedLength / 2;

    int upperZoneLength = half + (half - std::min((int)positionInSpectrum, half));
    int lowerZoneLength = half + (half - std::min((int)(600 - positionInSpectrum), half));

    selection.start = clamp((int)positionInSpectrum - lowerZoneLength, 0, 600);
    selection.end = clamp((int)positionInSpectrum + upperZoneLength, 0, 600);
}

// Cursors for granular Heads 2 and 3. While a note plays they follow the heads' live grain
// positions; otherwise they show where each head starts (the grain position plus Spread).
void cSamplePlayback::showGranularHeadCursors()
{
    const uint8_t heads = editorInstrument->getGranularHeads();
    const uint32_t grainLength =
      (currentEnvelopeGranLen > 0) ? currentEnvelopeGranLen
                                   : static_cast<uint32_t>(editorInstrument->granular.grainLength);
    const bool playing = (displayedTrack != kNoTrack);

    for (uint8_t h = 0; h < 2; h++)
    {
        uint16_t position = 0;
        const bool visible = (h + 2 <= heads);

        if (visible &&
            !(playing && instrumentPlayer[displayedTrack].getGranularHeadPosition(h + 1, position)))
        {
            // Same offset the audio engine uses: Spread% of (head / heads) of the sample, wrapped
            const uint32_t offset =
              ((uint32_t)editorInstrument->getGranularSpread() * (h + 1) * 65536u) / (100u * heads);
            position = (uint16_t)(currentEnvelopeGranPos + offset);
        }

        if (!visible)
        {
            display.setControlHide(granularHeadCursor[h]);
            display.refreshControl(granularHeadCursor[h]);
            continue;
        }

        const uint16_t inSpectrum = position * (600.0 / MAX_16BIT);
        setGranularCursorSelection(granularHeadSelection[h], inSpectrum, grainLength);
        display.setControlValue(granularHeadCursor[h], inSpectrum);
        display.setControlShow(granularHeadCursor[h]);
        display.refreshControl(granularHeadCursor[h]);
    }
}

void cSamplePlayback::hideGranularHeadCursors()
{
    for (uint8_t h = 0; h < 2; h++)
    {
        display.setControlHide(granularHeadCursor[h]);
        display.refreshControl(granularHeadCursor[h]);
    }
}

void cSamplePlayback::showGrainLengthValue()
{
    grainLengthMs = editorInstrument->granular.grainLength / 44.1f;

    sprintf(grainLengthTextValue, "%0.1f ms", grainLengthMs);

    showGranularPositionValue();

    if (granular2ndScreenActive())
        return;

    display.setControlText2(label[2], grainLengthTextValue);
    display.setControlShow(label[2]);
    display.refreshControl(label[2]);
}

void cSamplePlayback::showShapeText()
{
    if (granular2ndScreenActive())
        return;
    switch (editorInstrument->granular.shape)
    {
        case granularShapeSquare: strcpy(shapeText, "Square"); break;
        case granularShapeTriangle: strcpy(shapeText, "Triangle"); break;
        case granularShapeGauss: strcpy(shapeText, "Gauss"); break;
        default: break;
    }

    display.setControlText2(label[3], shapeText);
    display.setControlShow(label[3]);
    display.refreshControl(label[3]);
}

void cSamplePlayback::showLoopTypeText()
{
    if (granular2ndScreenActive())
        return;
    switch (editorInstrument->granular.type)
    {
        case granularLoopForward: strcpy(loopTypeText, "Forward"); break;
        case granularLoopBackward: strcpy(loopTypeText, "Backward"); break;
        case granularLoopPingPong: strcpy(loopTypeText, "PingPong"); break;
        default: break;
    }

    display.setControlText2(label[4], loopTypeText);
    display.setControlShow(label[4]);
    display.refreshControl(label[4]);
}

void cSamplePlayback::showGranularScanValue()
{
    if (!granularPageActive(1))
        return;

    int scan = editorInstrument->getGranularScan();

    if (scan == 0)
        strcpy(granularScanText, "Off");
    else
        sprintf(granularScanText, "%+d%%", scan);

    display.setControlText2(label[1], granularScanText);
    display.setControlShow(label[1]);
    display.refreshControl(label[1]);
}

void cSamplePlayback::showGranularHeadsValue()
{
    if (!granularPageActive(1))
        return;

    sprintf(granularHeadsText, "%d", (int)editorInstrument->getGranularHeads());

    display.setControlText2(label[2], granularHeadsText);
    display.setControlShow(label[2]);
    display.refreshControl(label[2]);
}

void cSamplePlayback::showGranularSpreadValue()
{
    if (!granularPageActive(1))
        return;

    sprintf(granularSpreadText, "%d%%", (int)editorInstrument->getGranularSpread());

    display.setControlText2(label[3], granularSpreadText);
    display.setControlShow(label[3]);
    display.refreshControl(label[3]);
}

void cSamplePlayback::showGranularDirectionText()
{
    if (!granularPageActive(1))
        return;

    switch (editorInstrument->getGranularDirection())
    {
        case strInstrument::granularDirectionAlternate: strcpy(granularDirectionText, "Alternate"); break;
        case strInstrument::granularDirectionOpposite: strcpy(granularDirectionText, "Opposite"); break;
        default: strcpy(granularDirectionText, "Same"); break;
    }

    display.setControlText2(label[4], granularDirectionText);
    display.setControlShow(label[4]);
    display.refreshControl(label[4]);
}

void cSamplePlayback::showGranularRateText()
{
    if (!granularPageActive(2))
        return;

    // Travel speed of Heads 2 and 3 compared with Scan
    switch (editorInstrument->getGranularRate())
    {
        case strInstrument::granularRateHalfDouble: strcpy(granularRateText, ".5x  2x"); break;
        case strInstrument::granularRateFaster: strcpy(granularRateText, "2x  3x"); break;
        case strInstrument::granularRateSlower: strcpy(granularRateText, ".5x  .25x"); break;
        default: strcpy(granularRateText, "Same"); break;
    }

    display.setControlText2(label[1], granularRateText);
    display.setControlShow(label[1]);
    display.refreshControl(label[1]);
}

void cSamplePlayback::showGranularPitchText()
{
    if (!granularPageActive(2))
        return;

    // Interval of Heads 2 and 3 from the played note
    switch (editorInstrument->getGranularPitch())
    {
        case strInstrument::granularPitchDetune: strcpy(granularPitchText, "Detune"); break;
        case strInstrument::granularPitchOctaves: strcpy(granularPitchText, "-12  +12"); break;
        case strInstrument::granularPitchFifth: strcpy(granularPitchText, "+7  +12"); break;
        case strInstrument::granularPitchSub: strcpy(granularPitchText, "-12  -24"); break;
        case strInstrument::granularPitchUp: strcpy(granularPitchText, "+12  +24"); break;
        default: strcpy(granularPitchText, "Same"); break;
    }

    display.setControlText2(label[2], granularPitchText);
    display.setControlShow(label[2]);
    display.refreshControl(label[2]);
}

void cSamplePlayback::showGranularJitterValue()
{
    if (!granularPageActive(2))
        return;

    const int jitter = editorInstrument->getGranularJitter();
    if (jitter == 0)
        strcpy(granularJitterText, "Off");
    else
        sprintf(granularJitterText, "%d%%", jitter);

    display.setControlText2(label[3], granularJitterText);
    display.setControlShow(label[3]);
    display.refreshControl(label[3]);
}

void cSamplePlayback::showGranularDensityText()
{
    if (!granularPageActive(2))
        return;

    // Grains of each head playing at once, or tempo-synced grain timing
    switch (editorInstrument->getGranularDensity())
    {
        case strInstrument::granularDensity1: strcpy(granularDensityText, "1x"); break;
        case strInstrument::granularDensity2: strcpy(granularDensityText, "2x"); break;
        case strInstrument::granularDensity3: strcpy(granularDensityText, "3x"); break;
        case strInstrument::granularSync4: strcpy(granularDensityText, "Sync 1/4"); break;
        case strInstrument::granularSync8: strcpy(granularDensityText, "Sync 1/8"); break;
        case strInstrument::granularSync8T: strcpy(granularDensityText, "Sync 1/8T"); break;
        case strInstrument::granularSync16: strcpy(granularDensityText, "Sync 1/16"); break;
        case strInstrument::granularSync16T: strcpy(granularDensityText, "Sync 1/16T"); break;
        case strInstrument::granularSync32: strcpy(granularDensityText, "Sync 1/32"); break;
        default: strcpy(granularDensityText, "Auto"); break;
    }

    display.setControlText2(label[4], granularDensityText);
    display.setControlShow(label[4]);
    display.refreshControl(label[4]);
}

float cSamplePlayback::samplePosToTime(uint32_t pos)
{
    return pos / (44100.0 / (1 + editorInstrument->tuneResample / (-12)));
}
void cSamplePlayback::formatTimeSeconds(char* destBuffer, size_t destBufferSize, float value)
{
    int valueCharacters = destBufferSize - 2; // 's' and '/0'

    char* valueBuffer = new char[valueCharacters];

    snprintf(
      valueBuffer, valueCharacters, "%.3f", value); // keep constant number of characters for value

    snprintf(destBuffer, destBufferSize, "%ss", valueBuffer);

    delete[] valueBuffer;
}

//==============================================================================================================

// Granular: the Preview label shows the audio CPU peak since the last reading, so you can see
// how close Heads, Density and Sync are taking the Tracker to its limit (crackles near 100%).
void cSamplePlayback::showGranularCpuMeter()
{
    if (editorInstrument == nullptr || editorInstrument->playMode != playModeGranular)
        return;

    Profiler::AudioProcessorUsage usage;
    Profiler::getInstance()->getAudioProcessingUsagePercent(usage, true);

    snprintf(granularCpuText,
             sizeof(granularCpuText),
             usage.tempMaximum >= 90 ? "CPU %d%%!" : "CPU %d%%",
             usage.tempMaximum);

    display.setControlText2(label[0], granularCpuText);
    display.setControlShow(label[0]);
    display.refreshControl(label[0]);
}
