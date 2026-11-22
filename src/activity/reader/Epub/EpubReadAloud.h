#pragma once

/**
 * @file EpubReadAloud.h
 * @brief Read-aloud in the EPUB reader: plays a media-overlay book's narration (Storyteller's
 *        "readaloud" EPUBs) and keeps the page on the sentence being read.
 *
 * Toggled by the "Read Aloud" reader action (a button, double tap or the quick actions popup,
 * which always offers it for such books). Starting plays from the first sentence on the page;
 * while it plays, the reader turns to whichever page the current sentence starts on, crossing
 * chapters as the narration does. Turning pages by hand (or jumping through the contents)
 * moves the narration to the first sentence of the new page; a page with no narration
 * (front matter, images) pauses it. Leaving the book stops it.
 *
 * Where things come from: ReadAloudIndex (sentences -> audio times, from the book's SMIL files),
 * Section::loadAnchors() (sentences -> pages, recorded while the chapter is laid out) and
 * AudioPlayer (playback, reading the audio in place from the EPUB). All on the main loop.
 */

#if INX_HAS_AUDIO_MODULE

#include <Epub/PsramAllocator.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

class EpubActivity;
class ReadAloudIndex;

class EpubReadAloud {
 public:
  EpubReadAloud();
  ~EpubReadAloud();

  /** True for books with media overlays: offer the action. */
  static bool available(const EpubActivity& act);
  /** True while this book's narration is open (playing or paused). */
  bool active(const EpubActivity& act) const;

  /** Starts the narration at the current page, or pauses / resumes it. */
  void toggle(EpubActivity& act);
  /**
   * Main-loop follow-up while the reader is idle: moves the page with the narration, or the
   * narration with a page the reader turned to. True if it changed the page (render next loop).
   */
  bool tick(EpubActivity& act);
  /** Leaving the book: stops the narration. */
  void exit(EpubActivity& act);

 private:
  bool ensureIndex(EpubActivity& act);
  /** Sentence -> page for the reader's current section; false until it is loaded. */
  bool ensurePages(EpubActivity& act);
  /** The page clip `index` starts on in the loaded section, -1 if unknown. */
  int pageOfClip(int index) const;
  /** The clip to play for a page of the loaded section: the first starting on it, else the one running into it. */
  int clipForPage(int page) const;
  void play(EpubActivity& act, int clip);
  void showPopup(EpubActivity& act, const char* message);

  std::unique_ptr<ReadAloudIndex> index_;
  bool indexFailed_ = false;

  int pagesSpine_ = -1;                 ///< spine item pagesByHash_ belongs to
  const void* pagesSection_ = nullptr;  ///< the Section it was loaded for (rebuilt -> reload)
  int pagesPageCount_ = -1;             ///< and its page count (re-laid out -> reload)
  /**
   * (sentence id hash, page) sorted by hash, and (clip, page) of that spine item in reading order.
   * In PSRAM: internal RAM is what the USB headphones' transfers need.
   */
  std::vector<std::pair<uint32_t, uint16_t>, EpubPsramAllocator<std::pair<uint32_t, uint16_t>>> pagesByHash_;
  std::vector<std::pair<int, uint16_t>, EpubPsramAllocator<std::pair<int, uint16_t>>> spineClips_;
  /** The page of a sentence id hash in the loaded section, -1 if unknown. */
  int pageOfHash(uint32_t hash) const;

  /** Where the reader was when tick() last looked, to tell the reader's page turns from ours. */
  int seenSpine_ = -1;
  int seenPage_ = -1;
  /** The sentence last jumped to: counts as current until playback reaches it (see tick()). */
  int heldClip_ = -1;
  /** A chapter change we made: go to this clip's page once that section is loaded. */
  int pendingClip_ = -1;
};

#endif  // INX_HAS_AUDIO_MODULE
