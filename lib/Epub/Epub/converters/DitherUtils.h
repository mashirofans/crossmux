#pragma once

#include <stdint.h>

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
