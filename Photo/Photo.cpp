#include "Photo.h"
#include "IPlug_include_in_plug_src.h"
#include "IControls.h"
#include <cmath>
#include <functional>

// ----------------------------------------------------------------------
// Mode Relatif : gros bouton "PHOTO" (maintenu = analyse en direct, relache
// = la derniere analyse est gardee en memoire) et bouton "CORBEILLE" (vide
// la memoire). Ce sont de VRAIS parametres : REAPER les voit et peut leur
// assigner un CC, une note ou une automation.
// ----------------------------------------------------------------------
class MomentaryParamButton : public IControl
{
public:
  MomentaryParamButton(const IRECT& bounds, int paramIdx) : IControl(bounds, paramIdx) {}

  void OnMouseDown(float x, float y, const IMouseMod& mod) override { SetValue(1.); SetDirty(true); }
  void OnMouseUp(float x, float y, const IMouseMod& mod) override { SetValue(0.); SetDirty(true); }

protected:
  bool Pressed() const { return GetValue() > 0.5; }
};

// Bouton de capture d'une voix (mode Boutons) : maintenu = analyse en direct,
// relache = gel. Lie au parametre "Capture N".
class CaptureButtonControl : public MomentaryParamButton
{
public:
  CaptureButtonControl(const IRECT& bounds, int paramIdx, int voiceNum)
  : MomentaryParamButton(bounds, paramIdx), mVoiceNum(voiceNum) {}

  void Draw(IGraphics& g) override
  {
    IColor fill = Pressed() ? IColor(255, 220, 80, 80) : IColor(255, 90, 90, 100);
    g.FillRoundRect(fill, mRECT, 4.f);
    g.DrawRoundRect(COLOR_WHITE, mRECT, 4.f);
    char label[8];
    snprintf(label, sizeof(label), "%d", mVoiceNum);
    IText txt(11.f, COLOR_WHITE, "Roboto-Regular", EAlign::Center, EVAlign::Middle);
    g.DrawText(txt, label, mRECT);
  }

private:
  int mVoiceNum;
};

class PhotoHoldControl : public MomentaryParamButton
{
public:
  PhotoHoldControl(const IRECT& bounds, int paramIdx, const std::atomic<bool>* memFull)
  : MomentaryParamButton(bounds, paramIdx), mMemFull(memFull) {}

  void Draw(IGraphics& g) override
  {
    const bool full = mMemFull && mMemFull->load();
    IColor fill = Pressed() ? IColor(255, 235, 70, 60)          // maintenu : rouge vif
                : full      ? IColor(255, 232, 180, 90)        // memoire pleine : ambre
                            : IColor(255, 70, 70, 80);         // memoire vide : gris
    g.FillRoundRect(fill, mRECT, 10.f);
    g.DrawRoundRect(COLOR_WHITE, mRECT, 10.f, nullptr, 2.f);
    IText big(26.f, Pressed() || full ? COLOR_BLACK : COLOR_WHITE, "Roboto-Regular", EAlign::Center, EVAlign::Middle);
    g.DrawText(big, "PHOTO", mRECT.GetFromTop(mRECT.H() * 0.65f));
    IText small(12.f, Pressed() || full ? COLOR_BLACK : COLOR_LIGHT_GRAY, "Roboto-Regular", EAlign::Center, EVAlign::Middle);
    g.DrawText(small, Pressed() ? "analyse en direct" : (full ? "en memoire" : "memoire vide"), mRECT.GetFromBottom(mRECT.H() * 0.35f));
  }

private:
  const std::atomic<bool>* mMemFull;
};

class TrashControl : public MomentaryParamButton
{
public:
  TrashControl(const IRECT& bounds, int paramIdx) : MomentaryParamButton(bounds, paramIdx) {}

  void Draw(IGraphics& g) override
  {
    IColor fill = Pressed() ? IColor(255, 200, 80, 80) : IColor(255, 90, 60, 60);
    g.FillRoundRect(fill, mRECT, 8.f);
    g.DrawRoundRect(COLOR_WHITE, mRECT, 8.f);
    IText txt(14.f, COLOR_WHITE, "Roboto-Regular", EAlign::Center, EVAlign::Middle);
    g.DrawText(txt, "CORBEILLE", mRECT);
  }
};

// Bouton d'onglet (haut a droite) : bascule entre l'ecran principal et le
// panneau Temperament.
class TabButtonControl : public IControl
{
public:
  TabButtonControl(const IRECT& bounds, std::function<void()> onClick) : IControl(bounds), mOnClick(onClick) {}

  void Draw(IGraphics& g) override
  {
    g.FillRoundRect(mOpen ? IColor(255, 232, 140, 40) : IColor(255, 70, 70, 85), mRECT, 4.f);
    g.DrawRoundRect(COLOR_WHITE, mRECT, 4.f);
    IText txt(11.f, mOpen ? COLOR_BLACK : COLOR_WHITE, "Roboto-Regular", EAlign::Center, EVAlign::Middle);
    g.DrawText(txt, mOpen ? "< RETOUR" : "TEMPERAMENT >", mRECT);
  }
  void OnMouseDown(float x, float y, const IMouseMod& mod) override { if (mOnClick) mOnClick(); }
  void SetOpen(bool open) { mOpen = open; SetDirty(false); }

private:
  std::function<void()> mOnClick;
  bool mOpen = false;
};

Photo::Photo(const InstanceInfo& info)
: iplug::Plugin(info, MakeConfig(kNumParams, 1))
{
  GetParam(kParamBaseShape)->InitEnum("Forme", 0, 4, "", IParam::kFlagsNone, "", "Sinus", "Saw", "Triangle", "Carre");
  GetParam(kParamSkew)->InitPercentage("Skew", 50.);
  // Obturateur : cadence des micro-photos tant qu'un bouton de capture est
  // maintenu. 0 = a chaque analyse FFT (~11,6 ms) ; jusqu'a 2 s. Courbe
  // exponentielle : plus de finesse du cote rapide.
  GetParam(kParamFreqSmooth)->InitDouble("Obturateur", 0., 0., 2000., 0.1, "ms", IParam::kFlagsNone, "", IParam::ShapePowCurve(3.));

  for (int h = 0; h < WavetableEngine::kNumHarmonics; h++)
  {
    char name[16];
    snprintf(name, sizeof(name), "H%d", h + 2); // harmonique 2 a 17
    // Courbe exponentielle (puissance 3) : beaucoup de precision au debut de course.
    GetParam(kParamHarmonic1 + h)->InitDouble(name, 0., 0., 100., 0.01, "%", IParam::kFlagsNone, "", IParam::ShapePowCurve(3.));
  }

  GetParam(kParamStereoHz)->InitDouble("Ecart D/G", 0., -10., 10., 0.01, "Hz");

  GetParam(kParamMode)->InitEnum("Mode", 0, 4, "", IParam::kFlagsNone, "", "Boutons", "Clavier", "Synthe", "Relatif");
  GetParam(kParamAttack)->InitDouble("Attaque", 5., 0., 2000., 0.1, "ms");
  GetParam(kParamDecay)->InitDouble("Decay", 200., 1., 5000., 0.1, "ms");
  GetParam(kParamSustain)->InitPercentage("Sustain", 80.);
  GetParam(kParamRelease)->InitDouble("Release", 300., 1., 8000., 0.1, "ms");
  GetParam(kParamVelSens)->InitPercentage("Velocite", 100.);
  GetParam(kParamKbVolume)->InitPercentage("Clavier Volume", 100.);
  GetParam(kParamKbOctave)->InitEnum("Clavier Octave", tuning::kPitchNeutralIdx, 15, "", IParam::kFlagsNone, "",
                                      "/16", "/8", "/4", "/3", "/2", "1",
                                      "x1.125", "x1.2", "x1.25", "x1.5", "x2", "x3", "x4", "x8", "x16");
  GetParam(kParamKbFine)->InitDouble("Clavier Fine", 0., -20., 20., 0.1, "Hz");
  GetParam(kParamPhotoHold)->InitBool("Photo", false);
  GetParam(kParamTrash)->InitBool("Corbeille", false);
  for (int v = 0; v < kNumVoices; v++)
  {
    char name[24];
    snprintf(name, sizeof(name), "Capture %d", v + 1);
    GetParam(CaptureParam(v))->InitBool(name, false);
  }

  GetParam(kParamTuneRoot)->InitEnum("Tune Racine", 0, 12, "", IParam::kFlagsNone, "",
    "Do", "Do#", "Re", "Mib", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "Sib", "Si");
  for (int i = 0; i < tuning::kNumPitchClasses; i++)
  {
    char name[24];
    snprintf(name, sizeof(name), "Tune Cents %d", i);
    GetParam(kParamTuneStart + i)->InitDouble(name, 0., -200., 200., 0.01, "ct");
  }
  GetParam(kParamTuneStretch)->InitDouble("Tune Octave", 0., -100., 100., 0.01, "ct");
  GetParam(kParamTuneGlobal)->InitDouble("Tune Hauteur", 0., -100., 100., 0.01, "ct");

  for (int v = 0; v < kNumVoices; v++)
  {
    char nameVol[24], nameOct[24], nameFine[24];
    snprintf(nameVol, sizeof(nameVol), "V%d Volume", v + 1);
    snprintf(nameOct, sizeof(nameOct), "V%d Octave", v + 1);
    snprintf(nameFine, sizeof(nameFine), "V%d Fine", v + 1);

    GetParam(VoiceParam(v, kVoiceVolume))->InitPercentage(nameVol, 100.);
    GetParam(VoiceParam(v, kVoiceOctave))->InitEnum(nameOct, tuning::kPitchNeutralIdx, 15, "", IParam::kFlagsNone, "",
                                                      "/16", "/8", "/4", "/3", "/2", "1",
                                      "x1.125", "x1.2", "x1.25", "x1.5", "x2", "x3", "x4", "x8", "x16");
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

    mMainControls.clear();
    mTuneControls.clear();
    mTuningOpen = false;
    auto attachMain = [&](IControl* ctl) { pGraphics->AttachControl(ctl); mMainControls.push_back(ctl); };
    auto attachTune = [&](IControl* ctl) { pGraphics->AttachControl(ctl); ctl->Hide(true); mTuneControls.push_back(ctl); };

    // --- Forme de base, Skew, Lissage Freq ---
    IRECT topRow = bounds.GetFromTop(100.f).GetPadded(-10.f);
    mParamControls[kParamBaseShape] = new IVMenuButtonControl(topRow.GetGridCell(0, 0, 1, 4).GetCentredInside(160.f, 44.f), kParamBaseShape, "Forme de base");
    attachMain(mParamControls[kParamBaseShape]);
    mParamControls[kParamSkew] = new IVKnobControl(topRow.GetGridCell(0, 1, 1, 4).GetCentredInside(64.f), kParamSkew, "Skew", knobStyle);
    attachMain(mParamControls[kParamSkew]);
    mParamControls[kParamFreqSmooth] = new IVKnobControl(topRow.GetGridCell(0, 2, 1, 4).GetCentredInside(64.f), kParamFreqSmooth, "Obturateur", knobStyle);
    attachMain(mParamControls[kParamFreqSmooth]);
    mParamControls[kParamStereoHz] = new IVKnobControl(topRow.GetGridCell(0, 3, 1, 4).GetCentredInside(64.f), kParamStereoHz, "Ecart D/G", knobStyle);
    attachMain(mParamControls[kParamStereoHz]);

    // --- 16 harmoniques, en 2 rangees de 8 ---
    IRECT harmRow1 = IRECT(bounds.L, topRow.B, bounds.R, topRow.B + 90.f).GetPadded(-10.f);
    IRECT harmRow2 = IRECT(bounds.L, harmRow1.B, bounds.R, harmRow1.B + 90.f).GetPadded(-10.f);
    for (int h = 0; h < 8; h++)
    {
      mParamControls[kParamHarmonic1 + h] = new IVKnobControl(harmRow1.GetGridCell(0, h, 1, 8).GetCentredInside(56.f), kParamHarmonic1 + h, "", knobStyle);
      attachMain(mParamControls[kParamHarmonic1 + h]);
    }
    for (int h = 8; h < 16; h++)
    {
      mParamControls[kParamHarmonic1 + h] = new IVKnobControl(harmRow2.GetGridCell(0, h - 8, 1, 8).GetCentredInside(56.f), kParamHarmonic1 + h, "", knobStyle);
      attachMain(mParamControls[kParamHarmonic1 + h]);
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

      mParamControls[CaptureParam(v)] = new CaptureButtonControl(btnArea.GetPadded(-3.f), CaptureParam(v), v + 1);
      attachMain(mParamControls[CaptureParam(v)]);
      mParamControls[VoiceParam(v, kVoiceVolume)] = new IVKnobControl(volArea.GetCentredInside(44.f), VoiceParam(v, kVoiceVolume), "Vol", tinyStyle);
      attachMain(mParamControls[VoiceParam(v, kVoiceVolume)]);
      mParamControls[VoiceParam(v, kVoiceOctave)] = new IVMenuButtonControl(octArea.GetPadded(-2.f), VoiceParam(v, kVoiceOctave), "");
      attachMain(mParamControls[VoiceParam(v, kVoiceOctave)]);
      mParamControls[VoiceParam(v, kVoiceFine)] = new IVKnobControl(fineArea.GetCentredInside(44.f), VoiceParam(v, kVoiceFine), "Fine", tinyStyle);
      attachMain(mParamControls[VoiceParam(v, kVoiceFine)]);
    }

    // --- Mode Clavier : Mode / ADSR / Velocite / Volume / Octave / Fine ---
    IRECT kbRow = IRECT(bounds.L, voicesRow.B, bounds.R, voicesRow.B + 100.f).GetPadded(-10.f);
    mParamControls[kParamMode] = new IVMenuButtonControl(kbRow.GetGridCell(0, 0, 1, 9).GetCentredInside(90.f, 34.f), kParamMode, "Mode");
    attachMain(mParamControls[kParamMode]);
    const int kbKnobs[] = { kParamAttack, kParamDecay, kParamSustain, kParamRelease, kParamVelSens, kParamKbVolume };
    const char* kbNames[] = { "Attaque", "Decay", "Sustain", "Release", "Velocite", "Volume" };
    for (int i = 0; i < 6; i++)
    {
      mParamControls[kbKnobs[i]] = new IVKnobControl(kbRow.GetGridCell(0, 1 + i, 1, 9).GetCentredInside(60.f), kbKnobs[i], kbNames[i], knobStyle);
      attachMain(mParamControls[kbKnobs[i]]);
    }
    mParamControls[kParamKbOctave] = new IVMenuButtonControl(kbRow.GetGridCell(0, 7, 1, 9).GetCentredInside(70.f, 34.f), kParamKbOctave, "Octave");
    attachMain(mParamControls[kParamKbOctave]);
    mParamControls[kParamKbFine] = new IVKnobControl(kbRow.GetGridCell(0, 8, 1, 9).GetCentredInside(60.f), kParamKbFine, "Fine", knobStyle);
    attachMain(mParamControls[kParamKbFine]);

    // --- Visualisation de la table d'onde (dessin a cabler plus tard) ---
    IRECT restArea = IRECT(bounds.L, kbRow.B, bounds.R, bounds.B).GetPadded(-20.f);
    IRECT btnArea = restArea.GetFromRight(230.f);
    IRECT waveArea = restArea.GetReducedFromRight(240.f);

    // --- Mode Relatif : gros bouton Photo + Corbeille ---
    const std::atomic<bool>* memFlag = nullptr;
#if IPLUG_DSP
    memFlag = &mMemValid;
#endif
    mPhotoBtn = new PhotoHoldControl(btnArea.GetFromTop(btnArea.H() * 0.68f).GetPadded(-4.f), kParamPhotoHold, memFlag);
    mParamControls[kParamPhotoHold] = mPhotoBtn;
    attachMain(mPhotoBtn);
    mParamControls[kParamTrash] = new TrashControl(btnArea.GetFromBottom(btnArea.H() * 0.32f).GetPadded(-4.f), kParamTrash);
    attachMain(mParamControls[kParamTrash]);
    mWaveView = new SpectralCurvePreviewControl(waveArea);
    attachMain(mWaveView);

    // --- Onglet Temperament (cache au depart) ---
    mTabBtn = new TabButtonControl(IRECT(bounds.R - 140.f, bounds.T + 2.f, bounds.R - 10.f, bounds.T + 18.f), [this]() { SetTuningView(!mTuningOpen); });
    pGraphics->AttachControl(mTabBtn);

    const IRECT tb = bounds.GetPadded(-20.f);
    attachTune(new ITextControl(IRECT(tb.L, tb.T + 4.f, tb.R - 150.f, tb.T + 36.f), "TEMPERAMENT  (La = 440 Hz fixe)", IText(20.f, COLOR_WHITE, "Roboto-Regular", EAlign::Near, EVAlign::Middle)));
    attachTune(new IVButtonControl(IRECT(tb.L, tb.T + 50.f, tb.L + 150.f, tb.T + 84.f), [this](IControl*) { ApplyTuningPreset(0); }, "Egal", knobStyle));
    attachTune(new IVButtonControl(IRECT(tb.L + 165.f, tb.T + 50.f, tb.L + 365.f, tb.T + 84.f), [this](IControl*) { ApplyTuningPreset(1); }, "La Monte Young", knobStyle));

    attachTune(new IVMenuButtonControl(IRECT(tb.L + 380.f, tb.T + 50.f, tb.L + 520.f, tb.T + 84.f), kParamTuneRoot, "Racine", knobStyle));
    mParamControls[kParamTuneRoot] = mTuneControls.back();

    attachTune(new ITextControl(IRECT(tb.L, tb.T + 110.f, tb.R, tb.T + 134.f), "Correction en cents, par degre a partir de la racine (R = racine, +1 = un demi-ton au-dessus...)", IText(14.f, COLOR_LIGHT_GRAY, "Roboto-Regular", EAlign::Near, EVAlign::Middle)));
    const IRECT noteRow = IRECT(tb.L, tb.T + 136.f, tb.R, tb.T + 250.f);
    for (int i = 0; i < tuning::kNumPitchClasses; i++)
    {
      char lbl[8];
      if (i == 0) snprintf(lbl, sizeof(lbl), "R"); else snprintf(lbl, sizeof(lbl), "+%d", i);
      mParamControls[kParamTuneStart + i] = new IVKnobControl(noteRow.GetGridCell(0, i, 1, 12).GetCentredInside(68.f), kParamTuneStart + i, lbl, knobStyle, true);
      attachTune(mParamControls[kParamTuneStart + i]);
    }

    attachTune(new ITextControl(IRECT(tb.L, tb.T + 280.f, tb.R, tb.T + 304.f), "Octave : cents par octave, cumulatif, pivot Do central.   Hauteur : decalage global en cents (mode Synthe).", IText(14.f, COLOR_LIGHT_GRAY, "Roboto-Regular", EAlign::Near, EVAlign::Middle)));
    mParamControls[kParamTuneStretch] = new IVKnobControl(IRECT(tb.L, tb.T + 306.f, tb.L + 120.f, tb.T + 420.f).GetCentredInside(68.f), kParamTuneStretch, "Octave", knobStyle, true);
    attachTune(mParamControls[kParamTuneStretch]);
    mParamControls[kParamTuneGlobal] = new IVKnobControl(IRECT(tb.L + 140.f, tb.T + 306.f, tb.L + 260.f, tb.T + 420.f).GetCentredInside(68.f), kParamTuneGlobal, "Hauteur", knobStyle, true);
    attachTune(mParamControls[kParamTuneGlobal]);

    attachTune(new ITextControl(IRECT(tb.L, tb.T + 450.f, tb.R, tb.T + 520.f),
      "Clic sur la valeur sous un potard : taper la valeur. Double-clic sur un potard : remise a 0. Racine : mode Synthe seulement (en Relatif l echelle part toujours du Do central).",
      IText(12.f, COLOR_LIGHT_GRAY, "Roboto-Regular", EAlign::Near, EVAlign::Top)));
  };
#endif

#if IPLUG_DSP
  OnReset();
#endif
}

void Photo::OnIdle()
{
#if IPLUG_DSP
  // Si l'audio ne tourne pas (hote arrete), la table est quand meme reconstruite.
  if (mEngineDirty.load() && mBuildMutex.try_lock())
  {
    mBuildMutex.unlock();
    if (mEngineDirty.exchange(false)) UpdateEngine();
  }
  if (mPhotoBtn && mMemValid.load() != mMemShown)
  {
    mMemShown = mMemValid.load();
    mPhotoBtn->SetDirty(false);
  }

  if (mWaveView && mWaveUIUpdated.exchange(false))
  {
    mWaveView->SetCurve(mWaveUIBuf, mWaveUISize);
    mWaveView->SetDirty(false);
  }
#endif
}

void Photo::SetTuningView(bool open)
{
  mTuningOpen = open;
  for (auto* ctl : mMainControls) ctl->Hide(open);
  for (auto* ctl : mTuneControls) ctl->Hide(!open);
  if (mTabBtn) static_cast<TabButtonControl*>(mTabBtn)->SetOpen(open);
  if (GetUI()) GetUI()->SetAllControlsDirty();
}

// Preset : 0 = tempérament égal (tout a 0), 1 = La Monte Young.
// Passe par les controles (comme un geste de l'utilisateur) pour que l'hote
// enregistre le changement et que les potards se mettent a jour.
void Photo::ApplyTuningPreset(int preset)
{
  double dev[tuning::kNumPitchClasses] = { 0. };
  if (preset == 1) tuning::YoungDeviations(dev);

  auto setParam = [&](int idx, double value) {
    IParam* p = GetParam(idx);
    const double norm = p->ToNormalized(value);
    BeginInformHostOfParamChangeFromUI(idx);
    if (mParamControls[idx])
      mParamControls[idx]->SetValueFromUserInput(norm);
    else
    {
      p->SetNormalized(norm);
      InformHostOfParamChange(idx, norm);
    }
    EndInformHostOfParamChangeFromUI(idx);
  };
  for (int i = 0; i < tuning::kNumPitchClasses; i++) setParam(kParamTuneStart + i, dev[i]);
  setParam(kParamTuneStretch, 0.);
  setParam(kParamTuneGlobal, preset == 1 ? tuning::kYoungGlobal : 0.);
  setParam(kParamTuneRoot, preset == 1 ? tuning::kYoungRoot : 0);
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
  {
    std::lock_guard<std::mutex> lk(mPendingMutex);
    mPendingImmediate = true; // table initiale : pas de fondu
  }
  UpdateEngine();
  mAnalyzer.Init(2048, GetSampleRate());
  for (int v = 0; v < kNumVoices; v++)
    mSnapshots[v].Init(GetSampleRate());
#endif
}

#if IPLUG_DSP

void Photo::UpdateEngine()
{
  std::lock_guard<std::mutex> build(mBuildMutex);

  int shapeIdx = (int)GetParam(kParamBaseShape)->Value();
  mBuilder.SetBaseShape((WavetableEngine::BaseShape)shapeIdx);
  mBuilder.SetSkew((float)(GetParam(kParamSkew)->Value() / 100.0));

  for (int h = 0; h < WavetableEngine::kNumHarmonics; h++)
  {
    float amp = (float)(GetParam(kParamHarmonic1 + h)->Value() / 100.0);
    mBuilder.SetHarmonicAmp(h, amp);
  }

  mBuilder.RebuildIfNeeded();

  const float* table = mBuilder.GetTable();
  int size = mBuilder.GetTableSize();

  // Publication pour le thread audio (copie breve), qui fera le fondu.
  {
    std::lock_guard<std::mutex> lk(mPendingMutex);
    for (int i = 0; i < size; i++) mPending[i] = table[i];
    mPendingNew.store(true);
  }

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
  // Boutons du mode Relatif : on note seulement l'etat, c'est le thread
  // audio qui agit (ProcessBlock).
  if (paramIdx == kParamPhotoHold) { mPhotoHeld.store(GetParam(kParamPhotoHold)->Value() > 0.5); return; }
  if (paramIdx == kParamTrash) { if (GetParam(kParamTrash)->Value() > 0.5) mTrashEdge.store(true); return; }
  // 12 boutons de capture (mode Boutons) : le parametre est la seule source
  // (clic, CC, note ou automation assignes dans REAPER).
  if (paramIdx >= kParamCaptureStart && paramIdx < kParamCaptureStart + kNumVoices)
  {
    mSnapshots[paramIdx - kParamCaptureStart].SetCapture(GetParam(paramIdx)->Value() > 0.5);
    return;
  }

  // Seuls ces parametres reconstruisent la table d'onde.
  if (paramIdx == kParamBaseShape || paramIdx == kParamSkew
      || (paramIdx >= kParamHarmonic1 && paramIdx <= kParamHarmonic16))
    mEngineDirty.store(true); // la reconstruction est faite une seule fois par bloc (ProcessBlock / OnIdle)
}

void Photo::ReadTuning(double* dev, int& root, double& stretch, double& global) const
{
  for (int i = 0; i < tuning::kNumPitchClasses; i++) dev[i] = GetParam(kParamTuneStart + i)->Value();
  root = (int)GetParam(kParamTuneRoot)->Value();
  stretch = GetParam(kParamTuneStretch)->Value();
  global = GetParam(kParamTuneGlobal)->Value();
}

// Frequence d'une touche MIDI selon le tempérament courant.
float Photo::TunedFreq(int note) const
{
  double dev[tuning::kNumPitchClasses], stretch, global; int root;
  ReadTuning(dev, root, stretch, global);
  return (float)tuning::Frequency(note, dev, root, stretch, global);
}

// Rapport de transposition d'une touche par rapport a la touche 60
// (Do central = photo telle quelle), selon le tempérament courant.
// La note racine ne compte pas ici : l'echelle est toujours lue a partir du Do.
float Photo::TunedRatio(int note) const
{
  double dev[tuning::kNumPitchClasses], stretch, global; int root;
  ReadTuning(dev, root, stretch, global);
  (void)root; (void)global; // la hauteur globale s'annule dans un rapport // Relatif : la reference est toujours le Do central, donc racine = Do
  return (float)tuning::Ratio(note, dev, 0, stretch);
}

int Photo::AllocVoice()
{
  for (int v = 0; v < kNumVoices; v++)
    if (mSnapshots[v].IsFree()) return v;
  // Toutes occupees : on vole la plus ancienne.
  int oldest = 0;
  for (int v = 1; v < kNumVoices; v++)
    if (mSnapshots[v].Age() < mSnapshots[oldest].Age()) oldest = v;
  return oldest;
}

void Photo::ProcessMidiMsg(const IMidiMsg& msg)
{
  const int mode = (int)GetParam(kParamMode)->Value(); // 0 Boutons, 1 Clavier, 2 Synthe, 3 Relatif

  switch (msg.StatusMsg())
  {
    case IMidiMsg::kNoteOn:
      if (msg.Velocity() > 0)
      {
        if (mode == 0) break; // mode Boutons : les notes MIDI sont ignorees

        const float sens = (float)(GetParam(kParamVelSens)->Value() / 100.0);
        const float v01 = (float)msg.Velocity() / 127.f;
        const float velGain = 1.f - sens * (1.f - v01); // sens 100% : gain = velocite
        const int v = AllocVoice();
        if (mode == 1)
          mSnapshots[v].NoteOn(mAnalyzer, msg.NoteNumber(), velGain, ++mVoiceCounter); // une photo figee par note
        else if (mode == 3)
        {
          // Relatif : la photo est transposee selon la touche.
          //  - bouton Photo maintenu : photo EN DIRECT (la voix la suit, voir ProcessBlock)
          //  - sinon, memoire pleine : la photo gardee
          //  - sinon : une photo figee prise a l'instant de la note
          const float ratio = TunedRatio(msg.NoteNumber());
          if (!mPhotoHeld.load() && mMemValid.load())
            mSnapshots[v].NoteOnPhoto(mMem, mMemN, ratio, msg.NoteNumber(), velGain, ++mVoiceCounter);
          else
            mSnapshots[v].NoteOnPhoto(mAnalyzer.Peaks(), mAnalyzer.NumPeaks(), ratio, msg.NoteNumber(), velGain, ++mVoiceCounter);
        }
        else
          mSnapshots[v].NoteOnTone(TunedFreq(msg.NoteNumber()), kToneAmp, msg.NoteNumber(), velGain, ++mVoiceCounter);
        break;
      }
      // velocite 0 : equivalent a un note-off
      // fallthrough
    case IMidiMsg::kNoteOff:
      if (mode != 0)
      {
        for (int v = 0; v < kNumVoices; v++)
          if (mSnapshots[v].IsHeld() && mSnapshots[v].Note() == msg.NoteNumber())
            mSnapshots[v].NoteOff();
      }
      break;
    default:
      break;
  }
}

void Photo::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  const int n = std::min(nFrames, 8192);
  for (int i = 0; i < n; i++)
    mInBuf[i] = (float)inputs[0][i];

  // Table d'onde : une seule reconstruction par bloc au maximum (les potards
  // Harm/Skew peuvent envoyer beaucoup de changements d'affilee).
  if (mEngineDirty.load() && mBuildMutex.try_lock())
  {
    mBuildMutex.unlock();
    if (mEngineDirty.exchange(false)) UpdateEngine();
  }
  // Nouvelle table publiee ? On gele l'etat courant du fondu dans mTabA, puis
  // on repart de 0 vers la nouvelle table (mTabB). try_lock : l'audio n'attend jamais.
  if (mPendingNew.load())
  {
    std::unique_lock<std::mutex> lk(mPendingMutex, std::try_to_lock);
    if (lk.owns_lock() && mPendingNew.exchange(false))
    {
      const int ts = WavetableEngine::kTableSize;
      if (mPendingImmediate)
      {
        for (int i = 0; i < ts; i++) { mTabA[i] = mPending[i]; mTabB[i] = mPending[i]; }
        mXfade = 1.f;
        mPendingImmediate = false;
      }
      else
      {
        const float t = std::min(mXfade, 1.f);
        for (int i = 0; i < ts; i++) { mTabA[i] += t * (mTabB[i] - mTabA[i]); mTabB[i] = mPending[i]; }
        mXfade = 0.f;
      }
    }
  }
  // Duree du fondu enchaine : ~15 ms.
  const float xfadeStep = 1.f / std::max(1.f, 0.015f * (float)GetSampleRate());

  // Changement de mode : on coupe tout (sinon le son fige du mode Boutons
  // resterait audible au clavier, et inversement).
  const int mode = (int)GetParam(kParamMode)->Value(); // 0 Boutons, 1 Clavier, 2 Synthe, 3 Relatif
  const bool kbMode = (mode != 0); // Clavier et Synthe : voix au clavier, ADSR, reglages globaux
  if (mode != mLastMode)
  {
    mLastMode = mode;
    for (int v = 0; v < kNumVoices; v++) mSnapshots[v].Silence();
  }

  // Analyse FFT UNIQUE, partagee par toutes les voix.
  mAnalyzer.Feed(mInBuf, n);

  // Mode Relatif : corbeille, et memoire de la photo au relachement du bouton.
  if (mTrashEdge.exchange(false)) { mMemValid.store(false); mMemN = 0; }
  const bool held = (mode == 3) && mPhotoHeld.load();
  if (mWasHeld && !held && mode == 3 && mAnalyzer.NumPeaks() > 0)
  {
    // Relachement : la DERNIERE analyse est gardee en memoire.
    mMemN = mAnalyzer.NumPeaks();
    for (int i = 0; i < mMemN; i++) mMem[i] = mAnalyzer.Peaks()[i];
    mMemValid.store(true);
  }
  mWasHeld = held;

  const float stereoHz = (float)GetParam(kParamStereoHz)->Value();
  const float shutterMs = (float)GetParam(kParamFreqSmooth)->Value(); // le parametre "Obturateur"
  const float atk = (float)GetParam(kParamAttack)->Value();
  const float dec = (float)GetParam(kParamDecay)->Value();
  const float sus = (float)(GetParam(kParamSustain)->Value() / 100.0);
  const float rel = (float)GetParam(kParamRelease)->Value();
  const float kbVol = (float)(GetParam(kParamKbVolume)->Value() / 100.0);
  const int kbOct = (int)GetParam(kParamKbOctave)->Value();
  const float kbFine = (float)GetParam(kParamKbFine)->Value();

  for (int v = 0; v < kNumVoices; v++)
  {
    // Mode Boutons : reglages propres a chaque voix. Mode Clavier : globaux.
    float volume = kbMode ? kbVol : (float)(GetParam(VoiceParam(v, kVoiceVolume))->Value() / 100.0);
    int octaveIdx = kbMode ? kbOct : (int)GetParam(VoiceParam(v, kVoiceOctave))->Value(); // 0-14, 4 = neutre
    float fineHz = kbMode ? kbFine : (float)GetParam(VoiceParam(v, kVoiceFine))->Value();

    mSnapshots[v].SetStereoOffsetHz(stereoHz);
    mSnapshots[v].SetShutterMs(shutterMs);
    mSnapshots[v].SetVolume(volume);
    mSnapshots[v].SetPitchMultiplier(tuning::PitchMultiplier(octaveIdx));
    mSnapshots[v].SetFineTuneHz(fineHz);
    mSnapshots[v].SetEnvEnabled(kbMode);
    mSnapshots[v].SetAdsr(atk, dec, sus, rel);
    if (!kbMode) mSnapshots[v].Update(mAnalyzer, n);
    else if (held) mSnapshots[v].FollowLive(mAnalyzer, n); // Photo maintenu : les notes en cours prennent des micro-photos du son reel
    else mSnapshots[v].StopFollow();
  }

  {
    const int tsize = WavetableEngine::kTableSize;
    for (int i = 0; i < n; i++)
    {
      const bool fading = mXfade < 1.f;
      if (fading) { mXfade += xfadeStep; if (mXfade > 1.f) mXfade = 1.f; }

      float sumL = 0.f, sumR = 0.f;
      for (int v = 0; v < kNumVoices; v++)
      {
        float l, r;
        if (mXfade < 1.f) mSnapshots[v].Synthesize(mTabA, mTabB, mXfade, tsize, l, r);
        else mSnapshots[v].Synthesize(mTabB, nullptr, 0.f, tsize, l, r);
        sumL += l;
        sumR += r;
      }

      outputs[0][i] = sumL;
      outputs[1][i] = sumR;
    }
  }

  for (int i = n; i < nFrames; i++) { outputs[0][i] = 0.f; outputs[1][i] = 0.f; }
}

#endif
