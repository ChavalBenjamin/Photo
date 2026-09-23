#include "Photo.h"
#include "IPlug_include_in_plug_src.h"
#include "IControls.h"
#include <cmath>

// ----------------------------------------------------------------------
// Bouton "photo" minimal : appui = demarre la capture en direct,
// relachement = fige (gele) sur la derniere valeur captee. Pas un
// controle standard iPlug2 (aucun ne correspond a ce geste "maintenu"),
// ecrit a la main - premiere fois dans ce projet, a verifier a l'usage.
// ----------------------------------------------------------------------
class PhotoButtonControl : public IControl
{
public:
  PhotoButtonControl(const IRECT& bounds, SnapshotEngine* engine)
  : IControl(bounds), mEngine(engine) {}

  void Draw(IGraphics& g) override
  {
    IColor fill = mPressed ? IColor(255, 220, 80, 80) : IColor(255, 90, 90, 100);
    g.FillRoundRect(fill, mRECT, 8.f);
    g.DrawRoundRect(COLOR_WHITE, mRECT, 8.f);
    IText txt(14.f, COLOR_WHITE, "Roboto-Regular", EAlign::Center, EVAlign::Middle);
    g.DrawText(txt, mPressed ? "PHOTO..." : "Photo", mRECT);
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
  bool mPressed = false;
};

Photo::Photo(const InstanceInfo& info)
: iplug::Plugin(info, MakeConfig(kNumParams, 1))
{
  GetParam(kParamBaseShape)->InitEnum("Forme", 0, 4, "", IParam::kFlagsNone, "", "Sinus", "Saw", "Triangle", "Carre");
  GetParam(kParamSkew)->InitPercentage("Skew", 50.);

  for (int h = 0; h < WavetableEngine::kNumHarmonics; h++)
  {
    char name[16];
    snprintf(name, sizeof(name), "H%d", h + 2); // harmonique 2 a 17
    GetParam(kParamHarmonic1 + h)->InitPercentage(name, 0.);
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

    const IRECT bounds = pGraphics->GetBounds();

    // --- Forme de base, Skew, bouton photo ---
    IRECT topRow = bounds.GetFromTop(100.f).GetPadded(-10.f);
    mParamControls[kParamBaseShape] = new IVMenuButtonControl(topRow.GetGridCell(0, 0, 1, 3).GetCentredInside(160.f, 44.f), kParamBaseShape, "Forme de base");
    pGraphics->AttachControl(mParamControls[kParamBaseShape]);
    mParamControls[kParamSkew] = new IVKnobControl(topRow.GetGridCell(0, 1, 1, 3).GetCentredInside(64.f), kParamSkew, "Skew", knobStyle);
    pGraphics->AttachControl(mParamControls[kParamSkew]);

#if IPLUG_DSP
    pGraphics->AttachControl(new PhotoButtonControl(topRow.GetGridCell(0, 2, 1, 3).GetCentredInside(140.f, 50.f), &mSnapshot));
#endif

    // --- 16 harmoniques, en 2 rangees de 8 ---
    IRECT harmRow1 = IRECT(bounds.L, topRow.B, bounds.R, topRow.B + 100.f).GetPadded(-10.f);
    IRECT harmRow2 = IRECT(bounds.L, harmRow1.B, bounds.R, harmRow1.B + 100.f).GetPadded(-10.f);
    for (int h = 0; h < 8; h++)
    {
      mParamControls[kParamHarmonic1 + h] = new IVKnobControl(harmRow1.GetGridCell(0, h, 1, 8).GetCentredInside(64.f), kParamHarmonic1 + h, "", knobStyle);
      pGraphics->AttachControl(mParamControls[kParamHarmonic1 + h]);
    }
    for (int h = 8; h < 16; h++)
    {
      mParamControls[kParamHarmonic1 + h] = new IVKnobControl(harmRow2.GetGridCell(0, h - 8, 1, 8).GetCentredInside(64.f), kParamHarmonic1 + h, "", knobStyle);
      pGraphics->AttachControl(mParamControls[kParamHarmonic1 + h]);
    }

    // --- Visualisation de la table d'onde (dessin a cabler plus tard,
    // callback laisse vide pour l'instant - le composant l'accepte en
    // optionnel).
    IRECT waveArea = IRECT(bounds.L, harmRow2.B, bounds.R, bounds.B).GetPadded(-20.f);
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
  mSnapshot.Init(2048, GetSampleRate());
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

  mEngine.RebuildIfNeeded();

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
  // NOTE : premiere utilisation de l'API MIDI dans ce projet - IMidiMsg
  // lui-meme confirme fonctionnel, mais NoteNumberToFrequency n'existait
  // pas - remplace par le calcul standard directement (formule
  // universelle, aucune dependance a une methode iPlug2 specifique).
  switch (msg.StatusMsg())
  {
    case IMidiMsg::kNoteOn:
      if (msg.Velocity() > 0)
      {
        float freq = 440.f * std::pow(2.f, ((float)msg.NoteNumber() - 69.f) / 12.f);
        mTestOsc.NoteOn(freq);
      }
      else
        mTestOsc.NoteOff(); // certains claviers envoient NoteOn velocite 0 au lieu de NoteOff
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

  mSnapshot.Feed(bufMono, n);

  for (int i = 0; i < n; i++)
  {
    float sample = mTestOsc.Process(mEngine) + mSnapshot.Synthesize();
    outputs[0][i] = sample;
    outputs[1][i] = sample;
  }
}

#endif
