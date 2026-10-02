#pragma once

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>

#include <utility>

// Render a complete menu/list frame in B/W plus two selector planes when the
// user enables UI anti-aliasing. The callback must redraw the same complete
// frame each time; it should not mutate navigation state while drawing.
namespace uiAa {

template <typename DrawFn>
void display(GfxRenderer& renderer, DrawFn&& draw) {
  if (!SETTINGS.uiAntiAliasing || renderer.isInverted() ||
      !renderer.grayscaleCapabilities().supported()) {
    renderer.displayBuffer();
    return;
  }

  if (!renderer.storeBwBuffer()) {
    renderer.displayBuffer();
    return;
  }

  const bool combinedBase = renderer.supportsTextOnlyCombinedBase();
  if (combinedBase) {
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH, DisplayRefreshContext::TextOnlyAntiAliasing);
  } else {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH, DisplayRefreshContext::TextOnlyAntiAliasing);
  }

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  draw();
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  draw();
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.restoreBwBuffer();
}

}  // namespace uiAa
