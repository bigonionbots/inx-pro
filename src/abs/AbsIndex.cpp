/**
 * @file AbsIndex.cpp
 * @brief Definitions for the compact Audiobookshelf library index.
 */

#include "AbsIndex.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {

constexpr char MAGIC[8] = {'A', 'B', 'S', 'I', 'D', 'X', '0', '1'};

/** ASCII-only lower case: enough for sorting and matching; other bytes compare as they are. */
char lowerAscii(const char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool lessNoCase(std::string_view a, std::string_view b) {
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    const char x = lowerAscii(a[i]);
    const char y = lowerAscii(b[i]);
    if (x != y) return static_cast<unsigned char>(x) < static_cast<unsigned char>(y);
  }
  return a.size() < b.size();
}

bool containsNoCase(std::string_view hay, std::string_view needle) {
  if (needle.empty()) return true;
  if (needle.size() > hay.size()) return false;
  for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
    size_t k = 0;
    while (k < needle.size() && lowerAscii(hay[i + k]) == lowerAscii(needle[k])) ++k;
    if (k == needle.size()) return true;
  }
  return false;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

/** "Brandon Sanderson" -> "Sanderson Brandon": authors sort by surname (the last word). */
std::string surnameFirst(std::string_view name) {
  const size_t space = name.rfind(' ');
  if (space == std::string_view::npos) return std::string(name);
  return std::string(name.substr(space + 1)) + " " + std::string(name.substr(0, space));
}

template <typename T>
void put(AbsIndex::PsramVector<uint8_t>& out, const T& value) {
  const auto* p = reinterpret_cast<const uint8_t*>(&value);
  out.insert(out.end(), p, p + sizeof(T));
}

template <typename T>
bool get(const uint8_t*& p, const uint8_t* end, T& value) {
  if (static_cast<size_t>(end - p) < sizeof(T)) return false;
  std::memcpy(&value, p, sizeof(T));
  p += sizeof(T);
  return true;
}

}  // namespace

void AbsIndex::clear() {
  pool_.clear();
  bookPoolEnd_ = 0;
  books_.clear();
  byTitle_.clear();
  authors_.clear();
  authorBooks_.clear();
}

uint32_t AbsIndex::intern(std::string_view s) {
  const uint32_t at = static_cast<uint32_t>(pool_.size());
  pool_.append(s.data(), s.size());
  pool_.push_back('\0');
  return at;
}

std::string_view AbsIndex::text(const uint32_t offset) const {
  if (offset >= pool_.size()) return {};
  return std::string_view(pool_.c_str() + offset);
}

void AbsIndex::add(std::string_view id, std::string_view title, std::string_view sortTitle, std::string_view author,
                   const uint32_t durationSec, const uint64_t sizeBytes, const float progress, const bool finished) {
  if (pool_.size() > bookPoolEnd_) pool_.resize(bookPoolEnd_);  // drop author names from a finish()
  Book b;
  b.id = intern(id);
  b.title = intern(title);
  b.sortTitle = sortTitle.empty() || sortTitle == title ? b.title : intern(sortTitle);
  b.author = intern(author);
  b.durationSec = durationSec;
  b.sizeBytes = sizeBytes;
  const float clamped = progress < 0 ? 0 : (progress > 1 ? 1 : progress);
  b.progress = static_cast<uint16_t>(clamped * 1000.0f + 0.5f);
  b.finished = finished ? 1 : 0;
  books_.push_back(b);
  bookPoolEnd_ = pool_.size();
}

void AbsIndex::finish() {
  byTitle_.resize(books_.size());
  for (uint32_t i = 0; i < books_.size(); ++i) byTitle_[i] = i;
  std::stable_sort(byTitle_.begin(), byTitle_.end(), [this](const uint32_t a, const uint32_t b) {
    return lessNoCase(text(books_[a].sortTitle), text(books_[b].sortTitle));
  });

  // Authors: every name in "A, B" (a book is under each), keyed by surname. One (key, title rank,
  // book) record per name, sorted and grouped: PSRAM arrays only, no per-author allocations.
  pool_.resize(bookPoolEnd_);
  struct Record {
    uint32_t key;   // offset in `keys`
    uint32_t name;  // offset in pool_
    uint32_t rank;  // position in byTitle_
    uint32_t book;
  };
  PsramVector<Record> records;
  EpubPsramString keys;
  for (uint32_t rank = 0; rank < byTitle_.size(); ++rank) {
    const uint32_t book = byTitle_[rank];
    std::string_view names = text(books_[book].author);
    while (!names.empty()) {
      const size_t comma = names.find(',');
      const std::string_view name = trim(names.substr(0, comma));
      names = comma == std::string_view::npos ? std::string_view() : names.substr(comma + 1);
      if (name.empty()) continue;
      Record r;
      r.key = static_cast<uint32_t>(keys.size());
      const std::string key = surnameFirst(name);
      keys.append(key);
      keys.push_back('\0');
      r.name = intern(name);
      r.rank = rank;
      r.book = book;
      records.push_back(r);
    }
  }
  const auto keyOf = [&keys](const Record& r) { return std::string_view(keys.c_str() + r.key); };
  std::stable_sort(records.begin(), records.end(), [&](const Record& a, const Record& b) {
    const std::string_view ka = keyOf(a), kb = keyOf(b);
    if (lessNoCase(ka, kb)) return true;
    if (lessNoCase(kb, ka)) return false;
    return a.rank < b.rank;
  });
  authors_.clear();
  authorBooks_.clear();
  for (size_t i = 0; i < records.size();) {
    size_t j = i + 1;
    while (j < records.size() && !lessNoCase(keyOf(records[i]), keyOf(records[j])) &&
           !lessNoCase(keyOf(records[j]), keyOf(records[i]))) {
      ++j;
    }
    Author a;
    a.name = records[i].name;
    a.first = static_cast<uint32_t>(authorBooks_.size());
    a.count = static_cast<uint32_t>(j - i);
    for (size_t k = i; k < j; ++k) authorBooks_.push_back(records[k].book);
    authors_.push_back(a);
    i = j;
  }
}

AbsIndex::PsramVector<uint32_t> AbsIndex::search(std::string_view query) const {
  std::vector<std::string_view> words;
  for (query = trim(query); !query.empty();) {
    const size_t space = query.find(' ');
    words.push_back(query.substr(0, space));
    query = space == std::string_view::npos ? std::string_view() : trim(query.substr(space + 1));
  }
  PsramVector<uint32_t> found;
  if (words.empty()) return found;
  for (const uint32_t i : byTitle_) {
    const std::string_view title = text(books_[i].title);
    const std::string_view author = text(books_[i].author);
    const bool all = std::all_of(words.begin(), words.end(), [&](std::string_view w) {
      return containsNoCase(title, w) || containsNoCase(author, w);
    });
    if (all) found.push_back(i);
  }
  return found;
}

char AbsIndex::letterOf(const uint32_t book) const {
  const std::string_view t = text(books_[book].sortTitle);
  const char c = t.empty() ? '#' : static_cast<char>(std::toupper(static_cast<unsigned char>(t.front())));
  return c >= 'A' && c <= 'Z' ? c : '#';
}

size_t AbsIndex::firstAt(const char letter) const {
  // '#' (digits, punctuation, other scripts) sorts before or after the letters depending on
  // the byte, so look for the first match rather than relying on order.
  for (size_t i = 0; i < byTitle_.size(); ++i) {
    const char at = letterOf(byTitle_[i]);
    if (at == letter) return i;
    if (letter != '#' && at != '#' && at > letter) return i;  // no title under it: the next one
  }
  return byTitle_.empty() ? 0 : byTitle_.size() - 1;
}

// Layout: MAGIC, u32 book count, u32 pool bytes, Book[count] (as stored in memory), the books'
// part of the pool. byTitle and the authors are rebuilt by finish() on loading: cheap, and keeps
// the file simple.
AbsIndex::PsramVector<uint8_t> AbsIndex::serialize() const {
  PsramVector<uint8_t> out;
  out.reserve(sizeof(MAGIC) + 8 + books_.size() * sizeof(Book) + bookPoolEnd_);
  out.insert(out.end(), MAGIC, MAGIC + sizeof(MAGIC));
  put(out, static_cast<uint32_t>(books_.size()));
  put(out, static_cast<uint32_t>(bookPoolEnd_));
  for (const Book& b : books_) put(out, b);
  out.insert(out.end(), pool_.begin(), pool_.begin() + static_cast<std::ptrdiff_t>(bookPoolEnd_));
  return out;
}

bool AbsIndex::deserialize(const uint8_t* data, const size_t length) {
  clear();
  const uint8_t* p = data;
  const uint8_t* end = data + length;
  if (length < sizeof(MAGIC) || std::memcmp(p, MAGIC, sizeof(MAGIC)) != 0) return false;
  p += sizeof(MAGIC);
  uint32_t count = 0;
  uint32_t poolBytes = 0;
  if (!get(p, end, count) || !get(p, end, poolBytes)) return false;
  if (static_cast<size_t>(end - p) != static_cast<size_t>(count) * sizeof(Book) + poolBytes) return false;
  books_.resize(count);
  for (Book& b : books_) get(p, end, b);
  pool_.assign(reinterpret_cast<const char*>(p), poolBytes);
  bookPoolEnd_ = poolBytes;
  for (const Book& b : books_) {
    if (b.id >= poolBytes || b.title >= poolBytes || b.sortTitle >= poolBytes || b.author >= poolBytes) {
      clear();
      return false;
    }
  }
  if (poolBytes > 0 && pool_.back() != '\0') {
    clear();
    return false;
  }
  finish();
  return true;
}
