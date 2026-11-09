#ifndef INTERFACE_SAMPLEPLAYBACK_H_
#define INTERFACE_SAMPLEPLAYBACK_H_

#include "core/modulesBase.h"

#include "mtSequencer.h"

#include "mtStructs.h"

#undef MAX_SELECT_NODES
#define MAX_SELECT_NODES 4

const char playModeFunctLabels[playModeCount][15] = {
    "1-Shot", "Forward Loop", "Backward Loop", "Pingpong Loop",
    "Slice",  "Beat Slice",   "Wavetable",     "Granular",

};

constexpr uint8_t MAX_WAVETABLE_WINDOWS_COUNTER = 11;
constexpr uint8_t MIN_WAVETABLE_WINDOWS_COUNTER = 5;

class cSamplePlayback : public ModuleBase
{

  public:
    enum ParamsCC
    {
        kWavetablePosition_GranularPosition = 16,
        kWavetableWindow_GranularPositionLength = 17,
        kGranularShape = 18,
        kGranularLoop = 19
    };

    static constexpr int kNoTrack = -1;

    virtual void update();
    virtual void start(uint32_t options);
    virtual void stop();

    virtual void initDisplayControls();
    virtual void destroyDisplayControls() {}

    cSamplePlayback()
    {
        playModeListControl = -1;
        frameControl = -1;
        wtPositionCursor = -1;
        bgLabel = -1;

        editorInstrument = nullptr;

        progressCursor = -1;
        granularCursor = -1;
        granularHeadCursor[0] = -1;
        granularHeadCursor[1] = -1;
        granularHeadProgress[0] = -1;
        granularHeadProgress[1] = -1;
        spectrumControl[0] = -1;
        spectrumControl[1] = -1;
        pointsControl = -1;
        slicePointsControl = -1;
        label[8] = { -1 };
        // popupLabel = -1;
        lastSampleLength = 0;
    }
    virtual ~cSamplePlayback() {}

    void showDefaultScreen();
    void showZoomValue();
    void showPlayModeList();
    void showTitleBar();
    void hideUselessControls();
    void resizeWaveforms();

    void setDefaultScreenFunct();
    void cancelPopups();

    void processPoints();
    void processSlicePoints();
    void showPreviewValue();
    void hidePreviewValue();

    char startPointValueText[9];
    char endPointValueText[9];
    char loopPoint1ValueText[9];
    char loopPoint2ValueText[9];
    char playTimeValueText[9];
    char equalSliceNumberText[8];
    void showStartPointValue();
    void showEndPointValue();
    void showLoopPoint1Value();
    void showLoopPoint2Value();
    void showSlicesSelectValue();
    void showSlicesAdjustValue();
    void showSlicesEqualNumberValue();
    void showSlicesPlayThruValue();

    void onEndTrackDisplay();

    void noteOnHandle(uint8_t channel, uint8_t note, uint8_t velocity, int16_t source);
    void noteOffHandle(uint8_t channel, uint8_t note, uint8_t velocity, int16_t source);

    void lightUpPadBoard();

    void hideLoopPoints();
    void showLoopPoints();

    void showWavetablePositionCursor();
    void hideWavetablePositionCursor();

    void selectCorrectPoints();

    void refreshLiveParams();
    void refreshGranularAutomation();
    void refreshWavetableAutomation();
    void refreshPreviewTimeLabel();

    void calcPlayProgressValue();
    void activateLabelsBorder();

    void showAutoSlicePopup();
    void setAutoSlicePopupFunct();

    void modEqualSliceNumber(int16_t val);
    void modSlicePlayThru(int16_t val);
    bool slice2ndScreenActive();
    bool granular2ndScreenActive();

    void setDefault(uint8_t pos);
    void setDefaultStartPoint();
    void setDefaultLoopPoint1();
    void setDefaultLoopPoint2();
    void setDefaultEndPoint();
    void setDefaultZoom();
    void setDefaultSelectedSlice();
    void setDefaultEqualSliceNumber();
    void setDefaultWavetablePosition();
    void setDefaultWavetableWindow();
    void setDefaultGranularPosition();
    void setDefaultGranularLength();
    void setDefaultGranularShape();
    void setDefaultGranularLoop();

    void onControlChangeReceive(int param, int value);
    int getControlChangeDelta(ParamsCC param, int cc_value);
    void changeControlChangeParam(ParamsCC param, int delta);

    strFrameData frameData;

    strLabelData labelArrow;
    strLabelData labelDataMore;

    hControl label[8];
    hControl playModeListControl;
    hControl spectrumControl[2];
    hControl pointsControl;
    hControl slicePointsControl;
    hControl frameControl;
    hControl progressCursor;
    hControl granularCursor;
    hControl granularHeadCursor[2];   // granular Heads 2 and 3
    hControl granularHeadProgress[2]; // moving playheads of granular Heads 2 and 3
    hControl wtPositionCursor;
    hControl bgLabel;

    uint8_t selectedPlace = 6;

    //----------------------------------
    // spectrum + points

    enum enOverwriteSliceSource
    {
        overvwriteAutoSlice,
        overwriteEqualSlice
    };

    uint8_t refreshSpectrum = 0;
    uint8_t refreshPoints = 0;
    uint8_t refreshSlicePoints = 0;
    uint8_t currentAutoSlice = 0;
    uint8_t lastAutoSlice = 0;
    uint8_t overwriteSliceSource;

    uint8_t refreshSpectrumProgress = 0;
    uint32_t playProgressValue = 0;      // 0 - MAX_LEN_RECORD
    uint16_t playProgressInSpectrum = 0; // 0 - 600
    uint16_t granularHeadProgressInSpectrum[2] = { 0, 0 }; // Heads 2 and 3, 0 - 600
    uint8_t loopDirection = 0;
    elapsedMicros refreshPlayProgressValue = 0;
    float playPitch = 1.0;
    bool sliceScreenMode2;
    uint8_t granularScreenPage = 0; // granular page 0: Position..Loop, 1: Scan..Direction, 2: Rate, Pitch
    bool granularPageActive(uint8_t page) { return granular2ndScreenActive() && granularScreenPage == page; }
    int8_t displayedTrack = -1;

    strInstrument* editorInstrument = nullptr;
    uint32_t lastSampleLength = 0xFFFFFFFF;

    strZoomParams zoom;

    char zoomTextValue[7];

    char sliceSelectTextValue[11];
    char sliceAdjustTextValue[9];

    static strTrackerSpectrum spectrum[2];
    strTrackerPoints points;
    strSlicePoints slicePoints;

    wt_position_data_t wtPosition;

    strLineIndicatorSelection lineIndicatorSelection;
    strLineIndicatorSelection granularHeadSelection[2];
    uint32_t granularHeadColors[2][2];         // line, grain zone
    uint32_t granularHeadProgressColors[2][2]; // unused, playhead

    //----------------------------------
    // lista play mode
    strList playModeList;

    void listPlayMode();

    char* playModeNames[playModeCount];

    //----------------------------------
    // odtwarzanie
    uint8_t currentSeqState = 0;
    uint8_t lastSeqState = 0;
    uint8_t isPlayingSample = 0;
    uint8_t isPlayingWavetable = 0;
    uint32_t currentEnvelopeWtPos;
    uint32_t lastEnvelopeWtPos;
    uint32_t currentEnvelopeGranPos;
    uint32_t lastEnvelopeGranPos;
    uint32_t currentEnvelopeGranLen = 0;
    uint32_t lastEnvelopeGranLen = 0;
    int8_t playNote = 24;
    uint8_t glidePreviewDif = 0;
    uint8_t seqReleaseFlag = 0;
    uint8_t startNoteStoppedSeq = 0;

    //----------------------------------
    // multisel
    select_node_t selectNodes[MAX_SELECT_NODES];

    void addNode(editFunct_t funct, uint8_t nodeNum);
    void removeNode(uint8_t nodeNum);
    void stepThroughNodes(int16_t value);
    void clearAllNodes();
    void cancelMultiFrame();
    //--------------------------------------
    uint8_t loadedInstrumentType = 0;
    char wavetablePositionText[7];
    char wavetableWindowSizeText[7];
    void showWavetablePosition();
    void showWavetableWindowSize();
    uint16_t convertWavetableWindowsCounterToSize(uint8_t cnt);
    uint8_t convertWavetableWindowsSizeToCounter(uint16_t size);
    uint8_t wavetableWindowsCounter = 11;
    uint8_t refreshWavetablePosition;

    void processWavetableCursor(uint32_t position);

    char granularPositionTextValue[15];
    char grainLengthTextValue[15];
    char shapeText[15];
    char loopTypeText[15];
    char granularScanText[15];
    char granularHeadsText[15];
    char granularDirectionText[15];
    char granularRateText[15];
    char granularPitchText[15];
    elapsedMicros refreshGranularHeads = 0;
    char granularSpreadText[15];

    uint16_t granularPositionInSpectrum;
    float grainLengthMs;

    void showGranularPositionValue();
    void showGrainLengthValue();
    void showShapeText();
    void showLoopTypeText();
    void showGranularScanValue();
    void showGranularHeadsValue();
    void showGranularSpreadValue();
    void showGranularDirectionText();
    void showGranularRateText();
    void showGranularPitchText();
    void showGranularHeadCursors();
    void hideGranularHeadCursors();
    void setGranularCursorSelection(strLineIndicatorSelection& selection,
                                    uint16_t positionInSpectrum,
                                    uint32_t grainLength);

    float samplePosToTime(uint32_t pos);

    void formatTimeSeconds(char* destBuffer, size_t destBufferSize, float value);
};

extern cSamplePlayback samplePlayback;

#endif /* INTERFACE_SAMPLEPLAYBACK_H_ */
