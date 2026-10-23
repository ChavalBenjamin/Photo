#pragma once

#include <cmath>

// ============================================================================
// Tempérament : 12 corrections en cents (par degre a partir d'une note racine)
// + une projection d'octave cumulative. Base : tempérament égal, La 440 Hz.
// Tout a 0 = tempérament égal.
// ============================================================================
namespace tuning {

static constexpr int kNumPitchClasses = 12; // 0 = Do, 1 = Do#, ... 11 = Si

// Pitch multipliers du menu Octave (index = valeur du parametre enum),
// par ordre croissant. Index 5 = "1" (neutre, valeur par defaut).
static constexpr int kNumPitchChoices = 15;
static constexpr int kPitchNeutralIdx = 5;
static constexpr float kPitchChoices[kNumPitchChoices] = {
  1.f / 16.f, 1.f / 8.f, 1.f / 4.f, 1.f / 3.f, 1.f / 2.f, 1.f,
  1.125f, 1.2f, 1.25f, 1.5f, 2.f, 3.f, 4.f, 8.f, 16.f
};
inline float PitchMultiplier(int idx)
{
  if (idx < 0) idx = 0;
  if (idx >= kNumPitchChoices) idx = kNumPitchChoices - 1;
  return kPitchChoices[idx];
}

// Frequence (Hz) d'une touche MIDI.
//  dev[12]  : correction en cents par DEGRE a partir de la racine (0 = racine)
//  root     : classe de hauteur de la racine (0 = Do ... 11 = Si)
//  globalCents  : hauteur globale (decalage de toute l'echelle, en cents)
//  stretchCents : projection d'octave, cumulative, continue : + stretch cents
//                 par octave au-dessus de la touche 60, - en dessous.
// Base : tempérament égal, La 440 Hz.
inline double Frequency(int note, const double* dev, int root, double stretchCents, double globalCents = 0.0)
{
  if (note < 0) note = 0;
  if (note > 127) note = 127;
  const int deg = (((note % 12) - root) % 12 + 12) % 12;
  const double cents = dev[deg] + stretchCents * ((double)(note - 60)) / 12.0 + globalCents;
  return 440.0 * std::pow(2.0, ((double)(note - 69)) / 12.0 + cents / 1200.0);
}

// Rapport de transposition d'une touche par rapport a la touche 60
// (Do central = photo telle quelle).
inline double Ratio(int note, const double* dev, int root, double stretchCents)
{
  return Frequency(note, dev, root, stretchCents) / Frequency(60, dev, root, stretchCents);
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

// Preset Young : valeurs "pures" (ecart de chaque degre par rapport au
// tempérament égal, la racine a 0 ct), racine = Mi bemol (pc 3).
// Pour retrouver le La = 440 Hz de la piece, la hauteur globale est reglee a
// kYoungGlobal (le Mi bemol de Young est 74,69 ct sous le Mi bemol egal).
static constexpr int kYoungRoot = 3;
static constexpr double kYoungGlobal = -(674.690909 - 600.0);
inline void YoungDeviations(double out[12])
{
  for (int k = 0; k < 12; k++)
  {
    const int kk = (k == 0) ? 12 : k;
    out[k] = kYoungCents[kk] - 100.0 * kk;
  }
}

} // namespace tuning
