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

  // Distorsion de phase (a la Casio CZ) : 0.5 = neutre, ailleurs = un
  // point de la table se lit plus lentement (compresse), l'autre plus
  // vite (etire) - deforme QUAND chaque partie se joue, jamais son
  // amplitude. S'applique a la forme de base ET aux harmoniques
  // ensemble (meme phase deformee pour toutes).
  void SetSkew(float skew)
  {
    skew = std::clamp(skew, 0.001f, 0.999f);
    if (mSkew == skew) return;
    mSkew = skew;
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
      float rawPhase = (float)i / (float)kTableSize; // 0..1
      float phase = WarpPhase(rawPhase);
      float y = GetBaseShapeSample(phase);

      // Harmoniques 2 a 17 (l'harmonique 1/fondamentale est deja portee
      // par la forme de base elle-meme) - sinus purs, ajoutes par-dessus,
      // meme phase deformee (Skew) que la forme de base.
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
  // Point de rupture "skew" : avant, la portion 0..skew de la table se
  // lit compressee dans la premiere moitie ; apres, la portion skew..1
  // s'etire dans la seconde moitie. A skew=0.5, identite exacte.
  float WarpPhase(float phase) const
  {
    if (phase < mSkew)
      return phase * (0.5f / mSkew);
    else
      return 0.5f + (phase - mSkew) * (0.5f / (1.f - mSkew));
  }

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
  float mSkew = 0.5f; // neutre par defaut
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
