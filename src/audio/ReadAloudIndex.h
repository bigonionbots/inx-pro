#pragma once

/**
 * @file ReadAloudIndex.h
 * @brief The narration timeline of an EPUB 3 read-aloud book (media overlays, as Storyteller makes).
 *
 * A read-aloud book wraps each sentence of its text in an element with an id, and a SMIL file per
 * chapter pairs every id with a stretch of an audio file in the book ("sentence id17-s1 is
 * Audio/00001-00002.mp4 from 1.75 s to 5.97 s"). This index flattens those into one table of clips
 * (sentence id hash, audio track, start time, spine item), sorted by track and time, plus the audio
 * tracks with where their bytes sit in the EPUB: the audio must be stored uncompressed, so the
 * player reads it in place.
 *
 * Building parses every SMIL file (a few seconds for a long book), so the result is cached in the
 * book's cache folder (readaloud.bin) and loaded from there next time. The clips live in PSRAM
 * (12 bytes each: about 230 KB for a 19,000-sentence novel).
 *
 * Section::loadAnchors() supplies the other half: which page each sentence id starts on.
 */

#if INX_HAS_AUDIO_MODULE

#include <Epub/PsramAllocator.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Epub;

class ReadAloudIndex {
 public:
  struct Track {
    EpubPsramString href;  ///< item path in the EPUB
    uint64_t offset = 0;   ///< of its bytes in the EPUB file
    uint32_t size = 0;
  };

  struct Clip {
    uint32_t idHash;  ///< Section::anchorHash() of the sentence's element id
    uint32_t beginMs;
    uint16_t track;
    uint16_t spine;
  };

  /**
   * Loads the cached index, or builds and caches it. `progress` (0..100) is called while building.
   * False if the book has no usable media overlays (see error()). Main loop: reads the card.
   */
  bool load(const Epub& epub, const std::function<void(int)>& progress = nullptr);
  bool loaded() const { return !clips_.empty(); }
  const std::string& error() const { return error_; }

  const std::vector<Track, EpubPsramAllocator<Track>>& tracks() const { return tracks_; }
  size_t clipCount() const { return clips_.size(); }
  const Clip& clip(size_t index) const { return clips_[index]; }

  /** The clip playing at ms in a track: the last one starting at or before it; -1 if none. */
  int clipAt(int track, uint32_t ms) const;
  /** The first clip of a spine item, -1 if it has none. */
  int firstClipOfSpine(int spine) const;

 private:
  bool build(const Epub& epub, const std::function<void(int)>& progress);
  bool readCache(const std::string& path);
  void writeCache(const std::string& path) const;

  std::vector<Track, EpubPsramAllocator<Track>> tracks_;
  std::vector<Clip, EpubPsramAllocator<Clip>> clips_;
  std::string error_;
};

#endif  // INX_HAS_AUDIO_MODULE
