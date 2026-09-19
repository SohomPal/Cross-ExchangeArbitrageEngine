#include "adapters/kraken/kraken_book_processor.hpp"
#include "adapters/kraken/kraken_websocket_client.hpp"
#include "pipeline/joining_thread.hpp"
#include "pipeline/runtime_status.hpp"
#include "session/session.hpp"
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <csignal>
#include <iostream>
namespace pipeline {
namespace {
volatile std::sig_atomic_t kraken_stop = 0;
void request_stop(int) { kraken_stop = 1; }
core::ReceiveMonotonicTimestamp now() {
    return {std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count()};
}
} // namespace
int run_kraken(sessions::Capture& capture, std::size_t depth, int duration,
               recording::RawQueueConfig config, std::uintmax_t minimum_disk, int force_seconds) {
    namespace asio = boost::asio;
    using namespace adapters::kraken;
    SharedRuntime shared;
    recording::RawRecordingQueue queue(config);
    kraken_stop = 0;
    auto old_int = std::signal(SIGINT, request_stop), old_term = std::signal(SIGTERM, request_stop);
    nlohmann::json final_metrics = nlohmann::json::object();
    std::uint32_t checksum = 0;
    std::string final_book_hash;
    JoiningThread writer([&] { write_raw(queue, shared, capture.raw_path(), minimum_disk); });
    try {
        JoiningThread market([&] {
            try {
                asio::io_context io;
                asio::ssl::context ssl(asio::ssl::context::tls_client);
                ssl.set_default_verify_paths();
                ssl.set_verify_mode(asio::ssl::verify_peer);
                KrakenBookProcessor processor(depth);
                std::unique_ptr<KrakenWebSocketClient> client;
                asio::steady_timer timer(io), retry(io);
                adapters::coinbase::ReconnectPolicy backoff;
                std::uint64_t index = 0, connection = 0;
                bool stopping = false, recovering = false, connected = false, first_record = true;
                bool seen_record = false, forced = false;
                auto started = now(), connected_at = started;
                auto publish = [&] {
                    const auto& b = processor.book();
                    std::lock_guard lock(shared.mutex);
                    auto& s = shared.status;
                    s.connected = connected;
                    s.book_state = b.state();
                    s.bid_levels = b.bids().size();
                    s.ask_levels = b.asks().size();
                    s.best_bid = b.best_bid() ? std::optional{b.best_bid()->price} : std::nullopt;
                    s.best_ask = b.best_ask() ? std::optional{b.best_ask()->price} : std::nullopt;
                    checksum = processor.checksum();
                    final_metrics = processor.metrics();
                };
                auto stop = [&] {
                    stopping = true;
                    connected = false;
                    if (client)
                        client->abort();
                    retry.cancel();
                    queue.close();
                };
                std::function<void()> connect;
                auto recover = [&](std::string reason, bool stale = false) {
                    if (stopping || recovering)
                        return;
                    std::cerr << "Kraken recovery: " << reason << std::endl;
                    recovering = true;
                    connected = false;
                    if (stale)
                        processor.stale();
                    else
                        processor.disconnect();
                    {
                        std::lock_guard lock(shared.mutex);
                        shared.status.lifecycle_events.push_back(
                            {index, processor.book().state(), reason});
                    }
                    if (client)
                        client->abort();
                    retry.expires_after(backoff.next_delay());
                    retry.async_wait([&](auto ec) {
                        if (!ec && !stopping)
                            connect();
                    });
                    publish();
                };
                connect = [&] {
                    recovering = false;
                    first_record = true;
                    ++connection;
                    connected_at = now();
                    client = std::make_unique<KrakenWebSocketClient>(
                        io, ssl,
                        [&](std::string payload, auto wall, auto mono) {
                            if (stopping)
                                return false;
                            auto e = std::make_shared<const recording::RawEnvelope>(
                                recording::RawEnvelope{index, core::Venue::Kraken, connection, wall,
                                                       mono, std::move(payload)});
                            auto accepted = queue.try_push(e);
                            {
                                std::lock_guard lock(shared.mutex);
                                auto& s = shared.status;
                                ++s.received_messages;
                                s.progress.last_received_index = index;
                                if (accepted !=
                                    recording::RawRecordingQueue::PushResult::Accepted) {
                                    if (!s.fatal_error) {
                                        s.fatal_error = "RAW_RECORDING_QUEUE_FULL";
                                        s.runtime_state = RuntimeState::StoppedQueueFull;
                                    }
                                } else {
                                    ++s.enqueued_messages;
                                    s.progress.last_enqueued_index = index;
                                }
                            }
                            if (accepted != recording::RawRecordingQueue::PushResult::Accepted) {
                                processor.invalidate();
                                stop();
                                publish();
                                return false;
                            }
                            if (first_record) {
                                if (seen_record)
                                    processor.reset_connection();
                                first_record = false;
                                seen_record = true;
                            }
                            bool ok = processor.process(e->payload, wall, mono);
                            {
                                std::lock_guard lock(shared.mutex);
                                ++shared.status.processed_messages;
                                shared.status.progress.last_processed_index = index++;
                            }
                            publish();
                            if (!ok)
                                recover(processor.error.empty() ? processor.outcome
                                                                : processor.error);
                            else if (processor.book().state() == core::BookState::Valid)
                                backoff.reset();
                            return ok;
                        },
                        [&](std::string_view error) { recover(std::string(error)); },
                        [&] {
                            connected = true;
                            connected_at = now();
                        },
                        depth);
                    client->connect();
                };
                {
                    std::unique_lock lock(shared.mutex);
                    shared.changed.wait(lock,
                                        [&] { return shared.writer_ready || shared.writer_done; });
                }
                if (shared.copy_status().fatal_error)
                    stop();
                else {
                    {
                        std::lock_guard lock(shared.mutex);
                        shared.status.runtime_state = RuntimeState::Running;
                    }
                    connect();
                }
                std::function<void()> tick;
                tick = [&] {
                    auto t = now();
                    if (shared.copy_status().fatal_error)
                        processor.invalidate();
                    if (kraken_stop || shared.copy_status().fatal_error ||
                        (duration && t.nanoseconds - started.nanoseconds >=
                                         std::int64_t(duration) * 1000000000))
                        stop();
                    if (stopping) {
                        publish();
                        return;
                    }
                    if (!recovering && force_seconds && !forced &&
                        t.nanoseconds - started.nanoseconds >=
                            std::int64_t(force_seconds) * 1000000000) {
                        forced = true;
                        recover("forced_disconnect");
                    }
                    if (connected && !recovering) {
                        auto last = first_record ? connected_at
                                                 : processor.last_message.value_or(connected_at);
                        // Active book messages establish liveness even when heartbeats are
                        // suppressed.
                        if (t.nanoseconds - last.nanoseconds > 5000000000LL)
                            recover("message_timeout", true);
                        else if (processor.book().state() != core::BookState::Valid &&
                                 t.nanoseconds - connected_at.nanoseconds > 15000000000LL)
                            recover("snapshot_timeout");
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
                const auto& book = processor.book();
                const char* state = book.state() == core::BookState::Valid          ? "VALID"
                                    : book.state() == core::BookState::Initializing ? "INITIALIZING"
                                    : book.state() == core::BookState::Disconnected ? "DISCONNECTED"
                                    : book.state() == core::BookState::Stale        ? "STALE"
                                                                                    : "INVALID";
                nlohmann::json contents = {{"bids", nlohmann::json::array()},
                                           {"asks", nlohmann::json::array()},
                                           {"state", state},
                                           {"book_sequence", nullptr}};
                for (auto [p, q] : book.bids())
                    contents["bids"].push_back({p.raw(), q.raw()});
                for (auto [p, q] : book.asks())
                    contents["asks"].push_back({p.raw(), q.raw()});
                final_book_hash = sessions::sha256(contents.dump());
            } catch (const std::exception& e) {
                std::lock_guard lock(shared.mutex);
                shared.status.fatal_error = e.what();
                shared.status.runtime_state = RuntimeState::StoppedProcessingError;
                shared.status.book_state = core::BookState::Invalid;
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
                auto& s = shared.status;
                const char* state = s.book_state == core::BookState::Valid          ? "VALID"
                                    : s.book_state == core::BookState::Stale        ? "STALE"
                                    : s.book_state == core::BookState::Initializing ? "INITIALIZING"
                                                                                    : "INVALID";
                std::cout << "venue=kraken instrument=BTC_USD book=" << state
                          << " checksum=" << checksum << " checksum_failures="
                          << final_metrics.value("kraken_checksum_failures", std::uint64_t{0})
                          << " best_bid=" << (s.best_bid ? std::to_string(s.best_bid->raw()) : "NA")
                          << " best_ask=" << (s.best_ask ? std::to_string(s.best_ask->raw()) : "NA")
                          << " bid_levels=" << s.bid_levels << " ask_levels=" << s.ask_levels
                          << std::endl;
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
    auto final = shared.copy_status();
    if (!final.fatal_error)
        final.runtime_state = RuntimeState::Stopped;
    capture.set_metrics(final_metrics);
    if (!final_book_hash.empty())
        capture.set_final_book_hash(final_book_hash);
    capture.finalize(final);
    if (final.fatal_error)
        std::cerr << *final.fatal_error << '\n';
    return final.fatal_error ? 1 : 0;
}
} // namespace pipeline
