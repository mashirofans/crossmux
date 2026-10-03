#include "BmpToFramebufferConverter.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdint>
#include <memory>

#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "PixelCache.h"

namespace {

bool openBitmap(const std::string& imagePath, HalFile& file, Bitmap& bitmap) {
  if (!Storage.openFileForRead("BMP", imagePath, file)) {
    LOG_ERR("BMP", "Failed to open image: %s", imagePath.c_str());
    return false;
  }
  const auto error = bitmap.parseHeaders();
  if (error != BmpReaderError::Ok) {
    LOG_ERR("BMP", "Invalid BMP %s: %s", imagePath.c_str(), Bitmap::errorToString(error));
    return false;
  }
  return true;
}

int scaledDimension(const int source, const int limit) {
  if (source <= 0) return 0;
  if (limit <= 0 || source <= limit) return source;
  return limit;
}

uint8_t quantizeSample(const uint8_t gray, const int x, const int y, const RenderConfig& config,
                       ErrorDiffusionDither4Level* errorDither) {
  if (!config.useDithering) return static_cast<uint8_t>(std::min(3, gray / 85));
  if (errorDither && config.ditherMode == ImageDitherMode::ErrorDiffusion) {
    return errorDither->process(x, y, gray);
  }
  return applyDither4Level(gray, x, y, config.ditherMode);
}

}  // namespace

bool BmpToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  HalFile file;
  if (!Storage.openFileForRead("BMP", imagePath, file)) return false;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return false;
  return validateAndStoreDimensions(bitmap.getWidth(), bitmap.getHeight(), out, "BMP");
}

bool BmpToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                     const RenderConfig& config) {
  if (config.error) *config.error = ImageRenderError::Failed;
  if (config.output == DecodeOutput::NativeGrayscale16) {
    LOG_ERR("BMP", "Native grayscale output is not supported for BMP images");
    return false;
  }

  HalFile file;
  Bitmap bitmap(file, ImageDitherMode::None);
  if (!openBitmap(imagePath, file, bitmap)) return false;

  const int sourceWidth = bitmap.getWidth();
  const int sourceHeight = bitmap.getHeight();
  const int destWidth = config.useExactDimensions && config.maxWidth > 0
                            ? config.maxWidth
                            : scaledDimension(sourceWidth, config.maxWidth);
  const int destHeight = config.useExactDimensions && config.maxHeight > 0
                             ? config.maxHeight
                             : scaledDimension(sourceHeight, config.maxHeight);
  if (destWidth <= 0 || destHeight <= 0) return false;

  // PixelCache writes rows in raster order. A bottom-up BMP arrives in reverse
  // order, so leave caching disabled for that uncommon path rather than
  // retaining a full source image in RAM just to reverse its rows.
  const bool canCache = bitmap.isTopDown() && !config.cachePath.empty();
  const bool cacheOnly = config.output == DecodeOutput::CacheOnly;
  if (cacheOnly && !canCache) {
    LOG_ERR("BMP", "Cache-only decode requires a top-down BMP: %s", imagePath.c_str());
    return false;
  }

  PixelCache cache;
  bool caching = canCache;
  if (caching) {
    const int maxRowsPerSourceRow = std::max(1, (destHeight + sourceHeight - 1) / sourceHeight);
    if (!cache.begin(config.cachePath, destWidth, destHeight, config.x, config.y, maxRowsPerSourceRow)) {
      if (cacheOnly) return false;
      caching = false;
    }
  }

  auto grayRow = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(sourceWidth));
  auto sourceRow = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(bitmap.getRowBytes()));
  if (!grayRow || !sourceRow) {
    if (config.error) *config.error = ImageRenderError::OutOfMemory;
    if (caching) cache.abort();
    return false;
  }

  std::unique_ptr<ErrorDiffusionDither4Level> errorDither;
  if (config.useDithering && config.ditherMode == ImageDitherMode::ErrorDiffusion) {
    errorDither = makeUniqueNoThrow<ErrorDiffusionDither4Level>(destWidth);
    if (!errorDither || !errorDither->valid()) {
      if (config.error) *config.error = ImageRenderError::OutOfMemory;
      if (caching) cache.abort();
      return false;
    }
  }

  DirectPixelWriter writer;
  const bool writeFramebuffer = config.output == DecodeOutput::FrameBufferAndCache;
  if (writeFramebuffer) writer.init(renderer);

  for (int fileY = 0; fileY < sourceHeight; ++fileY) {
    if (bitmap.readNextRow(grayRow.get(), sourceRow.get(), nullptr, Bitmap::RowOutput::Gray8) !=
        BmpReaderError::Ok) {
      if (caching) cache.abort();
      return false;
    }
    const int sourceY = bitmap.isTopDown() ? fileY : sourceHeight - 1 - fileY;
    const int firstDestY = sourceY * destHeight / sourceHeight;
    const int lastDestY = (sourceY + 1) * destHeight / sourceHeight;
    for (int destY = firstDestY; destY < std::max(firstDestY + 1, lastDestY); ++destY) {
      if (destY >= destHeight) break;
      const int outY = config.y + destY;
      if (writeFramebuffer && (outY < 0 || outY >= renderer.getScreenHeight())) continue;

      DirectCacheWriter cacheWriter;
      if (caching) {
        if (!cache.advanceTo(destY)) {
          caching = false;
          cache.abort();
        } else {
          cacheWriter.init(cache.buffer, cache.bytesPerRow, cache.bandRows, cache.originX);
          cacheWriter.beginRow(outY, config.y + cache.bandStart);
        }
      }
      if (writeFramebuffer) writer.beginRow(outY);

      for (int destX = 0; destX < destWidth; ++destX) {
        const int sourceX = std::min(sourceWidth - 1, destX * sourceWidth / destWidth);
        const uint8_t value = quantizeSample(grayRow[sourceX], destX, destY, config, errorDither.get());
        if (writeFramebuffer && config.x + destX >= 0 && config.x + destX < renderer.getScreenWidth()) {
          writer.writePixel(config.x + destX, value);
        }
        if (caching) cacheWriter.writePixel(config.x + destX, value);
      }
    }
  }

  if (cacheOnly) {
    if (!caching || !cache.finalize()) return false;
  } else if (caching) {
    cache.finalize();
  }
  if (config.error) *config.error = ImageRenderError::None;
  return true;
}

bool BmpToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasBmpExtension(extension);
}
