#include "app/AudioService.h"

#include "app/Logging.h"

namespace snds
{
AudioService::AudioService (Settings* settingsToUse)
    : settings (settingsToUse),
      controller (playback, manager)
{
    playback.setMixStageFactory (mixService.stageFactory());

    controller.onPersist = [this] (const juce::String& stateXml)
    {
        if (settings == nullptr)
            return;

        settings->setAudioDeviceState (stateXml);

        if (const auto saved = settings->save(); saved.failed())
            logWarning ("Could not save the audio device choice: " + saved.getErrorMessage());
    };
}

AudioService::~AudioService()
{
    controller.shutdown();
}

juce::String AudioService::start()
{
    const auto error = controller.initialise (settings != nullptr ? settings->getAudioDeviceState() : juce::String());

    if (error.isNotEmpty())
        logWarning ("Audio: no device could be opened: " + error);

    return error;
}
} // namespace snds
