/**
 * @file abs_index_test.cpp
 * @brief Host check of AbsIndex (src/abs/AbsIndex.cpp): the cached Audiobookshelf library behind
 *        the A-Z, Authors and Search views.
 */

#include <cstdio>
#include <string>
#include <vector>

#include "abs/AbsIndex.h"

namespace {

int failures = 0;

void check(const bool ok, const char* what) {
  std::printf("AbsIndex %s: %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) ++failures;
}

std::vector<std::string> titles(const AbsIndex& index, const AbsIndex::PsramVector<uint32_t>& books) {
  std::vector<std::string> out;
  for (const uint32_t b : books) out.emplace_back(index.text(index.book(b).title));
  return out;
}

void build(AbsIndex& index) {
  index.clear();
  index.add("id-hobbit", "The Hobbit", "Hobbit, The", "J.R.R. Tolkien", 40000, 300, 0.5f, false);
  index.add("id-way", "The Way of Kings", "Way of Kings, The", "Brandon Sanderson", 160000, 900, 0, false);
  index.add("id-mist", "mistborn", "", "Brandon Sanderson", 90000, 500, 1.0f, true);
  index.add("id-good", "Good Omens", "", "Terry Pratchett, Neil Gaiman", 45000, 320, 0, false);
  index.add("id-1984", "1984", "", "George Orwell", 40000, 200, 0, false);
  index.add("id-ana", "Anansi Boys", "", "Neil Gaiman", 36000, 250, 0.2f, false);
  index.finish();
}

}  // namespace

int main() {
  AbsIndex index;
  build(index);

  const std::vector<std::string> az = titles(index, index.byTitle());
  const std::vector<std::string> expectedAz = {"1984",      "Anansi Boys",     "Good Omens",
                                               "The Hobbit", "mistborn",        "The Way of Kings"};
  check(az == expectedAz, "title order (case-insensitive, ignoring The)");

  // Authors by surname: Gaiman, Orwell, Pratchett, Sanderson, Tolkien.
  std::vector<std::string> authors;
  for (const auto& a : index.authors()) authors.emplace_back(index.text(a.name));
  check(authors == std::vector<std::string>({"Neil Gaiman", "George Orwell", "Terry Pratchett", "Brandon Sanderson",
                                             "J.R.R. Tolkien"}),
        "authors by surname, co-authors split");
  const auto& gaiman = index.authors()[0];
  std::vector<std::string> gaimanBooks;
  for (uint32_t i = 0; i < gaiman.count; ++i) {
    gaimanBooks.emplace_back(index.text(index.book(index.authorBooks()[gaiman.first + i]).title));
  }
  check(gaimanBooks == std::vector<std::string>({"Anansi Boys", "Good Omens"}), "an author's books in title order");

  check(titles(index, index.search("SANDERSON way")) == std::vector<std::string>({"The Way of Kings"}),
        "search: every word, title or author, any case");
  check(titles(index, index.search("gaiman")).size() == 2, "search: co-author");
  check(index.search("   ").empty() && index.search("zzz").empty(), "search: nothing");

  check(index.firstAt('#') == 0 && index.firstAt('A') == 1 && index.firstAt('H') == 3, "letter jumps");
  check(index.firstAt('C') == 2, "letter with no titles: the next one");
  check(index.letterOf(index.byTitle()[0]) == '#', "digits file under #");

  const auto& mist = index.book(index.byTitle()[4]);
  check(mist.finished == 1 && mist.progress == 1000, "progress kept");

  // Round trip, twice: the file must not grow (author names are rebuilt, not saved).
  const auto once = index.serialize();
  AbsIndex loaded;
  check(loaded.deserialize(once.data(), once.size()), "load");
  const auto twice = loaded.serialize();
  check(once == twice, "save after load is identical");
  check(titles(loaded, loaded.byTitle()) == expectedAz && loaded.authors().size() == 5, "loaded index the same");

  auto broken = once;
  broken[10] ^= 0xFF;  // the book count
  check(!loaded.deserialize(broken.data(), broken.size()) && loaded.empty(), "damaged file refused");
  return failures ? 1 : 0;
}
