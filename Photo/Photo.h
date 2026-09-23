#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include "WavetableEngine.h"
#include "SnapshotEngine.h"
#include "SpectralCurvePreviewControl.h"
#include <atomic>
#include <mutex>

// ============================================================================
// Photo - Etape 1+2 : moteur de table d'onde (forme de base + 16
// harmoniques + Skew), avec visualisation/dessin, lecture test via MIDI,
// et un premier bouton "photo" (capture FFT en direct, gel au
// relachement) - le vrai systeme a 12 boutons viendra dans une etape
// separee, ceci est la preuve de concept a une seule voix.
// ============================================================================

enum EParams
{
  kParamBaseShape = 0, // 0=Sine, 1=Saw, 2=Triangle, 3=Square
  kParamSkew,          // 0-100% (0.5 = neutre)
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
  kNumParams
};

using namespace iplug;
using namespace igraphics;

class Photo final : public iplug::Plugin
{
public:
  Photo(const InstanceInfo& info);

  void OnIdle() override;
  void OnUIOpen() override { SyncUIToState(); }
  void OnUIClose() override { mWaveView = nullptr; for (auto& c : mParamControls) c = nullptr; }

#if IPLUG_DSP
  void ProcessBlock(sample** inputs, sample** outputs, int nFrames) override;
  void ProcessMidiMsg(const IMidiMsg& msg) override;
  void OnParamChange(int paramIdx) override;
  void OnReset() override;
#endif

private:
  SpectralCurvePreviewControl* mWaveView = nullptr;
  IControl* mParamControls[kNumParams] = { nullptr };

  void ApplyAllState();
  void SyncUIToState();

#if IPLUG_DSP
  void UpdateEngine();

  WavetableEngine mEngine;
  // Test monophonique simple pour cette etape - le vrai systeme
  // polyphonique (12 voix) viendra plus tard.
  WavetableOscillator mTestOsc;

  // Premiere voix "photo" - preuve de concept, une seule pour l'instant.
  SnapshotEngine mSnapshot;

  std::mutex mWaveUIMutex;
  std::atomic<bool> mWaveUIUpdated { false };
  float mWaveUIBuf[WavetableEngine::kTableSize];
  int mWaveUISize = 0;
#endif
};
