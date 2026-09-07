#pragma once

#include "recording/raw_message.hpp"

#include <filesystem>
#include <optional>
#include <fstream>
#include <string>

namespace recording {

class RawEventReader {
public:
    explicit RawEventReader(const std::filesystem::path& input_file);

    std::optional<RawMessage> next();

    [[nodiscard]] bool has_error() const;
    [[nodiscard]] std::string_view error() const;

private:
    std::ifstream input_; 
    std::string last_error_;
    std::uint64_t expected_index_{0};
};

} // namespace recording
