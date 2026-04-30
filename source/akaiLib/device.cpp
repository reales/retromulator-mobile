#include "device.h"
#include "akaiIsoReader.h"

#include "../../Modules/SFZero/SFZero.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cstring>

namespace akaiLib
{
    static constexpr int kNumVoices = 64;

    Device::Device(const synthLib::DeviceCreateParams& _params)
        : synthLib::Device(_params)
        , m_synth(std::make_unique<sfzero::Synth>())
        , m_formatManager(std::make_unique<juce::AudioFormatManager>())
    {
        m_formatManager->registerBasicFormats();

        for(int i = 0; i < kNumVoices; ++i)
            m_synth->addVoice(new sfzero::Voice());
    }

    Device::~Device() = default;

    float Device::getSamplerate() const
    {
        return m_samplerate;
    }

    bool Device::setSamplerate(float _samplerate)
    {
        m_samplerate = _samplerate;
        if(m_synth)
            m_synth->setCurrentPlaybackSampleRate(static_cast<double>(_samplerate));
        return true;
    }

    void Device::getSupportedSamplerates(std::vector<float>& _dst) const
    {
        _dst.push_back(44100.0f);
        _dst.push_back(48000.0f);
        _dst.push_back(88200.0f);
        _dst.push_back(96000.0f);
    }

#if SYNTHLIB_DEMO_MODE == 0
    bool Device::getState(std::vector<uint8_t>& /*_state*/, synthLib::StateType /*_type*/)
    {
        return false;
    }

    bool Device::setState(const std::vector<uint8_t>& /*_state*/, synthLib::StateType /*_type*/)
    {
        return false;
    }
#endif

    // ── Sound file loading ────────────────────────────────────────────────────

    bool Device::loadSoundFile(const std::string& filePath)
    {
        if(filePath.empty())
            return false;

        const juce::File file(filePath);
        if(!file.existsAsFile())
            return false;

        // Clear any loaded ISO when switching to a regular sound file
        m_isoReader.reset();

        const auto ext = file.getFileExtension().toLowerCase();

        sfzero::Sound* sound = nullptr;

        if(ext == ".sf2")
            sound = new sfzero::SF2Sound(file);
        else if(ext == ".zbp" || ext == ".zbb")
            sound = new sfzero::ZBPSound(file);
        else if(ext == ".sfz" || ext == ".wav" || ext == ".aif" || ext == ".aiff" ||
                ext == ".flac" || ext == ".ogg")
        {
            // For WAV/AIFF/FLAC/OGG: create a minimal SFZ in memory that maps
            // the sample across the full key range.
            if(ext != ".sfz")
            {
                auto* s = new sfzero::Sound(file);
                // Build a single-region SFZ text that maps the file
                const juce::String sfzText =
                    "<group>\n<region> sample=" + file.getFileName() + " lokey=0 hikey=127\n";
                s->loadRegionsFromText(sfzText.toRawUTF8(),
                                       static_cast<unsigned int>(sfzText.length()));
                sound = s;
            }
            else
            {
                sound = new sfzero::Sound(file);
            }
        }
        else
            return false;

        // Only call loadRegions() if regions weren't already loaded from
        // inline SFZ text (e.g. for .wav/.aif/.flac/.ogg files).
        if(sound->getRegions().size() == 0)
            sound->loadRegions();
        sound->loadSamples(m_formatManager.get());

        const auto& errors = sound->getErrors();
        if(errors.size() > 0)
        {
            fprintf(stderr, "[Akai S1000] Sound load errors:\n");
            for(const auto& e : errors)
                fprintf(stderr, "  %s\n", e.toRawUTF8());
        }

        // Determine preset count
        const int presets = sound->numSubsounds();

        // Re-apply current global tuning to new regions
        if(m_tuneCents != 0)
        {
            for(auto* region : sound->getRegions())
                region->tune = m_tuneCents;
        }

        {
            std::lock_guard<std::mutex> lock(m_lock);

            m_synth->clearSounds();
            m_synth->addSound(sound);
            m_synth->setCurrentPlaybackSampleRate(static_cast<double>(m_samplerate));

            m_filePath      = filePath;
            m_presetCount   = (presets > 1) ? presets : 0;
            m_selectedPreset = 0;

            if(presets > 1)
                sound->useSubsound(0);
        }

        return true;
    }

    bool Device::loadSoundFromMemory(juce::MemoryBlock&& data, const std::string& fileName)
    {
        if(data.getSize() == 0 || fileName.empty())
            return false;

        m_isoReader.reset();

        const juce::String ext = juce::File(fileName).getFileExtension().toLowerCase();

        sfzero::Sound* sound = nullptr;

        if(ext == ".sf2")
        {
            auto* s = new sfzero::SF2Sound(juce::File(fileName));
            s->setMemoryData(std::move(data));
            sound = s;
        }
        else if(ext == ".zbp" || ext == ".zbb")
        {
            auto* s = new sfzero::ZBPSound(juce::File(fileName));
            s->setMemoryData(std::move(data));
            sound = s;
        }
        else if(ext == ".sfz" || ext == ".wav" || ext == ".aif" || ext == ".aiff" ||
                ext == ".flac" || ext == ".ogg")
        {
            // For WAV/AIFF/FLAC/OGG: wrap in a MemoryInputStream-based SFZ sound.
            // SFZSample already supports setMemoryData for loading from memory.
            auto* s = new sfzero::Sound(juce::File(fileName));
            if(ext != ".sfz")
            {
                const juce::String sfzText =
                    "<group>\n<region> sample=" + juce::File(fileName).getFileName() + " lokey=0 hikey=127\n";
                s->loadRegionsFromText(sfzText.toRawUTF8(),
                                       static_cast<unsigned int>(sfzText.length()));
                // Push memory data into every sample the reader created
                for(juce::HashMap<juce::String, sfzero::Sample*>::Iterator it(s->getSamples()); it.next();)
                    it.getValue()->setMemoryData(juce::MemoryBlock(data));
            }
            sound = s;
        }
        else
            return false;

        if(sound->getRegions().size() == 0)
            sound->loadRegions();
        sound->loadSamples(m_formatManager.get());

        if(m_tuneCents != 0)
        {
            for(auto* region : sound->getRegions())
                region->tune = m_tuneCents;
        }

        const int presets = sound->numSubsounds();
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_synth->clearSounds();
            m_synth->addSound(sound);
            m_synth->setCurrentPlaybackSampleRate(static_cast<double>(m_samplerate));
            m_filePath       = fileName;
            m_presetCount    = (presets > 1) ? presets : 0;
            m_selectedPreset = 0;
            if(presets > 1)
                sound->useSubsound(0);
        }
        return true;
    }

    // ── Preset support ────────────────────────────────────────────────────────

    int Device::getPresetCount() const
    {
        return m_presetCount;
    }

    std::string Device::getPresetName(int index) const
    {
        if(index < 0 || index >= m_presetCount)
            return {};

        if(m_synth->getNumSounds() == 0)
            return {};

        auto* sound = dynamic_cast<sfzero::Sound*>(m_synth->getSound(0).get());
        if(!sound)
            return {};

        return sound->subsoundName(index).toStdString();
    }

    bool Device::selectPreset(int index)
    {
        if(index < 0 || index >= m_presetCount)
            return false;

        if(m_synth->getNumSounds() == 0)
            return false;

        auto* sound = dynamic_cast<sfzero::Sound*>(m_synth->getSound(0).get());
        if(!sound)
            return false;

        std::lock_guard<std::mutex> lock(m_lock);
        sound->useSubsound(index);
        m_selectedPreset = index;
        return true;
    }

    int Device::getSelectedPreset() const
    {
        return m_selectedPreset;
    }

    // ── Akai ISO loading ──────────────────────────────────────────────────────

    bool Device::loadIsoFile(const std::string& filePath)
    {
        if(filePath.empty())
            return false;

        const juce::File file(filePath);
        if(!file.existsAsFile())
            return false;

        auto reader = std::make_unique<AkaiIsoReader>();
        if(!reader->open(file))
            return false;

        if(reader->getProgramRefs().isEmpty())
            return false;

        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_isoReader = std::move(reader);
            m_filePath = filePath;
            m_presetCount = 0;
            m_selectedPreset = -1;
        }

        // Load the first program
        return selectIsoPreset(0);
    }

    int Device::getIsoPresetCount() const
    {
        if(!m_isoReader)
            return 0;
        return m_isoReader->getProgramRefs().size();
    }

    std::string Device::getIsoPresetName(int index) const
    {
        if(!m_isoReader)
            return {};

        const auto& refs = m_isoReader->getProgramRefs();
        if(index < 0 || index >= refs.size())
            return {};

        const auto& ref = refs[index];
        // Format: "A: ProgramName" where A is the partition label
        return (ref.partitionLabel + ": " + ref.programName.trim()).toStdString();
    }

    bool Device::selectIsoPreset(int index)
    {
        if(!m_isoReader)
            return false;

        const auto& refs = m_isoReader->getProgramRefs();
        if(index < 0 || index >= refs.size())
            return false;

        // Extract samples and build SFZ for this program
        std::vector<std::pair<juce::String, juce::MemoryBlock>> wavFiles;
        juce::String sfzText = m_isoReader->loadProgram(index, wavFiles);

        if(sfzText.isEmpty() || wavFiles.empty())
            return false;

        // Create a temporary directory for the WAV files
        const juce::File tempDir = juce::File::getSpecialLocation(
            juce::File::tempDirectory).getChildFile("retromulator_iso_samples");
        tempDir.createDirectory();

        // Write WAV files to temp directory
        for(const auto& [name, data] : wavFiles)
        {
            const juce::File wavFile = tempDir.getChildFile(name);
            wavFile.replaceWithData(data.getData(), data.getSize());
        }

        // Create SFZ sound from the generated text
        auto* sound = new sfzero::Sound(tempDir.getChildFile("_iso_program.sfz"));
        sound->loadRegionsFromText(sfzText.toRawUTF8(),
                                   static_cast<unsigned int>(sfzText.length()));
        sound->loadSamples(m_formatManager.get());

        // Re-apply current global tuning
        if(m_tuneCents != 0)
        {
            for(auto* region : sound->getRegions())
                region->tune = m_tuneCents;
        }

        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_synth->clearSounds();
            m_synth->addSound(sound);
            m_synth->setCurrentPlaybackSampleRate(static_cast<double>(m_samplerate));
            m_presetCount = 0;
            m_selectedPreset = index;
        }

        return true;
    }

    // ── Auto-slice ───────────────────────────────────────────────────────────

    bool Device::isSliceable() const
    {
        if(m_filePath.empty())
            return false;

        // ZBP/ZBB with loaded presets: sliceable via in-memory sample
        if(m_presetCount > 0)
        {
            if(m_synth->getNumSounds() == 0)
                return false;
            auto* sound = dynamic_cast<sfzero::Sound*>(m_synth->getSound(0).get());
            return sound && !sound->getRegions().isEmpty()
                && sound->getRegions()[0] && sound->getRegions()[0]->sample
                && sound->getRegions()[0]->sample->getSampleLength() > 0;
        }

        const juce::File f(m_filePath);
        const auto ext = f.getFileExtension().toLowerCase();
        return (ext == ".wav" || ext == ".aif" || ext == ".aiff" ||
                ext == ".flac" || ext == ".ogg");
    }

    bool Device::autoSlice(int numSlices)
    {
        if(numSlices < 1 || m_filePath.empty())
            return false;

        juce::int64 totalFrames = 0;
        sfzero::Sample* sharedSample = nullptr;

        if(m_presetCount > 0)
        {
            // ZBP/ZBB: sample is already decoded in memory. Get length from first region.
            auto* existingSound = (m_synth->getNumSounds() > 0)
                ? dynamic_cast<sfzero::Sound*>(m_synth->getSound(0).get())
                : nullptr;
            if(!existingSound || existingSound->getRegions().isEmpty())
                return false;
            auto* firstRegion = existingSound->getRegions()[0];
            if(!firstRegion || !firstRegion->sample)
                return false;
            totalFrames = static_cast<juce::int64>(firstRegion->sample->getSampleLength());
            sharedSample = firstRegion->sample;
        }
        else
        {
            const juce::File file(m_filePath);
            if(!file.existsAsFile())
                return false;
            std::unique_ptr<juce::AudioFormatReader> reader(
                m_formatManager->createReaderFor(file));
            if(!reader)
                return false;
            totalFrames = static_cast<juce::int64>(reader->lengthInSamples);
        }

        if(totalFrames <= 0)
            return false;

        const juce::int64 sliceLen = totalFrames / numSlices;
        if(sliceLen <= 0)
            return false;

        sfzero::Sound* sound = nullptr;

        if(sharedSample)
        {
            // ZBB path: build regions directly on a new Sound, reusing the decoded sample.
            // The sample is owned by the old sound which stays alive until clearSounds() below.
            // We transfer the sample into the new sound's map so it owns it going forward,
            // and clear it from the old sound's map to prevent double-free.
            auto* existingSound = dynamic_cast<sfzero::Sound*>(m_synth->getSound(0).get());
            sound = new sfzero::Sound(existingSound->getFile());

            for(int i = 0; i < numSlices; ++i)
            {
                const int key = 60 + i;
                if(key > 127) break;

                const juce::int64 sliceStart = i * sliceLen;
                const juce::int64 sliceEnd   = (i == numSlices - 1)
                                               ? (totalFrames - 1)
                                               : ((i + 1) * sliceLen - 1);

                auto* region = new sfzero::Region();
                region->clear();
                region->sample          = sharedSample;
                region->lokey           = key;
                region->hikey           = key;
                region->pitch_keycenter = key;
                region->pitch_keytrack  = 0;
                region->loop_mode       = sfzero::Region::no_loop;
                region->offset          = static_cast<int>(sliceStart);
                region->end             = static_cast<int>(sliceEnd);
                sound->addRegion(region);
            }

            // Transfer sample ownership to the new sound to avoid double-free when
            // clearSounds() destroys the old sound.
            // Transfer sample ownership to the new sound to avoid double-free when
            // clearSounds() destroys the old sound.
            auto& oldMap = existingSound->getSamples();
            juce::String transferKey;
            for(juce::HashMap<juce::String, sfzero::Sample*>::Iterator it(oldMap); it.next();)
            {
                if(it.getValue() == sharedSample)
                    { transferKey = it.getKey(); break; }
            }
            if(transferKey.isNotEmpty())
            {
                sound->getSamples().set(transferKey, sharedSample);
                oldMap.remove(transferKey);
            }
        }
        else
        {
            const juce::File file(m_filePath);
            juce::String sfzText = "<group>\n";
            for(int i = 0; i < numSlices; ++i)
            {
                const int key = 60 + i;
                if(key > 127) break;

                const juce::int64 sliceStart = i * sliceLen;
                const juce::int64 sliceEnd   = (i == numSlices - 1)
                                               ? (totalFrames - 1)
                                               : ((i + 1) * sliceLen - 1);

                sfzText += "<region> sample=" + file.getFileName()
                         + " lokey=" + juce::String(key)
                         + " hikey=" + juce::String(key)
                         + " pitch_keycenter=" + juce::String(key)
                         + " offset=" + juce::String(sliceStart)
                         + " end=" + juce::String(sliceEnd)
                         + "\n";
            }
            sound = new sfzero::Sound(file);
            sound->loadRegionsFromText(sfzText.toRawUTF8(),
                                       static_cast<unsigned int>(sfzText.length()));
            sound->loadSamples(m_formatManager.get());
        }

        // Re-apply current global tuning to new regions
        if(m_tuneCents != 0)
        {
            for(auto* region : sound->getRegions())
                region->tune = m_tuneCents;
        }

        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_synth->clearSounds();
            m_synth->addSound(sound);
            m_synth->setCurrentPlaybackSampleRate(static_cast<double>(m_samplerate));
            m_presetCount   = 0;
            m_selectedPreset = 0;
        }

        return true;
    }

    void Device::applyGlobalTranspose()
    {
        if(m_synth->getNumSounds() == 0)
            return;

        auto* sound = dynamic_cast<sfzero::Sound*>(m_synth->getSound(0).get());
        if(!sound)
            return;

        // Update all regions' tune field (in cents) for smooth pitch control
        for(auto* region : sound->getRegions())
            region->tune = m_tuneCents;

        // Re-send last pitch wheel value per channel to force active voices
        // to recalculate their pitch ratio with the new transpose
        for(int ch = 0; ch < 16; ++ch)
            m_synth->handlePitchWheel(ch + 1, m_lastPitchWheel[ch]);
    }

    // ── Audio processing ──────────────────────────────────────────────────────

    void Device::processAudio(const synthLib::TAudioInputs& /*_inputs*/,
                              const synthLib::TAudioOutputs& _outputs,
                              size_t _samples)
    {
        if(_samples == 0)
            return;

        const int numSamples = static_cast<int>(_samples);

        // Render into a temporary stereo buffer
        juce::AudioSampleBuffer buffer(2, numSamples);
        buffer.clear();

        {
            std::lock_guard<std::mutex> lock(m_lock);
            juce::MidiBuffer emptyMidi;
            m_synth->renderNextBlock(buffer, emptyMidi, 0, numSamples);
        }

        // Copy to output
        if(_outputs[0])
            std::memcpy(_outputs[0], buffer.getReadPointer(0), _samples * sizeof(float));
        if(_outputs[1])
            std::memcpy(_outputs[1], buffer.getReadPointer(1), _samples * sizeof(float));
    }

    // ── MIDI handling ─────────────────────────────────────────────────────────

    bool Device::sendMidi(const synthLib::SMidiEvent& _ev,
                          std::vector<synthLib::SMidiEvent>& /*_response*/)
    {
        if(!_ev.sysex.empty())
            return false; // no sysex support for sample player

        const uint8_t status  = _ev.a & 0xf0;
        const int channel     = (_ev.a & 0x0f) + 1;

        switch(status)
        {
        case synthLib::M_NOTEON:
            if(_ev.c > 0)
                m_synth->noteOn(channel, _ev.b, _ev.c / 127.0f);
            else
                m_synth->noteOff(channel, _ev.b, 0.0f, true);
            return true;

        case synthLib::M_NOTEOFF:
            m_synth->noteOff(channel, _ev.b, _ev.c / 127.0f, true);
            return true;

        case synthLib::M_PITCHBEND:
            {
                const int pw = (_ev.c << 7) | _ev.b;
                m_lastPitchWheel[(_ev.a & 0x0f)] = pw;
                m_synth->handlePitchWheel(channel, pw);
            }
            return true;

        case synthLib::M_CONTROLCHANGE:
            // CC20 = global tuning in cents (±2400 cents / ±24 semitones, center 64)
            // Applies smooth pitch offset to all regions and recalculates active voices.
            if(_ev.b == 20)
            {
                // Map 0..127 → -2400..+2400 cents (center 64 = 0)
                const int tuneCents = static_cast<int>(
                    std::round((static_cast<double>(_ev.c) - 64.0) * (2400.0 / 63.0)));
                if(tuneCents != m_tuneCents)
                {
                    m_tuneCents = tuneCents;
                    applyGlobalTranspose();
                }
                return true;
            }
            m_synth->handleController(channel, _ev.b, _ev.c);
            return true;

        case synthLib::M_PROGRAMCHANGE:
            if(m_isoReader)
                selectIsoPreset(_ev.b);
            else if(m_presetCount > 0)
                selectPreset(_ev.b);
            return true;

        case synthLib::M_AFTERTOUCH:
            m_synth->handleChannelPressure(channel, _ev.b);
            return true;

        case synthLib::M_POLYPRESSURE:
            m_synth->handleAftertouch(channel, _ev.b, _ev.c);
            return true;

        default:
            return false;
        }
    }
}
