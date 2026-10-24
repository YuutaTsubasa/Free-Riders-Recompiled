#pragma once
// The threads that make the game's sound, ahead of its busy ones on Android.
//
// Each 5.33 ms the audio pump (diagnostic_main.cpp) calls the title's audio
// callback, which wakes the title's mixer (mixer_thread_entry, a thread the
// title makes time-critical) and waits for it to mix the frame. The mixer's
// voices are fed by the title's sound thread (sound_thread_entry). On the
// console those run ahead of the rest; here the title's priorities are only
// recorded (native_thread_posix.cpp), and every thread of the game runs at
// nice -4 (inherited from SDL's thread), so on a phone whose cores the race
// keeps busy the sound thread queues behind the title's job helpers and the
// render thread. A Pixel 10a with its A720s (where those threads run) held
// to about 0.9 GHz by heat crackled through races (Issue #176); with them
// at 1.4 GHz it did not. The frames still came on time (no
// NATIVE_AUDIO_UNDERRUN, no drops): what the mixer sent was silence.
//
// Measured on an AYN Thor with the guest workers and the render thread held
// to its three little cores (SFR_TEST_WORKER_CPUS=7 SFR_TEST_RENDER_CPUS=7):
// the race kept full speed but 104-490 of every 1875 frames were silent
// (NATIVE_AUDIO_FRAMES silent=), against 0-6 with nothing held. The pump and
// the mixer at nice -16 changed nothing; the sound thread at -10 nothing
// either, at -16 it cut the silence to a third, at -20 to a sixth.
//
// SFR_AUDIO_PRIORITY=1|0 (Android, default 1): 1 gives the pump and the
// mixer Android's THREAD_PRIORITY_AUDIO (nice -16; SDL's own audio thread
// has it already) and the sound thread THREAD_PRIORITY_URGENT_AUDIO (-19).
// Elsewhere it does nothing: Windows applies the title's priorities.
#include <cstdint>
#include <cstdlib>
#if defined(__ANDROID__)
#include <cerrno>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace sfr {

inline bool audio_priority_enabled() {
    static const bool enabled = [] {
        const char* text = std::getenv("SFR_AUDIO_PRIORITY");
#if defined(__ANDROID__)
        return !text || *text != '0';
#else
        return false;
#endif
    }();
    return enabled;
}

// The pump and the mixer each work well under a millisecond a frame.
inline constexpr int audio_thread_nice = -16;
inline constexpr uint32_t mixer_thread_entry = 0x82727730;
// The sound thread (the title makes it just after the mixer, above normal)
// is busy: a Thor race, 13% of a big core; held to its little cores, half
// of one.
inline constexpr uint32_t sound_thread_entry = 0x8222E008;
inline constexpr int sound_thread_nice = -19;

// The nice value a guest thread with this entry gets (0: left as it is).
inline int audio_thread_nice_for(uint32_t entry) {
    return entry == sound_thread_entry ? sound_thread_nice : entry == mixer_thread_entry ? audio_thread_nice : 0;
}

// Sets host thread tid (0 = the calling thread) to nice. Returns 0, or the
// error (-1 where it does nothing).
inline int raise_thread_for_audio(int tid, int nice = audio_thread_nice) {
#if defined(__ANDROID__)
    if (tid == 0) tid = int(gettid());
    return setpriority(PRIO_PROCESS, id_t(tid), nice) == 0 ? 0 : errno;
#else
    (void)tid;
    (void)nice;
    return -1;
#endif
}

}
