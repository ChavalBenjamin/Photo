#include "Photo.h"
#include "IPlug_include_in_plug_src.h"
#include "IControls.h"
#include <cmath>

Photo::Photo(const InstanceInfo& info)
: iplug::Plugin(info, MakeConfig(kNumParams, 1))
{
  GetParam(kParamBaseShape)->InitEnum("Forme", 0, 4, "", IParam::kFlagsNone, "", "Sinus", "Saw", "Triangle", "Carre");

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

    // --- Forme de base ---
    IRECT topRow = bounds.GetFromTop(70.f).GetPadded(-10.f);
    mParamControls[kParamBaseShape] = new IVMenuButtonControl(topRow.GetGridCell(0, 0, 1, 1).GetCentredInside(160.f, 44.f), kParamBaseShape, "Forme de base");
    pGraphics->AttachControl(mParamControls[kParamBaseShape]);

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
#endif
}

#if IPLUG_DSP

void Photo::UpdateEngine()
{
  int shapeIdx = (int)GetParam(kParamBaseShape)->Value();
  mEngine.SetBaseShape((WavetableEngine::BaseShape)shapeIdx);

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
  // et IMidiMsg::NoteNumberToFrequency n'ont jamais ete verifies dans
  // aucun des projets precedents, a confirmer a la compilation.
  switch (msg.StatusMsg())
  {
    case IMidiMsg::kNoteOn:
      if (msg.Velocity() > 0)
        mTestOsc.NoteOn((float)IMidiMsg::NoteNumberToFrequency(msg.NoteNumber()));
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
  for (int i = 0; i < nFrames; i++)
  {
    float sample = mTestOsc.Process(mEngine);
    outputs[0][i] = sample;
    outputs[1][i] = sample;
  }
}

#endif
