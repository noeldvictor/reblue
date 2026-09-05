/**
 * @file    native_pass.h
 * @brief   Host-owned nested pass attachments and logical content extents.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <cstddef>
#include <optional>
#include <vector>

namespace bd::gpu::scene {
struct PassExtent {
  float width = 0, height = 0;
  bool operator==(const PassExtent &) const = default;
};
template <class Image> struct PassAttachments {
  Image color{}, depth{};
  bool operator==(const PassAttachments &) const = default;
};

// A depth-only pass inherits its parent's logical content extent. The physical
// viewport still comes from the bound depth image; the two are not synonymous.
// No device shadows, surface headers, console nesting limit or guest addresses.
template <class Image> class NativePassStack {
public:
  struct SavedPass {
    PassAttachments<Image> attachments;
    PassExtent content;
  };
  std::size_t Depth() const { return saved_.size(); }
  PassExtent Content() const { return content_; }
  bool SetRootContent(PassExtent extent) {
    if (!saved_.empty())
      return false;
    content_ = extent;
    return true;
  }
  void Enter(PassAttachments<Image> previous,
             std::optional<PassExtent> color_extent) {
    saved_.push_back({previous, content_});
    if (color_extent)
      content_ = *color_extent;
  }
  const SavedPass *Previous() const {
    return saved_.empty() ? nullptr : &saved_.back();
  }
  std::optional<SavedPass> Leave() {
    if (saved_.empty())
      return {};
    const auto result = saved_.back();
    saved_.pop_back();
    content_ = result.content;
    return result;
  }
private:
  PassExtent content_;
  std::vector<SavedPass> saved_;
};
} // namespace bd::gpu::scene
