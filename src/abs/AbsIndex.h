#pragma once

/**
 * @file AbsIndex.h
 * @brief A whole Audiobookshelf library, compact and in PSRAM: sorted by title, grouped by
 *        author, searchable, and saved to the SD card as one file (see AbsLibrary).
 *
 * Every string lives in one pool and entries refer to it by offset, so a library of thousands
 * of books is a few large PSRAM allocations, not thousands of small ones in scarce internal RAM.
 * Plain C++ (no ESP-IDF), so test/gb_host/abs_index_test.cpp runs it on the PC.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Epub/PsramAllocator.h"

class AbsIndex {
 public:
  template <typename T>
  using PsramVector = std::vector<T, EpubPsramAllocator<T>>;

  /** One book. Strings are offsets into the pool (text(), below). */
  struct Book {
    uint32_t id = 0;
    uint32_t title = 0;
    uint32_t sortTitle = 0;  ///< the server's titleIgnorePrefix ("Hobbit, The"), else the title
    uint32_t author = 0;     ///< "Name One, Name Two" as the server gives it
    uint32_t durationSec = 0;
    uint64_t sizeBytes = 0;
    uint16_t progress = 0;  ///< per mille
    uint8_t finished = 0;
  };

  /** One author, with their books as a run of authorBooks(). */
  struct Author {
    uint32_t name = 0;  ///< pool offset
    uint32_t first = 0;
    uint32_t count = 0;
  };

  void clear();
  /** progress 0..1, the user's for this book. */
  void add(std::string_view id, std::string_view title, std::string_view sortTitle, std::string_view author,
           uint32_t durationSec, uint64_t sizeBytes, float progress, bool finished);
  /** Sorts and groups; call after the last add() (again after more add()s). */
  void finish();

  size_t size() const { return books_.size(); }
  bool empty() const { return books_.empty(); }
  const Book& book(uint32_t i) const { return books_[i]; }
  std::string_view text(uint32_t offset) const;

  /** Book numbers in title order (by sortTitle, case-insensitive). */
  const PsramVector<uint32_t>& byTitle() const { return byTitle_; }
  /** Authors by surname (each named author of a book separately). */
  const PsramVector<Author>& authors() const { return authors_; }
  /** Book numbers, run by run per author (Author::first, count), each run in title order. */
  const PsramVector<uint32_t>& authorBooks() const { return authorBooks_; }

  /** Books whose title or author contains every word of `query` (case-insensitive), in title order. */
  PsramVector<uint32_t> search(std::string_view query) const;
  /** Position in byTitle() of the first title under `letter` ('A'..'Z', '#' for the rest). */
  size_t firstAt(char letter) const;
  /** The letter a book is filed under in the A-Z list. */
  char letterOf(uint32_t book) const;

  /** The whole index as one block (see the .cpp for the layout), and back. */
  PsramVector<uint8_t> serialize() const;
  bool deserialize(const uint8_t* data, size_t length);

 private:
  uint32_t intern(std::string_view s);

  EpubPsramString pool_;
  /** Where the books' strings end in pool_; finish() adds the author names after it. */
  size_t bookPoolEnd_ = 0;
  PsramVector<Book> books_;
  PsramVector<uint32_t> byTitle_;
  PsramVector<Author> authors_;
  PsramVector<uint32_t> authorBooks_;
};
