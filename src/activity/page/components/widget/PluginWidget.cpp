#include "PluginWidget.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>

#include <algorithm>
#include <cctype>
#include <ctime>

#include "system/Fonts.h"

extern HalGPIO gpio;

namespace {

constexpr int kMargin = 16;
constexpr int kFooterGap = 8;
constexpr int kFooterFont = MONTSERRAT_10_FONT_ID;
// Largest first: the body uses the biggest size whose wrapped text fits the slot.
constexpr int kBodyFonts[] = {MONTSERRAT_18_FONT_ID, MONTSERRAT_16_FONT_ID, MONTSERRAT_14_FONT_ID,
                              MONTSERRAT_12_FONT_ID, MONTSERRAT_10_FONT_ID};

struct Moment {
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int weekday = 0;
};

bool readMoment(Moment& moment) {
#ifdef SIMULATOR
  const std::time_t now = std::time(nullptr);
  const std::tm* local = std::localtime(&now);
  if (!local) return false;
  moment = {local->tm_year + 1900, local->tm_mon + 1, local->tm_mday,
            local->tm_hour,        local->tm_min,     local->tm_wday == 0 ? 7 : local->tm_wday};
  return true;
#else
  HalGPIO::DateTime dateTime;
  if (!gpio.readDateTime(dateTime)) return false;
  moment = {dateTime.year, dateTime.month, dateTime.day, dateTime.hour, dateTime.minute, dateTime.weekday};
  return true;
#endif
}

/** A whitespace-free fragment of one style. Fragments without a space between them form one unbreakable word. */
struct Piece {
  std::string text;
  EpdFontFamily::Style style;
  bool spaceBefore;
  bool breakBefore;
};

std::vector<Piece> splitPieces(const std::vector<PluginWidget::Run>& runs) {
  std::vector<Piece> pieces;
  bool space = false;
  bool lineBreak = false;
  for (const PluginWidget::Run& run : runs) {
    const std::string& text = run.text;
    size_t index = 0;
    while (index < text.size()) {
      const unsigned char c = static_cast<unsigned char>(text[index]);
      if (c == '\n') {
        lineBreak = true;
        space = false;
        ++index;
        continue;
      }
      if (std::isspace(c)) {
        space = true;
        ++index;
        continue;
      }
      size_t end = index;
      while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) ++end;
      pieces.push_back({text.substr(index, end - index), run.style, space, lineBreak});
      space = false;
      lineBreak = false;
      index = end;
    }
  }
  return pieces;
}

void appendRun(PluginWidget::Line& line, const std::string& text, const EpdFontFamily::Style style) {
  if (!line.runs.empty() && line.runs.back().style == style) {
    line.runs.back().text += text;
  } else {
    line.runs.push_back({text, style});
  }
}

std::vector<PluginWidget::Line> wrap(const GfxRenderer& renderer, const int font, const std::vector<Piece>& pieces,
                                     const int width) {
  std::vector<PluginWidget::Line> lines;
  PluginWidget::Line line;
  int lineWidth = 0;
  const int spaceWidth = renderer.text.getSpaceWidth(font);

  size_t index = 0;
  while (index < pieces.size()) {
    size_t end = index + 1;
    while (end < pieces.size() && !pieces[end].spaceBefore && !pieces[end].breakBefore) ++end;
    int wordWidth = 0;
    for (size_t i = index; i < end; ++i) {
      wordWidth += renderer.text.getUntranslatedWidth(font, pieces[i].text.c_str(), pieces[i].style);
    }

    if (!line.runs.empty() && (pieces[index].breakBefore || lineWidth + spaceWidth + wordWidth > width)) {
      lines.push_back(std::move(line));
      line = PluginWidget::Line();
      lineWidth = 0;
    }
    if (!line.runs.empty()) {
      appendRun(line, " ", pieces[index].style);
      lineWidth += spaceWidth;
    }
    for (size_t i = index; i < end; ++i) appendRun(line, pieces[i].text, pieces[i].style);
    lineWidth += wordWidth;
    index = end;
  }
  if (!line.runs.empty()) lines.push_back(std::move(line));
  return lines;
}

/** Drops trailing UTF-8 characters until the text fits. Plugin text is never passed through UI translation. */
std::string fitWidth(const GfxRenderer& renderer, const int font, std::string text, const int maxWidth,
                     const EpdFontFamily::Style style) {
  while (!text.empty() && renderer.text.getUntranslatedWidth(font, text.c_str(), style) > maxWidth) {
    text.pop_back();
    while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back();
  }
  return text;
}

}  // namespace

bool PluginWidget::resolve() const {
  if (!resolved_) {
    resolved_ = true;
    available_ = PluginManager::findHomeWidgetPlugin(link_);
    INX_SERIAL.printf("[%lu] [PLUGIN] home-widget found=%d plugin=%s\n", millis(), available_ ? 1 : 0,
                      link_.id.c_str());
  }
  return available_;
}

const char* PluginWidget::label() const { return resolve() ? link_.label.c_str() : "Plugin Widget"; }

bool PluginWidget::refreshKey(uint64_t& key) const {
  Moment moment;
  if (!readMoment(moment)) return false;
  const int bucket = link_.refreshMinutes > 0 ? (moment.hour * 60 + moment.minute) / link_.refreshMinutes : 0;
  key = static_cast<uint64_t>(moment.year) * 100000000ULL + static_cast<uint64_t>(moment.month) * 1000000ULL +
        static_cast<uint64_t>(moment.day) * 10000ULL + static_cast<uint64_t>(bucket);
  return true;
}

bool PluginWidget::needsRefresh() const {
  if (!resolve() || link_.refreshMinutes <= 0) return false;
  uint64_t key = 0;
  return refreshKey(key) && key != loadedKey_;
}

void PluginWidget::load(const int width, const int height) const {
  loaded_ = true;
  loadedWidth_ = width;
  loadedHeight_ = height;
  before_.clear();
  highlight_.clear();
  after_.clear();
  footer_.clear();
  error_.clear();

  JsonDocument arguments;
  Moment moment;
  if (readMoment(moment)) {
    arguments["year"] = moment.year;
    arguments["month"] = moment.month;
    arguments["day"] = moment.day;
    arguments["hour"] = moment.hour;
    arguments["minute"] = moment.minute;
    arguments["weekday"] = moment.weekday;
  }
  arguments["width"] = width;
  arguments["height"] = height;
  std::string argumentJson;
  serializeJson(arguments, argumentJson);

  std::string output;
  if (!PluginManager::invokeJson(link_.id.c_str(), link_.function.c_str(), argumentJson, output, error_)) {
    INX_SERIAL.printf("[%lu] [PLUGIN] home-widget plugin=%s error=%s\n", millis(), link_.id.c_str(), error_.c_str());
    return;
  }
  JsonDocument result;
  if (deserializeJson(result, output) != DeserializationError::Ok || !result.is<JsonObject>()) {
    error_ = "Widget result is not an object";
    INX_SERIAL.printf("[%lu] [PLUGIN] home-widget plugin=%s error=%s\n", millis(), link_.id.c_str(), error_.c_str());
    return;
  }
  before_ = result["before"] | "";
  highlight_ = result["highlight"] | "";
  after_ = result["after"] | "";
  footer_ = result["footer"] | "";
}

void PluginWidget::renderMessage(const int x, const int y, const int width, const int height,
                                 const char* message) const {
  const int font = MONTSERRAT_12_FONT_ID;
  const std::string shown = fitWidth(renderer_, font, message, width - 2 * kMargin, EpdFontFamily::REGULAR);
  const int textWidth = renderer_.text.getUntranslatedWidth(font, shown.c_str());
  renderer_.text.renderUntranslated(font, x + (width - textWidth) / 2,
                                    y + (height - renderer_.text.getLineHeight(font)) / 2, shown.c_str());
}

void PluginWidget::render(const int x, const int y, const int width, const int height) const {
  if (width <= 2 * kMargin || height <= 2 * kMargin) return;
  if (!resolve()) {
    renderMessage(x, y, width, height, "No plugin widget installed");
    return;
  }

  uint64_t key = 0;
  refreshKey(key);
  if (!loaded_ || key != loadedKey_ || width != loadedWidth_ || height != loadedHeight_) {
    loadedKey_ = key;
    load(width, height);
  }
  if (!error_.empty()) {
    renderMessage(x, y, width, height, (link_.label + ": plugin error").c_str());
    return;
  }

  const int contentX = x + kMargin;
  const int contentWidth = width - 2 * kMargin;
  int bodyBottom = y + height - kMargin;
  if (!footer_.empty()) {
    const std::string footer = fitWidth(renderer_, kFooterFont, footer_, contentWidth, EpdFontFamily::ITALIC);
    const int footerY = bodyBottom - renderer_.text.getLineHeight(kFooterFont);
    const int footerWidth = renderer_.text.getUntranslatedWidth(kFooterFont, footer.c_str(), EpdFontFamily::ITALIC);
    renderer_.text.renderUntranslated(kFooterFont, contentX + contentWidth - footerWidth, footerY, footer.c_str(), true,
                                      EpdFontFamily::ITALIC);
    bodyBottom = footerY - kFooterGap;
  }

  const std::vector<Piece> pieces = splitPieces(
      {{before_, EpdFontFamily::REGULAR}, {highlight_, EpdFontFamily::BOLD}, {after_, EpdFontFamily::REGULAR}});
  if (pieces.empty()) return;

  const int bodyTop = y + kMargin;
  const int bodyHeight = bodyBottom - bodyTop;
  int font = kBodyFonts[0];
  std::vector<Line> lines;
  for (const int candidate : kBodyFonts) {
    font = candidate;
    lines = wrap(renderer_, font, pieces, contentWidth);
    if (static_cast<int>(lines.size()) * renderer_.text.getLineHeight(font) <= bodyHeight) break;
  }

  const int lineHeight = renderer_.text.getLineHeight(font);
  const int lineCount = std::min(static_cast<int>(lines.size()), std::max(0, bodyHeight / std::max(1, lineHeight)));
  int lineY = bodyTop + std::max(0, (bodyHeight - lineCount * lineHeight) / 2);
  for (int index = 0; index < lineCount; ++index) {
    int textX = contentX;
    for (const Run& run : lines[static_cast<size_t>(index)].runs) {
      const int available = contentX + contentWidth - textX;
      if (available <= 0) break;
      const std::string shown = fitWidth(renderer_, font, run.text, available, run.style);
      renderer_.text.renderUntranslated(font, textX, lineY, shown.c_str(), true, run.style);
      textX += renderer_.text.getUntranslatedWidth(font, shown.c_str(), run.style);
    }
    lineY += lineHeight;
  }
}
