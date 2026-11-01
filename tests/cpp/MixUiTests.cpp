#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "FakeAudioDevice.h"
#include "PlaybackTestKit.h"
#include "SongFixtures.h"
#include "TestHelpers.h"
#include "app/CacheStore.h"
#include "app/MixService.h"
#include "app/SongPackageEdit.h"
#include "app/SongSession.h"
#include "dsp/MixParameters.h"
#include "ui/MasterSection.h"
#include "ui/MixControls.h"
#include "ui/MixPanel.h"
#include "ui/StemCard.h"
#include "ui/TempoFxSection.h"

#include <cmath>
#include <numbers>

namespace snds
{
namespace
{
using namespace snds::test;

/** Button::triggerClick posts the click to the message queue; run it. */
void click (juce::Button& button)
{
    button.triggerClick();
    MessageLoop::pump (40);
}

int gainIndex (dsp::Stem stem)
{
    return dsp::parameterIndex (stem, dsp::StemControl::gain);
}

//==============================================================================
class TempoOverrideTests final : public juce::UnitTest
{
public:
    TempoOverrideTests()
        : juce::UnitTest ("MixTempoOverride", "snds")
    {
    }

    void runTest() override
    {
        ScopedTempDirectory temp;
        const auto cache = temp.child ("cache");
        const auto package = SongFixture().write (cache);
        const auto file = package.getChildFile ("song.json");
        const auto original = file.loadFileAsString();

        beginTest ("MixTempoOverride: only the override changes in song.json; the file stays a complete package");
        expect (original.contains ("\"userOverride\": null") || original.contains ("\"userOverride\":null"),
                "fixture has a null override");

        expect (writeBpmOverride (package, SongMetadata::BpmOverride { 128.5, "numeric" }).wasOk());
        auto loaded = loadSongPackage (package);
        expect (loaded.isComplete(), loaded.reason);
        expect (loaded.metadata.bpm.userOverride.has_value());
        expectWithinAbsoluteError (loaded.metadata.bpm.userOverride->value, 128.5, 1.0e-6);
        expectEquals (loaded.metadata.bpm.userOverride->mode, juce::String ("numeric"));
        expectWithinAbsoluteError (loaded.metadata.effectiveBpm(), 128.5, 1.0e-6);

        // Everything outside the override value is byte-for-byte what it was.
        const auto edited = file.loadFileAsString();
        const auto start = original.indexOf ("\"userOverride\"");
        const auto tailOfOriginal = original.fromFirstOccurrenceOf ("\"userOverride\"", false, false)
                                        .fromFirstOccurrenceOf ("null", false, false);
        expectEquals (edited.substring (0, start), original.substring (0, start));
        expect (edited.endsWith (tailOfOriginal), "the text after the override is unchanged");

        beginTest ("MixTempoOverride: half, double, numeric again, and back to detected");
        expect (writeBpmOverride (package, SongMetadata::BpmOverride { 64.25, "half" }).wasOk());
        loaded = loadSongPackage (package);
        expectEquals (loaded.metadata.bpm.userOverride->mode, juce::String ("half"));
        expectWithinAbsoluteError (loaded.metadata.bpm.userOverride->value, 64.25, 1.0e-6);
        expect (writeBpmOverride (package, SongMetadata::BpmOverride { 90.0, "double" }).wasOk());
        expectEquals (loadSongPackage (package).metadata.bpm.userOverride->mode, juce::String ("double"));
        expect (writeBpmOverride (package, std::nullopt).wasOk());
        loaded = loadSongPackage (package);
        expect (loaded.isComplete() && ! loaded.metadata.bpm.userOverride.has_value());
        expectEquals (file.loadFileAsString(), original, "clearing restores the file exactly");

        beginTest ("MixTempoOverride: bad values and bad files fail and leave song.json untouched");
        for (const double bad : { 0.0, 19.9, 400.1, 1.0e9, -5.0 })
        {
            const auto result = writeBpmOverride (package, SongMetadata::BpmOverride { bad, "numeric" });
            expect (result.failed(), juce::String (bad));
            expectEquals (file.loadFileAsString(), original);
        }

        const auto bare = temp.child ("bare");
        bare.createDirectory();
        expect (writeBpmOverride (bare, SongMetadata::BpmOverride { 100.0, "numeric" }).failed(), "no song.json");
        bare.getChildFile ("song.json").replaceWithText ("{ \"bpm\": {} }");
        expect (writeBpmOverride (bare, SongMetadata::BpmOverride { 100.0, "numeric" }).failed(),
                "no userOverride entry");
        expectEquals (bare.getChildFile ("song.json").loadFileAsString(), juce::String ("{ \"bpm\": {} }"));

        beginTest ("MixTempoOverride: no temporary file is left behind");
        expect (writeBpmOverride (package, SongMetadata::BpmOverride { 111.0, "numeric" }).wasOk());
        int files = 0;

        for (const auto& entry : juce::RangedDirectoryIterator (package, false))
            files += entry.getFile().existsAsFile() ? 1 : 0;

        expectEquals (files, 5); // song.json and the four stems
    }
};

TempoOverrideTests tempoOverrideTests;

//==============================================================================
class SessionTempoTests final : public juce::UnitTest
{
public:
    SessionTempoTests()
        : juce::UnitTest ("MixSessionTempo", "snds")
    {
    }

    void runTest() override
    {
        beginTest ("MixSessionTempo: the session stores the user's tempo in the package and reports the change");
        ScopedTempDirectory temp;
        DataPaths paths { temp.child ("home") };
        CacheStore cache (paths);
        OriginalLocations originals (paths.home.getChildFile ("originals.json"));
        const auto package = SongFixture().write (paths.cacheDirectory());
        DispatchQueue queue;

        SessionEnvironment environment;
        environment.paths = paths;
        environment.cache = &cache;
        environment.originals = &originals;
        environment.dispatcher = queue.dispatcher();
        SongSession session (environment);

        int changes = 0;
        session.onBpmChanged = [&] (const OpenSong&) { ++changes; };
        expect (session.setBpmOverride (SongMetadata::BpmOverride { 100.0, "numeric" }).failed(),
                "no song is open yet");

        session.openCached (package.getFileName());
        expect (queue.pumpUntil ([&] { return session.openSong().has_value(); }));
        expect (session.setBpmOverride (SongMetadata::BpmOverride { 133.0, "numeric" }).wasOk());
        expectEquals (changes, 1);
        expectWithinAbsoluteError (session.openSong()->song().effectiveBpm(), 133.0, 1.0e-9);
        expectWithinAbsoluteError (loadSongPackage (package).metadata.effectiveBpm(), 133.0, 1.0e-6);

        expect (session.setBpmOverride (SongMetadata::BpmOverride { 5.0, "numeric" }).failed());
        expectEquals (changes, 1, "a failed write changes nothing");
        expectWithinAbsoluteError (session.openSong()->song().effectiveBpm(), 133.0, 1.0e-9);

        expect (session.setBpmOverride (std::nullopt).wasOk());
        expect (! session.openSong()->song().bpm.userOverride.has_value());
        expectWithinAbsoluteError (
            session.openSong()->song().effectiveBpm(), 120.0, 1.0e-9); // the fixture's detected tempo
    }
};

SessionTempoTests sessionTempoTests;

//==============================================================================
class MixServiceTests final : public juce::UnitTest
{
public:
    MixServiceTests()
        : juce::UnitTest ("MixService", "snds")
    {
    }

    void runTest() override
    {
        beginTest ("MixService: the engine runs a MixProcessor, its meters read, and a new song starts from defaults");
        MixService mix;
        audio::PlaybackEngine engine;
        engine.setMixStageFactory (mix.stageFactory());
        engine.prepare (44100.0, 512);

        dsp::MeterSnapshot meters;
        expect (! mix.readMeters (meters), "no song yet");
        expect (mix.processor() == nullptr);

        expect (engine.load (makeMarkerSources (44100, 44100 * 3)));
        expect (mix.processor() != nullptr);
        engine.play();
        renderOffline (engine, 20000, 512);
        expect (mix.readMeters (meters));
        expect (meters.stems[0].peak > 0.0f && meters.masterPeak > 0.0f, "levels were published");

        mix.parameters().set (dsp::Stem::drums, dsp::StemControl::gain, -12.0f);
        mix.parameters().set (dsp::GlobalControl::masterGain, 3.0f);
        mix.beginSong (143.5);
        expectEquals (mix.parameters().get (gainIndex (dsp::Stem::drums)), 0.0f);
        expectEquals (mix.parameters().get (dsp::parameterIndex (dsp::GlobalControl::masterGain)), 0.0f);
        expectEquals (mix.parameters().get (dsp::parameterIndex (dsp::GlobalControl::tempoBpm)), 143.5f);

        mix.endSong();
        expect (! mix.readMeters (meters));

        beginTest ("MixService: each load gets its own processor; the old one stays valid while referenced");
        MixService second;
        audio::PlaybackEngine other;
        other.setMixStageFactory (second.stageFactory());
        other.prepare (44100.0, 512);
        expect (other.load (makeMarkerSources (44100, 44100)));
        const auto first = second.processor();
        expect (other.load (makeMarkerSources (44100, 44100)));
        expect (second.processor() != first);
        dsp::MeterSnapshot stale;
        expect (first->meters().read (stale) || true); // still alive: reading it is safe
    }
};

MixServiceTests mixServiceTests;

//==============================================================================
class MixControlTests final : public juce::UnitTest
{
public:
    MixControlTests()
        : juce::UnitTest ("MixControls", "snds")
    {
    }

    void runTest() override
    {
        MessageLoop loop;
        testParameterControls();
        testStemCard();
        testMeters();
        testMasterSection();
        testTempoSection();
        testMixPanel();
    }

private:
    void testParameterControls()
    {
        beginTest ("MixControls: sliders write the store along the parameter's own scale and follow it back");
        dsp::ParameterStore store;
        ParameterSlider gain (store, gainIndex (dsp::Stem::bass), juce::Slider::LinearVertical);
        expectEquals (gain.getTextFromValue (gain.getValue()), juce::String ("0.0 dB"));

        gain.setValue (dsp::toNormalised (gainIndex (dsp::Stem::bass), -6.0f));
        expectWithinAbsoluteError (store.get (gainIndex (dsp::Stem::bass)), -6.0f, 1.0e-3f);

        store.set (gainIndex (dsp::Stem::bass), 3.5f);
        gain.syncFromStore();
        expectWithinAbsoluteError (
            static_cast<float> (gain.getValue()), dsp::toNormalised (gainIndex (dsp::Stem::bass), 3.5f), 1.0e-6f);
        expectEquals (gain.getTextFromValue (gain.getValue()), dsp::formatDisplay (gainIndex (dsp::Stem::bass), 3.5f));

        // Exact text entry, in the parameter's own words.
        gain.setValue (gain.getValueFromText ("-3.25 dB"));
        expectWithinAbsoluteError (store.get (gainIndex (dsp::Stem::bass)), -3.25f, 1.0e-3f);
        gain.setValue (gain.getValueFromText ("not a number"));
        expectWithinAbsoluteError (store.get (gainIndex (dsp::Stem::bass)), -3.25f, 1.0e-3f);

        const auto lowCut = dsp::parameterIndex (dsp::Stem::drums, dsp::StemControl::lowCut);
        ParameterSlider cut (store, lowCut, juce::Slider::LinearHorizontal);
        cut.setValue (cut.getValueFromText ("200 Hz"));
        expectWithinAbsoluteError (store.get (lowCut), 200.0f, 0.5f);
        cut.setValue (0.0);
        expectEquals (store.get (lowCut), dsp::lowCutMinHz, "the left end is the bypassed default");

        ParameterToggle mute (store, dsp::parameterIndex (dsp::Stem::vocals, dsp::StemControl::mute), "M");
        expect (! mute.getToggleState());
        click (mute);
        expectEquals (store.get (dsp::parameterIndex (dsp::Stem::vocals, dsp::StemControl::mute)), 1.0f);
        store.resetToDefaults();
        mute.syncFromStore();
        expect (! mute.getToggleState());
    }

    void testStemCard()
    {
        beginTest ("MixControls: a stem card shows gain, mute, solo and a meter up front; secondary controls expand");
        dsp::ParameterStore store;
        StemCard card (store, dsp::Stem::vocals);
        card.setSize (200, card.getPreferredHeight());
        expect (card.gainSlider().isVisible() && card.muteButton().isVisible() && card.soloButton().isVisible());
        expect (card.moreButton().isVisible());
        expect (! card.isExpanded());

        for (const auto control : { dsp::StemControl::pan,
                                    dsp::StemControl::lowCut,
                                    dsp::StemControl::highCut,
                                    dsp::StemControl::reverbSend,
                                    dsp::StemControl::delaySend })
            expect (! card.secondarySlider (control).isVisible(), "hidden until expanded");

        const auto collapsed = card.getPreferredHeight();
        int heightChanges = 0;
        card.onPreferredHeightChanged = [&] { ++heightChanges; };
        click (card.moreButton());
        expect (card.isExpanded());
        expect (card.getPreferredHeight() > collapsed);
        expectEquals (heightChanges, 1);

        for (const auto control : { dsp::StemControl::pan,
                                    dsp::StemControl::lowCut,
                                    dsp::StemControl::highCut,
                                    dsp::StemControl::reverbSend,
                                    dsp::StemControl::delaySend })
        {
            auto& slider = card.secondarySlider (control);
            expect (slider.isVisible(), "shown when expanded");
            const auto index = dsp::parameterIndex (dsp::Stem::vocals, control);
            expectEquals (static_cast<float> (dsp::fromNormalised (index, static_cast<float> (slider.getValue()))),
                          dsp::parameterSpec (index).defaultValue,
                          "starts at the default");
        }

        // The secondary controls write the vocals' own parameters, not another stem's.
        card.secondarySlider (dsp::StemControl::pan)
            .setValue (dsp::toNormalised (dsp::parameterIndex (dsp::Stem::vocals, dsp::StemControl::pan), -0.5f));
        expectWithinAbsoluteError (
            store.get (dsp::parameterIndex (dsp::Stem::vocals, dsp::StemControl::pan)), -0.5f, 1.0e-3f);
        expectEquals (store.get (dsp::parameterIndex (dsp::Stem::drums, dsp::StemControl::pan)), 0.0f);

        click (card.soloButton());
        expectEquals (store.get (dsp::parameterIndex (dsp::Stem::vocals, dsp::StemControl::solo)), 1.0f);
        store.resetToDefaults();
        card.syncFromStore();
        expect (! card.soloButton().getToggleState());
        expectWithinAbsoluteError (
            static_cast<float> (card.secondarySlider (dsp::StemControl::pan).getValue()), 0.5f, 1.0e-6f);

        click (card.moreButton());
        expect (! card.isExpanded());
        expectEquals (card.getPreferredHeight(), collapsed);
    }

    void testMeters()
    {
        beginTest ("MixControls: the meter scale runs -60 to +6 dBFS and marks 0 dBFS");
        expectEquals (LevelMeter::position (0.0f), 0.0f);
        expectEquals (LevelMeter::position (1.0e-4f), 0.0f);
        expectWithinAbsoluteError (LevelMeter::position (1.0f), 60.0f / 66.0f, 1.0e-5f);
        expectWithinAbsoluteError (LevelMeter::position (0.5f), (60.0f - 6.0206f) / 66.0f, 1.0e-4f);
        expectEquals (LevelMeter::position (4.0f), 1.0f, "above +6 dBFS pins at the top");
        LevelMeter meter;
        meter.setSize (20, 100);
        meter.setLevels (0.5f, 0.25f);
        expectEquals (meter.getPeak(), 0.5f);
        expectEquals (meter.getRms(), 0.25f);
        meter.clear();
        expectEquals (meter.getPeak(), 0.0f);
    }

    void testMasterSection()
    {
        beginTest ("MixControls: the limiter indicator lights at 0.1 dB, shows the reduction, advises after it stays "
                   "lit, and clears");
        dsp::ParameterStore store;
        MasterSection master (store);
        master.setSize (150, 246);
        dsp::MeterSnapshot snapshot;
        snapshot.masterPeak = 0.966f;
        snapshot.masterRms = 0.4f;

        master.update (snapshot, 0.0);
        expect (! master.isLimiterLit());
        expectEquals (MasterSection::limiterText (snapshot), juce::String ("Limiter idle"));

        snapshot.limiterReductionDb = 0.05f; // below the activity threshold
        master.update (snapshot, 100.0);
        expect (! master.isLimiterLit());

        snapshot.limiterReductionDb = 2.44f;
        master.update (snapshot, 200.0);
        expect (master.isLimiterLit() && ! master.isAdviceShown());
        expectEquals (MasterSection::limiterText (snapshot), juce::String ("Limiter -2.4 dB"));

        master.update (snapshot, 200.0 + MasterSection::adviceAfterMs - 1.0);
        expect (! master.isAdviceShown(), "not yet");
        master.update (snapshot, 200.0 + MasterSection::adviceAfterMs);
        expect (master.isAdviceShown(), "stayed lit: say what to turn down");

        snapshot.limiterReductionDb = 0.0f;
        master.update (snapshot, 5000.0);
        expect (! master.isLimiterLit() && ! master.isAdviceShown());

        // A short flicker never reaches the advice.
        snapshot.limiterReductionDb = 1.0f;
        master.update (snapshot, 6000.0);
        snapshot.limiterReductionDb = 0.0f;
        master.update (snapshot, 6500.0);
        snapshot.limiterReductionDb = 1.0f;
        master.update (snapshot, 7400.0);
        expect (master.isLimiterLit() && ! master.isAdviceShown(), "the timer restarted");

        master.clearLevels();
        expect (! master.isLimiterLit());
    }

    void testTempoSection()
    {
        beginTest (
            "MixControls: tempo is typed or halved and doubled, flagged when not detected, and reset to detected");
        dsp::ParameterStore store;
        TempoFxSection section (store);
        section.setSize (1000, 100);
        double chosen = 0.0;
        juce::String mode;
        int cleared = 0;
        section.onTempoChosen = [&] (double bpm, const juce::String& m)
        {
            chosen = bpm;
            mode = m;
            store.set (dsp::GlobalControl::tempoBpm, static_cast<float> (bpm));
        };
        section.onTempoCleared = [&] { ++cleared; };

        expectEquals (section.tempoEditor().getText(), juce::String ("120"));
        expect (section.noteLabel().getText().contains ("Not detected: using 120"), section.noteLabel().getText());
        expect (! section.detectedButton().isVisible());

        section.tempoEditor().setText ("128.5", false);
        section.commitTempoText();
        expectWithinAbsoluteError (chosen, 128.5, 1.0e-9);
        expectEquals (mode, juce::String ("numeric"));

        chosen = 0.0;
        section.tempoEditor().setText ("500", false);
        section.commitTempoText();
        expectEquals (chosen, 0.0, "out of range is not applied");
        expect (section.noteLabel().getText().contains ("20 to 400"));
        expectEquals (section.tempoEditor().getText(), juce::String ("128.5"), "the box goes back to the real tempo");
        section.tempoEditor().setText ("", false);
        section.commitTempoText();
        expectEquals (chosen, 0.0);

        // Return or losing focus with the box untouched commits nothing, at any displayed rounding.
        chosen = 0.0;
        section.commitTempoText();
        expectEquals (chosen, 0.0, "an untouched box is not a choice");

        // The box rounds to one decimal: 179.839 shows as 179.8, and committing that untouched text must not
        // turn the detected tempo into one the user "set".
        store.set (dsp::GlobalControl::tempoBpm, 179.839f);
        section.syncFromStore();
        expectEquals (section.tempoEditor().getText(), juce::String ("179.8"));
        section.commitTempoText();
        expectEquals (chosen, 0.0, "an untouched, rounded box is not a choice");
        section.tempoEditor().setText ("180", false);
        section.commitTempoText();
        expectWithinAbsoluteError (chosen, 180.0, 1.0e-9); // typing a different value is
        store.set (dsp::GlobalControl::tempoBpm, 128.5f);
        section.syncFromStore();
        chosen = 0.0;

        click (section.halveButton());
        expectWithinAbsoluteError (chosen, 64.25, 1.0e-6);
        expectEquals (mode, juce::String ("half"));
        click (section.doubleButton());
        expectWithinAbsoluteError (chosen, 128.5, 1.0e-6);
        expectEquals (mode, juce::String ("double"));

        store.set (dsp::GlobalControl::tempoBpm, 400.0f);
        chosen = 0.0;
        click (section.doubleButton());
        expectEquals (chosen, 0.0, "already at the top of the range");
        store.set (dsp::GlobalControl::tempoBpm, 20.0f);
        click (section.halveButton());
        expectEquals (chosen, 0.0, "already at the bottom");

        TempoFxSection::TempoInfo info;
        info.detected = 143.2;
        expectEquals (TempoFxSection::describeTempo (info, 143.2), juce::String ("Detected from the song"));
        info.overridden = true;
        expectEquals (TempoFxSection::describeTempo (info, 90.0), juce::String ("Set by you (detected: 143.2)"));
        section.setTempoInfo (info);
        expect (section.detectedButton().isVisible());
        click (section.detectedButton());
        expectEquals (cleared, 1);
        info.detected.reset();
        expectEquals (TempoFxSection::describeTempo (info, 90.0), juce::String ("Set by you (none detected)"));

        // The delay division choice writes the store and follows it back.
        section.divisionBox().setSelectedId (4, juce::sendNotificationSync);
        expectEquals (store.get (dsp::parameterIndex (dsp::GlobalControl::delayDivision)), 3.0f);
        store.resetToDefaults();
        section.syncFromStore();
        expectEquals (section.divisionBox().getSelectedId(), static_cast<int> (dsp::DelayDivision::eighth) + 1);
    }

    void testMixPanel()
    {
        beginTest (
            "MixControls: the panel shows live per-stem and master levels, follows resets, and flags the limiter");
        MixService mix;
        audio::PlaybackEngine engine;
        engine.setMixStageFactory (mix.stageFactory());
        engine.prepare (44100.0, 512);

        MixPanel panel (mix);
        panel.setSize (1100, panel.getPreferredHeight());
        expect (! panel.isEnabled() || mix.processor() == nullptr);

        // The same 0.2-amplitude tone in all four stems.
        audio::PlaybackSources sources;
        sources.sampleRate = 44100;
        sources.lengthFrames = 44100 * 6;

        for (auto& stem : sources.stems)
            stem = std::make_unique<SyntheticSource> (
                44100,
                sources.lengthFrames,
                [] (std::int64_t f, int)
                {
                    return 0.2f
                         * static_cast<float> (
                               std::sin (2.0 * std::numbers::pi * 440.0 * static_cast<double> (f) / 44100.0));
                });

        expect (engine.load (std::move (sources)));
        panel.refresh();
        expect (panel.isEnabled(), "a song is loaded");

        mix.parameters().set (dsp::Stem::drums, dsp::StemControl::gain, -12.0f);
        mix.parameters().set (dsp::Stem::bass, dsp::StemControl::mute, 1.0f);
        engine.play();
        renderOffline (engine, 44100, 512);
        panel.refresh();

        auto peakOf = [&] (int stem) { return panel.stemCards()[static_cast<size_t> (stem)]->meterPeak(); };
        expectWithinAbsoluteError (peakOf (1), 0.0f, 0.01f, "the muted bass reads silent after its release");
        expect (peakOf (2) > 0.15f && peakOf (2) < 0.25f, "other plays at 0 dB: " + juce::String (peakOf (2)));
        expectWithinAbsoluteError (peakOf (0), peakOf (2) * 0.251f, 0.02f, "drums are 12 dB down");
        expect (panel.master().isLimiterLit() == false);

        // Controls follow the store after a reset.
        expectEquals (static_cast<float> (panel.stemCards()[0]->gainSlider().getParameterValue()), -12.0f);
        mix.parameters().resetToDefaults();
        panel.refresh();
        expectEquals (panel.stemCards()[0]->gainSlider().getParameterValue(), 0.0f);
        expect (! panel.stemCards()[1]->muteButton().getToggleState(), "mute is back off");
        expectWithinAbsoluteError (static_cast<float> (panel.stemCards()[0]->gainSlider().getValue()),
                                   dsp::toNormalised (gainIndex (dsp::Stem::drums), 0.0f),
                                   1.0e-6f);

        // Four stems at +6 dB and the master up: far over the ceiling, so the limiter works and says so.
        for (int i = 0; i < 4; ++i)
            mix.parameters().set (static_cast<dsp::Stem> (i), dsp::StemControl::gain, 6.0f);

        mix.parameters().set (dsp::GlobalControl::masterGain, 6.0f);
        renderOffline (engine, 22050, 512);
        panel.refresh();
        expect (panel.master().isLimiterLit(), "the limiter indicator is lit");

        beginTest ("MixControls: the note explains Original bypass and shared output protection");
        PlaybackView view;
        expect (MixPanel::describeNote (view).contains ("Original bypasses them"));
        expect (MixPanel::describeNote (view).contains ("output protection"));
        view.state = PlaybackState::playing;
        view.originalSelected = true;
        expect (MixPanel::describeNote (view).contains ("Original is playing and bypasses mix controls"));
        expect (MixPanel::describeNote (view).contains ("Output protection"));
    }
};

MixControlTests mixControlTests;
} // namespace
} // namespace snds
