/**
 * @file    core/config_audit.cpp
 * @brief   Reports settings in the profile config that did not take effect.
 *
 * A setting that is silently ignored is indistinguishable from a setting that
 * does nothing, and this project has now lost time to that four separate ways
 * in one session:
 *
 *   - `bd_no_encounters = true` sat in a profile for a whole session. There is
 *     no such cvar. Every run was taken believing encounters were suppressed.
 *   - `bd_ab_flag = bd_mv_resolve` - unquoted, so not a TOML string. The value
 *     was dropped, every frame reported `ab_arm=255`, and the experiment read
 *     exactly like one that found nothing.
 *   - `bd_stereo_separation = 0.7` against a `.range(0.0, 0.2)` that had been
 *     widened in source but not rebuilt. Rejected, silently fell back to the
 *     default, and the result read as the effect saturating.
 *   - `bd_msaa = 1`, which the validator rejects, is already recorded in
 *     CLAUDE.md as producing "a run that proves MSAA does not matter" with MSAA
 *     still on.
 *
 * All four are the same failure and all four are caught by one question asked
 * after the config has loaded: **does the live value match what the file
 * asked for?** That covers unknown names, type mismatches, range rejections and
 * lifecycle refusals without needing to know which happened.
 *
 * The parse here is deliberately dumb - flat `key = value`, which is what these
 * profiles are - because this must never be the thing that fails. It reports
 * and never changes anything.
 *
 * @copyright Copyright (c) 2026 re:Blue contributors
 * @license   BSD 3-Clause - see LICENSE
 */
#include "core/config_audit.h"

#include "core/logging.h"

#include <rex/cvar.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <string>
#include <unordered_set>

namespace bd {

namespace {

std::string Trim(std::string_view v) {
  const auto b = v.find_first_not_of(" \t\r\n");
  if (b == std::string_view::npos)
    return {};
  const auto e = v.find_last_not_of(" \t\r\n");
  return std::string(v.substr(b, e - b + 1));
}

// Strips the quotes a TOML string carries and the config reader does not.
std::string Unquote(std::string s) {
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\'')))
    return s.substr(1, s.size() - 2);
  return s;
}

bool AsNumber(const std::string &s, double &out) {
  try {
    size_t used = 0;
    out = std::stod(s, &used);
    return used == s.size();
  } catch (...) {
    return false;
  }
}

// True when the live value is what the file asked for. Textual comparison is
// not enough on its own: 0.03 reads back as 0.030000, and true/false round-trip
// through several spellings.
bool Agrees(const std::string &want, const std::string &live) {
  if (want == live)
    return true;

  auto lower = [](std::string v) {
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return v;
  };
  const std::string w = lower(want), l = lower(live);
  if (w == l)
    return true;
  if ((w == "true" || w == "1") && (l == "true" || l == "1"))
    return true;
  if ((w == "false" || w == "0") && (l == "false" || l == "0"))
    return true;

  double dw = 0.0, dl = 0.0;
  if (AsNumber(want, dw) && AsNumber(live, dl)) {
    const double scale = std::max({1.0, std::abs(dw), std::abs(dl)});
    return std::abs(dw - dl) <= 1e-6 * scale;
  }
  return false;
}

} // namespace

void AuditProfileConfig(const std::filesystem::path &config_path) {
  std::ifstream in(config_path);
  if (!in)
    return; // No profile config is the normal case, not a problem.

  std::unordered_set<std::string> known;
  for (const auto &name : rex::cvar::ListFlags())
    known.insert(name);

  u32 checked = 0, bad = 0;
  std::string line;
  while (std::getline(in, line)) {
    const auto hash = line.find('#');
    if (hash != std::string::npos)
      line = line.substr(0, hash);
    const auto eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    const std::string key = Trim(std::string_view(line).substr(0, eq));
    if (key.empty() || key.front() == '[')
      continue;
    const std::string want =
        Unquote(Trim(std::string_view(line).substr(eq + 1)));
    if (want.empty())
      continue;

    ++checked;
    if (!known.count(key)) {
      ++bad;
      BD_WARN("[config] '{}' is not a cvar - this line does nothing", key);
      continue;
    }
    const std::string live = rex::cvar::GetFlagByName(key);
    if (!Agrees(want, live)) {
      ++bad;
      // Rejected by a range or type check, or refused because the cvar needs a
      // restart. Which one does not matter here: the point is that the run is
      // not the run the file describes.
      BD_WARN("[config] '{}' did not take effect: file says '{}', live value "
              "is '{}'",
              key, want, live);
    }
  }

  if (bad)
    BD_WARN("[config] {} of {} settings in {} did not take effect", bad,
            checked, config_path.filename().string());
  else if (checked)
    BD_INFO("[config] all {} settings in {} took effect", checked,
            config_path.filename().string());
}

} // namespace bd
