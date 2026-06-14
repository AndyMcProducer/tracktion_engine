/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2024
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

#if TRACKTION_ENABLE_ARA

//==============================================================================
#if JUCE_MSVC
#pragma warning(push, 0)
#elif JUCE_CLANG
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnon-virtual-dtor"
#pragma clang diagnostic ignored "-Wreorder"
#pragma clang diagnostic ignored "-Wunsequenced"
#pragma clang diagnostic ignored "-Wint-to-pointer-cast"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Woverloaded-virtual"
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#if __clang_major__ >= 10
#pragma clang diagnostic ignored "-Wpragma-pack"
#endif
#endif

#undef PRAGMA_ALIGN_SUPPORTED
#undef VST_FORCE_DEPRECATED
#define VST_FORCE_DEPRECATED 0

#ifndef JUCE_MSVC
#define __cdecl
#endif

// If you get an error here, in order to build with ARA support you'll need
// to include the SDK in your header search paths!
#include "ARA_API/ARAVST3.h"
#include "ARA_Library/Dispatch/ARAHostDispatch.h"

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"

namespace ARA
{
DEF_CLASS_IID (IMainFactory)
DEF_CLASS_IID (IPlugInEntryPoint)
DEF_CLASS_IID (IPlugInEntryPoint2)
} // namespace ARA

#if JUCE_MSVC
#pragma warning(pop)
#elif JUCE_CLANG
#pragma clang diagnostic pop
#endif

namespace tracktion
{
inline namespace engine
{
    using namespace ARA;

#if TRACKTION_ENABLE_ARA
#include "tracktion_ARALogging.h"
#include "tracktion_MelodyneInstanceFactory.h"
#endif

    struct ARAClipPlayer : private Selectable::Listener
    {
#include "tracktion_ARAWrapperFunctions.h"
#include "tracktion_ARAWrapperInterfaces.h"

        //==============================================================================
        ARAClipPlayer (Edit& ed, MelodyneFileReader& o, AudioClipBase& c)
            : Selectable::Listener (ed.tempoSequence), owner (o), clip (c), file (c.getAudioFile()), edit (ed)
        {
            TRACKTION_ASSERT_MESSAGE_THREAD
            jassert (file.getFile().existsAsFile());
        }

        ~ARAClipPlayer()
        {
            CRASH_TRACER
            TRACKTION_ASSERT_MESSAGE_THREAD

            contentAnalyserChecker = nullptr;
            modelUpdater = nullptr;
            contentUpdater = nullptr;

            // Needs to happen before killing off ARA stuff
            if (auto p = getPlugin())
            {
                p->hideWindowForShutdown();

                if (auto pi = p->getAudioPluginInstance())
                    pi->releaseResources();
            }

            if (auto doc = getDocument())
            {
                if (doc->dci != nullptr)
                {
                    {
                        const ScopedDocumentEditor sde (*this, false);
                        playbackRegionAndSource = nullptr;
                    }

                    melodyneInstance = nullptr;
                }
            }
        }

        //==============================================================================
        Edit& getEdit() { return edit; }
        AudioClipBase& getClip() { return clip; }
        ExternalPlugin* getPlugin() { return melodyneInstance != nullptr ? melodyneInstance->plugin.get() : nullptr; }
        const ARAFactory* getARAFactory() const { return melodyneInstance != nullptr ? melodyneInstance->factory : nullptr; }

        //==============================================================================
        bool initialise (ARAClipPlayer* clipToClone)
        {
            TRACKTION_ASSERT_MESSAGE_THREAD
            CRASH_TRACER

            if (auto doc = getDocument())
            {
                ExternalPlugin::Ptr p = MelodyneInstanceFactory::getInstance (edit.engine).createPlugin (edit);

                if (p == nullptr || getDocument() == nullptr)
                    return false;

                melodyneInstance.reset (MelodyneInstanceFactory::getInstance (edit.engine).createInstance (*p, doc->dcRef));

                if (melodyneInstance == nullptr)
                    return false;

                updateContent (clipToClone);

                return playbackRegionAndSource != nullptr
                       && playbackRegionAndSource->playbackRegion != nullptr;
            }

            return false;
        }

        void contentHasChanged()
        {
            CRASH_TRACER
            updateContent (nullptr);
            owner.sendChangeMessage();
        }

        void selectableObjectChanged (Selectable*) override
        {
            if (auto doc = getDocument())
            {
                if (doc->musicalContext != nullptr)
                {
                    doc->beginEditing (true);
                    doc->musicalContext->update();
                    doc->endEditing (true);
                }
            }
        }

        void selectableObjectAboutToBeDeleted (Selectable*) override {}

        //==============================================================================
        void updateContent (ARAClipPlayer* clipToClone)
        {
            CRASH_TRACER
            TRACKTION_ASSERT_MESSAGE_THREAD

            if (juce::MessageManager::getInstance()->isThisTheMessageThread()
                && getEdit().getTransport().isAllowedToReallocate())
            {
                contentUpdater = nullptr;
                internalUpdateContent (clipToClone);
            }
            else
            {
                if (contentUpdater == nullptr)
                {
                    contentUpdater = std::make_unique<ContentUpdater> (*this);
                }
                else
                {
                    if (! contentUpdater->isTimerRunning()) //To avoid resetting it
                        contentUpdater->startTimer (100);
                }
            }
        }

        //==============================================================================
        juce::MidiMessageSequence getAnalysedMIDISequence()
        {
            CRASH_TRACER

            const int midiChannel = 1;
            juce::MidiMessageSequence result;

            if (auto doc = getDocument())
            {
                const ARADocumentControllerInterface* dci = doc->dci;
                ARADocumentControllerRef dcRef = doc->dcRef;
                ARAAudioSourceRef audioSourceRef = playbackRegionAndSource->audioSource->audioSourceRef;

                if (dci->isAudioSourceContentAvailable (dcRef, audioSourceRef, kARAContentTypeNotes))
                {
                    ARAContentReaderRef contentReaderRef = dci->createAudioSourceContentReader (dcRef, audioSourceRef, kARAContentTypeNotes, nullptr);
                    int numEvents = (int) dci->getContentReaderEventCount (dcRef, contentReaderRef);

                    for (int i = 0; i < numEvents; ++i)
                    {
                        if (auto note = static_cast<const ARAContentNote*> (dci->getContentReaderDataForEvent (dcRef, contentReaderRef, i)))
                        {
                            if (note->pitchNumber != kARAInvalidPitchNumber)
                            {
                                result.addEvent (juce::MidiMessage::noteOn (midiChannel, note->pitchNumber, static_cast<float> (note->volume)),
                                                 note->startPosition);

                                result.addEvent (juce::MidiMessage::noteOff (midiChannel, note->pitchNumber),
                                                 note->startPosition + note->noteDuration);
                            }
                        }
                    }

                    dci->destroyContentReader (dcRef, contentReaderRef);
                }

                result.updateMatchedPairs();
            }

            return result;
        }

        //==============================================================================
        void setViewSelection()
        {
            if (playbackRegionAndSource != nullptr)
                playbackRegionAndSource->setViewSelection();
        }

        //==============================================================================
        void startProcessing() { TRACKTION_ASSERT_MESSAGE_THREAD if (playbackRegionAndSource != nullptr) playbackRegionAndSource->enable(); }
        void stopProcessing() { TRACKTION_ASSERT_MESSAGE_THREAD if (playbackRegionAndSource != nullptr) playbackRegionAndSource->disable(); }

        class ContentAnalyser
        {
        public:
            ContentAnalyser (const ARAClipPlayer& p) : pimpl (p)
            {
            }

            bool isAnalysing()
            {
                callBlocking ([this]
                              { updateAnalysingContent(); });

                return analysingContent;
            }

            void updateAnalysingContent()
            {
                CRASH_TRACER

                auto doc = pimpl.getDocument();

                if (doc == nullptr)
                {
                    analysingContent = false;
                    return;
                }

                const ARADocumentControllerInterface* dci = doc->dci;
                ARADocumentControllerRef dcRef = doc->dcRef;
                ARAAudioSourceRef audioSourceRef = nullptr;

                if (pimpl.playbackRegionAndSource != nullptr)
                    if (pimpl.playbackRegionAndSource->audioSource != nullptr)
                        audioSourceRef = pimpl.playbackRegionAndSource->audioSource->audioSourceRef;

                if (dci != nullptr && dcRef != nullptr && audioSourceRef != nullptr)
                {
                    if (firstCall)
                    {
                        auto araFactory = pimpl.getARAFactory();
                        for (ARAContentType contentType : { kARAContentTypeBarSignatures, kARAContentTypeTempoEntries })
                        {
                            for (int i = 0; i < (int) araFactory->analyzeableContentTypesCount; i++)
                            {
                                if (araFactory->analyzeableContentTypes[i] == contentType)
                                {
                                    typesBeingAnalyzed.push_back (contentType);
                                    break;
                                }
                            }
                        }

                        if (! typesBeingAnalyzed.empty())
                            dci->requestAudioSourceContentAnalysis (dcRef, audioSourceRef, (ARASize) typesBeingAnalyzed.size(), typesBeingAnalyzed.data());

                        firstCall = false;
                    }

                    analysingContent = false;
                    for (ARAContentType contentType : typesBeingAnalyzed)
                    {
                        analysingContent = (dci->isAudioSourceContentAnalysisIncomplete (dcRef, audioSourceRef, contentType) != kARAFalse);
                        if (analysingContent)
                            break;
                    }
                }
                else
                {
                    analysingContent = false;
                }
            }

        private:
            const ARAClipPlayer& pimpl;
            std::vector<ARAContentType> typesBeingAnalyzed;
            volatile bool analysingContent = false;
            bool firstCall = true;

            ContentAnalyser() = delete;
            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ContentAnalyser)
        };

        friend class ContentAnalyser;

        std::unique_ptr<ContentAnalyser> contentAnalyserChecker;

        bool isAnalysingContent() const
        {
            return contentAnalyserChecker->isAnalysing();
        }

        ARADocument* getDocument() const;

    private:
        //==============================================================================
        MelodyneFileReader& owner;
        AudioClipBase& clip;
        const AudioFile file;
        Edit& edit;

        std::unique_ptr<MelodyneInstance> melodyneInstance;
        std::unique_ptr<PlaybackRegionAndSource> playbackRegionAndSource;
        HashCode currentHashCode = 0;

        //==============================================================================
        struct ScopedDocumentEditor
        {
            ScopedDocumentEditor (ARAClipPlayer& o, bool restartModelUpdaterLater)
                : owner (o), restartTimerLater (restartModelUpdaterLater)
            {
                if (restartTimerLater)
                    owner.modelUpdater = nullptr;

                owner.getDocument()->beginEditing (false);
            }

            ~ScopedDocumentEditor()
            {
                if (auto doc = owner.getDocument())
                {
                    doc->endEditing (false);

                    if (restartTimerLater)
                        owner.modelUpdater = std::make_unique<ModelUpdater> (*doc);
                }
            }

        private:
            ARAClipPlayer& owner;
            const bool restartTimerLater;

            JUCE_DECLARE_NON_COPYABLE (ScopedDocumentEditor)
        };

        //==============================================================================
        /** NB: Must delete the old objects *after* creating the new ones, because Melodyne crashes
            if you deselect a play region and then try to select a different one.
            But doing it in the opposite order seems to work ok.
    */
        void recreateTrack (ARAClipPlayer* clipToClone)
        {
            CRASH_TRACER
            TRACKTION_ASSERT_MESSAGE_THREAD

            jassert (melodyneInstance != nullptr);
            jassert (melodyneInstance->factory != nullptr);
            jassert (melodyneInstance->extensionInstance != nullptr);

            auto oldTrack = std::move (playbackRegionAndSource);

            playbackRegionAndSource = std::make_unique<PlaybackRegionAndSource> (*getDocument(), clip, *melodyneInstance->factory, *melodyneInstance->extensionInstance, juce::String::toHexString (currentHashCode), clipToClone != nullptr ? clipToClone->playbackRegionAndSource.get() : nullptr);

            if (oldTrack != nullptr)
            {
                const ScopedDocumentEditor sde (*this, false);
                oldTrack = nullptr;
            }
        }

        void internalUpdateContent (ARAClipPlayer* clipToClone)
        {
            CRASH_TRACER
            TRACKTION_ASSERT_MESSAGE_THREAD

            if (auto doc = getDocument())
            {
                jassert (doc->dci != nullptr);

                contentAnalyserChecker = nullptr;
                modelUpdater = nullptr; // Can't be editing the document in any way while restoring

                HashCode newHashCode = file.getHash()
                                       ^ file.getFile().getLastModificationTime().toMilliseconds()
                                       ^ static_cast<HashCode> (clip.itemID.getRawID());

                if (currentHashCode != newHashCode)
                {
                    currentHashCode = newHashCode;
                    const ScopedDocumentEditor sde (*this, true);

                    recreateTrack (clipToClone);
                }
                else
                {
                    if (playbackRegionAndSource != nullptr
                        && playbackRegionAndSource->playbackRegion != nullptr)
                    {
                        const ScopedDocumentEditor sde (*this, true);
                        playbackRegionAndSource->playbackRegion->updateRange();
                    }
                }

                modelUpdater = std::make_unique<ModelUpdater> (*doc);

                if (contentAnalyserChecker == nullptr)
                    contentAnalyserChecker = std::make_unique<ContentAnalyser> (*this);
            }
        }

        //==============================================================================
        struct ContentUpdater : public juce::Timer
        {
            ContentUpdater (ARAClipPlayer& p) : owner (p) { startTimer (100); }

            ARAClipPlayer& owner;

            void timerCallback() override
            {
                CRASH_TRACER

                if (owner.getEdit().getTransport().isAllowedToReallocate())
                {
                    owner.internalUpdateContent (nullptr);
                    stopTimer();
                }
            }

            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ContentUpdater)
        };

        std::unique_ptr<ContentUpdater> contentUpdater;

        //==============================================================================
        struct ModelUpdater : private juce::Timer
        {
            ModelUpdater (ARADocument& d) : document (d) { startTimer (3000); }

            ARADocument& document;

            void timerCallback() override
            {
                CRASH_TRACER
                if (document.dci != nullptr && document.dcRef != nullptr)
                    document.dci->notifyModelUpdates (document.dcRef);
            }

            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModelUpdater)
        };

        std::unique_ptr<ModelUpdater> modelUpdater;

        //==============================================================================
        ARAClipPlayer() = delete;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ARAClipPlayer)
    };

    //==============================================================================
    MelodyneFileReader::MelodyneFileReader (Edit& ed, AudioClipBase& clip)
    {
        TRACKTION_ASSERT_MESSAGE_THREAD
        CRASH_TRACER

        player = std::make_unique<ARAClipPlayer> (ed, *this, clip);

        if (! player->initialise (nullptr))
            player = nullptr;
    }

    MelodyneFileReader::MelodyneFileReader (Edit& ed, AudioClipBase& clip, MelodyneFileReader& other)
    {
        TRACKTION_ASSERT_MESSAGE_THREAD
        CRASH_TRACER

        if (other.player != nullptr)
        {
            player = std::make_unique<ARAClipPlayer> (ed, *this, clip);

            if (! player->initialise (other.player.get()))
                player = nullptr;
        }

        jassert (player != nullptr);
    }

    MelodyneFileReader::~MelodyneFileReader()
    {
        TRACKTION_ASSERT_MESSAGE_THREAD
        CRASH_TRACER

        if (player != nullptr)
            if (auto plugin = player->getPlugin())
                if (auto pi = plugin->getAudioPluginInstance())
                    pi->setPlayHead (nullptr);

        auto toDestroy = std::move (player);
    }

    //==============================================================================
    void MelodyneFileReader::showPluginWindow()
    {
        if (player != nullptr)
            player->setViewSelection();

        if (auto p = getPlugin())
            p->showWindowExplicitly();
    }

    void MelodyneFileReader::hidePluginWindow()
    {
        if (auto p = getPlugin())
            p->hideWindowForShutdown();
    }

    ExternalPlugin* MelodyneFileReader::getPlugin()
    {
        if (isValid())
            return player->getPlugin();

        return {};
    }

    //==============================================================================
    bool MelodyneFileReader::isAnalysingContent()
    {
        return player != nullptr && player->isAnalysingContent();
    }

    void MelodyneFileReader::sourceClipChanged()
    {
        if (player != nullptr)
            player->updateContent (nullptr);
    }

    //==============================================================================
    juce::MidiMessageSequence MelodyneFileReader::getAnalysedMIDISequence()
    {
        if (player != nullptr)
            return player->getAnalysedMIDISequence();

        return {};
    }

    void MelodyneFileReader::cleanUpOnShutdown()
    {
        MelodyneInstanceFactory::shutdown();
    }

    juce::String MelodyneFileReader::getPreferredPluginName (Engine& engine)
    {
        return engine.getPropertyStorage().getProperty (SettingID::araPreferredPlugin, juce::String());
    }

    void MelodyneFileReader::setPreferredPluginName (Engine& engine, const juce::String& name)
    {
        engine_ara_log (("MelodyneFileReader::setPreferredPluginName: " + name).toRawUTF8());
        engine.getPropertyStorage().setProperty (SettingID::araPreferredPlugin, name);
    }

    //==============================================================================
    struct ARADocumentHolder::Pimpl
    {
        Pimpl (Edit& e) : edit (e) {}

        void initialise()
        {
            TRACKTION_ASSERT_MESSAGE_THREAD
            araDocument.reset (ARAClipPlayer::createDocument (edit));

            if (araDocument != nullptr)
            {
                araDocument->beginRestoringState (edit.getARADocument().lastState);

                visitAllTrackItems (edit, [] (TrackItem& i)
                                    {
                if (auto c = dynamic_cast<AudioClipBase*> (&i))
                    c->loadMelodyneState();

                return true; });

                araDocument->endRestoringState();
            }
        }

        Edit& edit;
        std::unique_ptr<ARAClipPlayer::ARADocument> araDocument;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Pimpl)
    };

    ARADocumentHolder::ARADocumentHolder (Edit& e, const juce::ValueTree& v)
        : edit (e), lastState (v)
    {
    }

    ARADocumentHolder::~ARADocumentHolder()
    {
        TRACKTION_ASSERT_MESSAGE_THREAD
        CRASH_TRACER
        pimpl = nullptr;
    }

    ARADocumentHolder::Pimpl* ARADocumentHolder::getPimpl()
    {
        if (pimpl == nullptr)
        {
            CRASH_TRACER
            pimpl = std::make_unique<Pimpl> (edit);
            callBlocking ([this]()
                          { pimpl->initialise(); });
        }

        return pimpl.get();
    }

    void ARADocumentHolder::flushStateToValueTree()
    {
        TRACKTION_ASSERT_MESSAGE_THREAD

        if (pimpl != nullptr)
            if (pimpl->araDocument != nullptr)
                pimpl->araDocument->flushStateToValueTree (lastState);
    }

    ARAClipPlayer::ARADocument* ARAClipPlayer::getDocument() const
    {
        if (auto p = edit.getARADocument().getPimpl())
            return p->araDocument.get();

        return {};
    }

#if TRACKTION_ENABLE_ARA
    //==============================================================================
    static std::unique_ptr<juce::AudioPluginInstance> createMelodynePlugin (Engine& engine,
                                                                            const char* formatToTry,
                                                                            const juce::Array<juce::PluginDescription>& araDescs)
    {
        CRASH_TRACER

        juce::String error;
        auto& pfm = engine.getPluginManager().pluginFormatManager;

        for (auto pd : araDescs)
            if (pd.pluginFormatName == formatToTry)
                if (auto p = pfm.createPluginInstance (pd, 44100.0, 512, error))
                    return p;

        return {};
    }

    static std::unique_ptr<juce::AudioPluginInstance> createMelodynePlugin (Engine& engine)
    {
        CRASH_TRACER
        TRACKTION_ASSERT_MESSAGE_THREAD
        engine_ara_log ("createMelodynePlugin: Starting ARA plugin discovery");

        auto araDescs = engine.getPluginManager().getARACompatiblePlugDescriptions();
        engine_ara_log (("createMelodynePlugin: Found " + juce::String (araDescs.size()) + " ARA-compatible plugins").toRawUTF8());

        for (auto& pd : araDescs)
            engine_ara_log (("createMelodynePlugin: Available ARA plugin: " + pd.name + " (" + pd.pluginFormatName + ") - " + pd.fileOrIdentifier).toRawUTF8());

        auto preferred = MelodyneFileReader::getPreferredPluginName (engine);
        engine_ara_log (("createMelodynePlugin: Preferred plugin name from settings: " + preferred).toRawUTF8());

        if (preferred.isNotEmpty())
        {
            for (auto& pd : araDescs)
            {
                if (pd.name == preferred || pd.fileOrIdentifier == preferred)
                {
                    engine_ara_log (("createMelodynePlugin: Found match for preferred plugin: " + pd.name).toRawUTF8());
                    engine_ara_log ("createMelodynePlugin: Attempting instance creation...");
                    if (auto p = createMelodynePlugin (engine, pd.pluginFormatName.toRawUTF8(), { pd }))
                    {
                        engine_ara_log ("createMelodynePlugin: SUCCEEDED in creating preferred plugin instance");
                        return p;
                    }
                    engine_ara_log ("createMelodynePlugin: FAILED to create preferred plugin instance, falling back...");
                }
            }
        }

        if (preferred.isEmpty())
            engine_ara_log ("createMelodynePlugin: No preferred plugin set");
        else
            engine_ara_log ("createMelodynePlugin: Preferred plugin not found in available list, falling back to Melodyne search...");

        for (auto& pd : araDescs)
        {
            if (pd.name.containsIgnoreCase ("Melodyne"))
            {
                engine_ara_log (("createMelodynePlugin: Picking Melodyne fallback: " + pd.name).toRawUTF8());
                if (auto p = createMelodynePlugin (engine, pd.pluginFormatName.toRawUTF8(), { pd }))
                {
                    engine_ara_log ("createMelodynePlugin: Created Melodyne fallback instance");
                    return p;
                }
            }
        }

        engine_ara_log ("createMelodynePlugin: Final attempt - creating first available VST3 ARA plugin");
        if (auto p = createMelodynePlugin (engine, "VST3", araDescs))
        {
            engine_ara_log (("createMelodynePlugin: Created random VST3 ARA plugin: " + p->getName()).toRawUTF8());
            return p;
        }

        engine_ara_log ("createMelodynePlugin: TOTAL FAILURE to create any ARA plugin");
        return {};
    }

    MelodyneInstanceFactory::MelodyneInstanceFactory (Engine& engine)
    {
        TRACKTION_ASSERT_MESSAGE_THREAD
        CRASH_TRACER
        engine_ara_log ("MelodyneInstanceFactory::MelodyneInstanceFactory constructor started");

        plugin = createMelodynePlugin (engine);

        if (plugin != nullptr)
        {
            engine_ara_log (("MelodyneInstanceFactory: Found ARA dummy plugin: " + plugin->getName()).toRawUTF8());
            getFactoryForPlugin();

            if (factory != nullptr)
            {
                engine_ara_log ("MelodyneInstanceFactory: Found ARA factory");

                if (canBeUsedAsTimeStretchEngine (*factory))
                    engine_ara_log ("MelodyneInstanceFactory: Plugin supports time stretch engine roles");
                else
                    engine_ara_log ("MelodyneInstanceFactory: Plugin does NOT support time stretch (Editor-only ARA?)");

                // Initialize correctly regardless of time-stretch support
                ARAAssertFunction* assertFuncPtr = nullptr;
#if JUCE_LOG_ASSERTIONS || JUCE_DEBUG
                static ARAAssertFunction assertFunction = assertCallback;
                assertFuncPtr = &assertFunction;
#endif

                const SizedStruct<ARA_STRUCT_MEMBER (ARAInterfaceConfiguration, assertFunctionAddress)> interfaceConfig = {
                    std::min<ARAAPIGeneration> (factory->highestSupportedApiGeneration, kARAAPIGeneration_2_0_Final),
                    assertFuncPtr
                };

                factory->initializeARAWithConfiguration (&interfaceConfig);
            }
            else
            {
                engine_ara_log ("MelodyneInstanceFactory: ERROR: Factory is NULL");
                jassertfalse;
                plugin = nullptr;
            }
        }
        else
        {
            engine_ara_log ("MelodyneInstanceFactory: ERROR: No ARA plugin instance created");
        }
    }

    MelodyneInstanceFactory::~MelodyneInstanceFactory()
    {
        if (factory != nullptr)
            factory->uninitializeARA();

        plugin = nullptr;
    }
#endif

} // namespace engine
} // namespace tracktion

#else

namespace tracktion
{
inline namespace engine
{

    struct ARADocumentHolder::Pimpl
    {
    };
    struct ARAClipPlayer
    {
    };

    MelodyneFileReader::MelodyneFileReader (Edit&, AudioClipBase&) {}
    MelodyneFileReader::MelodyneFileReader (Edit&, AudioClipBase&, MelodyneFileReader&) {}
    MelodyneFileReader::~MelodyneFileReader() {}

    void MelodyneFileReader::cleanUpOnShutdown() {}
    ExternalPlugin* MelodyneFileReader::getPlugin() { return {}; }
    void MelodyneFileReader::showPluginWindow() {}
    void MelodyneFileReader::hidePluginWindow() {}
    bool MelodyneFileReader::isAnalysingContent() { return false; }
    juce::MidiMessageSequence MelodyneFileReader::getAnalysedMIDISequence() { return {}; }
    void MelodyneFileReader::sourceClipChanged() {}

    ARADocumentHolder::ARADocumentHolder (Edit& e, const juce::ValueTree&) : edit (e) { juce::ignoreUnused (edit); }
    ARADocumentHolder::~ARADocumentHolder() {}
    ARADocumentHolder::Pimpl* ARADocumentHolder::getPimpl() { return {}; }
    void ARADocumentHolder::flushStateToValueTree() {}

} // namespace engine
} // namespace tracktion

#endif
