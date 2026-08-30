/**
 * @file    core/threading.h
 * @brief   Native threading, sleep, and frame timing hooks.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause - see LICENSE
 */
#pragma once

namespace bd {

void EnableHighResTimer();
void DisableHighResTimer();

// Best effort. Never dies hard.
void DemoteThreadToBackground();

// big.LITTLE placement for the guest's threads (Android only; a no-op
// elsewhere and when bd_thread_policy is off).
//
// The Quest runtime pins our render thread to the big cluster, cores 4-6, and
// leaves guest threads on all eight - so five guest workers at ~70% each land
// on exactly the three cores the renderer needs, while the 2.84GHz prime core
// sits underused. Cheap enough to call every couple of seconds; it walks
// /proc/self/task and only issues a syscall when a thread's mask is wrong.
void ApplyThreadPolicy();

// TerminateProcess and spin. Never returns, runs no destructors (not a clean
// exit).
[[noreturn]] void TerminateProcessNow(int exit_code = 0);

} // namespace bd
