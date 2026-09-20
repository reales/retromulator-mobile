#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "threadtools.h"

#include "logging.h"

#include <atomic>

#ifdef DSP56K_USE_VTUNE_JIT_PROFILING_API
#include "vtuneSdk/include/ittnotify.h"
#endif

#ifdef _WIN32
#	include <Windows.h>
#else
#	include <pthread.h>
#	include <sched.h>
#	include <sys/resource.h>
#	include <sys/syscall.h>
#	include <unistd.h>
#	include <string.h>	// strerror
#endif

#ifdef __APPLE__
#	include <mach/mach.h>
#	include <mach/mach_time.h>
#	include <pthread/qos.h>
#	include <algorithm>
#endif

namespace dsp56k
{
#ifdef _WIN32
	constexpr DWORD MS_VC_EXCEPTION = 0x406D1388;

#pragma pack(push,8)
	typedef struct tagTHREADNAME_INFO
	{
		DWORD dwType; // Must be 0x1000.
		LPCSTR szName; // Pointer to name (in user addr space).
		DWORD dwThreadID; // Thread ID (-1=caller thread).
		DWORD dwFlags; // Reserved for future use, must be zero.
	} THREADNAME_INFO;
#pragma pack(pop)

	void SetThreadName( DWORD dwThreadID, const char* threadName)
	{
		THREADNAME_INFO info;
		info.dwType = 0x1000;
		info.szName = threadName;
		info.dwThreadID = dwThreadID;
		info.dwFlags = 0;

		__try  // NOLINT(clang-diagnostic-language-extension-token)
		{
			RaiseException( MS_VC_EXCEPTION, 0, sizeof(info)/sizeof(ULONG_PTR), reinterpret_cast<ULONG_PTR*>(&info) );
		}
		__except(EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}
#endif
	void ThreadTools::setCurrentThreadName(const std::string& _name)
	{
#ifdef _WIN32
		SetThreadName(-1, _name.c_str());
#elif defined(__APPLE__)
		pthread_setname_np(_name.c_str());
#else
		pthread_setname_np(pthread_self(), _name.c_str());
#endif

#ifdef DSP56K_USE_VTUNE_JIT_PROFILING_API
		__itt_thread_set_name(_name.c_str());
#endif
	}

	bool ThreadTools::setCurrentThreadPriority(ThreadPriority _priority)
	{
#ifdef _WIN32
		int prio;
		switch(_priority)
		{
		case ThreadPriority::Lowest:	prio = THREAD_PRIORITY_LOWEST; break;
		case ThreadPriority::Low:		prio = THREAD_PRIORITY_BELOW_NORMAL; break;
		case ThreadPriority::Normal:	prio = THREAD_PRIORITY_NORMAL; break;
		case ThreadPriority::High:		prio = THREAD_PRIORITY_ABOVE_NORMAL; break;
		case ThreadPriority::Highest:	prio = THREAD_PRIORITY_TIME_CRITICAL; break;
		default: return false;
		}
		if( !::SetThreadPriority(GetCurrentThread(), prio))
		{
			LOG("Failed to set thread priority to " << prio);
			return false;
		}
#elif defined(__APPLE__)
		const auto max = sched_get_priority_max(SCHED_OTHER);
		const auto min = sched_get_priority_min(SCHED_OTHER);
		const auto normal = (max - min) >> 1;
		const auto above = (max + normal) >> 1;
		const auto below = (min + normal) >> 1;

		int prio;
		switch(_priority)
		{
		case ThreadPriority::Lowest:	prio = min; break;
		case ThreadPriority::Low:		prio = below; break;
		case ThreadPriority::Normal:	prio = normal; break;
		case ThreadPriority::High:		prio = above; break;
		case ThreadPriority::Highest:	prio = max; break;
		default: return false;
		}

		// pthread_setschedparam permanently opts the thread out of the QOS class system,
		// after which pthread_set_qos_class_self_np fails with EPERM and the thread becomes
		// eligible for the efficiency cores. Set QOS first and skip setschedparam entirely
		// for the priorities that have a QOS equivalent.
		if (_priority == ThreadPriority::Highest)
		{
			const auto qos = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
			if (qos)
				LOG("Failed to set QOS class to USER_INTERACTIVE, error code " << qos);
			setCurrentThreadRealtimeParameters(0, 0);
#if defined(__aarch64__)
			// FPCR is per-thread; flush denormals to zero on the DSP thread too
			uint64_t fpcr;
			__asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
			__asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr | (1ull << 24)));
#endif
		}
		else if (_priority == ThreadPriority::Low || _priority == ThreadPriority::Lowest)
		{
			const auto qos = pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
			if (qos)
				LOG("Failed to set QOS class to BACKGROUND, error code " << qos);
		}
		else
		{
			sched_param sch_params;
			sch_params.sched_priority = prio;

			const auto result = pthread_setschedparam(pthread_self(), SCHED_OTHER, &sch_params);
			if(result)
				LOG("Failed to set thread priority to " << prio << ", error code " << result);
		}
#else
		// On Linux we adjust the 'nice' value of the thread
		int prio;
		switch (_priority)
		{
			case ThreadPriority::Lowest:	prio = 15;
			case ThreadPriority::Low:		prio = 10;
			case ThreadPriority::Normal:	prio = 0;
			case ThreadPriority::High:		prio = -5;
			case ThreadPriority::Highest:	prio = -10;
		}
#ifdef PRIO_THREAD
		const auto result = setpriority(PRIO_THREAD, 0, prio);
#else
		const auto tid = syscall(SYS_gettid);
		if (!tid)
		{
			LOG("Failed to get thread id for setting priority");
			return false;
		}
		const auto result = setpriority(PRIO_PROCESS, tid, prio);
#endif
		if (result != 0)
		{
			LOG("Failed to set thread priority to " << prio << ", error code " << result);
		}
#endif
		return true;
	}

	bool ThreadTools::setCurrentThreadRealtimeParameters(int _samplerate, int _blocksize)
	{
#ifdef __APPLE__
		bool usePeriod = true;
		if (!_samplerate || !_blocksize)
		{
			// no fixed call frequency known. Keep the window short: the budget is renewed once per
			// window, and a 46 ms one leaves the DSP workers unprotected for tens of ms at a time
			_samplerate = 48000;
			_blocksize = 128;
			usePeriod = false;
		}
	    // Compute the nominal "period" between activations, in microseconds.
	    // Example: 44100 Hz, 1024 buffer => 23,219 us
	    double periodUsec = static_cast<double>(_blocksize) * 1'000'000.0 / static_cast<double>(_samplerate);

	    // Reasonable assumptions:
		// computation = 25% - 35% of the period
	    // constraint = equal to or slightly above the period
	    // The exact numbers aren't critical, but they should stay consistent.
	    double computationUsec = periodUsec * 0.30;
	    double constraintUsec  = periodUsec * 1.05;

	    // Clamp to sane limits
	    computationUsec = std::max(computationUsec, 1000.0); // >= 1 ms
	    constraintUsec = std::max(constraintUsec, computationUsec + 1000.0); // Always > computation

	    // thread_policy_set expects mach absolute time units, not microseconds. On Apple Silicon
	    // the timebase is not 1 ns per tick, so passing microseconds makes the policy be rejected.
	    mach_timebase_info_data_t timebase{};
	    mach_timebase_info(&timebase);
	    const double usecToAbs = 1000.0 * static_cast<double>(timebase.denom) / static_cast<double>(timebase.numer);

	    uint32_t computation = static_cast<uint32_t>(computationUsec * usecToAbs);
	    uint32_t constraint  = static_cast<uint32_t>(constraintUsec * usecToAbs);
	    uint32_t period      = usePeriod ? static_cast<uint32_t>(periodUsec * usecToAbs) : 0;

	    // Prepare Mach real-time policy
	    thread_time_constraint_policy_data_t policy;
	    policy.period       = period;        // expected activation interval
	    policy.computation  = computation;   // estimated CPU time
	    policy.constraint   = constraint;    // must finish before this
	    policy.preemptible  = TRUE;

	    thread_port_t thread = pthread_mach_thread_np(pthread_self());
	    kern_return_t result = thread_policy_set(thread,
	                                             THREAD_TIME_CONSTRAINT_POLICY,
	                                             (thread_policy_t)&policy,
	                                             THREAD_TIME_CONSTRAINT_POLICY_COUNT);

	    if (result == KERN_SUCCESS)
	    {
			LOG("Success setting thread realtime parameters: period=" << period << ", computation=" << computation << ", constraint=" << constraint << " (mach abs units)");
	        return true;
	    }
		LOG("Failed to set thread realtime parameters, error code " << result);
#endif
		return false;
	}

	namespace
	{
		std::atomic<ThreadTools::AudioWorkgroupJoiner> g_workgroupJoiner{nullptr};

		// bumped whenever the host hands over a different workgroup. Worker threads compare it
		// against what they last joined, so calling joinAudioWorkgroup() every job costs one
		// relaxed load in the common case where nothing changed.
		std::atomic<uint32_t> g_workgroupGeneration{0};
		thread_local uint32_t t_joinedGeneration = 0;
	}

	void ThreadTools::setAudioWorkgroupJoiner(AudioWorkgroupJoiner _joiner)
	{
		g_workgroupJoiner.store(_joiner, std::memory_order_release);
		g_workgroupGeneration.fetch_add(1, std::memory_order_release);
	}

	void ThreadTools::joinAudioWorkgroup()
	{
		const auto generation = g_workgroupGeneration.load(std::memory_order_acquire);

		if (generation == t_joinedGeneration)
			return;

		if (const auto joiner = g_workgroupJoiner.load(std::memory_order_acquire))
		{
			joiner();
			t_joinedGeneration = generation;
		}
	}
}
