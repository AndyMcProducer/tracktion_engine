/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2024
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/
#pragma once

struct MelodyneInstance
{
    ExternalPlugin::Ptr plugin;
    const ARAFactory* factory = nullptr;
    const ARAPlugInExtensionInstance* extensionInstance = nullptr;
};

#if TRACKTION_ENABLE_ARA
#include "tracktion_ARALogging.h"
#endif

class MelodyneFileReader;
static std::unique_ptr<juce::AudioPluginInstance> createMelodynePlugin(Engine& engine);

//==============================================================================
struct MelodyneInstanceFactory
{
   public:
    static MelodyneInstanceFactory& getInstance(Engine& engine)
    {
        auto& p = getInstancePointer();

        if (p == nullptr)
            p = new MelodyneInstanceFactory(engine);

        return *p;
    }

    static void shutdown()
    {
        CRASH_TRACER
        auto& p = getInstancePointer();
        delete p;
        p = nullptr; // MUST null the pointer — otherwise getInstance() returns a dangling ptr
    }

    ExternalPlugin::Ptr createPlugin(Edit& ed)
    {
        if (plugin != nullptr)
        {
            auto newState = ExternalPlugin::create(ed.engine, plugin->getPluginDescription());
            ExternalPlugin::Ptr p = new ExternalPlugin(PluginCreationInfo(ed, newState, true));

            if (p->getAudioPluginInstance() != nullptr)
                return p;
        }

        return {};
    }

    MelodyneInstance* createInstance(ExternalPlugin& p, ARADocumentControllerRef dcRef)
    {
        TRACKTION_ASSERT_MESSAGE_THREAD
        jassert(plugin != nullptr);

        std::unique_ptr<MelodyneInstance> w(new MelodyneInstance());
        w->plugin = &p;
        w->factory = factory;
        w->extensionInstance = nullptr;

        if (!setExtensionInstance(*w, dcRef))
            w = nullptr;

        return w.release();
    }

    const ARAFactory* factory = nullptr;

   private:
    // Because ARA has some state which is global to the DLL, this dummy instance
    // of the plugin is kept hanging around until shutdown, forcing the DLL to
    // remain in memory until we're sure all other instances have gone away. Not
    // pretty, but not sure how else we could handle this.
    std::unique_ptr<juce::AudioPluginInstance> plugin;

    MelodyneInstanceFactory(Engine&);
    ~MelodyneInstanceFactory();

    static MelodyneInstanceFactory*& getInstancePointer()
    {
        static MelodyneInstanceFactory* instance;
        return instance;
    }

    void getFactoryForPlugin()
    {
        auto type = plugin->getPluginDescription().pluginFormatName;

        if (type == "VST3")
            factory = getFactoryVST3();

        if (factory != nullptr && factory->lowestSupportedApiGeneration > kARAAPIGeneration_2_0_Final)
            factory = nullptr;
    }

    bool setExtensionInstance(MelodyneInstance& w, ARADocumentControllerRef dcRef)
    {
        TRACKTION_ASSERT_MESSAGE_THREAD
        CRASH_TRACER

        if (dcRef == nullptr)
            return false;

        auto type = plugin->getPluginDescription().pluginFormatName;

        if (type == "VST3")
            return setExtensionInstanceVST3(w, dcRef);

        return false;
    }

    template <typename entrypoint_t>
    Steinberg::IPtr<entrypoint_t> getVST3EntryPoint(juce::AudioPluginInstance& p)
    {
        entrypoint_t* ep = nullptr;

        auto getIComponent = [](juce::AudioPluginInstance& p) -> Steinberg::Vst::IComponent*
        {
            struct VST3Visitor : public juce::ExtensionsVisitor
            {
                void visitVST3Client(const VST3Client& client) override
                {
                    icomponent = static_cast<Steinberg::Vst::IComponent*>(client.getIComponentPtr());
                }

                Steinberg::Vst::IComponent* icomponent = nullptr;
            };

            VST3Visitor vst3Visitor;
            p.getExtensions(vst3Visitor);

            return vst3Visitor.icomponent;
        };

        if (auto component = getIComponent(p))
            component->queryInterface(entrypoint_t::iid, (void**)&ep);

        return {ep};
    }

    ARAFactory* getFactoryVST3()
    {
        if (auto ep = getVST3EntryPoint<IPlugInEntryPoint>(*plugin))
        {
            ARAFactory* f = const_cast<ARAFactory*>(ep->getFactory());
            return f;
        }

        return {};
    }

    bool setExtensionInstanceVST3(MelodyneInstance& w, ARADocumentControllerRef dcRef)
    {
        if (auto p = w.plugin->getAudioPluginInstance())
        {
            auto vst3EntryPoint2 = getVST3EntryPoint<IPlugInEntryPoint2>(*p);

            if (vst3EntryPoint2 != nullptr)
            {
                ARAPlugInInstanceRoleFlags roles = kARAEditorViewRole;

                if (factory != nullptr)
                {
                    // If the plugin does not support time-stretching, we might try to only 
                    // request editor/view roles. However, some plugins (like SpectraLayers) 
                    // crash if we omit the Playback Renderer role here. So we request all.
                    if ((factory->supportedPlaybackTransformationFlags & kARAPlaybackTransformationTimestretch) == 0)
                        engine_ara_log("MelodyneInstanceFactory: Plugin does NOT support time stretch (Editor-only ARA?)");
                    else
                        engine_ara_log("MelodyneInstanceFactory: Plugin supports time stretch engine roles");

                    roles |= kARAPlaybackRendererRole | kARAEditorRendererRole;
                }
                else
                {
                    roles |= kARAPlaybackRendererRole | kARAEditorRendererRole;
                }

                engine_ara_log(("MelodyneInstanceFactory: Binding to document controller with roles: " + juce::String((int)roles)).toRawUTF8());
                w.extensionInstance = vst3EntryPoint2->bindToDocumentControllerWithRoles(dcRef, roles, roles);
            }
        }

        return w.extensionInstance != nullptr;
    }

    static bool canBeUsedAsTimeStretchEngine(const ARAFactory& factory) noexcept
    {
        return (factory.supportedPlaybackTransformationFlags & kARAPlaybackTransformationTimestretch) != 0 && (factory.supportedPlaybackTransformationFlags & kARAPlaybackTransformationTimestretchReflectingTempo) != 0;
    }

    static void ARA_CALL assertCallback(ARAAssertCategory category, const void* problematicArgument, const char* diagnosis)
    {
        juce::String categoryName;

        switch ((int)category)
        {
            case kARAAssertUnspecified:
                categoryName = "Unspecified";
                break;
            case kARAAssertInvalidArgument:
                categoryName = "Invalid Argument";
                break;
            case kARAAssertInvalidState:
                categoryName = "Invalid State";
                break;
            case kARAAssertInvalidThread:
                categoryName = "Invalid Thread";
                break;
            default:
                categoryName = "(Unknown)";
                break;
        };

        TRACKTION_LOG_ERROR("ARA assertion -> \"" + categoryName + "\": " + juce::String::fromUTF8(diagnosis) + ": " + juce::String(juce::pointer_sized_int(problematicArgument)));
        jassertfalse;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MelodyneInstanceFactory)
};

//==============================================================================
