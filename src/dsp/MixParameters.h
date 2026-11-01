#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string_view>

namespace snds::dsp
{
/** The real-time mix parameters: stable IDs, ranges, defaults and smoothing times.

    Every value is a plain float in its natural unit (dB, Hz, 0..1, BPM, ...). The mix state is
    ephemeral: these defaults are what every song starts with and what resetToDefaults restores.
    The IDs are a stable contract (fixtures and UI rely on them); do not rename them.
*/
inline constexpr int numStems = 4;

enum class Stem : int
{
    drums,
    bass,
    other,
    vocals
};

inline constexpr std::array<std::string_view, numStems> stemIds { "drums", "bass", "other", "vocals" };
inline constexpr std::array<std::string_view, numStems> stemNames { "Drums", "Bass", "Other", "Vocals" };

enum class StemControl : int
{
    gain,
    mute,
    solo,
    pan,
    lowCut,
    highCut,
    reverbSend,
    delaySend
};

inline constexpr int numStemControls = 8;

enum class GlobalControl : int
{
    reverbReturn,
    delayDivision,
    delayFeedback,
    delayReturn,
    masterGain,
    tempoBpm
};

inline constexpr int numGlobalControls = 6;
inline constexpr int numParameters = numStems * numStemControls + numGlobalControls;

constexpr int parameterIndex (Stem stem, StemControl control) noexcept
{
    return static_cast<int> (stem) * numStemControls + static_cast<int> (control);
}

constexpr int parameterIndex (GlobalControl control) noexcept
{
    return numStems * numStemControls + static_cast<int> (control);
}

//==============================================================================
// Ranges. Gain minimum is treated as minus infinity: a value at or below it is exact silence.
inline constexpr float gainMinDb = -60.0f; // == -inf
inline constexpr float gainMaxDb = 6.0f;
inline constexpr float panMin = -1.0f;
inline constexpr float panMax = 1.0f;
inline constexpr float lowCutMinHz = 20.0f; // bypassed at this endpoint
inline constexpr float lowCutMaxHz = 1000.0f;
inline constexpr float highCutMinHz = 500.0f;
inline constexpr float highCutMaxHz = 20000.0f; // bypassed at this endpoint
// Lowered from 0.9 after measuring headroom: the loop's worst-case build-up is 1 / (1 - max), so
// 0.6 keeps a resonant tone under 2.5x (0.9 allowed 10x and drove the limiter 40 dB).
inline constexpr float maxFeedback = 0.6f;
inline constexpr float tempoMinBpm = 20.0f;
inline constexpr float tempoMaxBpm = 400.0f;
inline constexpr float tempoDefaultBpm = 120.0f;

enum class DelayDivision : int
{
    quarter,
    eighth,
    dottedEighth,
    eighthTriplet,
    sixteenth
};

inline constexpr int numDelayDivisions = 5;

/** Length of a division in beats (quarter notes). */
constexpr float delayDivisionBeats (DelayDivision division) noexcept
{
    switch (division)
    {
        case DelayDivision::quarter:
            return 1.0f;
        case DelayDivision::eighth:
            return 0.5f;
        case DelayDivision::dottedEighth:
            return 0.75f;
        case DelayDivision::eighthTriplet:
            return 1.0f / 3.0f;
        case DelayDivision::sixteenth:
            return 0.25f;
    }
    return 1.0f;
}

constexpr double delayTimeSeconds (double bpm, DelayDivision division) noexcept
{
    return 60.0 / bpm * static_cast<double> (delayDivisionBeats (division));
}

/** The longest delay any BPM/division pair can ask for (slowest BPM, longest division). */
constexpr double maxDelayTimeSeconds() noexcept
{
    return delayTimeSeconds (static_cast<double> (tempoMinBpm), DelayDivision::quarter);
}

//==============================================================================
enum class ParameterKind
{
    continuous,
    toggle, // 0 or 1
    choice  // integer 0..n-1
};

enum class ParameterScale
{
    linear,
    decibel,    // normalised linearly in dB
    logarithmic // normalised linearly in log(value)
};

enum class ParameterUnit
{
    none,
    decibels,
    hertz,
    percent,
    pan,
    bpm,
    division,
    onOff
};

/** Small fixed-capacity string usable in constant expressions. */
template <std::size_t Capacity>
struct FixedString
{
    std::array<char, Capacity> chars {};
    std::size_t length = 0;

    constexpr FixedString& append (std::string_view text) noexcept
    {
        for (const auto c : text)
            if (length < Capacity)
                chars[length++] = c;
        return *this;
    }

    constexpr std::string_view view() const noexcept { return { chars.data(), length }; }
};

struct ParameterSpec
{
    FixedString<32> id;   // e.g. "stem.drums.gain"
    FixedString<32> name; // e.g. "Drums gain"
    ParameterKind kind = ParameterKind::continuous;
    ParameterScale scale = ParameterScale::linear;
    ParameterUnit unit = ParameterUnit::none;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float defaultValue = 0.0f;

    /** Time constant of the audible smoothing in seconds; 0 means the DSP handles the change
        itself (tempo/division are crossfaded by the delay, mute/solo are ramped as gains). */
    float smoothingSeconds = 0.0f;
};

namespace detail
{
constexpr ParameterSpec makeStemSpec (Stem stem, StemControl control) noexcept
{
    const auto s = static_cast<std::size_t> (stem);
    ParameterSpec spec;
    spec.id.append ("stem.").append (stemIds[s]).append (".");
    spec.name.append (stemNames[s]).append (" ");

    const auto set = [&spec] (std::string_view id,
                              std::string_view name,
                              ParameterKind kind,
                              ParameterScale scale,
                              ParameterUnit unit,
                              float lo,
                              float hi,
                              float def,
                              float smoothing)
    {
        spec.id.append (id);
        spec.name.append (name);
        spec.kind = kind;
        spec.scale = scale;
        spec.unit = unit;
        spec.minimum = lo;
        spec.maximum = hi;
        spec.defaultValue = def;
        spec.smoothingSeconds = smoothing;
    };

    using K = ParameterKind;
    using S = ParameterScale;
    using U = ParameterUnit;

    switch (control)
    {
        case StemControl::gain:
            set ("gain", "gain", K::continuous, S::decibel, U::decibels, gainMinDb, gainMaxDb, 0.0f, 0.020f);
            break;
        case StemControl::mute:
            set ("mute", "mute", K::toggle, S::linear, U::onOff, 0.0f, 1.0f, 0.0f, 0.010f);
            break;
        case StemControl::solo:
            set ("solo", "solo", K::toggle, S::linear, U::onOff, 0.0f, 1.0f, 0.0f, 0.010f);
            break;
        case StemControl::pan:
            set ("pan", "pan", K::continuous, S::linear, U::pan, panMin, panMax, 0.0f, 0.020f);
            break;
        case StemControl::lowCut:
            set ("lowCut",
                 "low cut",
                 K::continuous,
                 S::logarithmic,
                 U::hertz,
                 lowCutMinHz,
                 lowCutMaxHz,
                 lowCutMinHz,
                 0.030f);
            break;
        case StemControl::highCut:
            set ("highCut",
                 "high cut",
                 K::continuous,
                 S::logarithmic,
                 U::hertz,
                 highCutMinHz,
                 highCutMaxHz,
                 highCutMaxHz,
                 0.030f);
            break;
        case StemControl::reverbSend:
            set ("reverbSend", "reverb send", K::continuous, S::linear, U::percent, 0.0f, 1.0f, 0.0f, 0.020f);
            break;
        case StemControl::delaySend:
            set ("delaySend", "delay send", K::continuous, S::linear, U::percent, 0.0f, 1.0f, 0.0f, 0.020f);
            break;
    }
    return spec;
}

constexpr ParameterSpec makeGlobalSpec (GlobalControl control) noexcept
{
    ParameterSpec spec;
    const auto set = [&spec] (std::string_view id,
                              std::string_view name,
                              ParameterKind kind,
                              ParameterScale scale,
                              ParameterUnit unit,
                              float lo,
                              float hi,
                              float def,
                              float smoothing)
    {
        spec.id.append (id);
        spec.name.append (name);
        spec.kind = kind;
        spec.scale = scale;
        spec.unit = unit;
        spec.minimum = lo;
        spec.maximum = hi;
        spec.defaultValue = def;
        spec.smoothingSeconds = smoothing;
    };

    using K = ParameterKind;
    using S = ParameterScale;
    using U = ParameterUnit;

    switch (control)
    {
        case GlobalControl::reverbReturn:
            set ("fx.reverb.return", "Reverb return", K::continuous, S::linear, U::percent, 0.0f, 1.0f, 1.0f, 0.020f);
            break;
        case GlobalControl::delayDivision:
            set ("fx.delay.division",
                 "Delay division",
                 K::choice,
                 S::linear,
                 U::division,
                 0.0f,
                 static_cast<float> (numDelayDivisions - 1),
                 static_cast<float> (DelayDivision::eighth),
                 0.0f);
            break;
        case GlobalControl::delayFeedback:
            set ("fx.delay.feedback",
                 "Delay feedback",
                 K::continuous,
                 S::linear,
                 U::percent,
                 0.0f,
                 maxFeedback,
                 0.35f,
                 0.030f);
            break;
        case GlobalControl::delayReturn:
            set ("fx.delay.return", "Delay return", K::continuous, S::linear, U::percent, 0.0f, 1.0f, 1.0f, 0.020f);
            break;
        case GlobalControl::masterGain:
            set ("master.gain",
                 "Master gain",
                 K::continuous,
                 S::decibel,
                 U::decibels,
                 gainMinDb,
                 gainMaxDb,
                 0.0f,
                 0.020f);
            break;
        case GlobalControl::tempoBpm:
            set ("tempo.bpm",
                 "Tempo",
                 K::continuous,
                 S::linear,
                 U::bpm,
                 tempoMinBpm,
                 tempoMaxBpm,
                 tempoDefaultBpm,
                 0.0f);
            break;
    }
    return spec;
}

constexpr std::array<ParameterSpec, numParameters> makeSpecs() noexcept
{
    std::array<ParameterSpec, numParameters> specs {};
    for (int stem = 0; stem < numStems; ++stem)
        for (int control = 0; control < numStemControls; ++control)
            specs[static_cast<std::size_t> (stem * numStemControls + control)] =
                makeStemSpec (static_cast<Stem> (stem), static_cast<StemControl> (control));
    for (int control = 0; control < numGlobalControls; ++control)
        specs[static_cast<std::size_t> (parameterIndex (static_cast<GlobalControl> (control)))] =
            makeGlobalSpec (static_cast<GlobalControl> (control));
    return specs;
}
} // namespace detail

inline constexpr std::array<ParameterSpec, numParameters> parameterSpecs = detail::makeSpecs();

constexpr const ParameterSpec& parameterSpec (int index) noexcept
{
    return parameterSpecs[static_cast<std::size_t> (index)];
}

/** The index of the parameter with this stable ID, if there is one. */
std::optional<int> findParameter (std::string_view id) noexcept;

/** Clamps to the range, snaps toggles/choices to integers and turns NaN into the default. */
float clampParameterValue (int index, float value) noexcept;

/** Maps a value to 0..1 along the parameter's UI scale, and back (the result is clamped). */
float toNormalised (int index, float value) noexcept;
float fromNormalised (int index, float normalised) noexcept;

//==============================================================================
// Value helpers shared by the DSP.

/** dB to linear gain; at or below the minimum (-inf) the gain is exactly 0. */
inline float decibelsToGain (float decibels) noexcept
{
    return decibels <= gainMinDb ? 0.0f : std::pow (10.0f, decibels * 0.05f);
}

inline float gainToDecibels (float gain) noexcept
{
    return gain <= 0.0f ? gainMinDb : std::max (gainMinDb, 20.0f * std::log10 (gain));
}

/** The low/high cut at their default endpoints are bypassed (no filtering at all). */
inline bool isLowCutBypassed (float hertz) noexcept
{
    return hertz <= lowCutMinHz;
}

inline bool isHighCutBypassed (float hertz) noexcept
{
    return hertz >= highCutMaxHz;
}

inline DelayDivision toDelayDivision (float value) noexcept
{
    return static_cast<DelayDivision> (std::clamp (static_cast<int> (std::lround (value)), 0, numDelayDivisions - 1));
}

/** Half-time / double-time helpers for the BPM control; the result stays inside the BPM range. */
inline float halveTempo (float bpm) noexcept
{
    return std::clamp (bpm * 0.5f, tempoMinBpm, tempoMaxBpm);
}

inline float doubleTempo (float bpm) noexcept
{
    return std::clamp (bpm * 2.0f, tempoMinBpm, tempoMaxBpm);
}

//==============================================================================
// Text. formatDisplay is short ("-3.0 dB", "1.2 kHz", "Off"); formatExact is the precise value
// shown while editing ("-3.25 dB", "1203.5 Hz"). parseValue accepts what formatExact produces.
juce::String formatDisplay (int index, float value);
juce::String formatExact (int index, float value);

/** Parses typed text (e.g. "-6", "-6 dB", "-inf", "1.5k", "35%", "L30", "1/8T", "128 bpm") into a clamped value. */
std::optional<float> parseValue (int index, const juce::String& text);

juce::String divisionName (DelayDivision division);
} // namespace snds::dsp
