/**
 * @file    core/sampling_profiler.h
 * @brief   In-process sampling profiler for the recompiled guest.
 *
 * Horizon OS refuses shell perf on a Quest 2 whatever perf_event_paranoid says
 * and whether or not the app is profileable, so simpleperf - and with it
 * tools/profile_quest.py - has never produced a profile on the device this port
 * targets. Every performance session so far has therefore reasoned from static
 * instruction counts and frame timers, and got the answer wrong more than once.
 *
 * A process may always signal its own threads, so we sample ourselves: a timer
 * thread sends SIGPROF to the threads worth watching and the handler records
 * the interrupted PC. Nothing is symbolised on device - PCs are stored as
 * offsets into libreblue.so and resolved on the host by
 * tools/symbolize_profile.py, which reads the unstripped build artefact. That
 * keeps the on-device cost to a signal and a store, and means the 18,777
 * recompiled function names are available without shipping a symbol table.
 *
 * @copyright Copyright (c) 2026 re:Blue contributors
 * @license   BSD 3-Clause - see LICENSE
 */
#pragma once

namespace bd {

// Starts sampling if bd_sample_profiler is set. Safe to call every frame; only
// the first call with the cvar on does anything. No-op off Android.
void SamplingProfilerTick();

// Writes whatever has been collected so far. Called on shutdown; also runs
// periodically while sampling.
void SamplingProfilerDump();

} // namespace bd
