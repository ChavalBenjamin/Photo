#pragma once

#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include "WaveTableEngine.h"

// ============================================================================
// Mecanisme "photo"
//
// Trois briques :
//
//  - PeakAnalyzer : UNE seule analyse FFT partagee par toutes les voix. Elle
//    tourne en continu sur l'entree audio et publie, a chaque hop, les
//    kMaxPeaks pics dominants (frequence + amplitude, interpoles). Style
//    fiddle~ : fenetre de Hann, interpolation exacte des pics (Grandke
//    1983), seuil de detection absolu.
//
//  - Adsr : enveloppe d'amplitude (mode Clavier).
//
//  - SnapshotEngine : UNE voix. Elle reprend les pics de l'analyseur :
//      * mode Boutons : suit les pics en direct tant que la capture est
//        active (souris OU MIDI CC), puis se fige ;
//      * mode Clavier : au note-on, prend UNE photo (les derniers pics
//        analyses), la fige, et la joue avec l'ADSR et la velocite.
//    Dans les deux cas elle relit la table d'onde partagee, une lecture par
//    partiel, en DOUBLE (copie gauche et copie droite, ecart en Hz).
// ============================================================================

struct SpectralPeak
{
  float freq = 0.f;
  float amp = 0.f;
};

// ----------------------------------------------------------------------------
// PeakAnalyzer
// ----------------------------------------------------------------------------
class PeakAnalyzer
{
public:
  using cplx = std::complex<float>;
  static constexpr int kMaxPeaks = 10;
  static constexpr float kAmpFloor = 0.0003f; // ~ -70 dBFS : seuil absolu de detection

  void Init(int fftSize, double sampleRate)
  {
    mFFTSize = fftSize;
    mHopSize = mFFTSize / 4; // overlap 75%
    mSampleRate = (sampleRate > 1.0) ? sampleRate : 44100.0;

    mRing.assign(mFFTSize, 0.f);
    mWindow.resize(mFFTSize);
    mCplx.assign(mFFTSize, cplx(0.f, 0.f));
    mMag.assign(mFFTSize / 2 + 1, 0.f);
    mCandidates.clear();
    mCandidates.reserve(mFFTSize / 2);

    // Hann PERIODIQUE (denominateur N) : forme pour laquelle la formule
    // d'interpolation des pics est exacte.
    for (int i = 0; i < mFFTSize; i++)
      mWindow[i] = 0.5f - 0.5f * std::cos(2.f * kPi * (float)i / (float)mFFTSize);

    mWritePos = 0;
    mSamplesUntilHop = mHopSize;
    mNumPeaks = 0;
    mHopId = 0;
  }

  // A appeler en continu depuis ProcessBlock (meme sans capture, pour qu'une
  // photo au note-on dispose toujours des derniers pics).
  void Feed(const float* in, int nFrames)
  {
    for (int i = 0; i < nFrames; i++)
    {
      mRing[mWritePos] = in[i];
      mWritePos = (mWritePos + 1) % mFFTSize;

      if (--mSamplesUntilHop == 0)
      {
        mSamplesUntilHop = mHopSize;
        AnalyzeHop();
      }
    }
  }

  const SpectralPeak* Peaks() const { return mPeaks; }
  int NumPeaks() const { return mNumPeaks; }
  uint32_t HopId() const { return mHopId; } // change a chaque nouvelle analyse
  float BinHz() const { return (float)mSampleRate / (float)mFFTSize; }

private:
  void AnalyzeHop()
  {
    for (int i = 0; i < mFFTSize; i++)
    {
      int idx = (mWritePos + i) % mFFTSize;
      mCplx[i] = cplx(mRing[idx] * mWindow[i], 0.f);
    }

    FFT(mCplx);

    const int numBins = mFFTSize / 2;
    for (int k = 0; k <= numBins; k++)
      mMag[k] = std::abs(mCplx[k]);

    // Amplitude d'un sinus pur A sur une case, fenetre de Hann : |X| = A*N/4.
    const float magFloor = kAmpFloor * (float)mFFTSize * 0.25f;
    const float binHz = BinHz();

    mCandidates.clear();
    for (int k = 2; k < numBins - 1; k++)
    {
      const float m = mMag[k];
      if (m < magFloor) continue;
      if (!(m > mMag[k - 1] && m >= mMag[k + 1])) continue;

      const bool right = mMag[k + 1] > mMag[k - 1];
      const float alpha = (right ? mMag[k + 1] : mMag[k - 1]) / m; // 0.5..1
      const float d = (2.f * alpha - 1.f) / (alpha + 1.f);        // 0..0.5
      const float delta = right ? d : -d;

      // Reponse de la fenetre de Hann a delta case(s) du centre.
      const float pd = kPi * delta;
      const float sinc = (std::abs(delta) < 1e-4f) ? 1.f : std::sin(pd) / pd;
      const float resp = sinc / (1.f - delta * delta);

      SpectralPeak pk;
      pk.freq = ((float)k + delta) * binHz;
      pk.amp = std::clamp(4.f * m / ((float)mFFTSize * resp), 0.f, 2.f);
      mCandidates.push_back(pk);
    }

    std::sort(mCandidates.begin(), mCandidates.end(), [](const SpectralPeak& a, const SpectralPeak& b) { return a.amp > b.amp; });
    mNumPeaks = std::min((int)mCandidates.size(), kMaxPeaks);
    for (int i = 0; i < mNumPeaks; i++) mPeaks[i] = mCandidates[i];

    mHopId++;
  }

  static void FFT(std::vector<cplx>& a)
  {
    int n = (int)a.size();
    for (int i = 1, j = 0; i < n; i++)
    {
      int bit = n >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1)
    {
      float ang = -2.f * kPi / (float)len;
      cplx wlen(std::cos(ang), std::sin(ang));
      for (int i = 0; i < n; i += len)
      {
        cplx w(1.f, 0.f);
        for (int k = 0; k < len / 2; k++)
        {
          cplx u = a[i + k], v = a[i + k + len / 2] * w;
          a[i + k] = u + v;
          a[i + k + len / 2] = u - v;
          w *= wlen;
        }
      }
    }
  }

  static constexpr float kPi = 3.14159265358979323846f;

  int mFFTSize = 2048;
  int mHopSize = 512;
  int mSamplesUntilHop = 512;
  int mWritePos = 0;
  double mSampleRate = 44100.0;

  std::vector<float> mRing, mWindow, mMag;
  std::vector<cplx> mCplx;
  std::vector<SpectralPeak> mCandidates;

  SpectralPeak mPeaks[kMaxPeaks];
  int mNumPeaks = 0;
  uint32_t mHopId = 0;
};

// ----------------------------------------------------------------------------
// Adsr : enveloppe lineaire. Attaque 0 -> 1, decay 1 -> sustain, release
// depuis le niveau courant -> 0.
// ----------------------------------------------------------------------------
class Adsr
{
public:
  enum class Stage { Idle, Attack, Decay, Sustain, Release };

  void SetSampleRate(double sr) { mSampleRate = (sr > 1.0) ? sr : 44100.0; }

  // Temps en ms ; sustain entre 0 et 1.
  void SetParams(float attackMs, float decayMs, float sustain, float releaseMs)
  {
    mSustain = std::clamp(sustain, 0.f, 1.f);
    mAttackInc = 1.f / (std::max(attackMs, 0.5f) * 0.001f * (float)mSampleRate);
    mDecayDec = (1.f - mSustain) / (std::max(decayMs, 1.f) * 0.001f * (float)mSampleRate);
    mReleaseMs = std::max(releaseMs, 1.f);
  }

  void GateOn() { mStage = Stage::Attack; mGate = true; }
  void GateOff()
  {
    mGate = false;
    if (mStage == Stage::Idle) return;
    mStage = Stage::Release;
    mReleaseDec = mLevel / (mReleaseMs * 0.001f * (float)mSampleRate);
    if (mLevel <= 0.f) mStage = Stage::Idle;
  }
  void Reset() { mStage = Stage::Idle; mLevel = 0.f; mGate = false; }

  bool IsIdle() const { return mStage == Stage::Idle; }
  bool IsHeld() const { return mGate; }

  float Process()
  {
    switch (mStage)
    {
      case Stage::Idle: return 0.f;
      case Stage::Attack:
        mLevel += mAttackInc;
        if (mLevel >= 1.f) { mLevel = 1.f; mStage = Stage::Decay; }
        break;
      case Stage::Decay:
        mLevel -= mDecayDec;
        if (mLevel <= mSustain) { mLevel = mSustain; mStage = Stage::Sustain; }
        break;
      case Stage::Sustain:
        mLevel += (mSustain - mLevel) * 0.01f; // suit un Sustain bouge en direct
        break;
      case Stage::Release:
        mLevel -= mReleaseDec;
        if (mLevel <= 0.f) { mLevel = 0.f; mStage = Stage::Idle; }
        break;
    }
    return mLevel;
  }

private:
  double mSampleRate = 44100.0;
  Stage mStage = Stage::Idle;
  bool mGate = false;
  float mLevel = 0.f;
  float mSustain = 0.8f;
  float mAttackInc = 0.01f;
  float mDecayDec = 0.001f;
  float mReleaseDec = 0.001f;
  float mReleaseMs = 300.f;
};

// ----------------------------------------------------------------------------
// SnapshotEngine : une voix "photo"
// ----------------------------------------------------------------------------
class SnapshotEngine
{
public:
  static constexpr int kMaxPeaks = PeakAnalyzer::kMaxPeaks;

  void Init(double sampleRate)
  {
    mSampleRate = (sampleRate > 1.0) ? sampleRate : 44100.0;
    mEnv.SetSampleRate(mSampleRate);
    Silence();
    mLastHopId = 0;
    RecomputeCoeffs();
    mOctaveRatioSmooth = mOctaveRatioTarget;
    mFineHzSmooth = mFineTuneHz;
    mStereoHzSmooth = mStereoOffsetHz;
  }

  // Coupe tout de suite (changement de mode) : aucun partiel, enveloppe au repos.
  void Silence()
  {
    for (int p = 0; p < kMaxPeaks; p++)
    {
      mTgtFreq[p] = mTgtAmp[p] = 0.f;
      mFreqCur[p] = mAmpCur[p] = 0.f;
      mPeakPhase[p] = mPeakPhaseR[p] = 0.f;
    }
    mEnv.Reset();
    mNote = -1;
  }

  // ---- Reglages (appeles a chaque bloc) ----

  // Lissage de la frequence, par echantillon. 0 = saut direct (defaut).
  void SetFreqSmoothMs(float ms)
  {
    ms = std::clamp(ms, 0.f, 50.f);
    if (ms == mFreqSmoothMs) return;
    mFreqSmoothMs = ms;
    RecomputeCoeffs();
  }

  // Lissage de l'amplitude, par echantillon (anti-"grain" / escalier).
  void SetAmpSmoothMs(float ms)
  {
    ms = std::clamp(ms, 0.1f, 200.f);
    if (ms == mAmpSmoothMs) return;
    mAmpSmoothMs = ms;
    RecomputeCoeffs();
  }

  // Transposition par octave : -4 (/16) a +4 (x16), 0 = neutre.
  void SetOctaveShift(int shift)
  {
    mOctaveShift = std::clamp(shift, -4, 4);
    mOctaveRatioTarget = std::pow(2.f, (float)mOctaveShift);
  }

  // Ajustement fin, en Hz, ajoute a CHAQUE partiel APRES l'octave.
  void SetFineTuneHz(float hz) { mFineTuneHz = std::clamp(hz, -20.f, 20.f); }

  // Ecart de frequence de la copie DROITE par rapport a la gauche (-10..+10 Hz).
  void SetStereoOffsetHz(float hz) { mStereoOffsetHz = std::clamp(hz, -10.f, 10.f); }

  // Volume de cette voix (0-2).
  void SetVolume(float vol) { mVolume = std::clamp(vol, 0.f, 2.f); }

  // Enveloppe : active seulement en mode Clavier (en mode Boutons le son
  // tourne en continu, sans enveloppe).
  void SetEnvEnabled(bool on) { mUseEnv = on; }
  void SetAdsr(float attackMs, float decayMs, float sustain, float releaseMs)
  {
    mEnv.SetParams(attackMs, decayMs, sustain, releaseMs);
  }

  // ---- Mode Boutons ----

  // Thread-safe. Deux sources independantes : la souris et le MIDI (CC).
  void StartCapture() { mCaptureMouse.store(true); }
  void StopCapture() { mCaptureMouse.store(false); }
  void SetMidiCapture(bool on) { mCaptureMidi.store(on); }

  // A appeler a chaque bloc, APRES PeakAnalyzer::Feed : tant que la capture
  // est active, suit les pics de la derniere analyse.
  void Update(const PeakAnalyzer& an)
  {
    if (an.HopId() == mLastHopId) return;
    mLastHopId = an.HopId();
    if (mCaptureMouse.load() || mCaptureMidi.load())
      TrackPeaks(an.Peaks(), an.NumPeaks(), an.BinHz());
  }

  // ---- Mode Clavier ----

  // Prend UNE photo (les derniers pics analyses), la fige, et lance la note.
  // velGain : 0..1, deja calcule a partir de la velocite.
  void NoteOn(const PeakAnalyzer& an, int note, float velGain, uint32_t age)
  {
    const SpectralPeak* pk = an.Peaks();
    const int n = an.NumPeaks();
    for (int p = 0; p < kMaxPeaks; p++)
    {
      const bool has = p < n;
      mTgtFreq[p] = mFreqCur[p] = has ? pk[p].freq : 0.f;
      mTgtAmp[p] = mAmpCur[p] = has ? pk[p].amp : 0.f; // l'ADSR gere l'attaque
      mPeakPhase[p] = mPeakPhaseR[p] = 0.f;
    }
    mNote = note;
    mVelGain = std::clamp(velGain, 0.f, 1.f);
    mAge = age;
    mEnv.GateOn();
  }

  void NoteOff() { mEnv.GateOff(); }

  bool IsFree() const { return mEnv.IsIdle(); }
  bool IsHeld() const { return mEnv.IsHeld(); }
  int Note() const { return mNote; }
  uint32_t Age() const { return mAge; }

  // ---- Synthese ----

  // Un echantillon stereo : le tout est joue en DOUBLE. La copie gauche est
  // a la hauteur captee, la copie droite a hauteur + ecart (Hz). Octave et
  // Fine sont appliques ICI (en direct, meme sur un son gele). La table
  // reste la SEULE source de timbre.
  void Synthesize(const WavetableEngine& engine, float& outL, float& outR)
  {
    float gain = 1.f;
    if (mUseEnv)
    {
      if (mEnv.IsIdle()) { outL = outR = 0.f; return; }
      gain = mEnv.Process() * mVelGain;
    }

    const float* table = engine.GetTable();
    const int size = engine.GetTableSize();
    const float nyquist = (float)mSampleRate * 0.5f;
    const float sr = (float)mSampleRate;

    mOctaveRatioSmooth += (mOctaveRatioTarget - mOctaveRatioSmooth) * mTransposeCoeff;
    mFineHzSmooth += (mFineTuneHz - mFineHzSmooth) * mTransposeCoeff;
    mStereoHzSmooth += (mStereoOffsetHz - mStereoHzSmooth) * mTransposeCoeff;

    float sumL = 0.f, sumR = 0.f;
    for (int p = 0; p < kMaxPeaks; p++)
    {
      mAmpCur[p] += (mTgtAmp[p] - mAmpCur[p]) * mAmpCoeff;
      mFreqCur[p] += (mTgtFreq[p] - mFreqCur[p]) * mFreqCoeff;

      if (mAmpCur[p] < 0.00001f && mTgtAmp[p] <= 0.f) { mPeakPhase[p] = 0.f; mPeakPhaseR[p] = 0.f; continue; }

      const float freqL = mFreqCur[p] * mOctaveRatioSmooth + mFineHzSmooth;
      const float freqR = freqL + mStereoHzSmooth;

      if (freqL > 0.f && freqL < nyquist) // garde-fou : evite repliement / depassement de table
      {
        sumL += mAmpCur[p] * ReadTable(table, size, mPeakPhase[p]);
        mPeakPhase[p] += freqL / sr;
        if (mPeakPhase[p] >= 1.f) mPeakPhase[p] -= std::floor(mPeakPhase[p]);
      }
      if (freqR > 0.f && freqR < nyquist)
      {
        sumR += mAmpCur[p] * ReadTable(table, size, mPeakPhaseR[p]);
        mPeakPhaseR[p] += freqR / sr;
        if (mPeakPhaseR[p] >= 1.f) mPeakPhaseR[p] -= std::floor(mPeakPhaseR[p]);
      }
    }
    outL = sumL * mVolume * gain;
    outR = sumR * mVolume * gain;
  }

private:
  static float ReadTable(const float* table, int size, float phase)
  {
    float pos = phase * (float)size;
    int idx0 = (int)pos;
    if (idx0 >= size) idx0 = size - 1; // garde-fou : jamais de lecture hors table
    if (idx0 < 0) idx0 = 0;
    int idx1 = (idx0 + 1) % size;
    float frac = pos - (float)idx0;
    return table[idx0] * (1.f - frac) + table[idx1] * frac;
  }

  // Suivi de partiels : chaque pic reprend le slot le plus proche en
  // frequence de l'analyse precedente (evite les echanges de place).
  void TrackPeaks(const SpectralPeak* cand, int nCand, float binHz)
  {
    bool used[kMaxPeaks] = { false };
    int assigned[kMaxPeaks];
    for (int c = 0; c < nCand; c++)
    {
      assigned[c] = -1;
      int best = -1;
      float bestDist = 1e30f;
      for (int s = 0; s < kMaxPeaks; s++)
      {
        if (used[s] || mTgtAmp[s] <= 0.f) continue; // slot libre : pas un candidat au suivi
        const float dist = std::abs(cand[c].freq - mTgtFreq[s]);
        const float tol = std::max(2.f * binHz, 0.03f * mTgtFreq[s]);
        if (dist < tol && dist < bestDist) { bestDist = dist; best = s; }
      }
      if (best >= 0) { used[best] = true; assigned[c] = best; }
    }
    // Pics sans partiel precedent : un slot libre (le moins audible d'abord).
    for (int c = 0; c < nCand; c++)
    {
      if (assigned[c] >= 0) continue;
      int best = -1;
      float bestAmp = 1e30f;
      for (int s = 0; s < kMaxPeaks; s++)
        if (!used[s] && mAmpCur[s] < bestAmp) { bestAmp = mAmpCur[s]; best = s; }
      if (best < 0) continue; // ne devrait pas arriver (<= 10 candidats pour 10 slots)
      used[best] = true;
      assigned[c] = best;
      if (mAmpCur[best] < 0.001f) mFreqCur[best] = cand[c].freq; // nouveau partiel : pas de glissement depuis 0
    }
    for (int c = 0; c < nCand; c++)
      if (assigned[c] >= 0)
      {
        mTgtFreq[assigned[c]] = cand[c].freq;
        mTgtAmp[assigned[c]] = cand[c].amp;
      }
    // Slots sans pic ce hop : fondu vers 0 (la frequence reste, pour une sortie douce).
    for (int s = 0; s < kMaxPeaks; s++)
      if (!used[s]) mTgtAmp[s] = 0.f;
  }

  void RecomputeCoeffs()
  {
    const float sr = (float)mSampleRate;
    mAmpCoeff = 1.f - std::exp(-1.f / (mAmpSmoothMs * 0.001f * sr));
    mFreqCoeff = (mFreqSmoothMs <= 0.001f) ? 1.f : (1.f - std::exp(-1.f / (mFreqSmoothMs * 0.001f * sr)));
    mTransposeCoeff = 1.f - std::exp(-1.f / (0.005f * sr)); // Octave/Fine/Ecart : ~5 ms
  }

  double mSampleRate = 44100.0;
  uint32_t mLastHopId = 0;

  float mAmpSmoothMs = 10.f;
  float mFreqSmoothMs = 0.f;
  float mAmpCoeff = 0.01f;
  float mFreqCoeff = 1.f;
  float mTransposeCoeff = 0.01f;

  int mOctaveShift = 0;
  float mOctaveRatioTarget = 1.f;
  float mOctaveRatioSmooth = 1.f;
  float mFineTuneHz = 0.f;
  float mFineHzSmooth = 0.f;
  float mStereoOffsetHz = 0.f;
  float mStereoHzSmooth = 0.f;
  float mVolume = 1.f;

  Adsr mEnv;
  bool mUseEnv = false;
  float mVelGain = 1.f;
  int mNote = -1;
  uint32_t mAge = 0;

  std::atomic<bool> mCaptureMouse { false };
  std::atomic<bool> mCaptureMidi { false };

  // Cibles (mises a jour par hop) et valeurs courantes (lissees par echantillon).
  float mTgtFreq[kMaxPeaks] = { 0.f };
  float mTgtAmp[kMaxPeaks] = { 0.f };
  float mFreqCur[kMaxPeaks] = { 0.f };
  float mAmpCur[kMaxPeaks] = { 0.f };
  float mPeakPhase[kMaxPeaks] = { 0.f };  // phase gauche
  float mPeakPhaseR[kMaxPeaks] = { 0.f }; // phase droite
};
