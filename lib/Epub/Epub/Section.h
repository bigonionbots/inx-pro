#pragma once

/**
 * @file Section.h
 * @brief Public interface and types for Section.
 */

#include <ImageRenderMode.h>

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "Page.h"

class GfxRenderer;
class ChapterHtmlSlimParser;

/**
 * Represents a section (chapter) of an EPUB document.
 * Handles loading, creating, and caching section files that contain formatted pages.
 */
class Section {
 public:
 private:
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  FsFile file;
  uint32_t lutOffset = 0;
  std::vector<uint32_t> pageOffsets;

  struct CachedPage {
    int pageIndex = -1;
    uint32_t usedAt = 0;
    std::unique_ptr<Page> page;
  };
  std::array<CachedPage, 4> pageCache{};
  uint32_t pageCacheClock = 0;
  void clearPageCache();
  Page* loadPage(int pageIndex);

  /**
   * Writes the header information to the section file.
   *
   * @param fontId Font identifier for text rendering
   * @param lineCompression Line spacing factor
   * @param extraParagraphSpacing Whether to add extra spacing between paragraphs
   * @param paragraphAlignment Default paragraph alignment
   * @param viewportWidth Available width for layout
   * @param viewportHeight Available height for layout
   * @param hyphenationEnabled Whether hyphenation is enabled
   */
  void writeSectionFileHeader(int fontId, float lineCompression, float wordSpacing, bool extraParagraphSpacing,
                              uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                              bool hyphenationEnabled, bool respectCssParagraphIndent, bool bionicReadingEnabled);

  /**
   * Handles completion of a page during section creation.
   *
   * @param page Unique pointer to the completed page
   * @return The file position where the page was written
   */
  uint32_t onPageComplete(std::unique_ptr<Page> page, const std::function<void(Page&, uint16_t)>& pageBuiltFn);

 public:

  uint16_t pageCount = 0;
  int currentPage = 0;

  /**
   * Constructs a new Section.
   *
   * @param epub Shared pointer to the EPUB document
   * @param spineIndex Index of this section in the EPUB spine
   * @param renderer Reference to the graphics renderer
   */
  explicit Section(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer);
  explicit Section(const std::string& cachePath, int spineIndex, GfxRenderer& renderer);

  ~Section();

  /**
   * Loads and verifies a section file from disk.
   *
   * @param fontId Font identifier to verify against
   * @param lineCompression Line spacing factor to verify against
   * @param extraParagraphSpacing Paragraph spacing setting to verify against
   * @param paragraphAlignment Paragraph alignment to verify against
   * @param viewportWidth Viewport width to verify against
   * @param viewportHeight Viewport height to verify against
   * @param hyphenationEnabled Hyphenation setting to verify against
   * @return true if section file exists and settings match
   */
  bool loadSectionFile(int fontId, float lineCompression, float wordSpacing, bool extraParagraphSpacing,
                       uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                       bool hyphenationEnabled, bool respectCssParagraphIndent, bool bionicReadingEnabled);
  bool loadSectionFileForPreview(int* outFontId = nullptr);

  /** FNV-1a of an element id: how read-aloud anchors are matched to media-overlay clips. */
  static uint32_t anchorHash(const char* id, size_t length) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; ++i) {
      hash ^= static_cast<uint8_t>(id[i]);
      hash *= 16777619u;
    }
    return hash;
  }
  /**
   * For books with media overlays: the page each element id of a section starts on, as
   * (anchorHash(id), page) in reading order. Written alongside the section file when it is built.
   * False if the section was built without them.
   */
  static bool loadAnchors(const std::string& cachePath, int spineIndex,
                          std::vector<std::pair<uint32_t, uint16_t>>& out);
  static std::unique_ptr<Page> loadCachedPage(const std::string& cachePath, int spineIndex, int pageNumber);

  /**
   * Removes the section file from the filesystem.
   *
   * @return true if file was successfully removed or didn't exist
   */
  bool clearCache() const;

  /**
   * Creates a new section file by parsing the HTML content and building pages.
   * Can optionally skip image processing to only rebuild text layout.
   *
   * @param fontId Font identifier for text rendering
   * @param headerFontId Font identifier for header rendering
   * @param maxFontId Font identifier for header rendering
   * @param lineCompression Line spacing factor
   * @param extraParagraphSpacing Whether to add extra spacing between paragraphs
   * @param paragraphAlignment Default paragraph alignment
   * @param viewportWidth Available width for layout
   * @param viewportHeight Available height for layout
   * @param hyphenationEnabled Whether hyphenation is enabled
   * @param popupFn Optional callback for progress popups during image conversion
   * @param skipImages If true, skip processing new images and only use existing cached images
   * @return true if section file was successfully created
   */
  bool createSectionFile(int fontId, int headerFontId, int maxFontId, float lineCompression, float wordSpacing,
                         bool extraParagraphSpacing, uint8_t paragraphAlignment, uint16_t viewportWidth,
                         uint16_t viewportHeight, bool hyphenationEnabled, bool respectCssParagraphIndent,
                         bool bionicReadingEnabled, const std::function<void()>& popupFn = nullptr,
                         bool skipImages = false, const std::function<void(Page&, uint16_t)>& pageBuiltFn = nullptr,
                         bool warmImageDisplayCache = false,
                         ImageRenderMode warmImageRenderMode = ImageRenderMode::OneBit, bool warmImageQuality = false,
                         int warmImageYOffset = 0);

  /**
   * Loads a specific page from the section file.
   *
   * @return Unique pointer to the loaded page, or nullptr on failure
   */
  /** Returns a section-owned page; valid until this Section is destroyed. */
  Page* loadPageFromSectionFile();
  /** Deserializes a page into the bounded PSRAM cache without changing currentPage. */
  bool preloadPage(int pageIndex);
};
