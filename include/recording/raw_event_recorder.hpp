#pragma once

#include "recording/raw_message.hpp"

#include <filesystem>
#include <cstdint>
#include <fstream>
#include <string_view>

namespace recording {

class RawEventRecorder {
public:
    explicit RawEventRecorder(const std::filesystem::path& output_file);

    RawEventRecorder(const RawEventRecorder&) = delete;
    RawEventRecorder& operator=(const RawEventRecorder&) = delete;

    bool append(
        Venue venue,
        std::uint64_t connection_id,
        ReceiveWallTimestamp wall_time,
        ReceiveMonotonicTimestamp monotonic_time,
        std::string_view payload
    );

    bool flush();

    [[nodiscard]] std::uint64_t next_record_index() const;

private:
    std::ofstream output_;
    std::uint64_t next_record_index_{0};
};

} // namespace recording
