#pragma once

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdint.h>

#include "../../../Memory/Memory.h"

// The panel still renders four tones; these modes describe how an 8-bit image
// is spatially reduced to those tones. The enum lives in the converter layer so
// reader settings do not leak into the image decoder API.
enum class ImageDitherMode : uint8_t {
  None = 0,
  Bayer8x8 = 1,
  Bayer4x4 = 2,
  ErrorDiffusion = 3,
  Random = 4,
};

// 4x4 Bayer matrix for ordered dithering
inline const uint8_t bayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

// Apply Bayer dithering and quantize to 4 levels (0-3)
// Stateless - works correctly with any pixel processing order
inline uint8_t applyBayerDither4Level(uint8_t gray, int x, int y) {
  int bayer = bayer4x4[y & 3][x & 3];
  int dither = (bayer - 8) * 5;  // Scale to +/-40 (half of quantization step 85)

  int adjusted = gray + dither;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;

  if (adjusted < 64) return 0;
  if (adjusted < 128) return 1;
  if (adjusted < 192) return 2;
  return 3;
}

// An 8x8 ordered dither keeps the full 8-bit sample while reducing it to the
// panel's four native levels. The 64 thresholds provide a finer spatial
// approximation of the 256 input levels than the historical 4x4 matrix,
// without allocating error buffers or depending on raster callback order.
inline uint8_t applyHighQualityDither4Level(const uint8_t gray, const int x, const int y) {
  static constexpr uint8_t bayer8x8[8][8] = {
      {0, 32, 8, 40, 2, 34, 10, 42},   {48, 16, 56, 24, 50, 18, 58, 26},
      {12, 44, 4, 36, 14, 46, 6, 38},  {60, 28, 52, 20, 62, 30, 54, 22},
      {3, 35, 11, 43, 1, 33, 9, 41},   {51, 19, 59, 27, 49, 17, 57, 25},
      {15, 47, 7, 39, 13, 45, 5, 37},  {63, 31, 55, 23, 61, 29, 53, 21},
  };
  const int base = gray / 64;
  const int remainder = gray & 63;
  const int level = base + (remainder > bayer8x8[y & 7][x & 7] ? 1 : 0);
  return static_cast<uint8_t>(level > 3 ? 3 : level);
}

// A deterministic hash-based threshold avoids a visible repeating Bayer grid
// without storing a noise bitmap or depending on decoder traversal order.
inline uint8_t applyRandomDither4Level(const uint8_t gray, const int x, const int y) {
  uint32_t h = static_cast<uint32_t>(x) * 0x45d9f3bu ^ static_cast<uint32_t>(y) * 0x119de1f3u;
  h ^= h >> 16;
  h *= 0x7feb352du;
  h ^= h >> 15;
  const int base = gray / 64;
  const int remainder = gray & 63;
  const int level = base + (remainder > static_cast<int>(h & 63u) ? 1 : 0);
  return static_cast<uint8_t>(level > 3 ? 3 : level);
}

// Serpentine Floyd-Steinberg diffusion. The two error rows are allocated only
// when this mode is selected; for an 800px Read Pico image they use ~3.2 KiB.
class ErrorDiffusionDither4Level {
 public:
  explicit ErrorDiffusionDither4Level(const int width) : width_(width), rowSize_(width > 0 ? width + 2 : 0) {
    if (rowSize_ == 0 || rowSize_ > SIZE_MAX / (2 * sizeof(int16_t))) return;
    errors_ = makeUniqueNoThrow<int16_t[]>(rowSize_ * 2);
    if (errors_) {
      current_ = errors_.get();
      next_ = current_ + rowSize_;
      reset();
    }
  }

  bool valid() const { return width_ > 0 && errors_ != nullptr; }

  void reset() {
    if (!valid()) return;
    memset(current_, 0, rowSize_ * sizeof(int16_t));
    memset(next_, 0, rowSize_ * sizeof(int16_t));
    row_ = -1;
  }

  uint8_t process(const int x, const int y, const uint8_t gray) {
    if (!valid() || x < 0 || x >= width_) return static_cast<uint8_t>(gray / 85);
    beginRow(y);
    const bool reverse = (y & 1) != 0;
    const int index = x + 1;
    int adjusted = static_cast<int>(gray) + current_[index] / 16;
    adjusted = std::clamp(adjusted, 0, 255);
    const int level = std::clamp((adjusted + 42) / 85, 0, 3);
    const int error = adjusted - level * 85;
    if (!reverse) {
      current_[index + 1] += static_cast<int16_t>(error * 7);
      next_[index - 1] += static_cast<int16_t>(error * 3);
      next_[index] += static_cast<int16_t>(error * 5);
      next_[index + 1] += static_cast<int16_t>(error);
    } else {
      current_[index - 1] += static_cast<int16_t>(error * 7);
      next_[index + 1] += static_cast<int16_t>(error * 3);
      next_[index] += static_cast<int16_t>(error * 5);
      next_[index - 1] += static_cast<int16_t>(error);
    }
    current_[index] = 0;
    return static_cast<uint8_t>(level);
  }

 private:
  void beginRow(const int y) {
    if (row_ == y) return;
    if (row_ + 1 != y) {
      reset();
      row_ = y;
      return;
    }
    std::swap(current_, next_);
    memset(next_, 0, rowSize_ * sizeof(int16_t));
    row_ = y;
  }

  int width_ = 0;
  size_t rowSize_ = 0;
  int row_ = -1;
  std::unique_ptr<int16_t[]> errors_;
  int16_t* current_ = nullptr;
  int16_t* next_ = nullptr;
};

inline uint8_t applyDither4Level(const uint8_t gray, const int x, const int y, const ImageDitherMode mode) {
  switch (mode) {
    case ImageDitherMode::Bayer8x8:
      return applyHighQualityDither4Level(gray, x, y);
    case ImageDitherMode::Bayer4x4:
      return applyBayerDither4Level(gray, x, y);
    case ImageDitherMode::Random:
      return applyRandomDither4Level(gray, x, y);
    case ImageDitherMode::None:
    case ImageDitherMode::ErrorDiffusion:
    default:
      return static_cast<uint8_t>(gray / 85);
  }
}
