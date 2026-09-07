#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "recording/raw_event_recorder.hpp"
#include "recording/raw_event_reader.hpp"

#include <filesystem>
#include <fstream>

using namespace recording;
using core::ReceiveWallTimestamp;
using core::ReceiveMonotonicTimestamp;

TEST_CASE("RawRecorderTest PreservesPayloadExactly") {
    const std::string payload = R"({ "price_level": "62143.2700", "note": "a\"b" })";

    const auto tmp = std::filesystem::temp_directory_path() / "raw_recorder_test.jsonl";
    // ensure removal before
    std::error_code ec;
    std::filesystem::remove(tmp, ec);

    RawEventRecorder recorder{tmp};

    REQUIRE(recorder.next_record_index() == 0u);

    CHECK(recorder.append(core::Venue::Coinbase, 1, ReceiveWallTimestamp{100}, ReceiveMonotonicTimestamp{50}, payload));
    CHECK(recorder.flush());
    CHECK(recorder.next_record_index() == 1u);

    RawEventReader reader{tmp};
    const auto record = reader.next();

    REQUIRE(record.has_value());
    CHECK(record->payload == payload);
    CHECK(record->record_index == 0u);
    CHECK(record->venue == core::Venue::Coinbase);
    CHECK(record->connection_id == 1u);
    CHECK(record->receive_wall_time.nanoseconds == 100);
    CHECK(record->receive_monotonic_time.nanoseconds == 50);

    // cleanup
    std::filesystem::remove(tmp, ec);
}

TEST_CASE("RawRecorderTest NonOverwrite") {
    const auto tmp = std::filesystem::temp_directory_path() / "raw_recorder_existing.jsonl";
    std::ofstream out(tmp);
    out << "existing" << std::endl;
    out.close();

    RawEventRecorder recorder{tmp};
    // append should fail because constructor refused to open existing file
    CHECK_FALSE(recorder.append(core::Venue::Coinbase, 1, ReceiveWallTimestamp{0}, ReceiveMonotonicTimestamp{0}, "x"));

    std::error_code ec;
    std::filesystem::remove(tmp, ec);
}

TEST_CASE("RawReaderTest RejectsUnknownFormat") {
    const auto tmp = std::filesystem::temp_directory_path() / "raw_recorder_bad_format.jsonl";
    std::ofstream out(tmp);
    out << "{\"format_version\":99}\n";
    out.close();

    RawEventReader reader{tmp};
    auto r = reader.next();
    CHECK_FALSE(r.has_value());
    CHECK(reader.has_error());

    std::error_code ec;
    std::filesystem::remove(tmp, ec);
}
