#pragma once

/**
 * @file Audiobooks.h
 * @brief Home widget: a carousel of recently played audiobooks (t5s3_audio only; empty elsewhere).
 */

#include <string>
#include <vector>

#include "BaseCarousel.h"

/**
 * Covers of the recently played audiobooks (AudioPlayer::recentAudiobooks()), swiped through like the
 * Favorites carousel: Left (a row from the current one) or Centered (one large, its neighbours beside
 * it). The list is read from the card on each render, so it follows what was played last.
 */
class Audiobooks final : public BaseCarousel {
 public:
  explicit Audiobooks(GfxRenderer& renderer) : BaseCarousel(renderer) {}

  void render(int index, int x, int y, int width, int height, bool background = false,
              HomeTheme::CarouselStyle style = HomeTheme::CarouselStyle::Left, bool showLabel = true,
              HomeTheme::CarouselLabelColor labelColor = HomeTheme::CarouselLabelColor::Black,
              HomeTheme::CarouselShadowStyle shadowStyle = HomeTheme::CarouselShadowStyle::None,
              bool showProgress = true) const;
  void preview(int x, int y, int width, int height, bool background = false,
               HomeTheme::CarouselStyle style = HomeTheme::CarouselStyle::Left, bool showLabel = true,
               HomeTheme::CarouselLabelColor labelColor = HomeTheme::CarouselLabelColor::Black,
               HomeTheme::CarouselShadowStyle shadowStyle = HomeTheme::CarouselShadowStyle::None) const;
  /** The book at (x, y): its index in the recent list, or -1. Uses the list from the last render. */
  int hitTest(int index, int x, int y, int areaX, int areaY, int areaW, int areaH,
              HomeTheme::CarouselStyle style = HomeTheme::CarouselStyle::Left, bool showLabel = true) const;
  /** Books in the list from the last render (or a fresh read if there was none). */
  int count() const;

 private:
  struct Item {
    std::string title;
    std::string coverPath;  ///< "" or an image that may not exist (drawn as a titled card)
    int percent = -1;       ///< -1 if the length is unknown
  };
  struct CardBounds {
    int x;
    int y;
    int width;
    int height;
  };

  void load() const;
  bool hasCover(const Item& item) const;
  CardBounds cardBounds(const Item& item, int cardX, int y, int width, int height) const;
  void renderCover(const Item& item, const CardBounds& bounds, HomeTheme::CarouselShadowStyle shadowStyle,
                   bool showProgress) const;

  mutable bool loaded_ = false;
  mutable std::vector<Item> items_;
};
