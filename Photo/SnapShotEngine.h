#pragma once

#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
#include <atomic>
#include "WaveTableEngine.h"

// ============================================================================
// SnapshotEngine
//
// Le mecanisme "photo" : analyse FFT en direct de l'entree audio (micro
// ou Wave), extrait les kMaxPeaks (10) pics dominants frequence+amplitude,
// resynthetise via des lectures de la table d'onde partagee.
//
// Analyse inspiree de fiddle~ (Pd) : fenetre de Hann, pics locaux dont la
// frequence ET l'amplitude sont interpolees a partir des cases voisines
// (formule exacte pour une fenetre de Hann, Grandke 1983) - la precision
// ne depend plus de la largeur d'une case de FFT (~21 Hz a 2048 points).
// Seuil de detection ABSOLU (dBFS), pas relatif au pic le plus fort.
//
// Suivi de partiels : chaque nouveau pic reprend le partiel (slot) le plus
// proche en frequence de l'analyse precedente - evite les echanges de
// place entre partiels (sauts d'amplitude / de hauteur).
//
// Tant que la capture est active (souris OU MIDI), les pics se mettent a
// jour en direct a chaque hop. Au relachement, les dernieres valeurs
// restent figees et continuent d'etre jouees (le "gel" de la photo).
//
// Lissage PAR ECHANTILLON dans Synthesize() : l'amplitude glisse
// continument vers sa cible (plus d'escalier au rythme des hops), y
// compris au demarrage et au relachement de la capture. La frequence est
// lissee de la meme facon, mais a 0 ms par defaut (saut direct).
// ============================================================================

class SnapshotEngine
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

    // Hann PERIODIQUE (denominateur N) : c'est la forme pour laquelle la
    // formule d'interpolation des pics est exacte.
    for (int i = 0; i < mFFTSize; i++)
      mWindow[i] = 0.5f - 0.5f * std::cos(2.f * kPi * (float)i / (float)mFFTSize);

    mWritePos = 0;
    mSamplesUntilHop = mHopSize;

    for (int p = 0; p < kMaxPeaks; p++)
    {
      mTgtFreq[p] = mTgtAmp[p] = 0.f;
      mFreqCur[p] = mAmpCur[p] = 0.f;
      mPeakPhase[p] = mPeakPhaseR[p] = 0.f;
    }
    mStereoHzSmooth = mStereoOffsetHz;

    RecomputeCoeffs();
    mOctaveRatioSmooth = mOctaveRatioTarget;
    mFineHzSmooth = mFineTuneHz;
  }

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

  // Volume de cette voix (0-2, permet un peu de boost au-dela de l'unite).
  void SetVolume(float vol) { mVolume = std::clamp(vol, 0.f, 2.f); }

  // Thread-safe. Deux sources independantes : la souris (bouton de l'UI)
  // et le MIDI (CC). La capture est active des qu'UNE des deux est active.
  void StartCapture() { mCaptureMouse.store(true); }
  void StopCapture() { mCaptureMouse.store(false); }
  void SetMidiCapture(bool on) { mCaptureMidi.store(on); }

  // Alimente l'analyse avec le son d'entree - a appeler en continu
  // depuis ProcessBlock, meme quand pas de capture (pour que le prochain
  // appui reparte avec un historique frais).
  void Feed(const float* in, int nFrames)
  {
    bool capturing = mCaptureMouse.load() || mCaptureMidi.load();
    for (int i = 0; i < nFrames; i++)
    {
      mRing[mWritePos] = in[i];
      mWritePos = (mWritePos + 1) % mFFTSize;

      if (--mSamplesUntilHop == 0)
      {
        mSamplesUntilHop = mHopSize;
        if (capturing) AnalyzeHop();
      }
    }
  }

  // Ecart de frequence de la copie DROITE par rapport a la gauche, en Hz
  // (-10..+10), ajoute a chaque partiel. 0 = gauche et droite identiques
  // (image mono centree) ; sinon les deux copies "battent" et se dephasent
  // lentement : effet de phasing, et image stereo.
  void SetStereoOffsetHz(float hz) { mStereoOffsetHz = std::clamp(hz, -10.f, 10.f); }

  // Un echantillon stereo : le tout est joue en DOUBLE. La copie gauche est
  // a la hauteur captee, la copie droite a hauteur + ecart (Hz). Octave et
  // Fine sont appliques ICI (en direct, meme sur un son gele). La table
  // reste la SEULE source de timbre ; la capture ne fournit que
  // hauteur+volume.
  void Synthesize(const WavetableEngine& engine, float& outL, float& outR)
  {
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
    outL = sumL * mVolume;
    outR = sumR * mVolume;
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

  struct Peak { float freq; float amp; };

  void AnalyzeHop()
  {
    for (int i = 0; i < mFFTSize; i++)
    {
      int idx = (mWritePos + i) % mFFTSize;
      mCplx[i] = cplx(mRing[idx] * mWindow[i], 0.f);
    }

    FFT(mCplx, false);

    const int numBins = mFFTSize / 2;
    for (int k = 0; k <= numBins; k++)
      mMag[k] = std::abs(mCplx[k]);

    // Amplitude d'un sinus pur A sur une case, fenetre de Hann : |X| = A*N/4.
    const float magFloor = kAmpFloor * (float)mFFTSize * 0.25f;
    const float binHz = (float)mSampleRate / (float)mFFTSize;

    // --- Pics locaux + interpolation (frequence ET amplitude) ---
    mCandidates.clear();
    for (int k = 2; k < numBins - 1; k++)
    {
      const float m = mMag[k];
      if (m < magFloor) continue;
      if (!(m > mMag[k - 1] && m >= mMag[k + 1])) continue;

      const bool right = mMag[k + 1] > mMag[k - 1];
      const float alpha = (right ? mMag[k + 1] : mMag[k - 1]) / m; // 0.5..1
      float d = (2.f * alpha - 1.f) / (alpha + 1.f);              // 0..0.5
      const float delta = right ? d : -d;

      // Reponse de la fenetre de Hann a delta case(s) du centre.
      const float pd = kPi * delta;
      const float sinc = (std::abs(delta) < 1e-4f) ? 1.f : std::sin(pd) / pd;
      const float resp = sinc / (1.f - delta * delta);

      Peak pk;
      pk.freq = ((float)k + delta) * binHz;
      pk.amp = std::clamp(4.f * m / ((float)mFFTSize * resp), 0.f, 2.f);
      mCandidates.push_back(pk);
    }

    // Les kMaxPeaks plus forts.
    std::sort(mCandidates.begin(), mCandidates.end(), [](const Peak& a, const Peak& b) { return a.amp > b.amp; });
    if ((int)mCandidates.size() > kMaxPeaks) mCandidates.resize(kMaxPeaks);

    // --- Suivi de partiels : chaque pic reprend le slot le plus proche ---
    bool used[kMaxPeaks] = { false };
    int assigned[kMaxPeaks];
    const int nCand = (int)mCandidates.size();
    for (int c = 0; c < nCand; c++)
    {
      assigned[c] = -1;
      int best = -1;
      float bestDist = 1e30f;
      for (int s = 0; s < kMaxPeaks; s++)
      {
        if (used[s] || mTgtAmp[s] <= 0.f) continue; // slot libre : pas un candidat au suivi
        const float dist = std::abs(mCandidates[c].freq - mTgtFreq[s]);
        const float tol = std::max(2.f * binHz, 0.03f * mTgtFreq[s]);
        if (dist < tol && dist < bestDist) { bestDist = dist; best = s; }
      }
      if (best >= 0) { used[best] = true; assigned[c] = best; }
    }
    // Pics sans partiel precedent : on prend un slot libre (le moins audible d'abord).
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
      if (mAmpCur[best] < 0.001f) mFreqCur[best] = mCandidates[c].freq; // nouveau partiel : pas de glissement depuis 0
    }
    for (int c = 0; c < nCand; c++)
      if (assigned[c] >= 0)
      {
        mTgtFreq[assigned[c]] = mCandidates[c].freq;
        mTgtAmp[assigned[c]] = mCandidates[c].amp;
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
    mTransposeCoeff = 1.f - std::exp(-1.f / (0.005f * sr)); // Octave/Fine : ~5 ms
  }

  static void FFT(std::vector<cplx>& a, bool invert)
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
      float ang = 2.f * kPi / (float)len * (invert ? 1.f : -1.f);
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
    if (invert) for (auto& x : a) x /= (float)n;
  }

  static constexpr float kPi = 3.14159265358979323846f;

  int mFFTSize = 2048;
  int mHopSize = 512;
  int mSamplesUntilHop = 512;
  int mWritePos = 0;
  double mSampleRate = 44100.0;

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

  std::atomic<bool> mCaptureMouse { false };
  std::atomic<bool> mCaptureMidi { false };

  std::vector<float> mRing, mWindow, mMag;
  std::vector<cplx> mCplx;
  std::vector<Peak> mCandidates;

  // Cibles (mises a jour par hop) et valeurs courantes (lissees par echantillon).
  float mTgtFreq[kMaxPeaks] = { 0.f };
  float mTgtAmp[kMaxPeaks] = { 0.f };
  float mFreqCur[kMaxPeaks] = { 0.f };
  float mAmpCur[kMaxPeaks] = { 0.f };
  float mPeakPhase[kMaxPeaks] = { 0.f };  // phase gauche
  float mPeakPhaseR[kMaxPeaks] = { 0.f }; // phase droite
};
