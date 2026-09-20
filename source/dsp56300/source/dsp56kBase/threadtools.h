#pragma once

#include <string>

namespace dsp56k
{
	enum class ThreadPriority : int8_t
	{
		Lowest = -2,
		Low = -1,
		Normal = 0,
		High = 1,
		Highest = 2
	};

	class ThreadTools
	{
	public:
		static void setCurrentThreadName(const std::string& _name);
		static bool setCurrentThreadPriority(ThreadPriority _priority);
		static bool setCurrentThreadRealtimeParameters(int _samplerate, int _blocksize);

		// The audio workgroup the host renders on. The plugin layer installs a joiner (it owns the
		// JUCE handle); every emulation worker calls joinAudioWorkgroup() once it is realtime, so
		// the performance controller sees them as one audio workload. Apple only admits realtime
		// threads, so the RT policy must already be set - setCurrentThreadPriority(Highest) first.
		using AudioWorkgroupJoiner = void(*)();
		static void setAudioWorkgroupJoiner(AudioWorkgroupJoiner _joiner);
		static void joinAudioWorkgroup();
	};
}
