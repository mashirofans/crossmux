#pragma once

#include "ImageToFramebufferDecoder.h"

// Decoder for static BMP images embedded in EPUBs. GIF/SVG and other formats
// stay unsupported; this path deliberately reuses the firmware's bounded BMP
// reader so it accepts the same 1/2/4/8/24/32-bit BI_RGB files as the image
// viewer and cover renderer.
class BmpToFramebufferConverter final : public ImageToFramebufferDecoder {
 public:
  static bool getDimensionsStatic(const std::string& imagePath, ImageDimensions& out);

  bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                           const RenderConfig& config) override;

  bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const override {
    return getDimensionsStatic(imagePath, dims);
  }

  static bool supportsFormat(const std::string& extension);
  const char* getFormatName() const override { return "BMP"; }
};
