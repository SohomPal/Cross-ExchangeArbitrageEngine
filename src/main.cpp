#include "adapters/coinbase/coinbase_connection_manager.hpp"
#include "adapters/coinbase/coinbase_message_pipeline.hpp"
#include "recording/raw_event_reader.hpp"
#include "recording/raw_event_recorder.hpp"
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <sstream>

using namespace adapters::coinbase;
namespace asio = boost::asio;

static void display(const CoinbaseMessagePipeline& pipeline) {
    const auto& book = pipeline.book;
    const auto price = [](const auto& level) {
        if (!level)
            return std::string{"NA"};
        std::ostringstream out;
        const auto ticks = level->price.raw();
        out << ticks / 100 << '.' << std::setw(2) << std::setfill('0') << ticks % 100;
        return out.str();
    };
    const auto state = book.state() == core::BookState::Valid     ? "VALID"
                       : book.state() == core::BookState::Invalid ? "INVALID"
                                                                  : "INITIALIZING";
    std::cout << "venue=coinbase instrument=BTC_USD state=" << state << " sequence="
              << (book.last_sequence() ? std::to_string(*book.last_sequence()) : "NA")
              << " best_bid=" << price(book.best_bid()) << " best_ask=" << price(book.best_ask())
              << " bid_levels=" << book.bids().size() << " ask_levels=" << book.asks().size()
              << " raw_messages=" << pipeline.raw_messages
              << " parse_errors=" << pipeline.parse_errors << std::endl;
}

int main(int argc, char** argv) {
    try {
        std::string mode, path, venue = "coinbase", instrument = "BTC-USD";
        int force_seconds = 0;
        for (int i = 1; i < argc; i += 2) {
            if (i + 1 == argc)
                throw std::invalid_argument("option requires a value");
            const std::string_view option = argv[i];
            const std::string value = argv[i + 1];
            if (option == "--record" || option == "--replay" || option == "--output") {
                if (!path.empty())
                    throw std::invalid_argument("choose one recording or replay path");
                path = value;
                mode = option == "--replay" ? "replay" : "record";
            } else if (option == "--venue")
                venue = value;
            else if (option == "--instrument")
                instrument = value;
            else if (option == "--force-disconnect-after-seconds") {
                std::size_t consumed = 0;
                force_seconds = std::stoi(value, &consumed);
                if (force_seconds <= 0 || consumed != value.size())
                    throw std::invalid_argument("force disconnect seconds must be positive");
            } else
                throw std::invalid_argument("unknown option: " + std::string(option));
        }
        if (path.empty() || venue != "coinbase" || instrument != "BTC-USD" ||
            (mode == "replay" && force_seconds))
            throw std::invalid_argument(
                "Usage: arbitrage_engine --record NEW_FILE | --replay FILE "
                "[--venue coinbase --instrument BTC-USD] [--force-disconnect-after-seconds N]");
        if (mode == "replay") {
            recording::RawEventReader reader{path};
            CoinbaseMessagePipeline pipeline{[](auto, auto, auto) { return true; }};
            std::uint64_t connection_id = 0;
            while (auto record = reader.next()) {
                if (record->venue != core::Venue::Coinbase ||
                    record->connection_id < connection_id) {
                    pipeline.fail("expected ordered Coinbase connection IDs");
                    break;
                }
                if (record->connection_id != connection_id) {
                    connection_id = record->connection_id;
                    pipeline.reset_connection();
                }
                pipeline.process(record->payload, record->receive_wall_time,
                                 record->receive_monotonic_time);
            }
            if (reader.has_error())
                pipeline.fail(reader.error());
            display(pipeline);
            if (!pipeline.error.empty())
                std::cerr << pipeline.error << '\n';
            return pipeline.error.empty() ? 0 : 1;
        }
        recording::RawEventRecorder recorder{path};
        if (!recorder.flush())
            throw std::runtime_error("cannot open a new recording file");
        asio::io_context io;
        asio::ssl::context ssl{asio::ssl::context::tls_client};
        ssl.set_default_verify_paths();
        ssl.set_verify_mode(asio::ssl::verify_peer);
        asio::signal_set signals{io, SIGINT, SIGTERM};
        asio::steady_timer timer{io};
        bool stopping = false;
        auto stop_services = [&] {
            stopping = true;
            timer.cancel();
            signals.cancel();
        };
        auto now = [] {
            return core::ReceiveMonotonicTimestamp{
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count()};
        };
        std::unique_ptr<CoinbaseWebSocketClient> client;
        CoinbaseConnectionManager manager{
            io,
            [&](auto id, auto raw, auto wall, auto mono) {
                return recorder.append(core::Venue::Coinbase, id, wall, mono, raw) &&
                       recorder.flush();
            },
            [&](auto message, auto error, auto connected) {
                client = std::make_unique<CoinbaseWebSocketClient>(
                    io, ssl, std::move(message), std::move(error), "advanced-trade-ws.coinbase.com",
                    "443", std::move(connected));
                client->connect();
            },
            [&] {
                if (client)
                    client->abort();
            },
            now};
        asio::steady_timer fault_timer{io};
        if (force_seconds) {
            fault_timer.expires_after(std::chrono::seconds(force_seconds));
            fault_timer.async_wait([&](auto ec) {
                if (!ec)
                    manager.force_disconnect_for_test();
            });
        }
        auto status = [&] {
            const auto& book = manager.book();
            const char* state = "INVALID";
            switch (book.state()) {
            case core::BookState::Valid:
                state = "VALID";
                break;
            case core::BookState::Initializing:
                state = "INITIALIZING";
                break;
            case core::BookState::Disconnected:
                state = "DISCONNECTED";
                break;
            case core::BookState::Resyncing:
                state = "RESYNCING";
                break;
            case core::BookState::Stale:
                state = "STALE";
                break;
            case core::BookState::Invalid:
                break;
            }
            auto age = [&](auto timestamp) {
                return timestamp
                           ? std::to_string((now().nanoseconds - timestamp->nanoseconds) / 1000000)
                           : "NA";
            };
            auto price = [](auto level) {
                if (!level)
                    return std::string{"NA"};
                std::ostringstream out;
                out << std::fixed << std::setprecision(2) << level->price.raw() / 100.0;
                return out.str();
            };
            std::cout << "connection=" << (manager.connected() ? "CONNECTED" : "DISCONNECTED")
                      << " book=" << state << " connection_id=" << manager.connection_id()
                      << " sequence="
                      << (manager.sequence() ? std::to_string(*manager.sequence()) : "NA")
                      << " heartbeat_age_ms=" << age(manager.health().last_heartbeat)
                      << " l2_age_ms=" << age(manager.health().last_l2_message)
                      << " reconnects=" << manager.metrics().reconnect_attempts
                      << " gaps=" << manager.health().sequence_gaps
                      << " best_bid=" << price(book.best_bid())
                      << " best_ask=" << price(book.best_ask())
                      << " last_failure=" << manager.metrics().last_failure_reason << std::endl;
        };
        signals.async_wait([&](auto ec, int) {
            if (ec)
                return;
            stop_services();
            fault_timer.cancel();
            manager.stop();
        });
        std::function<void()> tick;
        tick = [&] {
            timer.expires_after(std::chrono::seconds(1));
            timer.async_wait([&](auto ec) {
                if (ec || stopping)
                    return;
                status();
                tick();
            });
        };
        tick();
        manager.start();
        io.run();
        const bool recorded = recorder.close();
        status();
        std::cout << "recorded_messages=" << recorder.next_record_index() << '\n';
        if (!recorded)
            std::cerr << "recording flush/close failed\n";
        return recorded ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
