/**
 * @file    pass_bindings.h
 * @brief   Host attachment binding, independent of engine device addresses.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
namespace bd::gpu {
struct GuestTexture;
// Render-thread operations, as are the compatibility target setters. Resource
// lookup/lifetime and engine getter shadows belong to the caller. No command
// list is opened here: outgoing queued draws flush at BindDrawFramebuffer.
void BindColorAttachment(GuestTexture *surface);
void BindDepthAttachment(GuestTexture *surface);
} // namespace bd::gpu
