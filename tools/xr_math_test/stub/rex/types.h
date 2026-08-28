/**
 * @file    stub/rex/types.h
 * @brief   Stand-in for the SDK's integer aliases, so xr_math.h can be
 *          compiled and run without the ReXGlue SDK present.
 *
 * This exists only for tools/xr_math_test. It is not on the include path of
 * any real build, and it deliberately declares nothing beyond the aliases
 * xr_math.h actually uses - if the test ever needs more from here, that is a
 * sign the code under test has grown a dependency it should not have.
 *
 * @copyright Copyright (c) 2026 re:Blue contributors
 * @license   BSD 3-Clause - see LICENSE
 */
#pragma once

#include <cstdint>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;
