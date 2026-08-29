/**
 * @file    engine/tourist_mode.h
 * @brief   Keeps the party alive so the world can be looked at.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license MIT
 */

#pragma once

namespace bd::engine {

// Tops the party up when bd_tourist_mode is set. Cheap and idempotent; call it
// once per frame from anywhere on the guest thread.
void TouristModeTick();

} // namespace bd::engine
