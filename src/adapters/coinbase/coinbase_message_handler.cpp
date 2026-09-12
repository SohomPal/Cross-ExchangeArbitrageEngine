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
        auto pushed = sink_(envelope);
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
    } catch (const std::exception& e) {
        book_.invalidate();
        result.outcome = progress.last_enqueued_index == index ? MessageOutcome::ApplyError
                                                               : MessageOutcome::RecordingRejected;
        if (progress.last_enqueued_index == index)
            progress.last_processed_index = index;
        result.error = e.what();
    }
    if (publish_)
        publish_(result, progress);
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
                                      envelope.receive_monotonic_time);
    if (parsed.status == ParseStatus::Error)
        return fail(MessageOutcome::ParseError, parsed.error);
    result.canonical_event_count = parsed.events.size();
    if (parsed.sequence) {
        switch (sequence_.observe(*parsed.sequence)) {
        case core::SequenceResult::Duplicate:
            ++health_.duplicate_sequences;
            result.outcome = MessageOutcome::Duplicate;
            return result;
        case core::SequenceResult::OutOfOrder:
            ++health_.out_of_order_sequences;
            result.outcome = MessageOutcome::OutOfOrder;
            return result;
        case core::SequenceResult::Gap:
            ++health_.sequence_gaps;
            return fail(MessageOutcome::SequenceGap, "Coinbase envelope sequence gap");
        default:
            break;
        }
    }
    if (auto heartbeat = parse_heartbeat(envelope.payload, envelope.receive_wall_time,
                                         envelope.receive_monotonic_time)) {
        health_.last_heartbeat = heartbeat->receive_monotonic_time;
        ++health_.heartbeat_count;
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
    auto staged = book_;
    for (const auto& event : parsed.events) {
        if (std::holds_alternative<core::BookUpdate>(event) &&
            staged.state() != core::BookState::Valid)
            return fail(MessageOutcome::ApplyError, "L2 update before snapshot");
        if (!std::visit([&](const auto& e) { return staged.apply(e); }, event))
            return fail(MessageOutcome::ApplyError, "book rejected event");
        if (std::holds_alternative<core::BookSnapshot>(event))
            ++result.snapshots;
        else
            ++result.updates;
    }
    book_ = std::move(staged);
    // Capture only after the entire transaction has installed levels, sequence and state.
    const auto updated = std::chrono::steady_clock::now();
    result.latency =
        std::chrono::duration_cast<std::chrono::nanoseconds>(updated.time_since_epoch()) -
        std::chrono::nanoseconds{envelope.receive_monotonic_time.nanoseconds};
    result.outcome = MessageOutcome::BookUpdated;
    return result;
}
} // namespace adapters::coinbase
