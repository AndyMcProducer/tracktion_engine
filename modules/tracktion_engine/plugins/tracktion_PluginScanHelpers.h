/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2024
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

#pragma once

namespace tracktion
{
inline namespace engine
{

struct PluginScanHelpers
{
    static constexpr const char* commandLineUID = "PluginScan";

    struct CustomScanner;

    enum class ScanStatus
    {
        success,
        timeout,
        cancel,
        crash,
        aborted
    };

    static juce::MemoryBlock createScanMessage(const juce::XmlElement& xml)
    {
        juce::MemoryOutputStream mo;
        xml.writeTo(mo, juce::XmlElement::TextFormat().withoutHeader().singleLine());
        return mo.getMemoryBlock();
    }

    //==============================================================================
    struct PluginScanMasterProcess : private juce::ChildProcessCoordinator
    {
        PluginScanMasterProcess(Engine& e) : eng(e) {}

        bool ensureChildProcessLaunched()
        {
            if (launched)
                return true;

            crashed = false;
            launched = launchWorkerProcess(juce::File::getSpecialLocation(juce::File::currentExecutableFile),
                                           commandLineUID, 0, 0);

            if (launched)
            {
                TRACKTION_LOG("----- Launched Plugin Scan Process");
            }
            else
            {
                TRACKTION_LOG_ERROR("Failed to launch child process");
                showVirusCheckerWarning();
            }

            return launched;
        }

        bool sendScanRequest(juce::AudioPluginFormat& format,
                             const juce::String& fileOrIdentifier,
                             int requestID)
        {
            juce::XmlElement m("SCAN");
            m.setAttribute("id", requestID);
            m.setAttribute("type", format.getName());
            m.setAttribute("file", fileOrIdentifier);

            return sendMessageToWorker(createScanMessage(m));
        }

        void terminate()
        {
            if (launched)
            {
                killWorkerProcess();
                launched = false;
            }
        }

        ScanStatus waitForReply(int requestID, const juce::String& fileOrIdentifier, juce::OwnedArray<juce::PluginDescription>& result, CustomScanner& scanner);

        void handleMessage(const juce::XmlElement& xml)
        {
            if (xml.hasTagName("FOUND"))
            {
                const juce::ScopedLock sl(replyLock);
                replies.add(new juce::XmlElement(xml));
            }
        }

        void handleConnectionLost() override
        {
            crashed = true;
        }

        volatile bool launched = false, crashed = false, isBusy = false;

       private:
        Engine& eng;
        juce::OwnedArray<juce::XmlElement> replies;
        juce::CriticalSection replyLock;
        bool hasShownVirusCheckerWarning = false;

        void showVirusCheckerWarning()
        {
            if (!hasShownVirusCheckerWarning)
            {
                hasShownVirusCheckerWarning = true;

                eng.getUIBehaviour().showWarningAlert("Plugin Scanning...",
                                                      TRANS("There are some problems in launching a child-process to scan for plugins.") + "\n\n" + TRANS("If you have a virus-checker or firewall running, you may need to temporarily disable it for the scan to work correctly."));
            }
        }

        std::unique_ptr<juce::XmlElement> findReply(int requestID)
        {
            for (int i = replies.size(); --i >= 0;)
                if (replies.getUnchecked(i)->getIntAttribute("id") == requestID)
                    return std::unique_ptr<juce::XmlElement>(replies.removeAndReturn(i));

            return {};
        }

        void handleMessageFromWorker(const juce::MemoryBlock& mb) override
        {
            if (auto xml = juce::parseXML(mb.toString()))
                handleMessage(*xml);
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginScanMasterProcess)
    };

    //==============================================================================
    struct PluginScanChildProcess : public juce::ChildProcessWorker,
                                    private juce::AsyncUpdater
    {
        PluginScanChildProcess()
        {
            juce::addDefaultFormatsToManager(pluginFormatManager);
        }

        void handleConnectionMade() override {}
        void handleConnectionLost() override { std::exit(0); }

        void handleScanMessage(int requestID, const juce::String& formatName, const juce::String& fileOrIdentifier)
        {
            juce::XmlElement result("FOUND");
            result.setAttribute("id", requestID);

            for (int i = 0; i < pluginFormatManager.getNumFormats(); ++i)
            {
                auto format = pluginFormatManager.getFormat(i);

                if (format->getName() == formatName)
                {
                    juce::OwnedArray<juce::PluginDescription> found;
                    format->findAllTypesForFile(found, fileOrIdentifier);

                    for (auto pd : found)
                        result.addChildElement(pd->createXml().release());

                    break;
                }
            }

            sendMessageToCoordinator(createScanMessage(result));
        }

        void handleMessage(const juce::XmlElement& xml)
        {
            if (xml.hasTagName("SCAN"))
                handleScanMessage(xml.getIntAttribute("id"),
                                  xml.getStringAttribute("type"),
                                  xml.getStringAttribute("file"));
        }

       private:
        juce::AudioPluginFormatManager pluginFormatManager;
        juce::OwnedArray<juce::XmlElement, juce::CriticalSection> pendingMessages;

        void handleMessageFromCoordinator(const juce::MemoryBlock& mb) override
        {
            if (auto xml = juce::parseXML(mb.toString()))
            {
                pendingMessages.add(xml.release());
                triggerAsyncUpdate();
            }
        }

        void handleMessageSafely(const juce::XmlElement& m)
        {
#if JUCE_WINDOWS
            __try
            {
#endif
                handleMessage(m);
#if JUCE_WINDOWS
            }
            __except (1)
            {
                juce::Process::terminate();
            }
#endif
        }

        void handleAsyncUpdate() override
        {
            while (pendingMessages.size() > 0)
                if (auto xml = pendingMessages.removeAndReturn(0))
                    handleMessageSafely(*xml);
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginScanChildProcess)
    };

    //==============================================================================
    struct CustomScanner : public juce::KnownPluginList::CustomScanner
    {
        CustomScanner(Engine& e) : eng(e) {}

        bool findPluginTypesFor(juce::AudioPluginFormat& format,
                                juce::OwnedArray<juce::PluginDescription>& result,
                                const juce::String& fileOrIdentifier) override;

        void scanFinished() override;
        void cancelScan();
        bool shouldAbortScan() const;

        Engine& eng;
        juce::OwnedArray<PluginScanMasterProcess> masterPool;
        juce::CriticalSection poolLock;
        std::atomic<bool> abortScan{false};

       private:
        static bool shouldUseSeparateProcessToScan(juce::AudioPluginFormat& format, const juce::String& fileOrIdentifier);
        static bool requiresUnblockedMessageThread(juce::AudioPluginFormat& format, const juce::String& fileOrIdentifier);
    };
};

//==============================================================================
inline PluginScanHelpers::ScanStatus PluginScanHelpers::PluginScanMasterProcess::waitForReply(int requestID, const juce::String& fileOrIdentifier, juce::OwnedArray<juce::PluginDescription>& result, CustomScanner& scanner)
{
    auto start = juce::Time::getCurrentTime();
    const auto timeout = juce::RelativeTime::seconds(30.0);

    for (;;)
    {
        {
            const juce::ScopedLock sl(replyLock);
            auto reply = findReply(requestID);

            if (reply != nullptr && reply->hasTagName("FOUND"))
            {
                if (reply->getNumChildElements() == 0)
                    TRACKTION_LOG("No plugins found in: " + fileOrIdentifier);

                for (auto e : reply->getChildIterator())
                {
                    juce::PluginDescription desc;

                    if (desc.loadFromXml(*e))
                    {
                        auto newDesc = new juce::PluginDescription(desc);
                        newDesc->lastInfoUpdateTime = juce::Time::getCurrentTime();
                        result.add(newDesc);

                        TRACKTION_LOG("Added " + desc.pluginFormatName + ": " + desc.name + "  [" + (juce::Time::getCurrentTime() - start).getDescription() + "]");
                    }
                }

                return ScanStatus::success;
            }
        }

        if (crashed)
        {
            TRACKTION_LOG_ERROR("Plugin crashed:  " + fileOrIdentifier);
            return ScanStatus::crash;
        }

        if (scanner.shouldAbortScan() || !launched)
        {
            TRACKTION_LOG("Plugin scan cancelled");
            return ScanStatus::aborted;
        }

        if (juce::Time::getCurrentTime() - start > timeout)
        {
            TRACKTION_LOG_ERROR("Plugin scan timed out: " + fileOrIdentifier);

            struct HangDialogTask : public juce::CallbackMessage
            {
                HangDialogTask(Engine& e, const juce::String& name, std::shared_ptr<std::atomic<int>> r)
                    : eng(e), pluginName(name), result(std::move(r)) {}

                void messageCallback() override
                {
                    auto& ui = eng.getUIBehaviour();
                    bool choice = ui.showOkCancelAlertBox("Plugin Scan Hanging",
                                                          "The plugin \"" + pluginName +
                                                              "\" is taking a long time to scan.\n\n"
                                                              "Would you like to keep waiting?",
                                                          "Wait", "Cancel Scan");
                    *result = choice ? 1 : 0;
                }

                Engine& eng;
                juce::String pluginName;
                std::shared_ptr<std::atomic<int>> result;
            };

            auto dialogResult = std::make_shared<std::atomic<int>>(-1);
            (new HangDialogTask(eng, fileOrIdentifier, dialogResult))->post();

            auto dialogStart = juce::Time::getCurrentTime();
            while (*dialogResult == -1 && juce::Time::getCurrentTime() - dialogStart < juce::RelativeTime::seconds(60.0))
            {
                juce::Thread::sleep(100);
                if (scanner.shouldAbortScan())
                    break;
            }

            if (*dialogResult == 0)
            {
                TRACKTION_LOG("User cancelled hanging scan: " + fileOrIdentifier);
                terminate();
                return ScanStatus::cancel;
            }

            if (*dialogResult == -1)
            {
                TRACKTION_LOG_ERROR("Plugin scan dialog timed out: " + fileOrIdentifier);
                terminate();
                return ScanStatus::timeout;
            }

            start = juce::Time::getCurrentTime();
            continue;
        }

        juce::Thread::sleep(10);
    }
}

inline bool PluginScanHelpers::CustomScanner::findPluginTypesFor(juce::AudioPluginFormat& format, juce::OwnedArray<juce::PluginDescription>& result, const juce::String& fileOrIdentifier)
{
    CRASH_TRACER

    if (eng.getPluginManager().usesSeparateProcessForScanning() && shouldUseSeparateProcessToScan(format, fileOrIdentifier))
    {
        TRACKTION_LOG("Scanning out-of-process: " + fileOrIdentifier);
        PluginScanMasterProcess* master = nullptr;
        {
            const juce::ScopedLock sl(poolLock);
            for (auto m : masterPool)
            {
                if (!m->isBusy && m->ensureChildProcessLaunched())
                {
                    master = m;
                    break;
                }
            }

            if (master == nullptr)
            {
                master = new PluginScanMasterProcess(eng);
                masterPool.add(master);
            }
            master->isBusy = true;
        }

        juce::ScopeGuard busyGuard([this, master]
                                   {
            const juce::ScopedLock sl(poolLock);
            master->isBusy = false; });

        auto status = ScanStatus::success;
        auto requestID = juce::Random().nextInt();

        if (master->ensureChildProcessLaunched())
        {
            if (!shouldAbortScan() && master->sendScanRequest(format, fileOrIdentifier, requestID) && !shouldAbortScan())
                status = master->waitForReply(requestID, fileOrIdentifier, result, *this);
            else
                status = ScanStatus::aborted;
        }

        if (status != ScanStatus::success && status != ScanStatus::aborted && !shouldAbortScan())
        {
            master->terminate();

            if (status == ScanStatus::crash)
                TRACKTION_LOG_ERROR("Plugin scan crashed, worker process terminated: " + fileOrIdentifier);
        }

        return status == ScanStatus::success;
    }

    format.findAllTypesForFile(result, fileOrIdentifier);
    return true;
}

inline void PluginScanHelpers::CustomScanner::scanFinished()
{
    // Don't clear masterPool here as it might be in use by background threads
    // The pool will stay alive until the scanner is destroyed
    TRACKTION_LOG("----- Ended Plugin Scan");
    abortScan = false;

    if (auto callback = eng.getPluginManager().scanCompletedCallback)
        callback();
}

inline void PluginScanHelpers::CustomScanner::cancelScan()
{
    abortScan = true;
}

inline bool PluginScanHelpers::CustomScanner::shouldAbortScan() const
{
    return abortScan || shouldExit();
}

inline bool PluginScanHelpers::CustomScanner::shouldUseSeparateProcessToScan(juce::AudioPluginFormat& format, const juce::String& fileOrIdentifier)
{
    auto name = format.getName();

    if (name.containsIgnoreCase("AudioUnit"))
        return !requiresUnblockedMessageThread(format, fileOrIdentifier);

    // Everything else (VST, VST3, CLAP, LADSPA, etc.) should use the separate process if possible
    return true;
}

inline bool PluginScanHelpers::CustomScanner::requiresUnblockedMessageThread(juce::AudioPluginFormat& format, const juce::String& fileOrIdentifier)
{
    juce::PluginDescription desc;
    desc.fileOrIdentifier = fileOrIdentifier;
    desc.uniqueId = desc.deprecatedUid = 0;

    return format.requiresUnblockedMessageThreadDuringCreation(desc);
}

} // namespace engine
} // namespace tracktion
