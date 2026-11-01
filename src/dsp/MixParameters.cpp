#include "dsp/MixParameters.h"

namespace snds::dsp
{
std::optional<int> findParameter (std::string_view id) noexcept
{
    for (int i = 0; i < numParameters; ++i)
        if (parameterSpec (i).id.view() == id)
            return i;
    return std::nullopt;
}

float clampParameterValue (int index, float value) noexcept
{
    const auto& spec = parameterSpec (index);

    if (! std::isfinite (value))
        return spec.defaultValue;

    value = std::clamp (value, spec.minimum, spec.maximum);

    if (spec.kind != ParameterKind::continuous)
        value = std::round (value);

    return value;
}

float toNormalised (int index, float value) noexcept
{
    const auto& spec = parameterSpec (index);
    value = clampParameterValue (index, value);

    if (spec.scale == ParameterScale::logarithmic)
        return std::log (value / spec.minimum) / std::log (spec.maximum / spec.minimum);

    return (value - spec.minimum) / (spec.maximum - spec.minimum);
}

float fromNormalised (int index, float normalised) noexcept
{
    const auto& spec = parameterSpec (index);
    normalised = std::isfinite (normalised) ? std::clamp (normalised, 0.0f, 1.0f) : 0.0f;

    const auto value = spec.scale == ParameterScale::logarithmic
                         ? spec.minimum * std::pow (spec.maximum / spec.minimum, normalised)
                         : spec.minimum + normalised * (spec.maximum - spec.minimum);
    return clampParameterValue (index, value);
}

//==============================================================================
juce::String divisionName (DelayDivision division)
{
    switch (division)
    {
        case DelayDivision::quarter:
            return "1/4";
        case DelayDivision::eighth:
            return "1/8";
        case DelayDivision::dottedEighth:
            return "Dotted 1/8";
        case DelayDivision::eighthTriplet:
            return "1/8T";
        case DelayDivision::sixteenth:
            return "1/16";
    }
    return {};
}

namespace
{
juce::String formatFrequency (float hertz, int hertzDecimals, int kiloDecimals)
{
    if (hertz >= 1000.0f)
        return juce::String (hertz / 1000.0f, kiloDecimals) + " kHz";
    return juce::String (hertz, hertzDecimals) + " Hz";
}

juce::String formatPan (float pan, int decimals)
{
    const auto percent = std::abs (pan) * 100.0f;
    if (juce::String (percent, decimals).getDoubleValue() == 0.0)
        return "Center";
    return juce::String (pan < 0.0f ? "L " : "R ") + juce::String (percent, decimals);
}

juce::String format (int index, float value, bool exact)
{
    const auto& spec = parameterSpec (index);
    value = clampParameterValue (index, value);

    switch (spec.unit)
    {
        case ParameterUnit::decibels:
            if (value <= gainMinDb)
                return "-inf dB";
            return juce::String (value, exact ? 2 : 1) + " dB";
        case ParameterUnit::hertz:
            if (spec.minimum == lowCutMinHz && isLowCutBypassed (value))
                return "Off";
            if (spec.maximum == highCutMaxHz && isHighCutBypassed (value))
                return "Off";
            return exact ? formatFrequency (value, 1, 3) : formatFrequency (value, 0, 1);
        case ParameterUnit::percent:
            return juce::String (value * 100.0f, exact ? 1 : 0) + "%";
        case ParameterUnit::pan:
            return formatPan (value, exact ? 1 : 0);
        case ParameterUnit::bpm:
            return juce::String (value, exact ? 2 : 1) + " BPM";
        case ParameterUnit::division:
            return divisionName (toDelayDivision (value));
        case ParameterUnit::onOff:
            return value >= 0.5f ? "On" : "Off";
        case ParameterUnit::none:
            break;
    }
    return juce::String (value);
}

// Leading numeric part of the text and the lower-case remainder (the unit), both trimmed.
bool splitNumber (const juce::String& text, double& number, juce::String& rest)
{
    const auto trimmed = text.trim();
    auto end = 0;

    while (end < trimmed.length()
           && (juce::CharacterFunctions::isDigit (trimmed[end]) || trimmed[end] == '.'
               || ((trimmed[end] == '-' || trimmed[end] == '+') && end == 0)))
        ++end;

    if (end == 0)
        return false;

    const auto numeric = trimmed.substring (0, end);
    if (! numeric.containsAnyOf ("0123456789"))
        return false;

    number = numeric.getDoubleValue();
    rest = trimmed.substring (end).trim().toLowerCase();
    return true;
}
} // namespace

juce::String formatDisplay (int index, float value)
{
    return format (index, value, false);
}

juce::String formatExact (int index, float value)
{
    return format (index, value, true);
}

std::optional<float> parseValue (int index, const juce::String& text)
{
    const auto& spec = parameterSpec (index);
    const auto lower = text.trim().toLowerCase();
    double number = 0.0;
    juce::String rest;

    if (lower.isEmpty())
        return std::nullopt;

    const auto result = [index] (double value) -> std::optional<float>
    {
        return std::isfinite (value) ? std::optional<float> (clampParameterValue (index, static_cast<float> (value)))
                                     : std::nullopt;
    };

    switch (spec.unit)
    {
        case ParameterUnit::decibels:
            if (lower == "-inf" || lower == "-infinity" || lower == "-inf db")
                return spec.minimum;
            if (splitNumber (lower, number, rest) && (rest.isEmpty() || rest == "db"))
                return result (number);
            return std::nullopt;

        case ParameterUnit::hertz:
            if (lower == "off")
                return spec.minimum == lowCutMinHz ? lowCutMinHz : highCutMaxHz;
            if (splitNumber (lower, number, rest))
            {
                if (rest.isEmpty() || rest == "hz")
                    return result (number);
                if (rest == "k" || rest == "khz")
                    return result (number * 1000.0);
            }
            return std::nullopt;

        case ParameterUnit::percent:
            if (splitNumber (lower, number, rest) && (rest.isEmpty() || rest == "%"))
                return result (number / 100.0);
            return std::nullopt;

        case ParameterUnit::pan:
        {
            if (lower == "c" || lower == "center" || lower == "centre")
                return 0.0f;
            const auto side = lower[0];
            if ((side == 'l' || side == 'r') && splitNumber (lower.substring (1), number, rest)
                && (rest.isEmpty() || rest == "%") && number >= 0.0)
                return result ((side == 'l' ? -number : number) / 100.0);
            if (splitNumber (lower, number, rest) && (rest.isEmpty() || rest == "%"))
                return result (number / 100.0);
            return std::nullopt;
        }

        case ParameterUnit::bpm:
            if (splitNumber (lower, number, rest) && (rest.isEmpty() || rest == "bpm"))
                return result (number);
            return std::nullopt;

        case ParameterUnit::division:
            for (int i = 0; i < numDelayDivisions; ++i)
                if (divisionName (static_cast<DelayDivision> (i)).toLowerCase().replace (" ", "")
                    == lower.replace (" ", ""))
                    return static_cast<float> (i);
            if (lower.replace (" ", "") == "1/8d" || lower.replace (" ", "") == "1/8.")
                return static_cast<float> (DelayDivision::dottedEighth);
            return std::nullopt;

        case ParameterUnit::onOff:
            if (lower == "on" || lower == "1" || lower == "true")
                return 1.0f;
            if (lower == "off" || lower == "0" || lower == "false")
                return 0.0f;
            return std::nullopt;

        case ParameterUnit::none:
            break;
    }
    return std::nullopt;
}
} // namespace snds::dsp
