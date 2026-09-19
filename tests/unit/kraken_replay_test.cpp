#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "adapters/kraken/kraken_book_processor.hpp"
#include "doctest.h"
#include <fstream>
using namespace adapters::kraken;
static std::string fixture(const char* name) {
    std::ifstream in(std::string(FIXTURE_DIR) + "/" + name);
    return {std::istreambuf_iterator<char>(in), {}};
}

#include "recording/raw_event_recorder.hpp"
#include "session/session.hpp"
#include <unistd.h>
TEST_CASE("session replay preserves checksum failures, transitions and final book") {
    bool reconnect = false;
    SUBCASE("same connection snapshot recovery") {}
    SUBCASE("reconnect requires a fresh snapshot") { reconnect = true; }
    auto dir =
        std::filesystem::temp_directory_path() / ("kraken-replay-" + std::to_string(getpid()));
    std::filesystem::remove_all(dir);
    sessions::Capture capture(dir, "kraken", 100);
    recording::RawEventRecorder writer(capture.raw_path());
    KrakenBookProcessor live;
    std::uint64_t count = 0;
    for (auto name : {"book_snapshot.json", "checksum_mismatch.json", "book_update.json",
                      "book_snapshot.json", "book_update.json"}) {
        auto raw = fixture(name);
        REQUIRE(writer.append(core::Venue::Kraken, reconnect && count >= 3 ? 2 : 1,
                              {static_cast<std::int64_t>(count)},
                              {static_cast<std::int64_t>(count)}, raw));
        if (reconnect && count == 3) {
            live.disconnect();
            live.reset_connection();
        }
        live.process(raw, {static_cast<std::int64_t>(count)}, {static_cast<std::int64_t>(count)});
        ++count;
    }
    REQUIRE(writer.close());
    pipeline::RuntimeStatus s;
    s.runtime_state = pipeline::RuntimeState::Stopped;
    s.received_messages = s.enqueued_messages = s.written_messages = s.processed_messages = count;
    s.progress.last_received_index = s.progress.last_enqueued_index =
        s.progress.last_written_index = s.progress.last_processed_index = count - 1;
    if (reconnect)
        s.lifecycle_events.push_back({3, core::BookState::Disconnected, "test_disconnect"});
    capture.finalize(s);
    REQUIRE(sessions::replay(dir, dir / "one.json", false, true) == 0);
    REQUIRE(sessions::replay(dir, dir / "two.json", false, true) == 0);
    nlohmann::json one, two;
    std::ifstream(dir / "one.json") >> one;
    std::ifstream(dir / "two.json") >> two;
    CHECK(one == two);
    CHECK(one["metrics"] == live.metrics());
    CHECK(one["checksum"] == live.checksum());
    CHECK(one["final_best_bid"] == live.book().best_bid()->price.raw());
    CHECK(one["metrics"]["kraken_checksum_failures"] == 1);
    CHECK(one["state_transitions"].size() == (reconnect ? 5 : 3));
    nlohmann::json bids = nlohmann::json::array(), asks = nlohmann::json::array();
    for (auto [p, q] : live.book().bids())
        bids.push_back({p.raw(), q.raw()});
    for (auto [p, q] : live.book().asks())
        asks.push_back({p.raw(), q.raw()});
    CHECK(one["final_book"]["bids"] == bids);
    CHECK(one["final_book"]["asks"] == asks);
    std::filesystem::remove_all(dir);
}
