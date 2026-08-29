/**
 * @file    engine/battle.h
 * @brief   Battle state. The active flag, the persistent counters and the
 *          party walk are always reachable. Enemies and phase need the
 *          BattleManagerTask, which has no root global and is captured each
 *          frame by the bdBattleSceneUpdate hook.
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 */
#pragma once

#include <rex/types.h>

#include "engine/character.h"

namespace bd::engine {

// Called once at the top of each bdMainGameStep. The captured manager EA is
// only trusted within the step that produced it: a freed manager block loses
// its DEAD sentinel once the heap reuses it, so the sentinel alone cannot prove
// liveness across frames.
void OnBattleGameStep();

// True while a battle is on screen. Reads the same liveness the battle manager
// does - the battle camera control task - rather than any state of its own, so
// it cannot disagree with the rest of this file about whether a battle exists.
//
// The VR camera uses it: battles are stationary set-pieces, so a follow camera
// has nothing to follow and a fixed diorama view suits them far better.
bool BattleActive();

class Battle {
public:
  Battle() = default;

  // The battle view task is live. True well before the manager is captured.
  explicit operator bool() const;
  bool IsActive() const;

  // Persistent counters, reachable without the manager root. False before the
  // counter block itself resolves (e.g. no save loaded yet). Wins/Escapes/
  // SurroundWins read 0 in that case rather than a real count.
  bool HasStats() const;
  u32 Wins() const;
  u32 Escapes() const;
  u32 SurroundWins() const;

  // The player side, which is the field party list.
  size_t CombatantCount() const;
  PlayableCharacter CombatantAt(size_t i) const;

  // Everything below needs the manager root. HasManager() is false until
  // bdBattleSceneUpdate has run this step, and the phase accessors return
  // kNoPhase while it is.
  static constexpr u32 kNoPhase = ~0u;

  bool HasManager() const;
  u32 Phase() const; // 0=result, 1=action-dispatch, 2=transition
  u32 SubPhase() const;
  u32 ActionStep() const;
  u32 CombinedNum() const;
  bool ResourcesLoaded() const;
  u32 CurrentActorAddress() const; // 0 when the manager root is not captured

  size_t EnemyCount() const;
  Enemy EnemyAt(size_t i) const;
};

} // namespace bd::engine
