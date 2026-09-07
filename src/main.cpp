#include "adapters/coinbase/coinbase_message_pipeline.hpp"
#include "adapters/coinbase/coinbase_websocket_client.hpp"
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
    if (argc != 3 ||
        (std::string_view(argv[1]) != "--record" && std::string_view(argv[1]) != "--replay")) {
        std::cerr << "Usage: arbitrage_engine --record NEW_FILE.jsonl | --replay FILE.jsonl\n";
        return 2;
    }
    try {
        if (std::string_view(argv[1]) == "--replay") {
            recording::RawEventReader reader{argv[2]};
            CoinbaseMessagePipeline pipeline{[](auto, auto, auto) { return true; }};
            while (auto record = reader.next()) {
                if (record->venue != core::Venue::Coinbase || record->connection_id != 1) {
                    pipeline.fail("expected a single Coinbase connection with id 1");
                    break;
                }
                if (!pipeline.process(record->payload, record->receive_wall_time,
                                      record->receive_monotonic_time))
                    break;
            }
            if (reader.has_error())
                pipeline.fail(reader.error());
            display(pipeline);
            if (!pipeline.error.empty())
                std::cerr << pipeline.error << '\n';
            return pipeline.error.empty() ? 0 : 1;
        }
        recording::RawEventRecorder recorder{argv[2]};
        if (!recorder.flush())
            throw std::runtime_error("cannot open a new recording file");
        CoinbaseMessagePipeline pipeline{[&](auto raw, auto wall, auto mono) {
            return recorder.append(core::Venue::Coinbase, 1, wall, mono, raw);
        }};
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
        CoinbaseWebSocketClient client{io, ssl,
                                       [&](auto raw, auto wall, auto mono) {
                                           if (pipeline.process(raw, wall, mono))
                                               return true;
                                           stop_services();
                                           return false;
                                       },
                                       [&](auto error) {
                                           pipeline.fail(error);
                                           stop_services();
                                       }};
        signals.async_wait([&](auto ec, int) {
            if (ec)
                return;
            stop_services();
            client.close();
        });
        std::function<void()> tick;
        tick = [&] {
            timer.expires_after(std::chrono::seconds(1));
            timer.async_wait([&](auto ec) {
                if (ec || stopping)
                    return;
                display(pipeline);
                tick();
            });
        };
        tick();
        client.connect();
        io.run();
        if (!recorder.close())
            pipeline.fail("recording flush/close failed");
        display(pipeline);
        std::cout << "recorded_messages=" << recorder.next_record_index() << '\n';
        if (!pipeline.error.empty())
            std::cerr << pipeline.error << '\n';
        return pipeline.error.empty() ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
