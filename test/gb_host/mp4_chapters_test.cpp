/**
 * @file mp4_chapters_test.cpp
 * @brief Host check of Mp4Source's chapter parsing (src/audio/Mp4Source.cpp) on files ffmpeg made.
 *
 * Usage: mp4_chapters_test <file.m4b> <expected count> [<start ms> <title>]... [--cover <image>]
 *
 * ffmpeg writes chapters the two ways audiobook files carry them: as a QuickTime chapter track
 * (-f ipod, i.e. .m4b) and as a Nero chpl box (-f mp4). test/run_gb_host_test.sh makes one of
 * each and checks every chapter's start (to the millisecond) and title. With --cover, the file's
 * embedded cover art (covr) must be exactly that image's bytes.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "audio/Mp4Source.h"

namespace {

class StdioFile final : public AudioFile {
 public:
  explicit StdioFile(FILE* f) : f_(f) {}
  ~StdioFile() override { std::fclose(f_); }
  bool seekSet(const uint64_t position) override { return fseeko(f_, static_cast<off_t>(position), SEEK_SET) == 0; }
  int read(void* buffer, const size_t length) override { return static_cast<int>(std::fread(buffer, 1, length, f_)); }
  uint64_t fileSize() override {
    const off_t at = ftello(f_);
    fseeko(f_, 0, SEEK_END);
    const off_t size = ftello(f_);
    fseeko(f_, at, SEEK_SET);
    return static_cast<uint64_t>(size);
  }

 private:
  FILE* f_;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return std::fprintf(stderr, "usage: %s <file> <count> [<start ms> <title>]...\n", argv[0]), 2;
  FILE* f = std::fopen(argv[1], "rb");
  if (!f) return std::perror(argv[1]), 1;
  Mp4Source source;
  if (!source.open(std::make_unique<StdioFile>(f))) {
    std::printf("MP4 chapters (%s): open failed: %s: FAIL\n", argv[1], source.error().c_str());
    return 1;
  }
  const auto& chapters = source.chapters();
  bool ok = static_cast<int>(chapters.size()) == std::atoi(argv[2]);
  const char* expectedCover = nullptr;
  if (argc >= 5 && std::strcmp(argv[argc - 2], "--cover") == 0) {
    expectedCover = argv[argc - 1];
    argc -= 2;
  }
  for (int i = 3, c = 0; i + 1 < argc; i += 2, ++c) {
    const uint32_t start = static_cast<uint32_t>(std::atoi(argv[i]));
    const bool match = c < static_cast<int>(chapters.size()) && chapters[c].title == argv[i + 1] &&
                       (chapters[c].startMs > start ? chapters[c].startMs - start : start - chapters[c].startMs) <= 1;
    if (!match) {
      std::printf("  chapter %d: expected %u ms \"%s\", got %u ms \"%s\"\n", c, start, argv[i + 1],
                  c < static_cast<int>(chapters.size()) ? chapters[c].startMs : 0,
                  c < static_cast<int>(chapters.size()) ? chapters[c].title.c_str() : "(none)");
    }
    ok &= match;
  }
  if (expectedCover) {
    uint64_t offset = 0;
    uint32_t length = 0;
    bool png = false;
    bool same = source.embeddedCover(offset, length, png);
    FILE* image = std::fopen(expectedCover, "rb");
    FILE* audio = std::fopen(argv[1], "rb");
    if (same && image && audio && fseeko(audio, static_cast<off_t>(offset), SEEK_SET) == 0) {
      uint32_t compared = 0;
      for (int a, b; (b = std::fgetc(image)) != EOF; ++compared) {
        a = compared < length ? std::fgetc(audio) : EOF;
        if (a != b) same = false;
      }
      same = same && compared == length && !png;
    } else {
      same = false;
    }
    if (image) std::fclose(image);
    if (audio) std::fclose(audio);
    std::printf("MP4 cover art (%s): %u bytes at %llu: %s\n", argv[1], length, static_cast<unsigned long long>(offset),
                same ? "ok" : "FAIL");
    ok &= same;
  }
  std::printf("MP4 chapters (%s): %zu found, duration %u ms: %s\n", argv[1], chapters.size(), source.durationMs(),
              ok ? "ok" : "FAIL");
  return ok ? 0 : 1;
}
