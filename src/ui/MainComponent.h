#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "app/AsyncTask.h"
#include "app/AudioService.h"
#include "app/CacheStore.h"
#include "app/DataPaths.h"
#include "app/DiagnosticsExport.h"
#include "app/OriginalLocations.h"
#include "app/PlaybackController.h"
#include "app/Readiness.h"
#include "app/Settings.h"
#include "app/SongSession.h"
#include "app/WorkerLocation.h"
#include "ui/AudioSettingsDialog.h"
#include "ui/CacheDialog.h"
#include "ui/ImportPanel.h"
#include "ui/MixPanel.h"
#include "ui/ReadinessPanel.h"
#include "ui/SongPanel.h"

#include <optional>

namespace snds
{
/** Top-level content of the main window: the header, the worker readiness panel, the import
    area (file choice, artwork confirmation, preparation progress, the library of prepared
    songs), and the song panel. Import stays disabled until the worker reports that it is ready.

    On start it quarantines job folders left behind by earlier runs, off the message thread.
*/
class MainComponent final : public juce::Component
{
public:
    /** `paths` may be empty when no data folder can be resolved; the window still opens, with
        import disabled. `settings` may be null. */
    MainComponent (WorkerLocation workerLocation,
                   int readinessTimeoutMs,
                   std::optional<DataPaths> paths = std::nullopt,
                   Settings* settings = nullptr,
                   AudioService* audio = nullptr);
    ~MainComponent() override;

    /** Opens a prepared song from the cache by key (automation: --open-song). */
    void openCachedSong (const juce::String& key);

    /** The renderer stepped down (for example OpenGL to the CPU tier). Shows a dismissable notice with
        the reason and logs it; audio is unaffected. Called by whoever owns the VisualHost. */
    void reportRendererFallback (const RendererFallbackInfo& info);

    /** Writes the diagnostics zip to `file` without asking (automation: --export-diagnostics). `done`
        gets the result on the message thread. */
    void exportDiagnosticsTo (const juce::File& file, std::function<void (const juce::Result&)> done);

    /** Starts a readiness check unless one is already running. */
    void startReadinessCheck();

    const Readiness& getReadiness() const noexcept { return readiness; }

    /** Called on the message thread whenever a readiness check finishes. */
    std::function<void (const Readiness&)> onReadinessChanged;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void readinessFinished (const Readiness& result);
    void startQuarantine();
    void refreshLibrary();
    void updateImportAllowed();
    void chooseSourceFile();
    void chooseOriginalFile();
    void exportDiagnostics();
    DiagnosticsInput gatherDiagnosticsInput() const;
    void writeDiagnostics (const DiagnosticsInput& input,
                           const juce::File& file,
                           juce::Time when,
                           std::function<void (const juce::Result&)> done);
    void showDiagnosticsResult (const juce::File& file, const juce::Result& result, bool reveal);
    void showCacheWindow();
    void refreshCache();
    void updateCacheLock();
    void removeFromCache (std::optional<juce::String> key); // nullopt: clear everything
    void verifyCachedSong (const juce::String& key);
    void recordRecent (const OpenSong& song);
    void openLibraryItem (const LibraryItem& item);
    void locateLibraryItem (const LibraryItem& item);
    void removeLibraryItem (const LibraryItem& item);
    void showAudioSettings();
    void handleRecovery (RecoveryKind kind);
    void layoutPage();
    void paintPage (juce::Graphics& g);
    void applyTempoOverride (double bpm, const juce::String& mode);
    void clearTempoOverride();

    const WorkerLocation workerLocation;
    const int readinessTimeoutMs;
    const std::optional<DataPaths> paths;
    Settings* const settings;
    AudioService* const audio; // not owned; may be null (no audio)

    Readiness readiness;
    std::unique_ptr<ReadinessChecker> checker;

    /** The scrolling surface: everything below the window frame lives on it, so a short window
        scrolls instead of clipping. */
    class Page final : public juce::Component
    {
    public:
        std::function<void (juce::Graphics&)> onPaint;
        std::function<void()> onResized;
        void paint (juce::Graphics& g) override
        {
            if (onPaint)
                onPaint (g);
        }
        void resized() override
        {
            if (onResized)
                onResized();
        }
    };

    Page page;
    juce::Viewport viewport;
    juce::Rectangle<int> headerArea;
    ReadinessPanel readinessPanel;
    ImportPanel importPanel;
    std::unique_ptr<MixPanel> mixPanel; // only with audio
    SongPanel songPanel;
    ErrorBanner noticeBanner; // app-level notices (renderer fallback); hidden when empty
    juce::TextButton audioSettingsButton { "Audio settings..." };
    juce::TextButton cacheButton { "Song cache..." };
    juce::TextButton exportButton { "Export diagnostics..." };
    juce::Component::SafePointer<CacheWindow> cacheWindow;
    juce::Component::SafePointer<juce::Component> audioSettingsDialog;

    std::unique_ptr<CacheStore> cache;
    std::unique_ptr<OriginalLocations> originals;
    std::unique_ptr<SongSession> session;
    std::unique_ptr<PlaybackController> playback;
    std::unique_ptr<AsyncTask<QuarantineReport>> quarantineTask;
    std::unique_ptr<AsyncTask<std::vector<LibraryItem>>> libraryTask;
    bool libraryReloadPending = false;
    std::unique_ptr<AsyncTask<CacheOverview>> cacheTask;
    std::unique_ptr<AsyncTask<juce::Result>> removalTask;
    std::shared_ptr<std::atomic<bool>> verificationCancel;
    std::unique_ptr<AsyncTask<CacheVerification>> verificationTask;
    std::unique_ptr<AsyncTask<juce::Result>> diagnosticsTask;
    bool cacheReloadPending = false;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
} // namespace snds
