#include "adapters/coinbase/coinbase_connection_manager.hpp"
#include "adapters/coinbase/coinbase_message_pipeline.hpp"
#include "pipeline/runtime_status.hpp"
#include "recording/raw_event_reader.hpp"
#include "recording/raw_event_recorder.hpp"
#include "session/session.hpp"
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

using namespace adapters::coinbase;

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
        auto positive = [](const std::string& value) {
            std::size_t consumed = 0;
            if (value.empty() || value.front() == '-')
                throw std::invalid_argument("expected positive integer");
            auto number = std::stoull(value, &consumed);
            if (!number || consumed != value.size())
                throw std::invalid_argument("expected positive integer");
            return number;
        };
        if (argc > 1 &&
            (std::string_view(argv[1]) == "replay" || std::string_view(argv[1]) == "capture")) {
            const bool replay = std::string_view(argv[1]) == "replay";
            std::string directory, output, capture_venue = "coinbase",
                                           capture_instrument = "BTC_USD";
            std::size_t capture_depth = 100;
            int duration_seconds = 0;
            bool allow_incomplete = false, include_final_book = false;
            recording::RawQueueConfig capture_queue;
            std::uintmax_t capture_minimum_disk = 64 * 1024 * 1024;
            int capture_force_seconds = 0;
            for (int i = 2; i < argc; ++i) {
                std::string option = argv[i];
                if (!replay && i + 1 < argc &&
                    (option == "--venue" || option == "--instrument" || option == "--depth" ||
                     option == "--duration-seconds")) {
                    std::string value = argv[++i];
                    if (option == "--venue")
                        capture_venue = value;
                    else if (option == "--instrument")
                        capture_instrument = value;
                    else if (option == "--depth")
                        capture_depth = positive(value);
                    else {
                        auto n = positive(value);
                        if (n > std::numeric_limits<int>::max())
                            throw std::invalid_argument("duration too large");
                        duration_seconds = static_cast<int>(n);
                    }
                } else if (option == "--allow-incomplete" && replay)
                    allow_incomplete = true;
                else if (option == "--include-final-book" && replay)
                    include_final_book = true;
                else if ((option == "--session" || (option == "--output" && replay)) &&
                         i + 1 < argc) {
                    auto& destination = option == "--session" ? directory : output;
                    if (!destination.empty())
                        throw std::invalid_argument("duplicate option");
                    destination = argv[++i];
                } else if (!replay && i + 1 < argc &&
                           (option == "--queue-messages" || option == "--queue-bytes" ||
                            option == "--minimum-free-disk-bytes" ||
                            option == "--force-disconnect-after-seconds")) {
                    auto value = positive(argv[++i]);
                    if (option == "--queue-messages")
                        capture_queue.maximum_messages = value;
                    else if (option == "--queue-bytes")
                        capture_queue.maximum_bytes = value;
                    else if (option == "--minimum-free-disk-bytes")
                        capture_minimum_disk = value;
                    else {
                        if (value > static_cast<unsigned>(std::numeric_limits<int>::max()))
                            throw std::invalid_argument("disconnect delay too large");
                        capture_force_seconds = static_cast<int>(value);
                    }
                } else
                    throw std::invalid_argument("unknown or missing session option: " + option);
            }
            if (!replay && directory.empty()) {
                auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
                directory = "data/sessions/session-" + std::to_string(ns);
            }
            if (directory.empty() || (replay && output.empty()))
                throw std::invalid_argument(
                    "capture --session NEW_DIRECTORY | replay --session DIRECTORY --output "
                    "NEW_RESULT [--allow-incomplete] [--include-final-book]");
            if (replay)
                return sessions::replay(directory, output, allow_incomplete, include_final_book);
            if (capture_instrument != "BTC_USD" &&
                !(capture_venue == "coinbase" && capture_instrument == "BTC-USD"))
                throw std::invalid_argument("unsupported instrument");
            if (capture_venue != "kraken" && (duration_seconds || capture_depth != 100))
                throw std::invalid_argument("depth and duration require Kraken capture");
            sessions::Capture capture(directory, capture_venue, capture_depth);
            if (capture_venue == "kraken")
                return pipeline::run_kraken(capture, capture_depth, duration_seconds, capture_queue,
                                            capture_minimum_disk, capture_force_seconds);
            return pipeline::run_live(capture.raw_path(), capture_force_seconds, capture_queue,
                                      capture_minimum_disk, &capture);
        }
        std::string mode, path, venue = "coinbase", instrument = "BTC-USD";
        int force_seconds = 0;
        recording::RawQueueConfig queue_config;
        std::uintmax_t minimum_free_disk_bytes = 64 * 1024 * 1024;
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
            } else if (option == "--queue-messages")
                queue_config.maximum_messages = positive(value);
            else if (option == "--queue-bytes")
                queue_config.maximum_bytes = positive(value);
            else if (option == "--minimum-free-disk-bytes")
                minimum_free_disk_bytes = positive(value);
            else if (option == "--venue")
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
        return pipeline::run_live(path, force_seconds, queue_config, minimum_free_disk_bytes);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
