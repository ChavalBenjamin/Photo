#pragma once

#include <vector>
#include <cmath>
#include <algorithm>

// ============================================================================
// WavetableEngine
//
// Synthese additive simple : une forme de base (Sinus/Saw/Triangle/Carre)
// plus 16 harmoniques individuellement reglables (amplitude 0-1 chacune),
// ajoutees par-dessus. Construit une seule table (un cycle complet), lue
// ensuite par un oscillateur a accumulateur de phase.
//
// Pas de limitation de bande (anti-repliement) a ce stade - simplification
// assumee pour cette premiere etape, a reprendre plus tard si besoin
// (mipmapping de tables a differentes resolutions selon la frequence).
// ============================================================================

class WavetableEngine
{
public:
  enum class BaseShape { Sine, Saw, Triangle, Square };
  static constexpr int kNumHarmonics = 16;
  static constexpr int kTableSize = 2048;

  void SetBaseShape(BaseShape shape)
  {
    if (shape == mBaseShape) return;
    mBaseShape = shape;
    mDirty = true;
  }

  void SetHarmonicAmp(int index, float amp)
  {
    if (index < 0 || index >= kNumHarmonics) return;
    amp = std::clamp(amp, 0.f, 1.f);
    if (mHarmonicAmps[index] == amp) return;
    mHarmonicAmps[index] = amp;
    mDirty = true;
  }

  const float* GetTable() const { return mTable.data(); }
  int GetTableSize() const { return kTableSize; }

  void RebuildIfNeeded()
  {
    if (!mDirty) return;
    mDirty = false;

    if (mTable.empty())
      mTable.assign(kTableSize, 0.f);

    for (int i = 0; i < kTableSize; i++)
    {
      float phase = (float)i / (float)kTableSize; // 0..1
      float y = GetBaseShapeSample(phase);

      // Harmoniques 2 a 17 (l'harmonique 1/fondamentale est deja portee
      // par la forme de base elle-meme) - sinus purs, ajoutes par-dessus.
      for (int h = 0; h < kNumHarmonics; h++)
      {
        if (mHarmonicAmps[h] <= 0.0001f) continue;
        int harmonicNumber = h + 2;
        y += mHarmonicAmps[h] * std::sin(2.f * (float)M_PI * (float)harmonicNumber * phase);
      }

      mTable[i] = y;
    }

    // Normalisation : evite l'ecretage si beaucoup d'harmoniques
    // s'additionnent en phase - preserve la forme relative.
    float peak = 0.f;
    for (float v : mTable) peak = std::max(peak, std::abs(v));
    if (peak > 1.f)
      for (float& v : mTable) v /= peak;
  }

private:
  float GetBaseShapeSample(float phase) const
  {
    switch (mBaseShape)
    {
      case BaseShape::Sine:
        return std::sin(2.f * (float)M_PI * phase);
      case BaseShape::Saw:
        return 2.f * phase - 1.f; // rampe -1..1, naive (non bandlimited)
      case BaseShape::Triangle:
        return (phase < 0.5f) ? (4.f * phase - 1.f) : (3.f - 4.f * phase);
      case BaseShape::Square:
        return (phase < 0.5f) ? 1.f : -1.f;
    }
    return 0.f;
  }

  BaseShape mBaseShape = BaseShape::Sine;
  float mHarmonicAmps[kNumHarmonics] = { 0.f };
  bool mDirty = true;
  std::vector<float> mTable;
};

// ============================================================================
// WavetableOscillator
//
// Lit une WavetableEngine via un accumulateur de phase, a une frequence
// donnee. Une instance par voix.
// ============================================================================

class WavetableOscillator
{
public:
  void SetSampleRate(double sr) { mSampleRate = sr; }
  void SetFrequency(float freqHz) { mFrequency = freqHz; }
  void NoteOn(float freqHz) { mFrequency = freqHz; mActive = true; }
  void NoteOff() { mActive = false; }
  bool IsActive() const { return mActive; }

  float Process(const WavetableEngine& engine)
  {
    if (!mActive) return 0.f;

    const float* table = engine.GetTable();
    int size = engine.GetTableSize();

    float pos = mPhase * (float)size;
    int idx0 = (int)pos;
    int idx1 = (idx0 + 1) % size;
    float frac = pos - (float)idx0;
    float sample = table[idx0] * (1.f - frac) + table[idx1] * frac;

    mPhase += mFrequency / (float)mSampleRate;
    if (mPhase >= 1.f) mPhase -= 1.f;

    return sample;
  }

private:
  double mSampleRate = 44100.0;
  float mFrequency = 440.f;
  float mPhase = 0.f;
  bool mActive = false;
};
