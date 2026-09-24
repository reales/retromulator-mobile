// JUCE's stock standalone app, plus saving on suspend: iOS kills a backgrounded app
// without calling systemRequestedQuit(), so the stock app never wrote its state.

#include <JuceHeader.h>

#if JucePlugin_Build_Standalone

#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

namespace retromulator
{
    class StandaloneApp final : public juce::JUCEApplication
    {
    public:
        StandaloneApp()
        {
            juce::PropertiesFile::Options options;
            options.applicationName     = juce::CharPointer_UTF8(JucePlugin_Name);
            options.filenameSuffix      = ".settings";
            options.osxLibrarySubFolder = "Application Support";
            options.folderName          = "";
            m_appProperties.setStorageParameters(options);
        }

        const juce::String getApplicationName() override           { return juce::CharPointer_UTF8(JucePlugin_Name); }
        const juce::String getApplicationVersion() override        { return JucePlugin_VersionString; }
        bool moreThanOneInstanceAllowed() override                 { return true; }
        void anotherInstanceStarted(const juce::String&) override  {}

        void initialise(const juce::String&) override
        {
            if(juce::Desktop::getInstance().getDisplays().displays.isEmpty())
            {
                m_pluginHolder = createPluginHolder();
                return;
            }

            m_mainWindow = std::make_unique<juce::StandaloneFilterWindow>(
                getApplicationName(),
                juce::LookAndFeel::getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId),
                createPluginHolder());
            m_mainWindow->setVisible(true);
        }

        void shutdown() override
        {
            m_pluginHolder = nullptr;
            m_mainWindow = nullptr;
            m_appProperties.saveIfNeeded();
        }

        // Settings writes are delayed by 5 s, so flush before iOS can kill the app.
        void suspended() override
        {
            saveState();
            m_appProperties.saveIfNeeded();
        }

        void systemRequestedQuit() override
        {
            saveState();

            if(juce::ModalComponentManager::getInstance()->cancelAllModalComponents())
            {
                juce::Timer::callAfterDelay(100, []
                {
                    if(auto* app = juce::JUCEApplicationBase::getInstance())
                        app->systemRequestedQuit();
                });
            }
            else
            {
                quit();
            }
        }

    private:
        std::unique_ptr<juce::StandalonePluginHolder> createPluginHolder()
        {
            juce::AudioDeviceManager::AudioDeviceSetup preferredSetup;
            preferredSetup.bufferSize = 1024;

            return std::make_unique<juce::StandalonePluginHolder>(m_appProperties.getUserSettings(), false,
                                                                  juce::String{}, &preferredSetup,
                                                                  juce::Array<juce::StandalonePluginHolder::PluginInOuts>{},
                                                                  true);
        }

        void saveState()
        {
            if(m_pluginHolder != nullptr)
                m_pluginHolder->savePluginState();
            if(m_mainWindow != nullptr && m_mainWindow->pluginHolder != nullptr)
                m_mainWindow->pluginHolder->savePluginState();
        }

        juce::ApplicationProperties m_appProperties;
        std::unique_ptr<juce::StandaloneFilterWindow> m_mainWindow;
        std::unique_ptr<juce::StandalonePluginHolder> m_pluginHolder;
    };
}

juce::JUCEApplicationBase* juce_CreateApplication()
{
    return new retromulator::StandaloneApp();
}

#if JUCE_IOS
// nullptr keeps JUCE's own app delegate
void* juce_GetIOSCustomDelegateClass()
{
    return nullptr;
}
#endif

#endif
