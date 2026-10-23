#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include "WaveTableEngine.h"
#include "SnapShotEngine.h"
#include "SpectralCurvePreviewControl.h"
#include <atomic>
#include <mutex>

// ============================================================================
// Photo - moteur de table d'onde (forme de base + 16 harmoniques + Skew)
// + 12 voix "photo" independantes, chacune avec son propre bouton de
// capture, son volume, sa transposition par octave et son ajustement
// fin. Toutes les 12 lisent la MEME table d'onde partagee.
// ============================================================================

static constexpr int kNumVoices = 12;

// Niveau d'une note du mode Synthe (laisse de la marge pour les accords).
static constexpr float kToneAmp = 0.5f;

enum EParams
{
  kParamBaseShape = 0, // 0=Sine, 1=Saw, 2=Triangle, 3=Square
  kParamSkew,          // 0-100% (0.5 = neutre)
  kParamFreqSmooth,    // "Obturateur" : cadence des micro-photos (0 = chaque analyse, jusqu a 2000 ms)
  kParamHarmonic1,
  kParamHarmonic2,
  kParamHarmonic3,
  kParamHarmonic4,
  kParamHarmonic5,
  kParamHarmonic6,
  kParamHarmonic7,
  kParamHarmonic8,
  kParamHarmonic9,
  kParamHarmonic10,
  kParamHarmonic11,
  kParamHarmonic12,
  kParamHarmonic13,
  kParamHarmonic14,
  kParamHarmonic15,
  kParamHarmonic16,
  kParamVoiceParamsStart, // 12 voix x 3 params (Volume, Octave, Fine)
  // Ajoute APRES les parametres de voix pour ne pas decaler les indices
  // des projets deja enregistres.
  kParamStereoHz = kParamVoiceParamsStart + kNumVoices * 3, // ecart D/G en Hz (-10..+10)
  kParamMode,      // 0 = Boutons (12 boutons / CC), 1 = Clavier (photo par note), 2 = Synthe (table d'onde), 3 = Relatif (photo transposee par la touche)
  kParamAttack,    // ADSR, mode Clavier (ms)
  kParamDecay,     // ms
  kParamSustain,   // %
  kParamRelease,   // ms
  kParamVelSens,   // sensibilite a la velocite (0-100%)
  kParamKbVolume,  // reglages GLOBAUX du mode Clavier
  kParamKbOctave,
  kParamKbFine,
  kParamPhotoHold, // mode Relatif : bouton Photo (maintenu = analyse en direct, relache = memoire)
  kParamTrash,     // mode Relatif : vide la memoire
  kParamCaptureStart, // 12 boutons de capture (mode Boutons), un parametre 0/1 chacun
  kNumParams = kParamCaptureStart + kNumVoices
};

// Helper : indice du parametre pour la voix "voiceIdx" (0-11), which =
// 0 (Volume), 1 (Octave), 2 (Fine).
// Parametre du bouton de capture de la voix "voiceIdx" (0-11). Assignable dans
// REAPER (CC, note, automation) comme tout parametre.
inline int CaptureParam(int voiceIdx) { return kParamCaptureStart + voiceIdx; }
inline int VoiceParam(int voiceIdx, int which) { return kParamVoiceParamsStart + voiceIdx * 3 + which; }
enum EVoiceParamWhich { kVoiceVolume = 0, kVoiceOctave = 1, kVoiceFine = 2 };

using namespace iplug;
using namespace igraphics;

class Photo final : public iplug::Plugin
{
public:
  Photo(const InstanceInfo& info);

  void OnIdle() override;
  void OnUIOpen() override { SyncUIToState(); }
  void OnUIClose() override { mWaveView = nullptr; mPhotoBtn = nullptr; for (auto& c : mParamControls) c = nullptr; }

#if IPLUG_DSP
  void ProcessBlock(sample** inputs, sample** outputs, int nFrames) override;
  void ProcessMidiMsg(const IMidiMsg& msg) override;
  void OnParamChange(int paramIdx) override;
  void OnReset() override;
#endif

private:
  SpectralCurvePreviewControl* mWaveView = nullptr;
  IControl* mPhotoBtn = nullptr;
  IControl* mParamControls[kNumParams] = { nullptr };

  void ApplyAllState();
  void SyncUIToState();

#if IPLUG_DSP
  void UpdateEngine();

  // Protege le moteur : RebuildIfNeeded() (thread principal, a chaque
  // changement de potard Harm/Skew) ne doit jamais s'executer en meme
  // temps que la lecture de la table (thread audio, dans ProcessBlock).
  std::mutex mEngineMutex;
  WavetableEngine mEngine;

  // UNE analyse FFT partagee par les 12 voix (au lieu d'une par voix).
  PeakAnalyzer mAnalyzer;
  SnapshotEngine mSnapshots[kNumVoices];

  // Mode Clavier : attribution des voix (la plus ancienne est volee).
  int AllocVoice();
  uint32_t mVoiceCounter = 0;
  int mLastMode = 0;

  // Mode Relatif : photo gardee en memoire. Les pics ne sont touches que par
  // le thread audio ; l'UI ne lit que le drapeau mMemValid.
  std::atomic<bool> mPhotoHeld { false };
  std::atomic<bool> mTrashEdge { false };
  std::atomic<bool> mMemValid { false };
  bool mWasHeld = false;
  SpectralPeak mMem[PeakAnalyzer::kMaxPeaks];
  int mMemN = 0;
  bool mMemShown = false; // etat affiche par le bouton Photo (thread UI)

  // Tampon d'entree mono, propre a l'instance (pas partage entre instances).
  float mInBuf[8192];

  std::mutex mWaveUIMutex;
  std::atomic<bool> mWaveUIUpdated { false };
  float mWaveUIBuf[WavetableEngine::kTableSize];
  int mWaveUISize = 0;
#endif
};
