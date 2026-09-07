#include "recording/raw_event_recorder.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <system_error>

using nlohmann::json;

namespace recording {

static const int kFormatVersion = 1;

RawEventRecorder::RawEventRecorder(const std::filesystem::path& output_file) {
    const auto parent = output_file.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        // ignore errors here; open will fail if not creatable
    }

    // Refuse to overwrite existing file.
    if (std::filesystem::exists(output_file)) {
        // leave output_ in a bad state
        return;
    }

    output_.open(output_file, std::ios::out | std::ios::binary);
}

bool RawEventRecorder::append(
    Venue venue,
    std::uint64_t connection_id,
    ReceiveWallTimestamp wall_time,
    ReceiveMonotonicTimestamp monotonic_time,
    std::string_view payload
) {
    if (!output_.is_open() || !output_.good()) return false;

    json j;
    j["format_version"] = kFormatVersion;
    j["record_index"] = next_record_index_;
    switch (venue) {
        case Venue::Coinbase:
            j["venue"] = "coinbase";
            break;
        case Venue::Kraken:
            j["venue"] = "kraken";
            break;
        default:
            j["venue"] = "unknown";
            break;
    }
    j["connection_id"] = connection_id;
    j["receive_wall_ns"] = wall_time.nanoseconds;
    j["receive_monotonic_ns"] = monotonic_time.nanoseconds;

    // Store payload as raw string without parsing it.
    j["payload"] = std::string(payload);

    const std::string line = j.dump();

    output_ << line << '\n';
    if (!output_.good()) return false;

    output_.flush();
    if (!output_.good()) return false;

    ++next_record_index_;
    return true;
}

bool RawEventRecorder::flush() {
    if (!output_.is_open()) return false;
    output_.flush();
    return output_.good();
}

bool RawEventRecorder::close() {
    if (!output_.is_open()) return false;
    output_.flush();
    output_.close();
    return !output_.fail();
}

std::uint64_t RawEventRecorder::next_record_index() const { return next_record_index_; }

} // namespace recording
