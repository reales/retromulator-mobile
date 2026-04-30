#include "n2xhardware.h"

#include <chrono>
#include <cstdio>
#include <new>
#include <thread>

#include "n2xromloader.h"
#include "dsp56kBase/threadtools.h"
#include "synthLib/deviceException.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

namespace n2x
{
	constexpr uint32_t g_syncEsaiFrameRate = 16;
	constexpr uint32_t g_syncHaltDspEsaiThreshold = 32;

	static_assert((g_syncEsaiFrameRate & (g_syncEsaiFrameRate - 1)) == 0, "esai frame sync rate must be power of two");
	static_assert(g_syncHaltDspEsaiThreshold >= g_syncEsaiFrameRate * 2, "esai DSP halt threshold must be greater than two times the sync rate");

	Rom initRom(const std::vector<uint8_t>& _romData, const std::string& _romName)
	{
		if(_romData.empty())
			return RomLoader::findROM();
		Rom rom(_romData, _romName);
		if(rom.isValid())
			return rom;
		return RomLoader::findROM();
	}

	Hardware::Hardware(const std::vector<uint8_t>& _romData, const std::string& _romName)
		: m_rom(initRom(_romData, _romName))
		, m_uc(*this, m_rom)
		, m_dspA(*this, m_uc.getHdi08A(), 0)
		, m_dspB(*this, m_uc.getHdi08B(), 1)
#if TARGET_OS_IPHONE
		, m_samplerateInv(1.0 / 48360.0)  // match actual DSP B production rate on iOS (speedPercent=200)
#else
		, m_samplerateInv(1.0 / g_samplerate)
#endif
		, m_semDspAtoB(2)
	{
		if(!m_rom.isValid())
			throw synthLib::DeviceException(synthLib::DeviceError::FirmwareMissing, "No firmware found, expected firmware .bin with a size of " + std::to_string(Rom::MySize) + " bytes");

		m_dspA.getPeriph().getEsai().setCallback([this](dsp56k::Audio*){ onEsaiCallbackA(); }, 0);
		m_dspB.getPeriph().getEsai().setCallback([this](dsp56k::Audio*){ onEsaiCallbackB(); }, 0);

		m_ucThread.reset(new std::thread([this]
		{
			ucThreadFunc();
		}));

		{
			// During boot we must drive the DSPs forward without blocking on audio
			// output.  processAudio() calls processAudioOutputInterleaved() which
			// does a blocking semaphore wait per frame — on iOS interpreter mode the
			// DSPs are too slow and haven't produced output yet, so it deadlocks.
			//
			// Instead we: 1) notify the DSP rate-limiter so both DSPs can run,
			//              2) drain ESAI ring buffers to prevent deadlocks,
			//              3) sleep to give DSP threads time.
			//
			// We only wait for m_bootFinished (set by LCD pattern from UC thread).
			// The DSPs may still be initialising ESAI when this completes — that is
			// OK because the normal processAudio path will block-wait for output,
			// which is the intended synchronisation once audio callbacks start.
			auto& esaiB = m_dspB.getPeriph().getEsai();
			while(!m_bootFinished.load())
			{
				// Let DSPs advance — use large counts so both DSPs can
				// execute many ESAI frames per boot iteration.  On iOS the
				// interpreter is much slower and the semaphore throttle in
				// onEsaiCallbackB starves the DSPs if we only allow 8 frames.
				m_haltDSPSem.notify(256);
				for(int i = 0; i < 16; ++i)
					m_semDspAtoB.notify();

				// Drain DSP B output (nobody consumes it during boot).
				while(esaiB.getAudioOutputs().size() > 0)
					esaiB.getAudioOutputs().pop_front();

				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}

			// Final drain — the boot loop may have exited before the last
			// batch of DSP output was consumed.
			while(esaiB.getAudioOutputs().size() > 0)
				esaiB.getAudioOutputs().pop_front();

		}

		// The boot loop pumped hundreds of thousands of semaphore credits
		// into m_haltDSPSem (256 per iteration).  Those leftover credits
		// let the DSPs run free after boot, filling the output buffer to
		// its maximum (~33k frames) before gating takes effect — which is
		// the root cause of the ~1 second note-to-sound latency on iOS.
		// Reset the semaphore so gating starts from zero.
		new (&m_haltDSPSem) dsp56k::SpscSemaphoreWithCount();

		// Pre-allocate audio buffers for a large block size so that ensureBufferSize()
		// never reallocates vectors while the UC thread is running (data race fix).
		ensureBufferSize(4096);

		m_midiOffsetCounter = 0;
	}

	Hardware::~Hardware()
	{
		m_destroy = true;

		while(m_destroy)
			processAudio(8,64);

		m_dspA.terminate();
		m_dspB.terminate();

		m_esaiFrameIndex = 0;
		m_esaiLatency = 0;

		while(!m_dspA.getDSPThread().runThread() || !m_dspB.getDSPThread().runThread())
		{
			// DSP A waits for space to push to DSP B
			m_semDspAtoB.notify();

			if(m_dspB.getPeriph().getEsai().getAudioInputs().full())
				m_dspB.getPeriph().getEsai().getAudioInputs().pop_front();

			// DSP B waits for ESAI rate limiting and for DSP A to provide audio data
			m_haltDSPSem.notify(999999);
			if(m_dspA.getPeriph().getEsai().getAudioOutputs().empty())
				m_dspA.getPeriph().getEsai().getAudioOutputs().push_back({});
		}

		m_ucThread->join();
	}

	bool Hardware::isValid() const
	{
		return m_rom.isValid();
	}

	void Hardware::processUC()
	{
		if(m_remainingUcCycles <= 0)
			syncUCtoDSP();

		const auto deltaCycles = m_uc.exec();

		if(m_esaiFrameIndex > 0)
			m_remainingUcCycles -= static_cast<int64_t>(deltaCycles);
	}

	void Hardware::processAudio(uint32_t _frames, const uint32_t _latency)
	{
		getMidi().process(_frames);

		ensureBufferSize(_frames);

#if TARGET_OS_IPHONE
		++m_debugCallbackCount;
#endif

		// If ESAI hasn't started yet (DSP firmware still initialising), output
		// silence and keep driving the DSPs forward without blocking.  The normal
		// processAudioOutputInterleaved path does a blocking semaphore wait and
		// would stall the audio thread indefinitely on iOS interpreter mode.
		if(m_esaiFrameIndex == 0)
		{
			for(auto& ao : m_audioOutputs)
				std::fill(ao.begin(), ao.begin() + _frames, 0);

			// Keep driving DSPs so they can finish ESAI init.
			// Only drain DSP B output (nobody reads it yet).
			m_haltDSPSem.notify(_frames);
			for(int i = 0; i < 16; ++i)
				m_semDspAtoB.notify();
			auto& esaiB = m_dspB.getPeriph().getEsai();
			while(esaiB.getAudioOutputs().size() > 0)
				esaiB.getAudioOutputs().pop_front();
			return;
		}

		dsp56k::TWord* outputs[12]{nullptr};
		outputs[1] = &m_audioOutputs[0].front();
		outputs[0] = &m_audioOutputs[1].front();
		outputs[3] = &m_audioOutputs[2].front();
		outputs[2] = &m_audioOutputs[3].front();
		outputs[4] = m_dummyOutput.data();
		outputs[5] = m_dummyOutput.data();
		outputs[6] = m_dummyOutput.data();
		outputs[7] = m_dummyOutput.data();
		outputs[8] = m_dummyOutput.data();
		outputs[9] = m_dummyOutput.data();
		outputs[10] = m_dummyOutput.data();
		outputs[11] = m_dummyOutput.data();

		auto& esaiA = m_dspA.getPeriph().getEsai();
		auto& esaiB = m_dspB.getPeriph().getEsai();

		while (_frames)
		{
			const auto processCount = std::min(_frames, static_cast<uint32_t>(64));
			_frames -= processCount;

			advanceSamples(processCount, _latency);

#if TARGET_OS_IPHONE
			// On iOS, wait for ALL frames before entering processAudioOutputInterleaved
			// to avoid any blocking inside its per-frame semaphore wait.
			const auto requiredSize = static_cast<size_t>(processCount);
#else
			const auto requiredSize = processCount > 8 ? processCount - 8 : 0;
#endif

			if(esaiB.getAudioOutputs().size() < requiredSize)
			{
				// reduce thread contention by waiting for output buffer to be full enough to let us grab the data without entering the read mutex too often

				std::unique_lock uLock(m_requestedFramesAvailableMutex);
				m_requestedFrames = requiredSize;
#if TARGET_OS_IPHONE
				// On iOS, use a timeout to prevent the audio callback from
				// blocking indefinitely.  If the DSPs are momentarily preempted
				// by the OS, an unbounded wait would trigger the audio watchdog
				// and crash the app.  Output silence instead.
				const bool ready = m_requestedFramesAvailableCv.wait_for(uLock, std::chrono::milliseconds(20), [&]()
				{
					if(esaiB.getAudioOutputs().size() < requiredSize)
						return false;
					m_requestedFrames = 0;
					return true;
				});
				if(!ready)
				{
					m_requestedFrames = 0;
					// Drain whatever frames ARE available so gating credits
					// don't accumulate (which would grow latency).
					const auto available = std::min(
						static_cast<uint32_t>(esaiB.getAudioOutputs().size()),
						processCount);
					++m_debugUnderrunCount;
					fprintf(stderr, "[N2X UNDERRUN #%u] cb=%u needed=%u avail=%u zeroed=%u bufSize=%zu\n",
						m_debugUnderrunCount, m_debugCallbackCount,
						processCount, available, processCount - available,
						esaiB.getAudioOutputs().size());
					if(available > 0)
						esaiB.processAudioOutputInterleaved(outputs, available);
					// Zero-fill the remainder
					for(uint32_t ch = 0; ch < 4; ++ch)
						for(uint32_t s = available; s < processCount; ++s)
							outputs[ch][s] = 0;
					outputs[0] += processCount;
					outputs[1] += processCount;
					outputs[2] += processCount;
					outputs[3] += processCount;
					continue;
				}
#else
				m_requestedFramesAvailableCv.wait(uLock, [&]()
				{
					if(esaiB.getAudioOutputs().size() < requiredSize)
						return false;
					m_requestedFrames = 0;
					return true;
				});
#endif
			}

	#if TARGET_OS_IPHONE
			if((m_debugCallbackCount % 200) == 0)
			{
				fprintf(stderr, "[N2X SYNC] cb=%u bufSize=%zu req=%u underruns=%u\n",
					m_debugCallbackCount,
					esaiB.getAudioOutputs().size(),
					processCount, m_debugUnderrunCount);
			}
#endif
			// read output of DSP B to regular audio output
			esaiB.processAudioOutputInterleaved(outputs, processCount);

			outputs[0] += processCount;
			outputs[1] += processCount;
			outputs[2] += processCount;
			outputs[3] += processCount;
		}
	}
	
	void Hardware::processAudio(const synthLib::TAudioOutputs& _outputs, const uint32_t _frames, const uint32_t _latency)
	{
		processAudio(_frames, _latency);

		// DSP B outputs two stereo pairs across 4 channels:
		//   m_audioOutputs[1] = Pair1 L (_tx[0][0])
		//   m_audioOutputs[0] = Pair1 R (_tx[1][0])
		//   m_audioOutputs[3] = Pair2 L (_tx[0][1])
		//   m_audioOutputs[2] = Pair2 R (_tx[1][1])
		// Sum both pairs and scale up: the firmware divides by voice count internally,
		// so we compensate with *4 to restore unity gain.
		for(size_t i=0; i<_frames; ++i)
		{
			_outputs[0][i] = 4.0f * (dsp56k::dsp2sample<float>(m_audioOutputs[1][i]) + dsp56k::dsp2sample<float>(m_audioOutputs[3][i]));
			_outputs[1][i] = 4.0f * (dsp56k::dsp2sample<float>(m_audioOutputs[0][i]) + dsp56k::dsp2sample<float>(m_audioOutputs[2][i]));
		}
	}

	bool Hardware::sendMidi(const synthLib::SMidiEvent& _ev)
	{
		m_midiIn.push_back(_ev);
		return true;
	}

	size_t Hardware::getOutputBufferSize()
	{
		return m_dspB.getPeriph().getEsai().getAudioOutputs().size();
	}

	void Hardware::notifyBootFinished()
	{
		m_bootFinished = true;
	}

	void Hardware::ensureBufferSize(const uint32_t _frames)
	{
		if(m_dummyInput.size() >= _frames)
			return;

		m_dummyInput.resize(_frames, 0);
		m_dummyOutput.resize(_frames, 0);

		for (auto& audioOutput : m_audioOutputs)
			audioOutput.resize(_frames, 0);

		m_dspAtoBBuffer.resize(_frames * 4);
	}

	void Hardware::onEsaiCallbackA()
	{
		// forward DSP A output to DSP B input
		const auto out = m_dspA.getPeriph().getEsai().getAudioOutputs().pop_front();

		dsp56k::Audio::RxFrame in;
		in.resize(out.size());

		in[0] = dsp56k::Audio::RxSlot{out[0][0]};
		in[1] = dsp56k::Audio::RxSlot{out[1][0]};
		in[2] = dsp56k::Audio::RxSlot{out[2][0]};
		in[3] = dsp56k::Audio::RxSlot{out[3][0]};

		m_dspB.getPeriph().getEsai().getAudioInputs().push_back(in);

		m_semDspAtoB.wait();
	}

	void Hardware::processMidiInput()
	{
		++m_midiOffsetCounter;

		while(!m_midiIn.empty())
		{
			const auto& e = m_midiIn.front();

			if(e.offset > m_midiOffsetCounter)
				break;

			getMidi().write(e);
			m_midiIn.pop_front();
		}
	}

	void Hardware::onEsaiCallbackB()
	{
		m_semDspAtoB.notify();

		++m_esaiFrameIndex;

		processMidiInput();

		if((m_esaiFrameIndex & (g_syncEsaiFrameRate-1)) == 0)
			m_esaiFrameAddedCv.notify_one();

		m_requestedFramesAvailableMutex.lock();

		if(m_requestedFrames && m_dspB.getPeriph().getEsai().getAudioOutputs().size() >= m_requestedFrames)
		{
			m_requestedFramesAvailableMutex.unlock();
			m_requestedFramesAvailableCv.notify_one();
		}
		else
		{
			m_requestedFramesAvailableMutex.unlock();
		}

		// Gate the DSPs frame-by-frame to keep them in sync with the audio
		// thread.  Without this the DSPs run faster than real-time and the
		// output buffer grows unboundedly, adding massive latency.
		m_haltDSPSem.wait(1);
	}

	void Hardware::syncUCtoDSP()
	{
		assert(m_remainingUcCycles <= 0);

		// we can only use ESAI to clock the uc once it has been enabled
		if(m_esaiFrameIndex <= 0)
			return;

		if(m_esaiFrameIndex == m_lastEsaiFrameIndex)
		{
			resumeDSPs();
			std::unique_lock uLock(m_esaiFrameAddedMutex);
			m_esaiFrameAddedCv.wait(uLock, [this]{return m_esaiFrameIndex > m_lastEsaiFrameIndex;});
		}

		const auto esaiFrameIndex = m_esaiFrameIndex;
		const auto esaiDelta = esaiFrameIndex - m_lastEsaiFrameIndex;

		const auto ucClock = m_uc.getSim().getSystemClockHz();
		const double ucCyclesPerFrame = static_cast<double>(ucClock) * m_samplerateInv;

		// if the UC consumed more cycles than it was allowed to, remove them from remaining cycles
		m_remainingUcCyclesD += static_cast<double>(m_remainingUcCycles);

		// add cycles for the ESAI time that has passed
		m_remainingUcCyclesD += ucCyclesPerFrame * static_cast<double>(esaiDelta);

		// set new remaining cycle count
		m_remainingUcCycles = static_cast<int64_t>(m_remainingUcCyclesD);

		// and consume them
		m_remainingUcCyclesD -= static_cast<double>(m_remainingUcCycles);

		if(esaiDelta > g_syncHaltDspEsaiThreshold)
			haltDSPs();

		m_lastEsaiFrameIndex = esaiFrameIndex;
	}

	void Hardware::ucThreadFunc()
	{
		dsp56k::ThreadTools::setCurrentThreadName("MC68331");
		dsp56k::ThreadTools::setCurrentThreadPriority(dsp56k::ThreadPriority::Highest);

		while(!m_destroy)
		{
			processUC();
			processUC();
			processUC();
			processUC();
			processUC();
			processUC();
			processUC();
			processUC();
		}
		resumeDSPs();
		m_destroy = false;
	}

	void Hardware::advanceSamples(const uint32_t _samples, const uint32_t _latency)
	{
		// if the latency was higher first but now is lower, we might report < 0 samples. In this case we
		// cannot notify but have to wait for another sample block until we can notify again

		const auto latencyDiff = static_cast<int>(_latency) - static_cast<int>(m_esaiLatency);
		m_esaiLatency = _latency;

		const auto notifyCount = static_cast<int>(_samples) + latencyDiff + m_dspNotifyCorrection;

		if (notifyCount > 0)
		{
			m_haltDSPSem.notify(notifyCount);
			m_dspNotifyCorrection = 0;
		}
		else
		{
			m_dspNotifyCorrection = notifyCount;
		}
	}

	void Hardware::haltDSPs()
	{
		if(m_dspHalted)
			return;
		m_dspHalted = true;
//		LOG("Halt");
		m_dspA.getHaltDSP().haltDSP();
		m_dspB.getHaltDSP().haltDSP();
	}

	void Hardware::resumeDSPs()
	{
		if(!m_dspHalted)
			return;
		m_dspHalted = false;
//		LOG("Resume");
		m_dspA.getHaltDSP().resumeDSP();
		m_dspB.getHaltDSP().resumeDSP();
	}

	bool Hardware::getButtonState(const ButtonType _type) const
	{
		return m_uc.getFrontPanel().getButtonState(_type);
	}

	void Hardware::setButtonState(const ButtonType _type, const bool _pressed)
	{
		m_uc.getFrontPanel().setButtonState(_type, _pressed);
	}

	uint8_t Hardware::getKnobPosition(KnobType _knob) const
	{
		return m_uc.getFrontPanel().getKnobPosition(_knob);
	}

	void Hardware::setKnobPosition(KnobType _knob, uint8_t _value)
	{
		return m_uc.getFrontPanel().setKnobPosition(_knob, _value);
	}
}
