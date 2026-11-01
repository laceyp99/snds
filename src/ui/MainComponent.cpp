#include "ui/MainComponent.h"

#include "app/AppInfo.h"
#include "app/DiagnosticsExport.h"
#include "app/Logging.h"
#include "app/WindowAttention.h"
#include "ui/RecentList.h"

namespace snds
{
namespace
{
constexpr int margin = 16;
constexpr int songPanelHeight = 340; // room for the visuals' controls and renderer detail line

} // namespace

MainComponent::MainComponent (WorkerLocation location,
                              int timeoutMs,
                              std::optional<DataPaths> dataPaths,
                              Settings* appSettings,
                              AudioService* audioService)
    : workerLocation (std::move (location)),
      readinessTimeoutMs (timeoutMs),
      paths (std::move (dataPaths)),
      settings (appSettings),
      audio (audioService)
{
    readinessPanel.onRecheck = [this] { startReadinessCheck(); };
    exportButton.onClick = [this] { exportDiagnostics(); };
    page.addAndMakeVisible (exportButton);
    noticeBanner.onAction = [this] (RecoveryKind kind)
    {
        if (kind == RecoveryKind::dismiss)
        {
            noticeBanner.setReport (std::nullopt);
            resized();
        }
        else
        {
            handleRecovery (kind);
        }
    };
    noticeBanner.onPreferredHeightChanged = [this] { resized(); };
    page.addChildComponent (noticeBanner);
    songPanel.getTransport().onRecovery = [this] (RecoveryKind kind) { handleRecovery (kind); };
    songPanel.onRendererFallback = [this] (const RendererFallbackInfo& info) { reportRendererFallback (info); };
    readinessPanel.onPreferredHeightChanged = [this] { resized(); };
    importPanel.onPreferredHeightChanged = [this] { resized(); };
    viewport.setViewedComponent (&page, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (12);
    addAndMakeVisible (viewport);
    page.onPaint = [this] (juce::Graphics& g) { paintPage (g); };
    page.onResized = [this] { layoutPage(); };

    page.addAndMakeVisible (readinessPanel);
    page.addAndMakeVisible (importPanel);
    page.addAndMakeVisible (songPanel);

    if (audio != nullptr)
    {
        playback = std::make_unique<PlaybackController> (
            audio->engine(), &audio->devices(), messageThreadDispatcher(), &audio->mix());

        mixPanel = std::make_unique<MixPanel> (audio->mix());
        mixPanel->setController (playback.get());
        mixPanel->onPreferredHeightChanged = [this] { resized(); };
        mixPanel->onTempoChosen = [this] (double bpm, const juce::String& mode) { applyTempoOverride (bpm, mode); };
        mixPanel->onTempoCleared = [this] { clearTempoOverride(); };
        page.addAndMakeVisible (*mixPanel);

        songPanel.setController (playback.get());
        songPanel.setMixService (&audio->mix()); // the visuals read the newest processor's analysis
        audio->devices().onStatusChanged = [this] (const audio::DeviceStatus&)
        {
            playback->deviceStatusChanged();
            songPanel.getTransport().refresh();
        };
        playback->onChanged = [this] { songPanel.getTransport().refresh(); };
        playback->deviceStatusChanged(); // the device may have started before this window

        audioSettingsButton.onClick = [this] { showAudioSettings(); };
        page.addAndMakeVisible (audioSettingsButton);
    }

    importPanel.setForceCpu (settings != nullptr && settings->getForceCpu());
    importPanel.onForceCpuChanged = [this] (bool forceCpu)
    {
        if (settings == nullptr)
            return;

        settings->setForceCpu (forceCpu);

        if (const auto saved = settings->save(); saved.failed())
            logWarning ("Could not save settings: " + saved.getErrorMessage());
    };

    if (paths.has_value())
    {
        cache = std::make_unique<CacheStore> (*paths);
        originals = std::make_unique<OriginalLocations> (paths->home.getChildFile ("originals.json"));

        SessionEnvironment environment;
        environment.paths = *paths;
        environment.worker = workerLocation;
        environment.cache = cache.get();
        environment.originals = originals.get();
        environment.forceCpu = [this] { return settings != nullptr && settings->getForceCpu(); };
        // Windows will not delete files the player has open: let go of a song before its cache entry goes.
        environment.beforeCacheRemoval = [this] (const juce::String& key)
        {
            if (playback != nullptr)
                playback->releaseSong (key);
        };

        session = std::make_unique<SongSession> (std::move (environment));
        session->onStatusChanged = [this] (const SessionStatus& status)
        {
            importPanel.setStatus (status);
            songPanel.setLocateEnabled (! status.isBusy());
            updateImportAllowed();

            if (status.justPrepared)
                requestWindowAttention (*this);
        };
        session->onSongOpened = [this] (const OpenSong& song)
        {
            recordRecent (song);
            if (cacheWindow != nullptr)
                cacheWindow->view().setOpenKey (song.entry.key);

            songPanel.setSong (song);
            if (playback != nullptr)
                playback->setSong (song);

            if (mixPanel != nullptr)
                mixPanel->setSong (song); // after the player: a new song has just reset the mix to its tempo

            refreshLibrary();
        };

        session->onSongClosed = [this]
        {
            songPanel.setSong (std::nullopt);

            if (playback != nullptr)
                playback->setSong (std::nullopt);

            if (mixPanel != nullptr)
                mixPanel->setSong (std::nullopt);

            if (cacheWindow != nullptr)
                cacheWindow->view().setOpenKey ({});

            refreshLibrary();
        };

        session->onBpmChanged = [this] (const OpenSong& song)
        {
            songPanel.setSong (song);

            if (mixPanel != nullptr)
                mixPanel->setSong (song);

            refreshLibrary();
        };

        importPanel.onImport = [this] { chooseSourceFile(); };
        importPanel.onCancel = [this] { session->cancel(); };
        importPanel.onPlay = [this]
        {
            if (playback != nullptr && playback->view().canPlay() && playback->view().state != PlaybackState::playing)
                playback->togglePlay();
        };
        importPanel.onRecovery = [this] (RecoveryKind kind) { handleRecovery (kind); };
        importPanel.onArtworkChosen = [this] (ArtworkChoice choice) { session->provideArtwork (std::move (choice)); };
        importPanel.onOpenItem = [this] (const LibraryItem& item) { openLibraryItem (item); };
        importPanel.onLocateItem = [this] (const LibraryItem& item) { locateLibraryItem (item); };
        importPanel.onRemoveItem = [this] (const LibraryItem& item) { removeLibraryItem (item); };
        songPanel.onLocateOriginal = [this] { chooseOriginalFile(); };

        cacheButton.onClick = [this] { showCacheWindow(); };
        page.addAndMakeVisible (cacheButton);

        startQuarantine();
        refreshLibrary();
    }
    else
    {
        SessionStatus unavailable;
        unavailable.state = SessionState::failed;
        unavailable.headline = "SNDS has no data folder.";
        unavailable.detail = "Neither SNDS_HOME nor LOCALAPPDATA is set, so songs cannot be stored.";
        importPanel.setStatus (unavailable);
    }

    setSize (1180, 900);
    startReadinessCheck();
}

MainComponent::~MainComponent()
{
    if (verificationCancel != nullptr)
        verificationCancel->store (true);
    verificationTask.reset();
    // The dialog holds the device controller; close it first, then let go of the engine.
    if (audioSettingsDialog != nullptr)
        delete audioSettingsDialog.getComponent();

    if (cacheWindow != nullptr)
        delete cacheWindow.getComponent();

    if (audio != nullptr)
        audio->devices().onStatusChanged = nullptr;

    // Cancels a check in progress and kills the worker process of a running job.
    checker.reset();
    session.reset();
    playback.reset(); // unloads the engine, so no stream still has the song files open
    quarantineTask.reset();
    libraryTask.reset();
    cacheTask.reset();
    removalTask.reset();
    diagnosticsTask.reset();
}

void MainComponent::startQuarantine()
{
    // Nothing is running yet, so every job folder is abandoned; a job started later is protected
    // by its active mark even if this pass is still going.
    quarantineTask = std::make_unique<AsyncTask<QuarantineReport>> (
        messageThreadDispatcher(),
        [store = cache.get()] { return store->quarantineAbandonedJobs(); },
        [this] (QuarantineReport report)
        {
            if (report.foldersCleaned > 0 || report.skippedActive > 0 || report.problems.size() > 0)
                logInfo ("Quarantined " + juce::String (report.foldersCleaned) + " abandoned job folder(s), freed "
                         + juce::String (report.bytesFreed) + " bytes"
                         + (report.problems.isEmpty() ? "" : "; problems: ") + report.problems.joinIntoString ("; "));

            refreshLibrary();
        });
}

void MainComponent::refreshLibrary()
{
    if (cache == nullptr)
        return;

    if (libraryTask != nullptr)
    {
        libraryReloadPending = true;
        return;
    }

    // The original locations are read here (message thread); the disk work happens in the task.
    auto recents = settings != nullptr ? settings->getRecentSongs() : std::vector<RecentSong>();
    std::vector<juce::File> originalPaths;

    for (const auto& song : recents)
    {
        const auto known = originals != nullptr ? originals->find (song.key) : std::nullopt;
        originalPaths.push_back (known ? *known : juce::File (song.sourcePath));
    }

    libraryTask = std::make_unique<AsyncTask<std::vector<LibraryItem>>> (
        messageThreadDispatcher(),
        [store = cache.get(), recents = std::move (recents), originalPaths = std::move (originalPaths)]
        { return buildRecentItems (recents, originalPaths, store->listPlayableEntries()); },
        [this] (std::vector<LibraryItem> items)
        {
            libraryTask.reset();
            importPanel.setLibrary (std::move (items));

            if (std::exchange (libraryReloadPending, false))
                refreshLibrary();
        });
}

void MainComponent::updateImportAllowed()
{
    updateCacheLock();
    const bool idle = session != nullptr && ! session->status().isBusy();
    importPanel.setImportAllowed (idle && readiness.isReady());
    importPanel.setOpenAllowed (idle);
}

void MainComponent::chooseSourceFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose a song", juce::File(), "*.wav;*.mp3");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<MainComponent> (this)] (const juce::FileChooser& fc)
                          {
                              if (safe != nullptr && safe->session != nullptr && fc.getResult() != juce::File())
                                  safe->session->importSong (fc.getResult());
                          });
}

void MainComponent::chooseOriginalFile()
{
    if (session == nullptr || ! session->openSong())
        return;

    const auto& open = session->openSong();

    chooser = std::make_unique<juce::FileChooser> (
        "Locate " + open->song().source.fileName, open->originalFile.getParentDirectory(), "*.wav;*.mp3");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<MainComponent> (this)] (const juce::FileChooser& fc)
                          {
                              if (safe != nullptr && safe->session != nullptr && fc.getResult() != juce::File())
                                  safe->session->locateOriginal (fc.getResult());
                          });
}

void MainComponent::recordRecent (const OpenSong& song)
{
    if (settings == nullptr)
        return;

    auto list = settings->getRecentSongs();
    const auto& metadata = song.song();
    const auto earlier = recent::find (list, song.entry.key);

    // Keep the last place the original was seen when this open could not find it.
    const auto path = song.originalAvailable ? song.originalFile.getFullPathName()
                    : earlier                ? earlier->sourcePath
                                             : metadata.source.path;

    settings->setRecentSongs (
        recent::withRecorded (std::move (list), { song.entry.key, metadata.source.fileName, path }));

    if (const auto saved = settings->save(); saved.failed())
        logWarning ("Could not save recent songs: " + saved.getErrorMessage());
}

void MainComponent::openLibraryItem (const LibraryItem& item)
{
    if (session == nullptr)
        return;

    // A valid prepared copy always opens as it is; only a song without one is prepared again.
    if (item.opensFromCache())
        session->openCached (item.key);
    else
        session->importSong (juce::File (item.sourcePath));
}

void MainComponent::locateLibraryItem (const LibraryItem& item)
{
    chooser = std::make_unique<juce::FileChooser> (
        "Locate " + item.title, juce::File (item.sourcePath).getParentDirectory(), "*.wav;*.mp3");
    chooser->launchAsync (
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<MainComponent> (this), item] (const juce::FileChooser& fc)
        {
            if (safe == nullptr || safe->session == nullptr || fc.getResult() == juce::File())
                return;

            auto& session = *safe->session;

            if (! item.opensFromCache())
                return session.importSong (fc.getResult());

            // Prepared already: open it, then let the session check that this file is its original.
            if (! session.openSong() || session.openSong()->entry.key != item.key)
                session.openCached (item.key);

            if (session.openSong() && session.openSong()->entry.key == item.key)
                session.locateOriginal (fc.getResult());
        });
}

void MainComponent::removeLibraryItem (const LibraryItem& item)
{
    if (settings == nullptr)
        return;

    settings->setRecentSongs (recent::without (settings->getRecentSongs(), item.key));

    if (const auto saved = settings->save(); saved.failed())
        logWarning ("Could not save recent songs: " + saved.getErrorMessage());

    refreshLibrary();
}

void MainComponent::openCachedSong (const juce::String& key)
{
    if (session != nullptr)
        session->openCached (key);
}

// What only the running app knows is gathered here; the export reads the rest from disk.
DiagnosticsInput MainComponent::gatherDiagnosticsInput() const
{
    DiagnosticsInput input;
    input.paths = paths;
    input.worker = workerLocation;
    input.readiness = readiness;
    input.rendererStatus = songPanel.getVisualsDetail();

    if (settings != nullptr)
    {
        input.forceCpu = settings->getForceCpu();
        input.customAudioDevice = settings->getAudioDeviceState().isNotEmpty();
        input.recents = settings->getRecentSongs();
    }

    if (audio != nullptr)
        input.device = audio->devices().getStatus();

    return input;
}

void MainComponent::writeDiagnostics (const DiagnosticsInput& input,
                                      const juce::File& file,
                                      juce::Time when,
                                      std::function<void (const juce::Result&)> done)
{
    diagnosticsTask = std::make_unique<AsyncTask<juce::Result>> (
        messageThreadDispatcher(),
        [input, file, when] { return writeDiagnosticsZip (input, PathRedactor::fromEnvironment(), file, when); },
        [safe = juce::Component::SafePointer<MainComponent> (this), file, done = std::move (done)] (juce::Result result)
        {
            if (safe == nullptr)
                return;

            safe->diagnosticsTask.reset();
            safe->showDiagnosticsResult (file, result, ! done);

            if (done)
                done (result);
        });
}

void MainComponent::exportDiagnosticsTo (const juce::File& file, std::function<void (const juce::Result&)> done)
{
    if (diagnosticsTask != nullptr)
        return;

    writeDiagnostics (gatherDiagnosticsInput(), file, juce::Time::getCurrentTime(), std::move (done));
}

void MainComponent::exportDiagnostics()
{
    if (diagnosticsTask != nullptr)
        return;

    const auto input = gatherDiagnosticsInput();
    const auto now = juce::Time::getCurrentTime();
    const auto suggested =
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (diagnosticsFileName (now));

    chooser = std::make_unique<juce::FileChooser> ("Save diagnostics", suggested, "*.zip");
    chooser->launchAsync (
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::warnAboutOverwriting,
        [safe = juce::Component::SafePointer<MainComponent> (this), input, now] (const juce::FileChooser& fc)
        {
            if (safe != nullptr && fc.getResult() != juce::File())
                safe->writeDiagnostics (input, fc.getResult().withFileExtension ("zip"), now, {});
        });
}

void MainComponent::showDiagnosticsResult (const juce::File& file, const juce::Result& result, bool reveal)
{
    ErrorReport report;
    report.area = FailureArea::environment;
    report.severity = result.wasOk() ? Severity::info : Severity::error;
    report.code = result.wasOk() ? "diagnostics_saved" : "diagnostics_failed";
    report.message =
        result.wasOk() ? "Diagnostics saved to " + file.getFullPathName() + "." : "The diagnostics could not be saved.";
    report.guidance = result.wasOk() ? "Attach the zip to a bug report. Your user folder name is replaced in it, "
                                       "and no song audio is included."
                                     : result.getErrorMessage();
    report.actions.push_back ({ RecoveryKind::dismiss, "Dismiss" });
    noticeBanner.setReport (report);
    resized();

    logInfo (report.message + (result.wasOk() ? "" : " " + result.getErrorMessage()));

    if (result.wasOk() && reveal)
        file.revealToUser();
}

void MainComponent::showCacheWindow()
{
    if (cache == nullptr || session == nullptr)
        return;

    if (cacheWindow != nullptr)
    {
        cacheWindow->toFront (true);
        return;
    }

    auto* window = new CacheWindow();
    auto& view = window->view();
    view.onOpen = [this, safe = juce::Component::SafePointer<CacheWindow> (window)] (const juce::String& key)
    {
        // Close the window after this click has been handled, then open the song.
        juce::MessageManager::callAsync (
            [this, safe, key]
            {
                if (safe != nullptr)
                    delete safe.getComponent();

                if (session != nullptr)
                    session->openCached (key);
            });
    };
    view.onDelete = [this] (const juce::String& key) { removeFromCache (key); };
    view.onVerify = [this] (const juce::String& key) { verifyCachedSong (key); };
    view.onClearAll = [this] { removeFromCache (std::nullopt); };

    if (session->openSong())
        view.setOpenKey (session->openSong()->entry.key);

    window->centreAroundComponent (this, window->getWidth(), window->getHeight());
    window->setVisible (true);
    window->toFront (true);
    cacheWindow = window;
    updateCacheLock();
    refreshCache();
}

void MainComponent::updateCacheLock()
{
    if (cacheWindow == nullptr || session == nullptr)
        return;

    const auto reason = verificationTask != nullptr ? "Verifying prepared audio..."
                      : removalTask != nullptr      ? "Deleting..."
                      : session->status().isBusy()
                          ? "A song is being prepared or opened. Songs can be opened or deleted when that is done."
                          : "";
    cacheWindow->view().setLocked (verificationTask != nullptr || removalTask != nullptr || session->status().isBusy(),
                                   reason);
}

void MainComponent::verifyCachedSong (const juce::String& key)
{
    if (cache == nullptr || session == nullptr || session->status().isBusy() || verificationTask != nullptr
        || removalTask != nullptr)
        return;

    verificationCancel = std::make_shared<std::atomic<bool>> (false);
    verificationTask = std::make_unique<AsyncTask<CacheVerification>> (
        messageThreadDispatcher(),
        [store = cache.get(), key, flag = verificationCancel]
        { return store->verifyEntry (key, [flag] { return flag->load(); }); },
        [this] (CacheVerification result)
        {
            verificationTask.reset();
            verificationCancel.reset();
            if (cacheWindow != nullptr)
                cacheWindow->view().showMessage (result.message, ! result.ok);
            updateCacheLock();
        });
    if (cacheWindow != nullptr)
        cacheWindow->view().showMessage ("Checking every stem. Playback remains available; no files are changed.",
                                         false);
    updateCacheLock();
}

void MainComponent::refreshCache()
{
    if (cache == nullptr || cacheWindow == nullptr)
        return;

    if (cacheTask != nullptr)
    {
        cacheReloadPending = true;
        return;
    }

    cacheTask = std::make_unique<AsyncTask<CacheOverview>> (
        messageThreadDispatcher(),
        [store = cache.get()] { return readCacheOverview (*store); },
        [this] (CacheOverview overview)
        {
            cacheTask.reset();

            if (cacheWindow != nullptr)
                cacheWindow->view().setOverview (std::move (overview));

            if (std::exchange (cacheReloadPending, false))
                refreshCache();
        });
}

void MainComponent::removeFromCache (std::optional<juce::String> key)
{
    if (cache == nullptr || session == nullptr || session->status().isBusy() || removalTask != nullptr
        || verificationTask != nullptr)
        return;

    // Windows will not delete files the player has open: close the song first, which lets go of its
    // stems, artwork and visuals. Clearing the cache closes whatever is open.
    if (session->openSong() && (! key || *key == session->openSong()->entry.key))
        session->closeSong();

    removalTask = std::make_unique<AsyncTask<juce::Result>> (
        messageThreadDispatcher(),
        [store = cache.get(), key] { return key ? store->deleteEntry (*key) : store->clearAll(); },
        [this, key] (juce::Result result)
        {
            removalTask.reset();
            logInfo (key ? "Deleted cache entry " + *key + ": " + (result.wasOk() ? "ok" : result.getErrorMessage())
                         : juce::String ("Cleared the cache: ") + (result.wasOk() ? "ok" : result.getErrorMessage()));

            if (cacheWindow != nullptr)
                cacheWindow->view().showMessage (result.wasOk() ? (key ? "Deleted." : "The cache is empty.")
                                                                : result.getErrorMessage(),
                                                 result.failed());

            updateCacheLock();
            refreshCache();
            refreshLibrary();
        });

    updateCacheLock();
}

void MainComponent::showAudioSettings()
{
    if (audio == nullptr)
        return;

    if (audioSettingsDialog != nullptr)
        audioSettingsDialog->toFront (true);
    else
        audioSettingsDialog = showAudioSettingsDialog (audio->devices(), this);
}

void MainComponent::startReadinessCheck()
{
    if (checker != nullptr && readiness.state == Readiness::State::checking)
        return;

    readiness = {};
    readinessPanel.setReadiness (readiness);
    updateImportAllowed();
    repaint();

    logInfo ("Checking worker readiness: " + workerLocation.pythonExecutable().getFullPathName() + " ("
             + workerLocation.source + ")");

    checker.reset();
    checker = std::make_unique<ReadinessChecker> (
        workerLocation,
        readinessTimeoutMs,
        [safeThis = juce::Component::SafePointer<MainComponent> (this)] (const Readiness& result)
        {
            juce::MessageManager::callAsync (
                [safeThis, result]
                {
                    if (safeThis != nullptr)
                        safeThis->readinessFinished (result);
                });
        });
}

void MainComponent::readinessFinished (const Readiness& result)
{
    readiness = result;
    readinessPanel.setReadiness (result);
    updateImportAllowed();
    repaint();

    const auto summary = "Worker readiness: " + Readiness::stateName (result.state) + ": " + result.headline;

    if (result.isReady())
        logInfo (summary + "\n" + result.details);
    else
        logWarning (summary + "\n" + result.details);

    if (onReadinessChanged != nullptr)
        onReadinessChanged (result);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void MainComponent::paintPage (juce::Graphics& g)
{
    const auto background = getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId);
    const auto text = getLookAndFeel().findColour (juce::Label::textColourId);

    g.fillAll (background);

    auto header = headerArea;
    g.setColour (text);
    g.setFont (juce::FontOptions (26.0f, juce::Font::bold));
    g.drawText (appinfo::name, header.removeFromLeft (90), juce::Justification::centredLeft);

    g.setColour (text.withAlpha (0.6f));
    g.setFont (juce::FontOptions (14.0f));
    g.drawText ("v" + appinfo::version(), header.removeFromLeft (120), juce::Justification::centredLeft);
}

void MainComponent::resized()
{
    viewport.setBounds (getLocalBounds());

    // The page is as tall as the window, or taller when everything needs more room (then it scrolls).
    const auto mixHeight = mixPanel != nullptr ? mixPanel->getPreferredHeight() + margin : 0;
    const auto needed = margin * 2 + 40 + margin / 2 + readinessPanel.getPreferredHeight() + margin
                      + importPanel.getPreferredHeight() + margin + mixHeight + songPanelHeight;
    const auto neededWithNotice = needed + (noticeBanner.isVisible() ? noticeBanner.getPreferredHeight() + margin : 0);
    page.setSize (viewport.getMaximumVisibleWidth(), std::max (viewport.getHeight(), neededWithNotice));
}

void MainComponent::layoutPage()
{
    auto bounds = page.getLocalBounds().reduced (margin);

    headerArea = bounds.removeFromTop (40);
    audioSettingsButton.setBounds (headerArea.removeFromRight (150).withSizeKeepingCentre (150, 28));
    headerArea.removeFromRight (8);
    cacheButton.setBounds (headerArea.removeFromRight (130).withSizeKeepingCentre (130, 28));
    headerArea.removeFromRight (8);
    exportButton.setBounds (headerArea.removeFromRight (180).withSizeKeepingCentre (180, 28));
    bounds.removeFromTop (margin / 2);

    if (noticeBanner.isVisible())
    {
        noticeBanner.setBounds (bounds.removeFromTop (noticeBanner.getPreferredHeight()));
        bounds.removeFromTop (margin);
    }

    readinessPanel.setBounds (bounds.removeFromTop (readinessPanel.getPreferredHeight()));
    bounds.removeFromTop (margin);

    songPanel.setBounds (bounds.removeFromBottom (songPanelHeight));
    bounds.removeFromBottom (margin);

    if (mixPanel != nullptr)
    {
        mixPanel->setBounds (bounds.removeFromBottom (mixPanel->getPreferredHeight()));
        bounds.removeFromBottom (margin);
    }

    importPanel.setBounds (bounds);
    page.repaint();
}

void MainComponent::reportRendererFallback (const RendererFallbackInfo& info)
{
    const auto report = reportFromRenderer (info);
    logWarning ("Renderer: " + report.toText());
    noticeBanner.setReport (report);
    resized();
}

void MainComponent::handleRecovery (RecoveryKind kind)
{
    switch (kind)
    {
        case RecoveryKind::recheck:
            startReadinessCheck();
            break;
        case RecoveryKind::retry:
            if (session != nullptr)
                session->retry();
            break;
        case RecoveryKind::retryOnCpu:
            if (session != nullptr)
                session->retryOnCpu();
            break;
        case RecoveryKind::chooseAnotherFile:
            chooseSourceFile();
            break;
        case RecoveryKind::locateOriginal:
            chooseOriginalFile();
            break;
        case RecoveryKind::openAudioSettings:
            showAudioSettings();
            break;
        case RecoveryKind::reconnectDevice:
            if (audio != nullptr)
                audio->devices().reconnect();
            break;
        case RecoveryKind::openLogs:
            if (paths.has_value())
                paths->logsDirectory().revealToUser();
            break;
        case RecoveryKind::exportDiagnostics:
            exportDiagnostics();
            break;
        case RecoveryKind::dismiss:
            if (session != nullptr)
                session->dismissFailure();
            break;
    }
}

void MainComponent::applyTempoOverride (double bpm, const juce::String& mode)
{
    if (session == nullptr)
        return;

    SongMetadata::BpmOverride entry;
    entry.value = bpm;
    entry.mode = mode;

    if (const auto saved = session->setBpmOverride (entry); saved.failed())
    {
        logWarning ("Could not store the tempo: " + saved.getErrorMessage());
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                "The tempo was not saved",
                                                "It is in use for now, but " + saved.getErrorMessage());
    }
}

void MainComponent::clearTempoOverride()
{
    if (session == nullptr || ! session->openSong())
        return;

    if (const auto saved = session->setBpmOverride (std::nullopt); saved.failed())
    {
        logWarning ("Could not clear the tempo: " + saved.getErrorMessage());
        return;
    }

    if (audio != nullptr) // the tempo the song would have had: detected, else the default
        audio->mix().parameters().set (dsp::GlobalControl::tempoBpm,
                                       static_cast<float> (session->openSong()->song().effectiveBpm()));
}
} // namespace snds
