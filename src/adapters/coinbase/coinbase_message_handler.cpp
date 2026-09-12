#include "adapters/coinbase/coinbase_message_handler.hpp"
#include "adapters/coinbase/coinbase_heartbeat_parser.hpp"
namespace adapters::coinbase {
const char* outcome_name(MessageOutcome outcome) {
    switch (outcome) {
    case MessageOutcome::BookUpdated:
        return "BookUpdated";
    case MessageOutcome::Ignored:
        return "Ignored";
    case MessageOutcome::Duplicate:
        return "Duplicate";
    case MessageOutcome::OutOfOrder:
        return "OutOfOrder";
    case MessageOutcome::SequenceGap:
        return "SequenceGap";
    case MessageOutcome::ParseError:
        return "ParseError";
    case MessageOutcome::RecordingRejected:
        return "RecordingRejected";
    case MessageOutcome::ApplyError:
        return "ApplyError";
    }
    return "Unknown";
}
ProcessResult CoinbaseMessageHandler::handle(std::string payload, core::ReceiveWallTimestamp wall,
                                             core::ReceiveMonotonicTimestamp mono) {
    const auto index = next_index_++;
    progress.last_received_index = index;
    ProcessResult result;
    try {
        auto envelope = std::make_shared<const recording::RawEnvelope>(recording::RawEnvelope{
            index, core::Venue::Coinbase, connection_id, wall, mono, std::move(payload)});
        std::uint64_t enqueue_ns = 0;
        core::StageTimer enqueue(profile_stages ? &enqueue_ns : nullptr);
        auto pushed = sink_(envelope);
        enqueue.stop();
        if (pushed != recording::RawRecordingQueue::PushResult::Accepted) {
            book_.invalidate();
            result.outcome = MessageOutcome::RecordingRejected;
            result.error = pushed == recording::RawRecordingQueue::PushResult::Full
                               ? "RAW_RECORDING_QUEUE_FULL"
                               : "RAW_RECORDING_QUEUE_CLOSED";
        } else {
            progress.last_enqueued_index = index;
            result = process(*envelope);
            progress.last_processed_index = index;
        }
        result.stages.enqueue_ns = enqueue_ns;
    } catch (const std::exception& e) {
        book_.invalidate();
        result.outcome = progress.last_enqueued_index == index ? MessageOutcome::ApplyError
                                                               : MessageOutcome::RecordingRejected;
        if (progress.last_enqueued_index == index)
            progress.last_processed_index = index;
        result.error = e.what();
    }
    {
        core::StageTimer publication(profile_stages ? &result.stages.status_publish_ns : nullptr);
        if (publish_)
            publish_(result, progress);
    }
    return result;
}
ProcessResult CoinbaseMessageHandler::process(const recording::RawEnvelope& envelope) {
    ProcessResult result;
    auto fail = [&](MessageOutcome outcome, std::string error) {
        book_.invalidate();
        result.outcome = outcome;
        result.error = std::move(error);
        return result;
    };
    health_.last_any_message = envelope.receive_monotonic_time;
    const auto parsed = parser_.parse(envelope.payload, envelope.receive_wall_time,
                                      envelope.receive_monotonic_time,
                                      profile_stages ? &result.stages : nullptr, false);
    if (parsed.status == ParseStatus::Error)
        return fail(MessageOutcome::ParseError, parsed.error);
    result.canonical_event_count = parsed.events.size();
    for (const auto& event : parsed.events) {
        if (const auto* snapshot = std::get_if<core::BookSnapshot>(&event)) {
            result.contains_snapshot = true;
            result.number_of_changes += snapshot->levels.size();
        } else {
            result.number_of_changes += std::get<core::BookUpdate>(event).changes.size();
        }
    }
    core::StageTimer sequence_timer(profile_stages ? &result.stages.sequence_validation_ns : nullptr);
    if (parsed.sequence) {
        switch (sequence_.observe(*parsed.sequence)) {
        case core::SequenceResult::Duplicate:
            sequence_timer.stop();
            ++health_.duplicate_sequences;
            result.outcome = MessageOutcome::Duplicate;
            return result;
        case core::SequenceResult::OutOfOrder:
            sequence_timer.stop();
            ++health_.out_of_order_sequences;
            result.outcome = MessageOutcome::OutOfOrder;
            return result;
        case core::SequenceResult::Gap:
            sequence_timer.stop();
            ++health_.sequence_gaps;
            return fail(MessageOutcome::SequenceGap, "Coinbase envelope sequence gap");
        default:
            break;
        }
    }
    sequence_timer.stop();
    if (parsed.heartbeat_channel) {
        core::StageTimer heartbeat_timer(profile_stages ? &result.stages.json_parse_ns : nullptr);
        const auto heartbeat = parse_heartbeat(envelope.payload, envelope.receive_wall_time,
                                                envelope.receive_monotonic_time);
        heartbeat_timer.stop();
        if (heartbeat) {
            health_.last_heartbeat = heartbeat->receive_monotonic_time;
            ++health_.heartbeat_count;
        }
        return result;
    }
    if (parsed.status == ParseStatus::Ignored)
        return result;
    health_.last_l2_message = envelope.receive_monotonic_time;
    if (book_.state() != core::BookState::Valid &&
        (parsed.events.empty() ||
         !std::holds_alternative<core::BookSnapshot>(parsed.events.front())))
        return fail(MessageOutcome::ApplyError, "L2 update before snapshot");
    if (parsed.events.empty())
        return result;
    core::StageTimer application(profile_stages ? &result.stages.book_apply_ns : nullptr);
    if (!book_.apply(parsed.events)) {
        application.stop();
        return fail(MessageOutcome::ApplyError, "book rejected event");
    }
    for (const auto& event : parsed.events) {
        if (std::holds_alternative<core::BookSnapshot>(event))
            ++result.snapshots;
        else
            ++result.updates;
    }
    application.stop();
    // Capture only after the entire transaction has installed levels, sequence and state.
    const auto updated = std::chrono::steady_clock::now();
    result.latency =
        std::chrono::duration_cast<std::chrono::nanoseconds>(updated.time_since_epoch()) -
        std::chrono::nanoseconds{envelope.receive_monotonic_time.nanoseconds};
    result.outcome = MessageOutcome::BookUpdated;
    return result;
}
} // namespace adapters::coinbase
