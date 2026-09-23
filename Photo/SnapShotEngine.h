#pragma once

#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
#include <atomic>

// ============================================================================
// SnapshotEngine
//
// Le mecanisme "photo" : analyse FFT en direct de l'entree audio (micro
// ou Wave), extrait jusqu'a 10 pics frequence+amplitude dominants,
// resynthetise additivement via 10 oscillateurs sinusoidaux simples.
//
// Tant que StartCapture() est actif, les 10 pics se mettent a jour en
// direct a chaque hop. Des que StopCapture() est appele, la mise a jour
// s'arrete : les 10 dernieres valeurs captees restent figees et
// continuent d'etre jouees (le "gel" de la photo).
//
// Suivi de partiels SIMPLIFIE : les pics sont tries par frequence (pas
// par force), pas de vrai suivi d'identite d'un hop a l'autre - une
// premiere version fonctionnelle, un vrai suivi de partiels serait un
// chantier a part si besoin plus tard.
// ============================================================================

class SnapshotEngine
{
public:
  using cplx = std::complex<float>;
  static constexpr int kMaxPeaks = 10;

  void Init(int fftSize, double sampleRate)
  {
    mFFTSize = fftSize;
    mHopSize = mFFTSize / 4; // overlap 75%, fixe pour cette premiere version
    mSampleRate = sampleRate;

    mRing.assign(mFFTSize, 0.f);
    mWindow.resize(mFFTSize);
    mCplx.assign(mFFTSize, cplx(0.f, 0.f));

    for (int i = 0; i < mFFTSize; i++)
      mWindow[i] = 0.5f - 0.5f * std::cos(2.f * kPi * i / (mFFTSize - 1));

    mWritePos = 0;
    mSamplesUntilHop = mHopSize;

    // Lissage des pics (evite le "zipper" d'un hop a l'autre).
    float hopMs = (float)mHopSize / (float)mSampleRate * 1000.f;
    mSmoothCoeff = 1.f - std::exp(-hopMs / kSmoothMs);
  }

  // Thread-safe : appele depuis l'UI (clic sur le bouton photo).
  void StartCapture() { mCapturing.store(true); }
  void StopCapture() { mCapturing.store(false); }

  // Alimente l'analyse avec le son d'entree - a appeler en continu
  // depuis ProcessBlock, meme quand StopCapture (pour que le prochain
  // Start reparte avec un historique frais).
  void Feed(const float* in, int nFrames)
  {
    bool capturing = mCapturing.load();
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

  // Resynthese additive : un echantillon a la fois, somme des 10
  // oscillateurs (frequence/amplitude figees ou en direct selon l'etat).
  float Synthesize()
  {
    float sum = 0.f;
    for (int p = 0; p < kMaxPeaks; p++)
    {
      if (mPeakAmpSmooth[p] < 0.0001f) { mPeakPhase[p] = 0.f; continue; }
      sum += mPeakAmpSmooth[p] * std::sin(mPeakPhase[p]);
      mPeakPhase[p] += 2.f * kPi * mPeakFreqSmooth[p] / (float)mSampleRate;
      if (mPeakPhase[p] >= 2.f * kPi) mPeakPhase[p] -= 2.f * kPi;
    }
    return sum;
  }

private:
  void AnalyzeHop()
  {
    for (int i = 0; i < mFFTSize; i++)
    {
      int idx = (mWritePos + i) % mFFTSize;
      mCplx[i] = cplx(mRing[idx] * mWindow[i], 0.f);
    }

    FFT(mCplx, false);

    int numBins = mFFTSize / 2;
    std::vector<float> mag(numBins + 1);
    float maxMag = 0.f;
    for (int k = 0; k <= numBins; k++)
    {
      mag[k] = std::abs(mCplx[k]);
      maxMag = std::max(maxMag, mag[k]);
    }
    if (maxMag < 1e-6f) return; // silence, rien a capturer ce hop

    // Pics locaux (maxima), au-dessus d'un plancher relatif au pic max.
    float noiseFloor = maxMag * 0.02f;
    struct Peak { int bin; float mag; };
    std::vector<Peak> candidates;
    for (int k = 1; k < numBins; k++)
    {
      if (mag[k] < noiseFloor) continue;
      if (mag[k] > mag[k - 1] && mag[k] > mag[k + 1])
        candidates.push_back({ k, mag[k] });
    }

    // Garde les kMaxPeaks plus forts, puis re-trie par frequence
    // (stabilite d'un hop a l'autre pour un contenu harmonique typique).
    std::sort(candidates.begin(), candidates.end(), [](const Peak& a, const Peak& b) { return a.mag > b.mag; });
    if ((int)candidates.size() > kMaxPeaks) candidates.resize(kMaxPeaks);
    std::sort(candidates.begin(), candidates.end(), [](const Peak& a, const Peak& b) { return a.bin < b.bin; });

    for (int p = 0; p < kMaxPeaks; p++)
    {
      float targetFreq = 0.f, targetAmp = 0.f;
      if (p < (int)candidates.size())
      {
        targetFreq = (float)candidates[p].bin * (float)mSampleRate / (float)mFFTSize;
        targetAmp = candidates[p].mag / maxMag; // normalise 0..1 relatif au pic dominant
      }
      mPeakFreqSmooth[p] += (targetFreq - mPeakFreqSmooth[p]) * mSmoothCoeff;
      mPeakAmpSmooth[p] += (targetAmp - mPeakAmpSmooth[p]) * mSmoothCoeff;
    }
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
  static constexpr float kSmoothMs = 15.f;

  int mFFTSize = 2048;
  int mHopSize = 512;
  int mSamplesUntilHop = 512;
  int mWritePos = 0;
  double mSampleRate = 44100.0;
  float mSmoothCoeff = 0.5f;

  std::atomic<bool> mCapturing { false };

  std::vector<float> mRing, mWindow;
  std::vector<cplx> mCplx;

  float mPeakFreqSmooth[kMaxPeaks] = { 0.f };
  float mPeakAmpSmooth[kMaxPeaks] = { 0.f };
  float mPeakPhase[kMaxPeaks] = { 0.f };
};
