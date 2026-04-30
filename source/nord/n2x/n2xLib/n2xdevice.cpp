#include "n2xdevice.h"

#include "n2xhardware.h"
#include "n2xtypes.h"

#include <cstdio>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

namespace n2x
{
	Device::Device(const synthLib::DeviceCreateParams& _params)
		: synthLib::Device(_params)
		, m_hardware(_params.romData, _params.romName)
		, m_state(&m_hardware, &getMidiTranslator())
		, m_midiParser(synthLib::MidiEventSource::Device)
	{
	}

	float Device::getSamplerate() const
	{
#if TARGET_OS_IPHONE
		// On iOS the interpreter runs at ~114.5 MIPS.  DSP B needs 2440
		// instructions per output sample (4 × 610 cyclesPerSample at
		// speedPercent=200).  Actual production rate ≈ 114.5e6 / 2440 ≈
		// 46926 Hz.  Report a conservative rate so the resampler never
		// over-requests — a small buffer surplus is preferable to
		// underruns that cause audible stuttering.
		return 47000.0f;
#else
		return static_cast<float>(g_samplerate) * 100.0f / static_cast<float>(m_clockPercent);
#endif
	}

	bool Device::isValid() const
	{
		return m_hardware.isValid();
	}

	bool Device::getState(std::vector<uint8_t>& _state, synthLib::StateType _type)
	{
		return m_state.getState(_state);
	}

	bool Device::setState(const std::vector<uint8_t>& _state, synthLib::StateType _type)
	{
		return m_state.setState(_state);
	}

	uint32_t Device::getChannelCountIn()
	{
		return 0;
	}

	uint32_t Device::getChannelCountOut()
	{
		return 2;
	}

	bool Device::setDspClockPercent(const uint32_t _percent)
	{
		auto effective = _percent;
#if TARGET_OS_IPHONE
		// On iOS the interpreter runs at ~114.5 MIPS vs 120 MHz native.
		// Double to halve the output sample rate (~98200 -> ~48360 Hz),
		// giving the firmware more instructions per sample for poly patches
		// while staying within the DSP's throughput.
		effective *= 2;
#endif
		bool res = m_hardware.getDSPA().getPeriph().getEsaiClock().setSpeedPercent(effective);
		res &= m_hardware.getDSPB().getPeriph().getEsaiClock().setSpeedPercent(effective);
		if(res)
		{
			m_clockPercent = effective;
#if TARGET_OS_IPHONE
			m_hardware.setEffectiveSamplerate(48360.0);
#else
			const auto effectiveSR = static_cast<double>(g_samplerate) * 100.0 / static_cast<double>(effective);
			m_hardware.setEffectiveSamplerate(effectiveSR);
#endif
		}
		return res;
	}

	uint32_t Device::getDspClockPercent() const
	{
		return const_cast<Hardware&>(m_hardware).getDSPA().getPeriph().getEsaiClock().getSpeedPercent();
	}

	uint64_t Device::getDspClockHz() const
	{
		return const_cast<Hardware&>(m_hardware).getDSPA().getPeriph().getEsaiClock().getSpeedInHz();
	}

	void Device::readMidiOut(std::vector<synthLib::SMidiEvent>& _midiOut)
	{
		m_hardware.getMidi().read(m_midiOutBuffer);
		m_midiParser.write(m_midiOutBuffer);
		m_midiOutBuffer.clear();
		m_midiParser.getEvents(_midiOut);

		for (const auto& midiOut : _midiOut)
		{
			if(midiOut.sysex.empty())
				LOG("TX " << HEXN(midiOut.a,2) << ' ' << HEXN(midiOut.b,2) << ' ' << HEXN(midiOut.c, 2));
			else
				LOG("TX Sysex of size " << midiOut.sysex.size());
			m_state.receive(midiOut);
		}
	}

	void Device::processAudio(const synthLib::TAudioInputs& _inputs, const synthLib::TAudioOutputs& _outputs, size_t _samples)
	{
		m_hardware.processAudio(_outputs, static_cast<uint32_t>(_samples), getExtraLatencySamples());
		m_numSamplesProcessed += static_cast<uint32_t>(_samples);

//		m_hardware.setButtonState(ButtonType::OscSync, (m_numSamplesProcessed & 65535) > 2048);
	}

	bool Device::sendMidi(const synthLib::SMidiEvent& _ev, std::vector<synthLib::SMidiEvent>& _response)
	{
		if(_ev.sysex.empty())
		{
			// drop program change messages. We do not have any valid presets in the device, this will select garbage
			if((_ev.a & 0xf0) == synthLib::M_PROGRAMCHANGE)
				return true;

			m_state.receive(_response, _ev);
			auto e = _ev;
			// Use the hardware's MIDI offset counter (DSP production position)
			// rather than m_numSamplesProcessed (audio consumption position).
			// On iOS the audio thread can burst-request samples faster than the
			// gated DSPs produce them, causing m_numSamplesProcessed to drift
			// ahead — which schedules notes into the DSP's future, adding
			// variable latency.  The hardware counter tracks actual DSP output
			// and stays tightly coupled to real-time.
			const auto hwOffset = m_hardware.getMidiOffsetCounter();
			e.offset += hwOffset + getExtraLatencySamples();
			m_hardware.sendMidi(e);
		}
		else
		{
			if(m_state.receive(_response, _ev))
				return true;

			// State handles all valid N2X sysex types (singles, multis, requests,
			// knob positions, part CCs). Anything unrecognised is either the
			// device's own output being fed back by the host (which would flood
			// the SciMidi queue and starve real program-change sysex), or simply
			// invalid — drop it in both cases.
		}
		return true;
	}
}
