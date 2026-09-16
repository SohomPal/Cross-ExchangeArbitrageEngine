#pragma once
#include "pipeline/runtime_status.hpp"
#include <nlohmann/json.hpp>
namespace sessions {
enum class SessionStatus {
    InProgress,
    Complete,
    RecordingFailure,
    QueueOverflow,
    ProcessingFailure,
    Interrupted
};
std::string sha256(std::string_view bytes);
std::string file_sha256(const std::filesystem::path& path);
nlohmann::json configuration();
class Capture {
  public:
    explicit Capture(const std::filesystem::path& directory);
    std::filesystem::path raw_path() const;
    void finalize(const pipeline::RuntimeStatus& status);

  private:
    std::filesystem::path directory_;
    nlohmann::json manifest_;
};
int replay(const std::filesystem::path& directory, const std::filesystem::path& output,
           bool allow_incomplete = false, bool include_final_book = false);
} // namespace sessions
