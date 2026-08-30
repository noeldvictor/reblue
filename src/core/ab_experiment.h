/**
 * @file    core/ab_experiment.h
 * @brief   Within-run A/B: flip one cvar every N frames and label the frames.
 *
 * Two whole runs cannot settle a change on this workload. Measured the hard
 * way on 2026-08-30: the same binary in the same configuration produced 5.12ms
 * and 8.62ms `other_ms` minutes apart, 68% apart, while the change being tested
 * was worth nothing at all - and an earlier pair of back-to-back reversed runs
 * had made that same change look like a third of the frame. Ordering the runs
 * does not help, because the drift is slower than a run.
 *
 * The fix is to stop comparing runs. `bd_ab_flag` names a boolean cvar,
 * `bd_ab_period` says how many frames to hold each value, and every frame is
 * labelled in the perf CSV with the arm it belongs to. Both populations then
 * come from one run, one scene, one thermal state, interleaved - so whatever
 * drifts, drifts through both of them.
 *
 * @copyright Copyright (c) 2026 re:Blue contributors
 * @license   BSD 3-Clause - see LICENSE
 */
#pragma once

#include <rex/types.h>

namespace bd {

// Advances the experiment by one frame and applies the arm's value to the cvar
// under test. Returns the current arm (0 or 1), or 255 when no experiment is
// configured. Cheap enough to call unconditionally.
u8 ABExperimentTick();

} // namespace bd
