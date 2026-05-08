#pragma once

#include "SynthType.h"
#include "jucePluginLib/processor.h"
#include "synthLib/midiTypes.h"
#ifndef CUSTOM
#  include "synthLib/deviceTypes.h"
#endif

#include <vector>
#include <string>
#include <cstdint>
#include <atomic>
#include <thread>
#include <functional>
#include <memory>

namespace akaiLib { class Device; }
namespace openWurliLib { class Device; }
namespace opl3Lib { class Device; }
namespace sidLib { class Device; }

namespace retromulator
{
    class HeadlessProcessor final : public pluginLib::Processor,
                                    private juce::MidiKeyboardStateListener
    {
    public:
        HeadlessProcessor();
        ~HeadlessProcessor() override;

        // ── Synth hot-swap ──────────────────────────────────────────────────
        void setSynthType(SynthType type, const std::string& romPath = {});
        // Async version: boots the device on a background thread, then calls
        // onComplete on the message thread. GUI stays responsive during boot.
        void setSynthTypeAsync(SynthType type, const std::string& romPath,
                               std::function<void()> onComplete);
        SynthType getSynthType() const { return m_synthType; }

        // ── Preset loading ──────────────────────────────────────────────────
        // Load raw SysEx data. Splits into individual messages, sends message[programIndex].
        // Copies the file to the data folder if a path is provided.
        bool loadPreset(const std::vector<uint8_t>& sysexData,
                        const std::string& sourcePath = {},
                        const std::string& patchName  = {},
                        int programIndex = 0);

        // Load a sound file from in-memory data (iOS: security-scoped URLs).
        // fileName is used only for extension/name detection.
        bool loadSoundFromMemory(juce::MemoryBlock&& data, const std::string& fileName);

        // Load SysEx from a file path. Auto-copies to data folder.
        bool loadPresetFromFile(const std::string& filePath,
                                const std::string& patchName = {},
                                int programIndex = 0);

        // Select a program within the currently loaded bank (0-based).
        // Returns false if index is out of range.
        bool selectProgram(int index);

        // ── Preset export ───────────────────────────────────────────────────
        // Export the current program as a single-patch .syx file.
        // The patch name (from m_programNames or m_patchName) is embedded using
        // the Aura editor name extension (10 ASCII bytes before F7).
        // Returns true on success.
        bool exportCurrentPresetToFile(const std::string& destPath) const;

        // Export the entire loaded bank as a .syx file.
        // Names for all programs are embedded in each message.
        bool exportCurrentBankToFile(const std::string& destPath) const;

        // Export the loaded Virus bank converted to a different model version.
        // targetVersion: 'A', 'B', or 'C'.  Remaps C-only parameters (analog
        // filters, saturation) to safe equivalents for the older model.
        // Returns the number of patches converted, or -1 on error.
        int exportConvertedVirusBank(const std::string& destPath, char targetVersion) const;

        // ── Sound file loading (Akai S1000) ──────────────────────────────────
        bool loadSoundFile(const std::string& filePath);
        bool loadAkaiIso(const std::string& filePath);
        bool selectAkaiIsoPreset(int index);
        bool selectSoundPreset(int index);

        // Device accessors for the new engine types
        akaiLib::Device* getAkaiDevice() const;
        openWurliLib::Device* getOpenWurliDevice() const;
        opl3Lib::Device* getOpl3Device() const;
        sidLib::Device* getSidDevice() const;

        // ── Program bank accessors ──────────────────────────────────────────
        // m_bankStride: number of raw sysex messages per logical program (1 for most
        // synths; >1 for JE-8086 where each performance = several sub-messages).
        int getProgramCount()   const
        {
            if(m_bankMessages.empty() && !m_programNames.empty())
                return static_cast<int>(m_programNames.size());
            return static_cast<int>(m_bankMessages.size()) / m_bankStride;
        }
        int getCurrentProgram() override { return m_currentProgram; }

        // ── Data folder helpers ─────────────────────────────────────────────
        static std::string getDataFolder();
        static std::string getSynthDataFolder(SynthType type);
        static std::string getLastLoadFolder(SynthType type);
        static void        setLastLoadFolder(SynthType type, const std::string& folder);

        // App-wide setting: when true, gearmulator (JIT-based) cores are hidden
        // from the synth selector. Defaults to true on iOS (App Store builds
        // ship without JIT and the interpreter cores need M-series silicon and
        // 512–1024 ms buffers to keep up). Defaults to false on desktop.
        static bool isJitlessCoresEnabled();
        static void setJitlessCoresEnabled(bool enabled);

       #if defined(JUCE_IOS) && JUCE_IOS
        // Returns the App Group shared container path (app + AUv3 extension share this).
        // Implemented in HeadlessProcessor_ios.mm.
        static std::string getIOSSharedDataFolder();
        // Sets MemoryBuffer::g_tempPath so the DSP56300 MMU memory system can
        // fall back to temp-file-backed mmap (shm_open is blocked on iOS).
        // Implemented in HeadlessProcessor_ios.mm.
        static void initIOSTempPath();
        // Request iOS audio session to use a specific buffer size.
        // Implemented in HeadlessProcessor_ios.mm.
        static void setIOSPreferredBufferSize(int samples);
        // Symlink Documents/Retromulator → App Group shared folder so
        // iTunes/Finder File Sharing exposes the shared data.
        // Implemented in HeadlessProcessor_ios.mm.
        static void linkDocumentsToSharedFolder();
       #endif

        // ── pluginLib::Processor pure virtuals ──────────────────────────────
        synthLib::Device* createDevice() override;
        pluginLib::Controller* createController() override;

        // ── State accessors for editor ──────────────────────────────────────
        const std::string& getRomPath()       const { return m_romPath; }
        const std::string& getSysexFilePath() const { return m_sysexFilePath; }
        const std::string& getPatchName()     const { return m_patchName; }
#ifndef CUSTOM
        // Raw device error — available in GPL builds only.
        // Use isFirmwareMissing() / hasDeviceError() from proprietary code.
        synthLib::DeviceError getDeviceError() const { return m_deviceError; }
#endif
        bool isFirmwareMissing() const;
        bool hasDeviceError() const;
        bool isBooting() const { return m_isBooting.load(); }

        // Returns true if a valid ROM can be found for the given synth type.
        // Wraps each synth-specific ROM loader — keeps GPL headers out of the editor.
        static bool isRomValid(SynthType type);

        // Registers an additional directory for all ROM loaders to search.
        static void addRomSearchPath(const std::string& path);

        // Extract bundled OPL3 .sbi presets from Archive.zip to writable data folder.
        static void installBundledOPL3();

        const std::vector<std::string>& getProgramNames() const { return m_programNames; }

        const std::string& getAkaiBrowseFolder() const { return m_akaiBrowseFolder; }
        void setAkaiBrowseFolder(const std::string& f)  { m_akaiBrowseFolder = f; }

        bool isAkaiIsoMode() const { return m_akaiIsoMode; }
        const std::string& getAkaiIsoPath() const { return m_akaiIsoPath; }

        int  getAkaiSliceCount() const     { return m_akaiSliceCount; }
        void setAkaiSliceCount(int count)  { m_akaiSliceCount = count; }
        bool applyAkaiAutoSlice(int count);
        int  getAkaiTuneCents() const      { return m_akaiTuneCents; }
        void setAkaiTuneCents(int cents)   { m_akaiTuneCents = cents; }

        // ── juce::AudioProcessor overrides ──────────────────────────────────
        bool hasEditor() const override { return true; }
        juce::AudioProcessorEditor* createEditor() override;

        const juce::String getName() const override { return "Retromulator"; }

        // State persistence (DAW save/load)
        void getStateInformation(juce::MemoryBlock& destData) override;
        void setStateInformation(const void* data, int sizeInBytes) override;

        // Called each audio block — used to resend bank message after DSP boot delay
        void processBpm(float _bpm) override;

        // Override to capture incoming pitch bend / CC1 for the on-screen wheels.
        void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

        juce::MidiKeyboardState& getKeyboardState() { return m_keyboardState; }

        // On-screen wheel mirroring of incoming MIDI. Editor polls these at animation rate.
        // pitchBend: 0..16383 (8192 = center). modWheel: 0..127. seq increments on every change.
        int  getIncomingPitchBend() const { return m_incomingPitchBend.load(std::memory_order_relaxed); }
        int  getIncomingModWheel()  const { return m_incomingModWheel.load(std::memory_order_relaxed); }
        uint32_t getPitchBendSeq()  const { return m_pitchBendSeq.load(std::memory_order_relaxed); }
        uint32_t getModWheelSeq()   const { return m_modWheelSeq.load(std::memory_order_relaxed); }

        int getSavedEditorWidth()  const { return m_savedEditorWidth; }
        int getSavedEditorHeight() const { return m_savedEditorHeight; }
        void saveEditorSize(int w, int h) { m_savedEditorWidth = w; m_savedEditorHeight = h; }

        // Set by setStateInformation so the editor can apply the restored size
        // if it was created before the DAW called setStateInformation.
        bool consumeEditorSizeDirty()
        {
            const bool v = m_editorSizeDirty;
            m_editorSizeDirty = false;
            return v;
        }

    private:
        SynthType   m_synthType = SynthType::None;
        std::string m_romPath;

        // GUI size saved/restored across DAW sessions and settings.xml
        int  m_savedEditorWidth  = 0;
        int  m_savedEditorHeight = 0;
        bool m_editorSizeDirty   = false; // true after setStateInformation restores a size

        void loadEditorSizeFromSettings();
        void saveEditorSizeToSettings(int w, int h);

        std::string m_sysexFilePath;   // path to the last loaded sysex file (in data folder)
        std::string m_patchName;       // human-readable patch name
        std::string m_akaiBrowseFolder; // Akai browse-folder path (empty = not in browse mode)
        int m_akaiSliceCount = 0;       // 0 = root play, 4/8/16/32 = auto-sliced
        int m_akaiTuneCents  = 0;       // CC20 global tuning in cents
        bool m_akaiIsoMode   = false;   // true when an Akai ISO is loaded
        std::string m_akaiIsoPath;      // path to loaded ISO/BIN/CUE file
        std::vector<uint8_t> m_sysexData; // raw sysex bytes of the loaded file

        // Split messages from the loaded bank; index into it for program selection.
        std::vector<synthLib::SysexBuffer> m_bankMessages;
        std::vector<std::string> m_programNames; // extracted name for each program slot
        int m_currentProgram = 0;
        int m_bankStride     = 1; // messages per logical program (>1 for JE-8086)

        // Send stride messages starting at index*stride to the device.
        void sendBankMessage(int index);

        // Copy a sysex file to the synth data folder. Returns the destination path.
        std::string copySysexToDataFolder(const std::string& sourcePath);

    public:
        // Detect whether a set of sysex messages are Virus ABC or TI presets.
        // Returns VirusABC, VirusTI, or None if not Virus sysex.
        static SynthType detectVirusType(const std::vector<synthLib::SysexBuffer>& messages);

        // Copy a sysex file to the *other* Virus variant's data folder.
        // Returns the destination path, or empty on failure.
        static std::string copySysexToFolder(const std::string& sourcePath, SynthType targetType);

        // Set when a bank message needs to be re-sent once the DSP has warmed up.
        // Consumed by processBpm (called each audio block) after setSynthType.
        // m_deviceBooted: once true, the boot-delay resend is never re-armed.
        juce::MidiKeyboardState m_keyboardState;

        // MidiKeyboardStateListener — routes on-screen key presses to the synth engine.
        void handleNoteOn (juce::MidiKeyboardState*, int midiChannel, int midiNoteNumber, float velocity) override;
        void handleNoteOff(juce::MidiKeyboardState*, int midiChannel, int midiNoteNumber, float velocity) override;

        std::atomic<int>      m_incomingPitchBend{8192};
        std::atomic<int>      m_incomingModWheel{0};
        std::atomic<uint32_t> m_pitchBendSeq{0};
        std::atomic<uint32_t> m_modWheelSeq{0};

        std::atomic<bool> m_isBooting{false};
        std::unique_ptr<std::thread> m_bootThread;
        void joinBootThread();
        std::atomic<bool> m_pendingResend{false};
        int m_resendBlocksRemaining = 0;
        bool m_deviceBooted = false;
    };
}
