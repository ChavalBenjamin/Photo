#pragma once

#include <cmath>

// ============================================================================
// Tempérament : correction en cents par classe de hauteur (12) + correction
// par octave (11, octaves MIDI 0..10 : touche / 12). Reference fixe : La 440 Hz
// (MIDI 69). Tout a 0 = tempérament égal.
// ============================================================================
namespace tuning {

static constexpr int kNumPitchClasses = 12; // 0 = Do, 1 = Do#, ... 11 = Si
static constexpr int kNumOctaves = 11;      // touche / 12 : 0..10

// Pitch multipliers du menu Octave (index = valeur du parametre enum).
// L'ordre des 9 premiers est historique (projets deja enregistres).
static constexpr int kNumPitchChoices = 15;
static constexpr float kPitchChoices[kNumPitchChoices] = {
  1.f / 16.f, 1.f / 8.f, 1.f / 4.f, 1.f / 2.f, 1.f, 2.f, 4.f, 8.f, 16.f,
  1.125f, 1.2f, 1.25f, 1.5f, 3.f, 1.f / 3.f
};
inline float PitchMultiplier(int idx)
{
  if (idx < 0) idx = 0;
  if (idx >= kNumPitchChoices) idx = kNumPitchChoices - 1;
  return kPitchChoices[idx];
}

// Frequence (Hz) d'une touche MIDI.
inline double Frequency(int note, const double* pcCents, const double* octCents)
{
  if (note < 0) note = 0;
  if (note > 127) note = 127;
  const int pc = note % 12;
  const int oc = note / 12;
  const double cents = pcCents[pc] + octCents[oc];
  return 440.0 * std::pow(2.0, ((double)(note - 69)) / 12.0 + cents / 1200.0);
}

// Rapport de transposition d'une touche par rapport a la touche 60
// (Do central = photo telle quelle).
inline double Ratio(int note, const double* pcCents, const double* octCents)
{
  return Frequency(note, pcCents, octCents) / Frequency(60, pcCents, octCents);
}

// La Monte Young - The Well-Tuned Piano. Hauteur (en cents au-dessus de la
// tonique Mi bemol = 1/1) de chacune des 12 touches, a partir de la tonique :
// touche 1 = Mi, 2 = Fa, ... 11 = Re, 12 = Mi bemol (1200).
// Table fournie par l'utilisateur (ratios 567/512, 9/8, 147/128, 21/16,
// 1323/1024, 189/128, 3/2, 49/32, 7/4, 441/256, 63/32, 2/1).
static constexpr double kYoungCents[13] = {
  0.0,
  176.645910, 203.910002, 239.606814, 470.780907, 443.516816, 674.690909,
  701.955001, 737.651813, 968.825906, 941.561815, 1172.735908, 1200.0
};

// Ecart par classe de hauteur (0 = Do) par rapport au tempérament égal,
// La = 440 Hz fixe, Mi bemol (pc 3) = tonique.
inline void YoungDeviations(double out[12])
{
  const double aOffset = kYoungCents[6] - 600.0; // la touche 6 (= La) reste a 440 Hz
  for (int pc = 0; pc < 12; pc++)
  {
    int k = ((pc - 3) % 12 + 12) % 12; // degre au-dessus de Mi bemol
    if (k == 0) k = 12;
    out[pc] = (kYoungCents[k] - 100.0 * k) - aOffset;
  }
}

} // namespace tuning
