#include "adapters/coinbase/coinbase_connection_manager.hpp"
#include "pipeline/joining_thread.hpp"
#include "pipeline/runtime_status.hpp"
#include <atomic>
#include <boost/asio/post.hpp>
#include <csignal>
#include <iostream>
#include <thread>
namespace pipeline {
namespace asio = boost::asio;
using namespace adapters::coinbase;
namespace {
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> stop_requested{false};
void signal_stop(int) { stop_requested.store(true, std::memory_order_relaxed); }
auto monotonic_now() {
    return core::ReceiveMonotonicTimestamp{std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count()};
}
const char* state_name(RuntimeState state) {
    switch (state) {
    case RuntimeState::Starting:
        return "Starting";
    case RuntimeState::Running:
        return "Running";
    case RuntimeState::Stopping:
        return "Stopping";
    case RuntimeState::Stopped:
        return "Stopped";
    case RuntimeState::StoppedRecordingError:
        return "StoppedRecordingError";
    case RuntimeState::StoppedQueueFull:
        return "StoppedQueueFull";
    case RuntimeState::StoppedProcessingError:
        return "StoppedProcessingError";
    }
    return "Unknown";
}
const char* book_name(core::BookState state) {
    switch (state) {
    case core::BookState::Initializing:
        return "INITIALIZING";
    case core::BookState::Valid:
        return "VALID";
    case core::BookState::Stale:
        return "STALE";
    case core::BookState::Resyncing:
        return "RESYNCING";
    case core::BookState::Disconnected:
        return "DISCONNECTED";
    case core::BookState::Invalid:
        return "INVALID";
    }
    return "UNKNOWN";
}
void display(const RuntimeStatus& s) {
    auto index = [](auto value) { return value ? std::to_string(*value) : "NA"; };
    auto price = [](auto value) { return value ? std::to_string(value->raw()) : "NA"; };
    std::cout << "runtime=" << state_name(s.runtime_state)
              << " connection=" << (s.connected ? "CONNECTED" : "DISCONNECTED")
              << " book=" << book_name(s.book_state) << " sequence=" << index(s.last_sequence)
              << " best_bid_ticks=" << price(s.best_bid) << " best_ask_ticks=" << price(s.best_ask)
              << " received=" << s.received_messages << " enqueued=" << s.enqueued_messages
              << " written=" << s.written_messages << " processed=" << s.processed_messages
              << " queue_messages=" << s.recording_queue_messages
              << " queue_bytes=" << s.recording_queue_bytes
              << " last_received_index=" << index(s.progress.last_received_index)
              << " last_enqueued_index=" << index(s.progress.last_enqueued_index)
              << " last_written_index=" << index(s.progress.last_written_index)
              << " last_processed_index=" << index(s.progress.last_processed_index);
    if (s.fatal_error)
        std::cout << " fatal_error=" << *s.fatal_error;
    auto tail = unpersisted_processed_range(s);
    if (!tail.empty())
        std::cout << ' ' << tail;
    std::cout << std::endl;
}
} // namespace
int run_live(const std::filesystem::path& path, int force_seconds, recording::RawQueueConfig config,
             std::uintmax_t minimum_free_disk_bytes) {
    SharedRuntime shared;
    recording::RawRecordingQueue queue{config};
    stop_requested = 0;
    auto old_int = std::signal(SIGINT, signal_stop);
    auto old_term = std::signal(SIGTERM, signal_stop);
    // Workers also join during exceptional unwinding. Close the queue if market creation fails.
    pipeline::JoiningThread writer(
        [&] { write_raw(queue, shared, path, minimum_free_disk_bytes); });
    try {
        pipeline::JoiningThread market([&] {
            try {
                asio::io_context io;
                asio::ssl::context ssl{asio::ssl::context::tls_client};
                ssl.set_default_verify_paths();
                ssl.set_verify_mode(asio::ssl::verify_peer);
                std::unique_ptr<CoinbaseWebSocketClient> client;
                CoinbaseConnectionManager manager{
                    io,
                    {},
                    [&](auto message, auto error, auto connected) {
                        client = std::make_unique<CoinbaseWebSocketClient>(
                            io, ssl, std::move(message), std::move(error),
                            "advanced-trade-ws.coinbase.com", "443", std::move(connected));
                        client->connect();
                    },
                    [&] {
                        if (client)
                            client->abort();
                    },
                    monotonic_now,
                    {},
                    {},
                    [&](auto envelope) { return queue.try_push(std::move(envelope)); },
                    [&](const ProcessResult& result, const RuntimeProgress& progress) {
                        const auto& book = manager.book();
                        auto bid = book.best_bid(), ask = book.best_ask();
                        auto messages = queue.message_count(), bytes = queue.payload_bytes();
                        std::lock_guard lock(shared.mutex);
                        auto& s = shared.status;
                        s.connected = manager.connected();
                        s.book_state = book.state();
                        s.bid_levels = book.bids().size();
                        s.ask_levels = book.asks().size();
                        s.last_sequence = manager.sequence();
                        s.best_bid = bid ? std::optional{bid->price} : std::nullopt;
                        s.best_ask = ask ? std::optional{ask->price} : std::nullopt;
                        s.recording_queue_messages = messages;
                        s.recording_queue_bytes = bytes;
                        ++s.received_messages;
                        if (result.outcome != MessageOutcome::RecordingRejected) {
                            ++s.enqueued_messages;
                            ++s.processed_messages;
                        } else if (!s.fatal_error) {
                            s.fatal_error = result.error;
                            s.runtime_state = result.error == "RAW_RECORDING_QUEUE_FULL"
                                                  ? RuntimeState::StoppedQueueFull
                                                  : RuntimeState::StoppedRecordingError;
                        }
                        s.progress.last_received_index = progress.last_received_index;
                        s.progress.last_enqueued_index = progress.last_enqueued_index;
                        s.progress.last_processed_index = progress.last_processed_index;
                    }};
                auto publish = [&] {
                    const auto& b = manager.book();
                    auto bid = b.best_bid(), ask = b.best_ask();
                    auto messages = queue.message_count(), bytes = queue.payload_bytes();
                    std::lock_guard lock(shared.mutex);
                    auto& s = shared.status;
                    s.connected = manager.connected();
                    s.book_state = b.state();
                    s.bid_levels = b.bids().size();
                    s.ask_levels = b.asks().size();
                    s.last_sequence = manager.sequence();
                    s.best_bid = bid ? std::optional{bid->price} : std::nullopt;
                    s.best_ask = ask ? std::optional{ask->price} : std::nullopt;
                    s.recording_queue_messages = messages;
                    s.recording_queue_bytes = bytes;
                };
                bool stopping = false;
                auto stop = [&](std::string error) {
                    // A writer can fail during drain after a normal stop request.
                    if (!error.empty())
                        manager.terminal_recording_failure(std::move(error));
                    else if (!stopping)
                        manager.stop();
                    if (!stopping) {
                        stopping = true;
                        queue.close();
                    }
                    publish();
                };
                {
                    std::unique_lock lock(shared.mutex);
                    shared.changed.wait(lock,
                                        [&] { return shared.writer_ready || shared.writer_done; });
                    shared.post_recording_failure = [&](std::string error) {
                        asio::post(io, [&, error = std::move(error)] { stop(error); });
                    };
                }
                // Clear posting while io and all callback owners are still alive, including on
                // throw.
                struct ClearPost {
                    SharedRuntime& s;
                    ~ClearPost() {
                        std::lock_guard lock(s.mutex);
                        s.post_recording_failure = {};
                    }
                } clear_post{shared};
                auto initial = shared.copy_status();
                if (initial.fatal_error)
                    stop(*initial.fatal_error);
                else if (stop_requested)
                    stop("");
                else {
                    {
                        std::lock_guard lock(shared.mutex);
                        shared.status.runtime_state = RuntimeState::Running;
                    }
                    manager.start();
                }
                asio::steady_timer timer{io}, fault{io};
                if (force_seconds && !stopping) {
                    fault.expires_after(std::chrono::seconds(force_seconds));
                    fault.async_wait([&](auto ec) {
                        if (!ec)
                            manager.force_disconnect_for_test();
                    });
                }
                std::function<void()> tick;
                tick = [&] {
                    auto current = shared.copy_status();
                    if (stop_requested || current.fatal_error || manager.terminal())
                        stop(current.fatal_error.value_or(""));
                    if (stopping) {
                        fault.cancel();
                        std::lock_guard lock(shared.mutex);
                        if (shared.writer_done)
                            return;
                        if (!shared.status.fatal_error)
                            shared.status.runtime_state = RuntimeState::Stopping;
                    }
                    publish();
                    timer.expires_after(std::chrono::milliseconds(50));
                    timer.async_wait([&](auto ec) {
                        if (!ec)
                            tick();
                    });
                };
                tick();
                io.run();
                publish();
            } catch (const std::exception& e) {
                queue.close();
                std::lock_guard lock(shared.mutex);
                shared.post_recording_failure = {};
                if (!shared.status.fatal_error) {
                    shared.status.fatal_error = e.what();
                    shared.status.runtime_state = RuntimeState::StoppedProcessingError;
                }
                shared.status.connected = false;
                shared.status.book_state = core::BookState::Invalid;
                shared.status.best_bid.reset();
                shared.status.best_ask.reset();
            }
            queue.close();
            std::lock_guard lock(shared.mutex);
            shared.market_done = true;
            shared.changed.notify_all();
        });
        {
            std::unique_lock lock(shared.mutex);
            while (!shared.market_done) {
                if (shared.changed.wait_for(lock, std::chrono::seconds(1),
                                            [&] { return shared.market_done; }))
                    break;
                auto status = shared.status;
                lock.unlock();
                display(status);
                lock.lock();
            }
        }
        market.join();
    } catch (...) {
        queue.close();
        writer.join();
        std::signal(SIGINT, old_int);
        std::signal(SIGTERM, old_term);
        throw;
    }
    writer.join();
    std::signal(SIGINT, old_int);
    std::signal(SIGTERM, old_term);
    {
        std::lock_guard lock(shared.mutex);
        if (!shared.status.fatal_error)
            shared.status.runtime_state = RuntimeState::Stopped;
        shared.status.recording_queue_messages = queue.message_count();
        shared.status.recording_queue_bytes = queue.payload_bytes();
    }
    auto final = shared.copy_status();
    display(final);
    return final.fatal_error ? 1 : 0;
}
} // namespace pipeline
