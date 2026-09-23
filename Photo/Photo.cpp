#include "Photo.h"
#include "IPlug_include_in_plug_src.h"
#include "IControls.h"
#include <cmath>

// ----------------------------------------------------------------------
// Bouton "photo" minimal : appui = demarre la capture en direct,
// relachement = fige (gele) sur la derniere valeur captee.
// ----------------------------------------------------------------------
class PhotoButtonControl : public IControl
{
public:
  PhotoButtonControl(const IRECT& bounds, SnapshotEngine* engine, int voiceNum)
  : IControl(bounds), mEngine(engine), mVoiceNum(voiceNum) {}

  void Draw(IGraphics& g) override
  {
    IColor fill = mPressed ? IColor(255, 220, 80, 80) : IColor(255, 90, 90, 100);
    g.FillRoundRect(fill, mRECT, 4.f);
    g.DrawRoundRect(COLOR_WHITE, mRECT, 4.f);
    char label[8];
    snprintf(label, sizeof(label), "%d", mVoiceNum);
    IText txt(11.f, COLOR_WHITE, "Roboto-Regular", EAlign::Center, EVAlign::Middle);
    g.DrawText(txt, label, mRECT);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    mPressed = true;
    if (mEngine) mEngine->StartCapture();
    SetDirty(false);
  }

  void OnMouseUp(float x, float y, const IMouseMod& mod) override
  {
    mPressed = false;
    if (mEngine) mEngine->StopCapture();
    SetDirty(false);
  }

private:
  SnapshotEngine* mEngine = nullptr;
  int mVoiceNum = 0;
  bool mPressed = false;
};

Photo::Photo(const InstanceInfo& info)
: iplug::Plugin(info, MakeConfig(kNumParams, 1))
{
  GetParam(kParamBaseShape)->InitEnum("Forme", 0, 4, "", IParam::kFlagsNone, "", "Sinus", "Saw", "Triangle", "Carre");
  GetParam(kParamSkew)->InitPercentage("Skew", 50.);
  GetParam(kParamFreqSmooth)->InitDouble("Lissage Freq", 15., 0., 50., 0.1, "ms");

  for (int h = 0; h < WavetableEngine::kNumHarmonics; h++)
  {
    char name[16];
    snprintf(name, sizeof(name), "H%d", h + 2); // harmonique 2 a 17
    GetParam(kParamHarmonic1 + h)->InitPercentage(name, 0.);
  }

  for (int v = 0; v < kNumVoices; v++)
  {
    char nameVol[24], nameOct[24], nameFine[24];
    snprintf(nameVol, sizeof(nameVol), "V%d Volume", v + 1);
    snprintf(nameOct, sizeof(nameOct), "V%d Octave", v + 1);
    snprintf(nameFine, sizeof(nameFine), "V%d Fine", v + 1);

    GetParam(VoiceParam(v, kVoiceVolume))->InitPercentage(nameVol, 100.);
    GetParam(VoiceParam(v, kVoiceOctave))->InitEnum(nameOct, 4, 9, "", IParam::kFlagsNone, "",
                                                      "/16", "/8", "/4", "/2", "1", "x2", "x4", "x8", "x16");
    GetParam(VoiceParam(v, kVoiceFine))->InitDouble(nameFine, 0., -20., 20., 0.1, "Hz");
  }

#if IPLUG_EDITOR
  mMakeGraphicsFunc = [&]() {
    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS,
                         GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT));
  };

  mLayoutFunc = [&](IGraphics* pGraphics) {
    pGraphics->AttachCornerResizer(EUIResizerMode::Scale, false);
    pGraphics->AttachPanelBackground(COLOR_GRAY);
    pGraphics->LoadFont("Roboto-Regular", ROBOTO_FN);

    const IVStyle knobStyle = DEFAULT_STYLE.WithLabelText(IText(10.f, COLOR_WHITE));
    const IVStyle tinyStyle = DEFAULT_STYLE.WithLabelText(IText(8.f, COLOR_WHITE));

    const IRECT bounds = pGraphics->GetBounds();

    // --- Forme de base, Skew, Lissage Freq ---
    IRECT topRow = bounds.GetFromTop(100.f).GetPadded(-10.f);
    mParamControls[kParamBaseShape] = new IVMenuButtonControl(topRow.GetGridCell(0, 0, 1, 3).GetCentredInside(160.f, 44.f), kParamBaseShape, "Forme de base");
    pGraphics->AttachControl(mParamControls[kParamBaseShape]);
    mParamControls[kParamSkew] = new IVKnobControl(topRow.GetGridCell(0, 1, 1, 3).GetCentredInside(64.f), kParamSkew, "Skew", knobStyle);
    pGraphics->AttachControl(mParamControls[kParamSkew]);
    mParamControls[kParamFreqSmooth] = new IVKnobControl(topRow.GetGridCell(0, 2, 1, 3).GetCentredInside(64.f), kParamFreqSmooth, "Lissage Freq", knobStyle);
    pGraphics->AttachControl(mParamControls[kParamFreqSmooth]);

    // --- 16 harmoniques, en 2 rangees de 8 ---
    IRECT harmRow1 = IRECT(bounds.L, topRow.B, bounds.R, topRow.B + 90.f).GetPadded(-10.f);
    IRECT harmRow2 = IRECT(bounds.L, harmRow1.B, bounds.R, harmRow1.B + 90.f).GetPadded(-10.f);
    for (int h = 0; h < 8; h++)
    {
      mParamControls[kParamHarmonic1 + h] = new IVKnobControl(harmRow1.GetGridCell(0, h, 1, 8).GetCentredInside(56.f), kParamHarmonic1 + h, "", knobStyle);
      pGraphics->AttachControl(mParamControls[kParamHarmonic1 + h]);
    }
    for (int h = 8; h < 16; h++)
    {
      mParamControls[kParamHarmonic1 + h] = new IVKnobControl(harmRow2.GetGridCell(0, h - 8, 1, 8).GetCentredInside(56.f), kParamHarmonic1 + h, "", knobStyle);
      pGraphics->AttachControl(mParamControls[kParamHarmonic1 + h]);
    }

    // --- 12 voix "photo", en colonnes : bouton / volume / octave / fine ---
    IRECT voicesRow = IRECT(bounds.L, harmRow2.B, bounds.R, harmRow2.B + 190.f).GetPadded(-10.f);
    for (int v = 0; v < kNumVoices; v++)
    {
      IRECT col = voicesRow.GetGridCell(0, v, 1, kNumVoices);
      IRECT btnArea = col.GetFromTop(36.f);
      IRECT volArea = IRECT(col.L, btnArea.B, col.R, btnArea.B + 50.f);
      IRECT octArea = IRECT(col.L, volArea.B, col.R, volArea.B + 26.f);
      IRECT fineArea = IRECT(col.L, octArea.B, col.R, octArea.B + 50.f);

#if IPLUG_DSP
      pGraphics->AttachControl(new PhotoButtonControl(btnArea.GetPadded(-3.f), &mSnapshots[v], v + 1));
#endif
      mParamControls[VoiceParam(v, kVoiceVolume)] = new IVKnobControl(volArea.GetCentredInside(44.f), VoiceParam(v, kVoiceVolume), "Vol", tinyStyle);
      pGraphics->AttachControl(mParamControls[VoiceParam(v, kVoiceVolume)]);
      mParamControls[VoiceParam(v, kVoiceOctave)] = new IVMenuButtonControl(octArea.GetPadded(-2.f), VoiceParam(v, kVoiceOctave), "");
      pGraphics->AttachControl(mParamControls[VoiceParam(v, kVoiceOctave)]);
      mParamControls[VoiceParam(v, kVoiceFine)] = new IVKnobControl(fineArea.GetCentredInside(44.f), VoiceParam(v, kVoiceFine), "Fine", tinyStyle);
      pGraphics->AttachControl(mParamControls[VoiceParam(v, kVoiceFine)]);
    }

    // --- Visualisation de la table d'onde (dessin a cabler plus tard) ---
    IRECT waveArea = IRECT(bounds.L, voicesRow.B, bounds.R, bounds.B).GetPadded(-20.f);
    mWaveView = new SpectralCurvePreviewControl(waveArea);
    pGraphics->AttachControl(mWaveView);
  };
#endif

#if IPLUG_DSP
  OnReset();
#endif
}

void Photo::OnIdle()
{
#if IPLUG_DSP
  if (mWaveView && mWaveUIUpdated.exchange(false))
  {
    mWaveView->SetCurve(mWaveUIBuf, mWaveUISize);
    mWaveView->SetDirty(false);
  }
#endif
}

void Photo::SyncUIToState()
{
  for (int i = 0; i < kNumParams; i++)
    if (mParamControls[i])
      mParamControls[i]->SetValueFromDelegate(GetParam(i)->GetNormalized());
}

void Photo::ApplyAllState()
{
#if IPLUG_DSP
  UpdateEngine();
  mTestOsc.SetSampleRate(GetSampleRate());
  for (int v = 0; v < kNumVoices; v++)
    mSnapshots[v].Init(2048, GetSampleRate());
#endif
}

#if IPLUG_DSP

void Photo::UpdateEngine()
{
  int shapeIdx = (int)GetParam(kParamBaseShape)->Value();
  mEngine.SetBaseShape((WavetableEngine::BaseShape)shapeIdx);
  mEngine.SetSkew((float)(GetParam(kParamSkew)->Value() / 100.0));

  for (int h = 0; h < WavetableEngine::kNumHarmonics; h++)
  {
    float amp = (float)(GetParam(kParamHarmonic1 + h)->Value() / 100.0);
    mEngine.SetHarmonicAmp(h, amp);
  }

  {
    std::lock_guard<std::mutex> lock(mEngineMutex);
    mEngine.RebuildIfNeeded();
  }

  const float* table = mEngine.GetTable();
  int size = mEngine.GetTableSize();
  int uiSize = std::min(size, WavetableEngine::kTableSize);
  for (int i = 0; i < uiSize; i++)
    mWaveUIBuf[i] = table[i];
  mWaveUISize = uiSize;
  mWaveUIUpdated.store(true);
}

void Photo::OnReset()
{
  ApplyAllState();
}

void Photo::OnParamChange(int paramIdx)
{
  UpdateEngine();
}

void Photo::ProcessMidiMsg(const IMidiMsg& msg)
{
  switch (msg.StatusMsg())
  {
    case IMidiMsg::kNoteOn:
      if (msg.Velocity() > 0)
      {
        float freq = 440.f * std::pow(2.f, ((float)msg.NoteNumber() - 69.f) / 12.f);
        mTestOsc.NoteOn(freq);
      }
      else
        mTestOsc.NoteOff();
      break;
    case IMidiMsg::kNoteOff:
      mTestOsc.NoteOff();
      break;
    default:
      break;
  }
}

void Photo::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  static float bufMono[8192];
  int n = std::min(nFrames, 8192);
  for (int i = 0; i < n; i++)
    bufMono[i] = (float)inputs[0][i];

  for (int v = 0; v < kNumVoices; v++)
  {
    float volume = (float)(GetParam(VoiceParam(v, kVoiceVolume))->Value() / 100.0);
    int octaveIdx = (int)GetParam(VoiceParam(v, kVoiceOctave))->Value(); // 0-8, 4 = neutre
    float fineHz = (float)GetParam(VoiceParam(v, kVoiceFine))->Value();

    mSnapshots[v].SetVolume(volume);
    mSnapshots[v].SetOctaveShift(octaveIdx - 4);
    mSnapshots[v].SetFineTuneHz(fineHz);
    mSnapshots[v].Feed(bufMono, n);
  }

  {
    std::lock_guard<std::mutex> lock(mEngineMutex);
    for (int i = 0; i < n; i++)
    {
      float sample = mTestOsc.Process(mEngine);
      for (int v = 0; v < kNumVoices; v++)
        sample += mSnapshots[v].Synthesize(mEngine);

      outputs[0][i] = sample;
      outputs[1][i] = sample;
    }
  }
}

#endif
