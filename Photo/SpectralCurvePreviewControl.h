#pragma once

#include "IControl.h"
#include <vector>
#include <string>
#include <algorithm>
#include <functional>
#include <cmath>
#include <cstdio>

// ============================================================================
// SpectralCurvePreviewControl
//
// Fenetre unique, bipolaire (-1 a 1) :
//  - En mode Dessin (SetDrawMode(true)) : interactive, on trace a la
//    souris (interpole entre les points pendant un glissement rapide) -
//    affiche le trace brut pendant qu'on dessine, prevenu au relachement.
//  - Sinon (ou une fois le trait relache) : affiche le resultat final
//    transforme (fourni via SetCurve, mis a jour depuis le thread
//    interface, jamais depuis l'audio).
//
// Axe X : toujours Hz (echelle log, 20Hz a 20kHz), commun a tous les
// modules. Axe Y : configurable via SetYAxisMarks (dB pour le Filtre,
// ms/sync pour le Delay...).
// ============================================================================

class SpectralCurvePreviewControl : public iplug::igraphics::IControl
{
public:
  using ShapeChangedFunc = std::function<void(const float*, int)>;

  struct AxisMark
  {
    float value; // position sur l'axe -1 a 1 (meme echelle que la courbe)
    std::string label;
    bool bold;
    bool danger = false; // rouge - marque un seuil critique (ex: minimum de delai effectif)
  };

  SpectralCurvePreviewControl(const iplug::igraphics::IRECT& bounds, ShapeChangedFunc onShapeChanged = nullptr)
  : IControl(bounds)
  , mOnShapeChanged(onShapeChanged)
  {
    mDrawnShape.assign(kDrawResolution, 0.f);
  }

  void SetDrawMode(bool drawMode) { mDrawMode = drawMode; SetDirty(false); }

  // Restaure un dessin depuis l'etat sauvegarde (distinct de DrawPointAt,
  // qui ne reagit qu'a la souris).
  void SetDrawnShapeExternal(const float* data, int size)
  {
    int n = std::min(size, (int)mDrawnShape.size());
    for (int i = 0; i < n; i++)
      mDrawnShape[i] = data[i];
    SetDirty(false);
  }

  void SetYAxisMarks(const std::vector<AxisMark>& marks) { mYMarks = marks; SetDirty(false); }

  // Spectre audio en fond (magnitude en dB par bande) - purement visuel,
  // dessine derriere la grille et la courbe.
  void SetSpectrumData(const float* magDb, int numBins, double sampleRate, int fftSize)
  {
    mSpectrumSize = std::min(numBins, kMaxSpectrumBins);
    for (int i = 0; i < mSpectrumSize; i++)
      mSpectrumDb[i] = magDb[i];
    mSpectrumSampleRate = sampleRate;
    mSpectrumFFTSize = fftSize;
    SetDirty(false);
  }

  // Resultat final transforme (affiche quand on n'est pas en train de
  // dessiner activement).
  void SetCurve(const float* buf, int size)
  {
    mResultSize = std::min(size, kMaxResultPoints);
    for (int i = 0; i < mResultSize; i++)
    {
      int srcIdx = (int)((float)i / (float)mResultSize * (float)size);
      srcIdx = std::min(srcIdx, size - 1);
      mResultBuffer[i] = buf[srcIdx];
    }
  }

  void OnMouseDown(float x, float y, const iplug::igraphics::IMouseMod& mod) override
  {
    if (!mDrawMode) return;
    mDrawing = true;
    mLastIdx = -1;
    DrawPointAt(x, y);
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const iplug::igraphics::IMouseMod& mod) override
  {
    if (mDrawMode && mDrawing) DrawPointAt(x, y);
  }

  void OnMouseUp(float x, float y, const iplug::igraphics::IMouseMod& mod) override
  {
    if (!mDrawMode) return;
    mDrawing = false;
    if (mOnShapeChanged) mOnShapeChanged(mDrawnShape.data(), (int)mDrawnShape.size());
  }

  void Draw(iplug::igraphics::IGraphics& g) override
  {
    using namespace iplug::igraphics;

    g.FillRect(IColor(255, 15, 15, 20), mRECT);

    float w = mRECT.W();
    float h = mRECT.H() * 0.42f;
    float midY = mRECT.MH();

    // --- Spectre audio en fond (silhouette discrete, dessinee AVANT tout
    // le reste pour rester derriere la grille et la courbe) ---
    if (mSpectrumSize > 1)
    {
      const float dbFloor = -80.f, dbCeil = 0.f;
      for (int k = 1; k < mSpectrumSize; k++) // saute la bande DC (k=0)
      {
        float freq = (float)k * (float)mSpectrumSampleRate / (float)mSpectrumFFTSize;
        if (freq < 20.f || freq > 20000.f) continue;
        float logPos = std::log(freq / 20.f) / std::log(20000.f / 20.f);
        float x = mRECT.L + w * logPos;
        float dbNorm = std::clamp((mSpectrumDb[k] - dbFloor) / (dbCeil - dbFloor), 0.f, 1.f);
        float barHeight = dbNorm * mRECT.H() * 0.9f;
        g.DrawLine(IColor(255, 40, 75, 65), x, mRECT.B, x, mRECT.B - barHeight, nullptr, 1.5f);
      }
    }

    // --- Reperes Hz fins (echelle log, 20Hz a 20kHz) : tous les 20Hz
    // jusque 100Hz, tous les 100Hz jusque 1000Hz, tous les 1k jusque 10k,
    // plus un repere a 15k.
    std::vector<float> minorFreqs;
    for (float f = 20.f; f <= 100.f; f += 20.f) minorFreqs.push_back(f);
    for (float f = 200.f; f <= 1000.f; f += 100.f) minorFreqs.push_back(f);
    for (float f = 2000.f; f <= 10000.f; f += 1000.f) minorFreqs.push_back(f);
    minorFreqs.push_back(15000.f);

    for (float f : minorFreqs)
    {
      float logPos = std::log(f / 20.f) / std::log(20000.f / 20.f);
      float x = mRECT.L + w * logPos;
      g.DrawLine(IColor(255, 30, 30, 35), x, mRECT.T, x, mRECT.B, nullptr, 1.f);
    }

    // --- Reperes Hz majeurs (100Hz/1k/10k), plus gros et plus epais ---
    const float majorFreqs[] = { 100.f, 1000.f, 10000.f };
    const char* majorLabels[] = { "100Hz", "1kHz", "10kHz" };
    IText majorFreqText(12.f, IColor(255, 190, 190, 200), "Roboto-Regular", EAlign::Center, EVAlign::Top);
    for (int m = 0; m < 3; m++)
    {
      float logPos = std::log(majorFreqs[m] / 20.f) / std::log(20000.f / 20.f);
      float x = mRECT.L + w * logPos;
      g.DrawLine(IColor(255, 70, 70, 78), x, mRECT.T, x, mRECT.B, nullptr, 2.f);
      g.DrawText(majorFreqText, majorLabels[m], IRECT(x - 30.f, mRECT.B - 18.f, x + 30.f, mRECT.B));
    }

    // --- Bornes 20Hz/20kHz (fines) ---
    const float edgeFreqs[] = { 20.f, 20000.f };
    const char* edgeLabels[] = { "20Hz", "20kHz" };
    IText edgeText(9.f, IColor(255, 110, 110, 120), "Roboto-Regular", EAlign::Center, EVAlign::Top);
    for (int m = 0; m < 2; m++)
    {
      float logPos = std::log(edgeFreqs[m] / 20.f) / std::log(20000.f / 20.f);
      float x = mRECT.L + w * logPos;
      g.DrawLine(IColor(255, 40, 40, 45), x, mRECT.T, x, mRECT.B, nullptr, 1.f);
      g.DrawText(edgeText, edgeLabels[m], IRECT(x - 25.f, mRECT.B - 14.f, x + 25.f, mRECT.B));
    }

    // --- Reperes Y (fournis par l'appelant - dB, ms, ou divisions sync) ---
    IText yText(9.f, IColor(255, 130, 130, 140), "Roboto-Regular", EAlign::Near, EVAlign::Middle);
    IText yTextBold(11.f, IColor(255, 190, 190, 200), "Roboto-Regular", EAlign::Near, EVAlign::Middle);
    // Divisions ternaires (label finissant par "T") en teinte ambree, pour
    // les distinguer d'un coup d'oeil des divisions binaires.
    IText yTextTernary(9.f, IColor(255, 200, 160, 100), "Roboto-Regular", EAlign::Near, EVAlign::Middle);
    for (const auto& mark : mYMarks)
    {
      bool isTernary = !mark.label.empty() && mark.label.back() == 'T';
      float y = midY - mark.value * h;
      IColor lineColor = mark.danger ? IColor(255, 210, 50, 50)
                       : isTernary ? IColor(255, 90, 70, 45) : IColor(255, 40, 40, 45);
      g.DrawLine(lineColor, mRECT.L, y, mRECT.R, y, nullptr, mark.bold ? 2.f : 1.f);
      IText yTextDanger(9.f, IColor(255, 230, 70, 70), "Roboto-Regular", EAlign::Near, EVAlign::Middle);
      const IText& text = mark.danger ? yTextDanger : isTernary ? yTextTernary : (mark.bold ? yTextBold : yText);
      g.DrawText(text, mark.label.c_str(), IRECT(mRECT.L + 2.f, y - 7.f, mRECT.L + 60.f, y + 7.f));
    }

    g.DrawLine(IColor(255, 60, 60, 65), mRECT.L, midY, mRECT.R, midY, nullptr, 1.f);

    if (mDrawMode && mDrawing)
    {
      // Pendant le trace : affiche le dessin brut, tel quel.
      int n = (int)mDrawnShape.size();
      for (int i = 0; i < n - 1; i++)
      {
        float x0 = mRECT.L + w * (float)i / (float)(n - 1);
        float x1 = mRECT.L + w * (float)(i + 1) / (float)(n - 1);
        float y0 = midY - mDrawnShape[i] * h;
        float y1 = midY - mDrawnShape[i + 1] * h;
        g.DrawLine(IColor(255, 200, 160, 130), x0, y0, x1, y1, nullptr, 1.5f);
      }
    }
    else
    {
      // Sinon : affiche le resultat final transforme.
      if (mResultSize < 2) return;
      for (int i = 0; i < mResultSize - 1; i++)
      {
        float x0 = mRECT.L + w * (float)i / (float)(mResultSize - 1);
        float x1 = mRECT.L + w * (float)(i + 1) / (float)(mResultSize - 1);
        float y0 = midY - mResultBuffer[i] * h;
        float y1 = midY - mResultBuffer[i + 1] * h;
        g.DrawLine(IColor(255, 130, 200, 160), x0, y0, x1, y1, nullptr, 1.5f);
      }
    }
  }

private:
  void DrawPointAt(float mx, float my)
  {
    float xFrac = std::clamp((mx - mRECT.L) / mRECT.W(), 0.f, 1.f);
    float yVal = std::clamp(-(my - mRECT.MH()) / (mRECT.H() * 0.42f), -1.f, 1.f);
    int idx = std::clamp((int)(xFrac * (kDrawResolution - 1)), 0, kDrawResolution - 1);

    if (mLastIdx >= 0 && mLastIdx != idx)
    {
      int lo = std::min(mLastIdx, idx);
      int hi = std::max(mLastIdx, idx);
      float loVal = (mLastIdx < idx) ? mLastY : yVal;
      float hiVal = (mLastIdx < idx) ? yVal : mLastY;
      for (int i = lo; i <= hi; i++)
      {
        float t = (hi > lo) ? (float)(i - lo) / (float)(hi - lo) : 0.f;
        mDrawnShape[i] = loVal + t * (hiVal - loVal);
      }
    }
    else
    {
      mDrawnShape[idx] = yVal;
    }

    mLastIdx = idx;
    mLastY = yVal;
    SetDirty(false);
  }

  static constexpr int kDrawResolution = 128;
  static constexpr int kMaxResultPoints = 512;

  bool mDrawMode = false;
  bool mDrawing = false;
  int mLastIdx = -1;
  float mLastY = 0.f;
  std::vector<float> mDrawnShape;

  float mResultBuffer[kMaxResultPoints] = { 0.f };
  int mResultSize = 0;

  std::vector<AxisMark> mYMarks;

  static constexpr int kMaxSpectrumBins = 1100;
  float mSpectrumDb[kMaxSpectrumBins] = { -80.f };
  int mSpectrumSize = 0;
  double mSpectrumSampleRate = 44100.0;
  int mSpectrumFFTSize = 2048;

  ShapeChangedFunc mOnShapeChanged;
};
