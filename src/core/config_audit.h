/**
 * @file    core/config_audit.h
 * @brief   Reports settings in the profile config that did not take effect.
 *
 * @copyright Copyright (c) 2026 re:Blue contributors
 * @license   BSD 3-Clause - see LICENSE
 */
#pragma once

#include <filesystem>

namespace bd {

// Warns for every line in the profile config whose live cvar value is not what
// the file asked for - an unknown name, a type the parser dropped, a value a
// range check rejected, or a cvar that needed a restart. Call once, after the
// config has loaded. Reports only; changes nothing.
void AuditProfileConfig(const std::filesystem::path &config_path);

} // namespace bd
