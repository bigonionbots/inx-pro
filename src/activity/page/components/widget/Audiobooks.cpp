/**
 * @file Audiobooks.cpp
 * @brief Home widget: a carousel of recently played audiobooks.
 */

#include "Audiobooks.h"

#include <BitmapRender.h>
#include <GfxRenderer.h>
#include <ImageRender.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cmath>

#include "state/SystemSetting.h"
#include "system/Fonts.h"
#include "util/SdIoMutex.h"

#if INX_HAS_AUDIO_MODULE
#include "system/AudioPlayer.h"
#endif

namespace {
constexpr int kHorizontalPadding = 20;
constexpr int kTopPadding = 20;
constexpr int kBottomPadding = 24;  // room for the progress bar under the covers
constexpr int kCardGap = 20;
constexpr int kBarGap = 8;
constexpr int kBarHeight = 5;
constexpr int kMaxVisible = 4;
constexpr const char* kLabel = "Audiobooks";
constexpr const char* kEmpty = "No audiobooks played yet";

void setImageOptions(ImageRender::Options& options) {
  options.cropToFill = true;
  options.useDisplayCache = true;
  options.asyncDisplayCache = true;
  if (SETTINGS.bitmapRoundedCorners == 0) {
    options.roundedOutside = BitmapRender::RoundedOutside::None;
  } else if (SETTINGS.bitmapRoundedCorners == 2) {
    options.roundedOutside = BitmapRender::RoundedOutside::SubtleSparseInkAlignedOutside;
  } else {
    options.roundedOutside = BitmapRender::RoundedOutside::SparseInkAlignedOutside;
  }
}

int wrap(const int index, const int count) { return count > 0 ? ((index % count) + count) % count : 0; }

struct Centered {
  int centerX, centerY, centerSize;
  int sideY, sideWidth, sideHeight;
  int gap;
};

/** One large square cover in the middle, the neighbours cut to slivers at either side. */
Centered centeredBounds(const int areaX, const int areaY, const int areaW, const int areaH) {
  const int gap = areaW >= 360 ? 12 : 6;
  const int size = std::max(40, std::min({areaW * 3 / 5, areaH - kTopPadding - kBottomPadding, 260}));
  const int centerX = areaX + (areaW - size) / 2;
  const int centerY = areaY + kTopPadding + (areaH - kTopPadding - kBottomPadding - size) / 2;
  const int sideWidth = std::max(0, (areaW - size) / 2 - gap);
  const int sideHeight = size * 9 / 10;
  return {centerX, centerY, size, centerY + (size - sideHeight) / 2, sideWidth, sideHeight, gap};
}
}  // namespace

void Audiobooks::load() const {
  items_.clear();
#if INX_HAS_AUDIO_MODULE
  for (const AudioPlayer::RecentAudiobook& r : AudioPlayer::recentAudiobooks()) {
    Item item;
    item.title = r.title;
    item.coverPath = r.coverPath;
    if (r.durationMs > 0) {
      item.percent = static_cast<int>(std::min<uint64_t>(100, 100ull * r.positionMs / r.durationMs));
    }
    items_.push_back(item);
  }
#endif
  loaded_ = true;
}

int Audiobooks::count() const {
  if (!loaded_) load();
  return static_cast<int>(items_.size());
}

bool Audiobooks::hasCover(const Item& item) const {
  if (item.coverPath.empty()) return false;
  SdIoMutex::Lock lock;
  return SdMan.exists(item.coverPath.c_str());
}

Audiobooks::CardBounds Audiobooks::cardBounds(const Item& item, const int cardX, const int y, const int width,
                                              const int height) const {
  // Audiobook covers are square; a portrait one keeps its shape (cropped to the card otherwise).
  const int contentHeight = std::max(24, height - kTopPadding - kBottomPadding);
  int sourceWidth = 1;
  int sourceHeight = 1;
  if (hasCover(item)) {
    int w = 0;
    int h = 0;
    if (ImageRender::getDimensions(item.coverPath, &w, &h) && w > 0 && h > 0) {
      sourceWidth = w;
      sourceHeight = h;
    }
  }
  const int maxCardWidth = std::max(24, (width - kHorizontalPadding - kCardGap * 2) * 9 / 20);
  const int cardWidth = std::min(
      maxCardWidth,
      std::max(24, static_cast<int>(std::lround(static_cast<float>(contentHeight) * sourceWidth / sourceHeight))));
  const int cardHeight =
      std::max(24, std::min(contentHeight,
                            static_cast<int>(std::lround(static_cast<float>(cardWidth) * sourceHeight / sourceWidth))));
  return {cardX, y + height - kBottomPadding - cardHeight, cardWidth, cardHeight};
}

void Audiobooks::renderCover(const Item& item, const CardBounds& bounds,
                             const HomeTheme::CarouselShadowStyle shadowStyle, const bool showProgress) const {
  renderShadow(renderer_, bounds.x + 6, bounds.y + 6, bounds.width, bounds.height, shadowStyle);
  renderer_.rectangle.fill(bounds.x, bounds.y, bounds.width, bounds.height, false);
  bool drawn = false;
  if (hasCover(item)) {
    ImageRender::Options options;
    setImageOptions(options);
    drawn =
        ImageRender::create(renderer_, item.coverPath).render(bounds.x, bounds.y, bounds.width, bounds.height, options);
  }
  renderer_.rectangle.render(bounds.x, bounds.y, bounds.width, bounds.height, true, SETTINGS.bitmapRoundedCorners != 0,
                             SETTINGS.bitmapRoundedCorners == 2);
  if (!drawn) {
    // No cover: the title on a blank card.
    const int font = MONTSERRAT_10_FONT_ID;
    const std::string title = renderer_.text.truncate(font, item.title.c_str(), std::max(1, bounds.width - 8));
    const int textWidth = renderer_.text.getWidth(font, title.c_str());
    renderer_.text.render(font, bounds.x + std::max(4, (bounds.width - textWidth) / 2),
                          bounds.y + std::max(4, (bounds.height - renderer_.text.getLineHeight(font)) / 2),
                          title.c_str(), true);
  }
  if (!showProgress || item.percent < 0) return;
  // How far into the book, as a bar under the cover.
  const int barY = bounds.y + bounds.height + kBarGap;
  renderer_.rectangle.render(bounds.x, barY, bounds.width, kBarHeight, true);
  renderer_.rectangle.fill(bounds.x + 1, barY + 1, std::max(1, bounds.width - 2), kBarHeight - 2, false);
  if (item.percent > 0) {
    renderer_.rectangle.fill(bounds.x + 1, barY + 1, std::max(1, (bounds.width - 2) * item.percent / 100),
                             kBarHeight - 2, true);
  }
}

void Audiobooks::render(const int index, const int x, const int y, const int width, const int height,
                        const bool background, const HomeTheme::CarouselStyle style, const bool showLabel,
                        const HomeTheme::CarouselLabelColor labelColor,
                        const HomeTheme::CarouselShadowStyle shadowStyle, const bool showProgress) const {
  if (width <= 0 || height <= 0) return;
  load();
  renderBackground(x, y, width, height, background);
  const ContentArea content = contentArea(y, height, showLabel);
  if (showLabel) renderLabel(x, y, kLabel, labelColor);
  if (items_.empty()) {
    renderer_.text.centered(systemFontId(), content.y + content.height / 2, kEmpty);
    return;
  }
  const int count = static_cast<int>(items_.size());
  const int current = wrap(index, count);

  if (style == HomeTheme::CarouselStyle::Centered) {
    const Centered layout = centeredBounds(x, content.y, width, content.height);
    if (count > 1 && layout.sideWidth > 0) {
      renderCover(items_[static_cast<size_t>(wrap(current - 1, count))],
                  {layout.centerX - layout.gap - layout.sideWidth, layout.sideY, layout.sideWidth, layout.sideHeight},
                  shadowStyle, false);
      renderCover(items_[static_cast<size_t>(wrap(current + 1, count))],
                  {layout.centerX + layout.centerSize + layout.gap, layout.sideY, layout.sideWidth, layout.sideHeight},
                  shadowStyle, false);
    }
    renderCover(items_[static_cast<size_t>(current)],
                {layout.centerX, layout.centerY, layout.centerSize, layout.centerSize}, shadowStyle, showProgress);
    return;
  }

  int cardX = x + kHorizontalPadding;
  for (int offset = 0; offset < std::min(kMaxVisible, count); ++offset) {
    const Item& item = items_[static_cast<size_t>(wrap(current + offset, count))];
    CardBounds card = cardBounds(item, cardX, content.y, width, content.height);
    if (card.x >= x + width) break;
    const int fullWidth = card.width;
    card.width = std::min(card.width, x + width - card.x);  // the last one cut at the edge
    renderCover(item, card, shadowStyle, showProgress && card.width == fullWidth);
    cardX += fullWidth + kCardGap;
  }
}

void Audiobooks::preview(const int x, const int y, const int width, const int height, const bool background,
                         const HomeTheme::CarouselStyle style, const bool showLabel,
                         const HomeTheme::CarouselLabelColor labelColor,
                         const HomeTheme::CarouselShadowStyle shadowStyle) const {
  if (width <= 0 || height <= 0) return;
  renderBackground(x, y, width, height, background);
  const ContentArea content = contentArea(y, height, showLabel);
  if (showLabel) renderLabel(x, y, kLabel, labelColor);
  const Item sample{"Audiobook", "", 40};
  if (style == HomeTheme::CarouselStyle::Centered) {
    const Centered layout = centeredBounds(x, content.y, width, content.height);
    if (layout.sideWidth > 0) {
      renderCover(sample,
                  {layout.centerX - layout.gap - layout.sideWidth, layout.sideY, layout.sideWidth, layout.sideHeight},
                  shadowStyle, false);
      renderCover(sample,
                  {layout.centerX + layout.centerSize + layout.gap, layout.sideY, layout.sideWidth, layout.sideHeight},
                  shadowStyle, false);
    }
    renderCover(sample, {layout.centerX, layout.centerY, layout.centerSize, layout.centerSize}, shadowStyle, true);
    return;
  }
  int cardX = x + kHorizontalPadding;
  for (int offset = 0; offset < 3; ++offset) {
    CardBounds card = cardBounds(sample, cardX, content.y, width, content.height);
    if (card.x >= x + width) break;
    const int fullWidth = card.width;
    card.width = std::min(card.width, x + width - card.x);
    renderCover(sample, card, shadowStyle, card.width == fullWidth);
    cardX += fullWidth + kCardGap;
  }
}

int Audiobooks::hitTest(const int index, const int x, const int y, const int areaX, const int areaY, const int areaW,
                        const int areaH, const HomeTheme::CarouselStyle style, const bool showLabel) const {
  if (!loaded_) load();
  if (items_.empty() || x < areaX || x >= areaX + areaW || y < areaY || y >= areaY + areaH) return -1;
  const ContentArea content = contentArea(areaY, areaH, showLabel);
  const int count = static_cast<int>(items_.size());
  const int current = wrap(index, count);
  const auto inside = [x, y](const CardBounds& b) {
    return x >= b.x && x < b.x + b.width && y >= b.y && y < b.y + b.height + kBarGap + kBarHeight;
  };

  if (style == HomeTheme::CarouselStyle::Centered) {
    const Centered layout = centeredBounds(areaX, content.y, areaW, content.height);
    if (inside({layout.centerX, layout.centerY, layout.centerSize, layout.centerSize})) return current;
    if (count < 2) return -1;
    if (inside({layout.centerX - layout.gap - layout.sideWidth, layout.sideY, layout.sideWidth, layout.sideHeight})) {
      return wrap(current - 1, count);
    }
    if (inside({layout.centerX + layout.centerSize + layout.gap, layout.sideY, layout.sideWidth, layout.sideHeight})) {
      return wrap(current + 1, count);
    }
    return -1;
  }

  int cardX = areaX + kHorizontalPadding;
  for (int offset = 0; offset < std::min(kMaxVisible, count); ++offset) {
    const int bookIndex = wrap(current + offset, count);
    const CardBounds card = cardBounds(items_[static_cast<size_t>(bookIndex)], cardX, content.y, areaW, content.height);
    if (inside(card)) return bookIndex;
    cardX += card.width + kCardGap;
  }
  return -1;
}
