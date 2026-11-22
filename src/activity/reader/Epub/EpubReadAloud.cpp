/**
 * @file EpubReadAloud.cpp
 * @brief Definitions for EpubReadAloud.
 */

#include "EpubReadAloud.h"

#if INX_HAS_AUDIO_MODULE

#include <Epub.h>
#include <Epub/Section.h>
#include <HardwareSerial.h>
#include <esp_heap_caps.h>

#include <algorithm>

#include "EpubActivity.h"
#include "audio/ReadAloudIndex.h"
#include "system/AudioModule.h"
#include "system/AudioPlayer.h"

namespace {

constexpr uint16_t UNKNOWN_PAGE = UINT16_MAX;
constexpr uint32_t POPUP_MS = 1200;

}  // namespace

EpubReadAloud::EpubReadAloud() = default;
EpubReadAloud::~EpubReadAloud() = default;

bool EpubReadAloud::available(const EpubActivity& act) { return act.epub && act.epub->hasMediaOverlays(); }

bool EpubReadAloud::active(const EpubActivity& act) const {
  return act.epub && AudioPlayer::isReadAloud(act.epub->getPath());
}

void EpubReadAloud::showPopup(EpubActivity& act, const char* message) { act.readerPopup(message, POPUP_MS); }

bool EpubReadAloud::ensureIndex(EpubActivity& act) {
  if (index_ && index_->loaded()) return true;
  if (indexFailed_) return false;
  index_.reset(new ReadAloudIndex());
  int shown = -1;
  const bool ok = index_->load(*act.epub, [&act, &shown](const int percent) {
    // A few steps only: every redraw is a panel refresh.
    const int step = percent / 25;
    if (step == shown) return;
    shown = step;
    act.loadingProgressShow("Preparing read-aloud...", percent);
  });
  if (shown >= 0) act.updateRequired = true;  // the progress box covered the page
  if (!ok) {
    INX_SERIAL.printf("[%lu] [RAL] %s\n", millis(), index_->error().c_str());
    indexFailed_ = true;
  }
  return ok;
}

bool EpubReadAloud::ensurePages(EpubActivity& act) {
  if (!act.section || !index_ || !index_->loaded()) return false;
  const int spine = act.currentSpineIndex;
  if (spine == pagesSpine_ && act.section.get() == pagesSection_ && act.section->pageCount == pagesPageCount_) {
    return true;
  }
  pagesSpine_ = spine;
  pagesSection_ = act.section.get();
  pagesPageCount_ = act.section->pageCount;
  pagesByHash_.clear();
  spineClips_.clear();

  std::vector<std::pair<uint32_t, uint16_t>> anchors;
  if (!Section::loadAnchors(act.epub->getCachePath(), spine, anchors)) {
    INX_SERIAL.printf("[%lu] [RAL] No sentence pages for spine %d\n", millis(), spine);
  }
  pagesByHash_.assign(anchors.begin(), anchors.end());
  std::vector<std::pair<uint32_t, uint16_t>>().swap(anchors);
  // By hash; an id that appears twice keeps its first page.
  std::stable_sort(pagesByHash_.begin(), pagesByHash_.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  pagesByHash_.erase(std::unique(pagesByHash_.begin(), pagesByHash_.end(),
                                 [](const auto& a, const auto& b) { return a.first == b.first; }),
                     pagesByHash_.end());
  for (size_t i = 0; i < index_->clipCount(); ++i) {
    const ReadAloudIndex::Clip& clip = index_->clip(i);
    if (clip.spine != spine) continue;
    const int page = pageOfHash(clip.idHash);
    spineClips_.emplace_back(static_cast<int>(i), page < 0 ? UNKNOWN_PAGE : static_cast<uint16_t>(page));
  }
  return true;
}

int EpubReadAloud::pageOfHash(const uint32_t hash) const {
  const auto found = std::lower_bound(pagesByHash_.begin(), pagesByHash_.end(), hash,
                                      [](const auto& entry, const uint32_t key) { return entry.first < key; });
  return found != pagesByHash_.end() && found->first == hash ? found->second : -1;
}

int EpubReadAloud::pageOfClip(const int index) const {
  if (!index_ || index < 0 || static_cast<size_t>(index) >= index_->clipCount()) return -1;
  const ReadAloudIndex::Clip& clip = index_->clip(index);
  if (clip.spine != pagesSpine_) return -1;
  return pageOfHash(clip.idHash);
}

int EpubReadAloud::clipForPage(const int page) const {
  int runningInto = -1;
  for (const auto& entry : spineClips_) {
    if (entry.second == UNKNOWN_PAGE) continue;
    if (entry.second == page) return entry.first;
    if (entry.second < page) runningInto = entry.first;
  }
  return runningInto;
}

void EpubReadAloud::play(EpubActivity& act, const int clip) {
  const ReadAloudIndex::Clip& c = index_->clip(static_cast<size_t>(clip));
  heldClip_ = clip;
  const std::string& path = act.epub->getPath();
  if (AudioPlayer::isReadAloud(path)) {
    AudioPlayer::seekTrack(c.track, c.beginMs);
    return;
  }
  std::vector<AudioPlayer::ReadAloudTrack> tracks;
  tracks.reserve(index_->tracks().size());
  for (const ReadAloudIndex::Track& track : index_->tracks()) {
    // The file name only picks the reader: the base name ("00001-00002.mp4") is short enough to
    // need no heap of its own.
    const size_t slash = track.href.find_last_of('/');
    tracks.push_back(AudioPlayer::ReadAloudTrack{
        std::string(slash == EpubPsramString::npos ? track.href.c_str() : track.href.c_str() + slash + 1), track.offset,
        track.size});
  }
  AudioModule::logf("Read aloud: chapter %d, %u sentences; RAM: internal %u bytes free, largest block %u",
                    static_cast<int>(c.spine), static_cast<unsigned>(spineClips_.size()),
                    static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                    static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
  if (!AudioPlayer::openReadAloud(path, act.epub->getTitle(), tracks, c.track, c.beginMs)) {
    const std::string message = AudioPlayer::view().message;
    showPopup(act, message.empty() ? "Could not play the narration" : message.c_str());
    return;
  }
  showPopup(act, "Read aloud");
}

void EpubReadAloud::toggle(EpubActivity& act) {
  if (!available(act)) {
    showPopup(act, "This book has no narration");
    return;
  }
  if (!ensureIndex(act)) {
    showPopup(act, index_ ? index_->error().c_str() : "No narration");
    return;
  }
  if (active(act)) {
    const AudioPlayer::View view = AudioPlayer::view();
    AudioPlayer::togglePause();
    showPopup(act, view.paused || view.finished ? "Read aloud" : "Read aloud paused");
    return;
  }
  if (!ensurePages(act)) return;
  const int spine = act.currentSpineIndex;
  const int page = act.section->currentPage;
  int clip = clipForPage(page);
  if (clip < 0) {
    // A page before the narration starts (a title page): the next narrated sentence.
    for (const auto& entry : spineClips_) {
      if (entry.second != UNKNOWN_PAGE && entry.second > page) {
        clip = entry.first;
        break;
      }
    }
  }
  if (clip < 0) {
    // Nothing narrated in this chapter: the first sentence of the next one that is.
    int bestSpine = INT32_MAX;
    for (size_t i = 0; i < index_->clipCount(); ++i) {
      const int s = index_->clip(i).spine;
      if (s > spine && s < bestSpine) {
        bestSpine = s;
        clip = static_cast<int>(i);
      }
    }
  }
  if (clip < 0) {
    showPopup(act, "No narration after this page");
    return;
  }
  seenSpine_ = spine;
  seenPage_ = page;
  pendingClip_ = -1;
  play(act, clip);
}

bool EpubReadAloud::tick(EpubActivity& act) {
  if (!act.epub || !AudioPlayer::isReadAloud(act.epub->getPath())) {
    seenSpine_ = -1;
    seenPage_ = -1;
    pendingClip_ = -1;
    return false;
  }
  if (!act.section || !ensurePages(act)) return false;  // a chapter is loading
  const int spine = act.currentSpineIndex;
  const int page = act.section->currentPage;
  const AudioPlayer::View view = AudioPlayer::view();

  if (pendingClip_ >= 0) {
    // We changed chapters with the narration: now that it is laid out, go to the sentence.
    const int clip = pendingClip_;
    pendingClip_ = -1;
    seenSpine_ = spine;
    seenPage_ = page;
    if (index_->clip(static_cast<size_t>(clip)).spine == spine) {
      const int target = pageOfClip(clip);
      if (target >= 0 && target != page && target < act.section->pageCount) {
        act.endPageTimer();
        act.invalidatePreparedPage();
        act.section->currentPage = target;
        seenPage_ = target;
        act.startPageTimer();
        act.updateRequired = true;
        return true;
      }
    }
    return false;
  }

  if (spine != seenSpine_ || page != seenPage_) {
    // The reader turned the page (or jumped): the narration follows it.
    seenSpine_ = spine;
    seenPage_ = page;
    const int clip = clipForPage(page);
    if (clip < 0) {
      if (!view.paused) AudioPlayer::togglePause();
      return false;
    }
    const ReadAloudIndex::Clip& c = index_->clip(static_cast<size_t>(clip));
    heldClip_ = clip;
    AudioPlayer::seekTrack(c.track, c.beginMs);
    return false;
  }

  if (view.paused || view.finished || !view.open) return false;
  int clip = index_->clipAt(view.track, view.positionMs);
  if (heldClip_ >= 0) {
    // After a jump to a sentence: the stream starts on the audio frame at or just before it,
    // which is still the previous sentence. Stay on the target until playback reaches it.
    if (clip < heldClip_ && clip >= 0 &&
        index_->clip(static_cast<size_t>(clip)).track == index_->clip(static_cast<size_t>(heldClip_)).track) {
      clip = heldClip_;
    } else {
      heldClip_ = -1;
    }
  }
  if (clip < 0) return false;
  const ReadAloudIndex::Clip& c = index_->clip(static_cast<size_t>(clip));
  if (c.spine != spine) {
    // The narration went on into another chapter.
    act.endPageTimer();
    act.invalidatePreparedPage();
    act.currentSpineIndex = c.spine;
    act.nextPageNumber = 0;
    act.section.reset();
    act.updateRequired = true;
    pendingClip_ = clip;
    seenSpine_ = c.spine;
    seenPage_ = 0;
    return true;
  }
  const int target = pageOfClip(clip);
  if (target < 0 || target == page || target >= act.section->pageCount) return false;
  act.endPageTimer();
  act.invalidatePreparedPage();
  act.section->currentPage = target;
  seenPage_ = target;
  act.startPageTimer();
  act.updateRequired = true;
  return true;
}

void EpubReadAloud::exit(EpubActivity& act) {
  if (active(act)) AudioPlayer::stop();
  heldClip_ = -1;
  seenSpine_ = -1;
  seenPage_ = -1;
  pendingClip_ = -1;
}

#endif  // INX_HAS_AUDIO_MODULE
