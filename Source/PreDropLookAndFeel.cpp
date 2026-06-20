#include "PreDropLookAndFeel.h"
#include "PreDropTheme.h"

#include <cmath>

using namespace PreDrop;

namespace
{
    float textWidth (const juce::Font& font, const juce::String& text)
    {
        juce::GlyphArrangement arrangement;
        arrangement.addLineOfText (font, text, 0.0f, 0.0f);
        return arrangement.getBoundingBox (0, -1, true).getWidth();
    }
}

PreDropLookAndFeel::PreDropLookAndFeel() = default;

void PreDropLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                           float sliderPosProportional, float rotaryStartAngle,
                                           float rotaryEndAngle, juce::Slider&)
{
    // Self-scaling from the actual footprint (authored at 172 px).
    const float s = (float) width / 172.0f;
    const juce::Point<float> centre ((float) x + (float) width * 0.5f,
                                     (float) y + (float) height * 0.5f);

    const float ringRadius = 72.5f * s;   // mid of the 67..78 px ring band
    const float ringThick  = 11.0f * s;
    const float amount     = juce::jlimit (0.0f, 1.0f, sliderPosProportional);
    const float curAngle   = rotaryStartAngle + (rotaryEndAngle - rotaryStartAngle) * amount;

    // --- 1. Track arc (full 270°) -------------------------------------------
    {
        juce::Path track;
        track.addCentredArc (centre.x, centre.y, ringRadius, ringRadius, 0.0f,
                             rotaryStartAngle, rotaryEndAngle, true);
        g.setColour (Palette::ringTrack);
        g.strokePath (track, juce::PathStrokeType (ringThick, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
    }

    // --- 2. Value arc: warm glow underlay + multicolour segments -------------
    if (amount > 0.0005f)
    {
        juce::Path value;
        value.addCentredArc (centre.x, centre.y, ringRadius, ringRadius, 0.0f,
                             rotaryStartAngle, curAngle, true);
        g.setColour (Palette::arcGlow);
        g.strokePath (value, juce::PathStrokeType (ringThick + 6.0f * s, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));

        const int   segments = juce::jmax (2, (int) std::ceil (amount * 96.0f));
        const float sweep    = curAngle - rotaryStartAngle;
        const float overlap  = (sweep / (float) segments) * 0.6f;

        for (int i = 0; i < segments; ++i)
        {
            const float f0 = (float) i / (float) segments;
            const float a0 = rotaryStartAngle + sweep * f0;
            // Overlap covers the seams between segments, but clamp the very last
            // one to curAngle so the lit tip ends exactly at the pointer (the
            // reference's conic gradient cuts hard to transparent there).
            const float a1 = juce::jmin (curAngle,
                                         rotaryStartAngle + sweep * ((float) (i + 1) / (float) segments) + overlap);

            juce::Path seg;
            seg.addCentredArc (centre.x, centre.y, ringRadius, ringRadius, 0.0f, a0, a1, true);
            g.setColour (ColourMath::arc (f0));
            g.strokePath (seg, juce::PathStrokeType (ringThick, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
        }
    }

    // --- 3. Knob body -------------------------------------------------------
    const float bodyRadius = 62.0f * s;
    const auto  bodyBounds = juce::Rectangle<float> (bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre (centre);

    {
        juce::Path bodyPath;
        bodyPath.addEllipse (bodyBounds);
        juce::DropShadow (juce::Colours::black.withAlpha (0.5f), (int) (22.0f * s),
                          { 0, (int) (8.0f * s) }).drawForPath (g, bodyPath);
    }

    {
        // Radial fill, highlight biased to 40% / 30% like the CSS gradient.
        const juce::Point<float> hi (bodyBounds.getX() + 0.40f * bodyBounds.getWidth(),
                                     bodyBounds.getY() + 0.30f * bodyBounds.getHeight());
        juce::ColourGradient body (Palette::knobBodyHi, hi.x, hi.y,
                                   Palette::knobBodyLo, centre.x, centre.y + bodyRadius, true);
        body.addColour (0.6, ColourMath::lerp (Palette::knobBodyHi, Palette::knobBodyLo, 0.6f));
        g.setGradientFill (body);
        g.fillEllipse (bodyBounds);

        // Rim darkening to suggest the inset/inner shadow.
        juce::ColourGradient rim (juce::Colours::transparentBlack, centre.x, centre.y,
                                  juce::Colours::black.withAlpha (0.55f), centre.x, centre.y - bodyRadius, true);
        rim.addColour (0.72, juce::Colours::transparentBlack);
        g.setGradientFill (rim);
        g.fillEllipse (bodyBounds);

        g.setColour (Palette::knobBorder);
        g.drawEllipse (bodyBounds, juce::jmax (1.0f, 1.0f * s));
    }

    // --- 4. Pointer dot -----------------------------------------------------
    {
        const float dotRadius = 4.5f * s;
        const float dotDist   = 45.5f * s;     // sits near the top edge of the body
        const juce::Point<float> dot (centre.x + dotDist * std::sin (curAngle),
                                      centre.y - dotDist * std::cos (curAngle));

        juce::Path dotPath;
        dotPath.addEllipse (juce::Rectangle<float> (dotRadius * 2.0f, dotRadius * 2.0f).withCentre (dot));
        juce::DropShadow (juce::Colours::white.withAlpha (0.55f), (int) (10.0f * s), {}).drawForPath (g, dotPath);

        const float haloRadius = dotRadius + 3.0f * s;
        g.setColour (juce::Colour::fromFloatRGBA (1.0f, 1.0f, 1.0f, 0.12f));
        g.fillEllipse (juce::Rectangle<float> (haloRadius * 2.0f, haloRadius * 2.0f).withCentre (dot));

        g.setColour (juce::Colours::white);
        g.fillEllipse (juce::Rectangle<float> (dotRadius * 2.0f, dotRadius * 2.0f).withCentre (dot));
    }

    // --- 5. Centred readout -------------------------------------------------
    drawKnobReadout (g, centre, amount, s);
}

void PreDropLookAndFeel::drawKnobReadout (juce::Graphics& g, juce::Point<float> centre,
                                          float amount, float scale) const
{
    const int pct = juce::roundToInt (amount * 100.0f);

    const auto numberFont = Fonts::sans (38.0f * scale, true);
    const auto pctFont    = Fonts::sans (16.0f * scale, false);
    const auto labelFont  = Fonts::sans (10.0f * scale, false, 0.22f);

    const juce::String numStr (pct);
    const juce::String pctStr ("%");

    const float numW   = textWidth (numberFont, numStr);
    const float pctW   = textWidth (pctFont, pctStr);
    const float gap    = 2.0f * scale;
    const float totalW = numW + gap + pctW;

    const float lineH  = 38.0f * scale;
    const float top    = centre.y - 26.0f * scale;
    const float startX = centre.x - totalW * 0.5f;

    g.setFont (numberFont);
    g.setColour (Palette::textStrong);
    g.drawText (numStr, juce::Rectangle<float> (startX, top, numW, lineH),
                juce::Justification::bottomLeft, false);

    // Baseline-align the "%" with the big number: bottomLeft anchors baseline+descent
    // to the rect bottom, and the two fonts have different descents, so lift the "%"
    // rect's bottom by the descent delta to put both baselines on the same line.
    const float pctLift = numberFont.getDescent() - pctFont.getDescent();
    g.setFont (pctFont);
    g.setColour (Palette::percentSign);
    g.drawText (pctStr, juce::Rectangle<float> (startX + numW + gap, top, pctW, lineH - pctLift),
                juce::Justification::bottomLeft, false);

    g.setFont (labelFont);
    g.setColour (Palette::textDim);
    g.drawText ("AMOUNT",
                juce::Rectangle<float> (centre.x - 60.0f * scale, top + lineH + 4.0f * scale,
                                        120.0f * scale, 14.0f * scale),
                juce::Justification::centredTop, false);
}

void PreDropLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                           float sliderPos, float /*minSliderPos*/, float /*maxSliderPos*/,
                                           juce::Slider::SliderStyle, juce::Slider&)
{
    const float s = uiScale;
    const float trackThickness = 4.0f * s;
    const float centreY = (float) y + (float) height * 0.5f;
    const float left    = (float) x;
    const float right   = (float) x + (float) width;

    const juce::Rectangle<float> trackRect (left, centreY - trackThickness * 0.5f,
                                            (float) width, trackThickness);
    g.setColour (Palette::sliderTrack);
    g.fillRoundedRectangle (trackRect, trackThickness * 0.5f);

    const float fillRight = juce::jlimit (left, right, sliderPos);
    if (fillRight > left + 0.5f)
    {
        const juce::Rectangle<float> fillRect (left, centreY - trackThickness * 0.5f,
                                               fillRight - left, trackThickness);
        juce::ColourGradient grad (Palette::cyan, left, 0.0f, Palette::violet, right, 0.0f, false);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (fillRect, trackThickness * 0.5f);
    }

    const float thumbRadius = 6.5f * s;
    const juce::Point<float> thumb (fillRight, centreY);

    juce::Path thumbPath;
    thumbPath.addEllipse (juce::Rectangle<float> (thumbRadius * 2.0f, thumbRadius * 2.0f).withCentre (thumb));
    juce::DropShadow (juce::Colours::black.withAlpha (0.5f), (int) juce::jmax (1.0f, 4.0f * s),
                      { 0, (int) juce::jmax (1.0f, 1.0f * s) }).drawForPath (g, thumbPath);

    g.setColour (Palette::textStrong);
    g.fillEllipse (juce::Rectangle<float> (thumbRadius * 2.0f, thumbRadius * 2.0f).withCentre (thumb));
}

void PreDropLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                               const juce::Colour&, bool shouldDrawButtonAsHighlighted,
                                               bool shouldDrawButtonAsDown)
{
    const float s      = uiScale;
    const auto  bounds = button.getLocalBounds().toFloat().reduced (juce::jmax (0.5f, s));
    const float radius = 10.0f * s;                 // --radius-button
    const float line   = juce::jmax (1.0f, s);
    const bool  on     = button.getToggleState();

    juce::Path pill;
    pill.addRoundedRectangle (bounds, radius);

    if (on)
    {
        // Engaged: accent gradient fill (#5e93ff -> #8b7bff) + outer accent glow.
        juce::DropShadow (Palette::badge.withAlpha (0.55f), (int) (22.0f * s), {})
            .drawForPath (g, pill);

        juce::ColourGradient grad (Palette::badge, bounds.getX(), bounds.getY(),
                                   Palette::violet, bounds.getX(), bounds.getBottom(), false);
        g.setGradientFill (grad);
        g.fillPath (pill);

        g.setColour (juce::Colour::fromFloatRGBA (0.59f, 0.67f, 1.0f, 0.6f)); // accent border
        g.strokePath (pill, juce::PathStrokeType (line));
    }
    else
    {
        // Idle: flat white-alpha fill (--layer-2) + hairline border (--layer-5).
        const float fill = shouldDrawButtonAsDown ? 0.10f
                          : shouldDrawButtonAsHighlighted ? 0.08f : 0.06f;
        g.setColour (juce::Colour::fromFloatRGBA (1.0f, 1.0f, 1.0f, fill));
        g.fillPath (pill);

        // --inset-top highlight line.
        g.setColour (juce::Colour::fromFloatRGBA (1.0f, 1.0f, 1.0f, 0.06f));
        g.drawLine (bounds.getX() + radius, bounds.getY() + line,
                    bounds.getRight() - radius, bounds.getY() + line, line);

        g.setColour (juce::Colour::fromFloatRGBA (1.0f, 1.0f, 1.0f, 0.12f));
        g.strokePath (pill, juce::PathStrokeType (line));
    }
}

void PreDropLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button,
                                         bool, bool)
{
    // GlowButton language: a small square status dot + caption, centred together.
    const float s    = uiScale;
    const bool  on   = button.getToggleState();
    const auto  area = button.getLocalBounds().toFloat();

    const auto  font = Fonts::sans (10.0f * s, true, 0.16f);   // bold, --tracking-cap
    const juce::String text = button.getButtonText();
    const float tw   = textWidth (font, text);
    const float dot  = 6.0f * s;
    const float gap  = 6.0f * s;
    const float xL   = area.getCentreX() - (dot + gap + tw) * 0.5f;
    const float cy   = area.getCentreY();

    const juce::Colour dotCol  = on ? juce::Colours::white : Palette::textStrong;
    const juce::Colour glowCol = on ? juce::Colours::white : Palette::badge;

    juce::Rectangle<float> dotRect (xL, cy - dot * 0.5f, dot, dot);
    juce::Path dotPath;
    dotPath.addRoundedRectangle (dotRect, 1.5f * s);
    juce::DropShadow (glowCol.withAlpha (on ? 0.9f : 0.6f), (int) (8.0f * s), {})
        .drawForPath (g, dotPath);
    g.setColour (dotCol);
    g.fillRoundedRectangle (dotRect, 1.5f * s);

    g.setFont (font);
    g.setColour (on ? juce::Colours::white : Palette::textStrong);
    g.drawText (text, juce::Rectangle<float> (xL + dot + gap, area.getY(), tw + 2.0f * s, area.getHeight()),
                juce::Justification::centredLeft, false);
}

void DepthKnobLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                             float sliderPosProportional, float rotaryStartAngle,
                                             float rotaryEndAngle, juce::Slider& slider)
{
    const float s = uiScale;
    const juce::Point<float> centre ((float) x + (float) width * 0.5f,
                                     (float) y + (float) height * 0.5f);

    const float ringRadius = juce::jmin ((float) width, (float) height) * 0.5f - 3.0f * s;
    const float ringThick  = 3.5f * s;
    const float amount     = juce::jlimit (0.0f, 1.0f, sliderPosProportional);
    const float curAngle   = rotaryStartAngle + (rotaryEndAngle - rotaryStartAngle) * amount;

    const juce::Colour accent = slider.findColour (juce::Slider::rotarySliderFillColourId);

    // --- Recessed disc behind the arc (Dial's rgba(8,11,18,0.6) seat) --------
    g.setColour (juce::Colour::fromFloatRGBA (0.031f, 0.043f, 0.071f, 0.6f));
    g.fillEllipse (juce::Rectangle<float> (ringRadius * 2.0f, ringRadius * 2.0f).withCentre (centre));

    // --- Track arc (full sweep) ---------------------------------------------
    {
        juce::Path track;
        track.addCentredArc (centre.x, centre.y, ringRadius, ringRadius, 0.0f,
                             rotaryStartAngle, rotaryEndAngle, true);
        g.setColour (Palette::ringTrack);
        g.strokePath (track, juce::PathStrokeType (ringThick, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
    }

    // --- Value arc in the effect colour, with a glow that scales with value --
    if (amount > 0.0005f)
    {
        juce::Path value;
        value.addCentredArc (centre.x, centre.y, ringRadius, ringRadius, 0.0f,
                             rotaryStartAngle, curAngle, true);
        const float glow = (3.0f + amount * 6.0f) * s;   // ~ drop-shadow(0 0 3+v*6)
        g.setColour (accent.withAlpha (0.25f));
        g.strokePath (value, juce::PathStrokeType (ringThick + glow, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
        g.setColour (accent);
        g.strokePath (value, juce::PathStrokeType (ringThick, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
    }

    // --- Filled core that scales + glows with value (no body, no pointer) ----
    {
        const float coreRadius = (4.0f + amount * 5.0f) * s;   // Dial: r = 4 + value*5
        const auto  coreBounds = juce::Rectangle<float> (coreRadius * 2.0f, coreRadius * 2.0f).withCentre (centre);
        juce::Path corePath;
        corePath.addEllipse (coreBounds);
        juce::DropShadow (accent.withAlpha (0.9f), (int) (6.0f * s), {}).drawForPath (g, corePath);
        g.setColour (accent);
        g.fillEllipse (coreBounds);
    }
}
