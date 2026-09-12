#include "pipeline/runtime_status.hpp"
#include "recording/raw_event_recorder.hpp"
namespace pipeline {
void SharedRuntime::recording_failed(std::string error) {
    std::lock_guard lock(mutex);
    if (!status.fatal_error) {
        status.fatal_error = error;
        status.runtime_state = RuntimeState::StoppedRecordingError;
    }
    if (post_recording_failure)
        post_recording_failure(std::move(error));
    changed.notify_all();
}
void write_raw(recording::RawRecordingQueue& queue, SharedRuntime& shared,
               const std::filesystem::path& path, std::uintmax_t minimum_free_disk_bytes,
               AppendRaw append) {
    try {
        auto directory = path.parent_path();
        if (directory.empty())
            directory = ".";
        std::filesystem::create_directories(directory);
        if (std::filesystem::space(directory).available < minimum_free_disk_bytes)
            throw std::runtime_error("Insufficient disk space");
        recording::RawEventRecorder recorder{path};
        if (!recorder.flush())
            throw std::runtime_error("cannot open a new recording file");
        {
            std::lock_guard lock(shared.mutex);
            shared.writer_ready = true;
            shared.changed.notify_all();
        }
        if (!append)
            append = [](auto& recorder, const auto& e) {
                return recorder.append(e.venue, e.connection_id, e.receive_wall_time,
                                       e.receive_monotonic_time, e.payload) &&
                       recorder.flush();
            };
        while (auto envelope = queue.wait_pop()) {
            const auto& e = **envelope;
            if (e.record_index != recorder.next_record_index() || !append(recorder, e))
                throw std::runtime_error("raw recording write/flush failed at index " +
                                         std::to_string(e.record_index));
            std::lock_guard lock(shared.mutex);
            ++shared.status.written_messages;
            shared.status.progress.last_written_index = e.record_index;
        }
        if (!recorder.close())
            throw std::runtime_error("raw recording final flush/close failed");
    } catch (const std::exception& e) {
        shared.recording_failed(e.what());
        queue.close();
    }
    std::lock_guard lock(shared.mutex);
    shared.writer_done = true;
    shared.changed.notify_all();
}
std::string unpersisted_processed_range(const RuntimeStatus& status) {
    const auto& p = status.progress;
    if (!p.last_processed_index ||
        (p.last_written_index && *p.last_written_index >= *p.last_processed_index))
        return {};
    return "unpersisted_processed_range=" +
           std::to_string(p.last_written_index ? *p.last_written_index + 1 : 0) + "-" +
           std::to_string(*p.last_processed_index);
}
} // namespace pipeline
