#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "adapters/kraken/kraken_book_processor.hpp"
#include "doctest.h"
#include <fstream>
using namespace adapters::kraken;
static std::string fixture(const char* name) {
    std::ifstream in(std::string(FIXTURE_DIR) + "/" + name);
    return {std::istreambuf_iterator<char>(in), {}};
}

TEST_CASE("official Kraken fixture checksum 3310070434") {
    auto m = *KrakenL2Parser{}.parse(fixture("book_snapshot.json")).book;
    KrakenChecksumBook b;
    b.replace_snapshot(m.changes);
    CHECK(b.checksum() == 3310070434u);
    std::reverse(m.changes.begin(), m.changes.end());
    b.replace_snapshot(m.changes);
    CHECK(b.checksum() == 3310070434u);
    CHECK(checksum_digits("0.00100000") == "100000");
    CHECK(checksum_digits("45281.0") == "452810");
    b.apply({core::Side::Bid, core::PriceTicks{100}, core::QuantityAtoms{100000000}, "1.0",
             "1.00000000"});
    CHECK(b.checksum() == 3310070434u);
    b.apply({core::Side::Ask, core::PriceTicks{9999999}, core::QuantityAtoms{100000000}, "99999.99",
             "1.00000000"});
    CHECK(b.checksum() == 3310070434u);
    b.truncate(10);
    CHECK(b.levels().size() == 20);
    KrakenChecksumBook small;
    small.apply(m.changes.front());
    CHECK(small.checksum() == 3756032784u);
    CHECK(KrakenChecksumBook{}.checksum() == 0);
}
