#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "adapters/coinbase/coinbase_subscription.hpp"
#include "adapters/coinbase/coinbase_websocket_client.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <doctest.h>
#include <filesystem>
using namespace adapters::coinbase;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using Error = boost::system::error_code;

TEST_CASE(
    "local TLS peer observes ordered subscriptions and normal close after callback rejection") {
    bool request_shutdown = false;
    SUBCASE("callback rejection stops reads") {}
    SUBCASE("shutdown with an outstanding read") { request_shutdown = true; }
    asio::io_context io;
    asio::ssl::context server_tls{asio::ssl::context::tls_server};
    server_tls.use_certificate_chain_file(std::string(TLS_FIXTURE_DIR) + "/localhost.crt");
    server_tls.use_private_key_file(std::string(TLS_FIXTURE_DIR) + "/localhost.key",
                                    asio::ssl::context::pem);
    asio::ssl::context client_tls{asio::ssl::context::tls_client};
    client_tls.load_verify_file(std::string(TLS_FIXTURE_DIR) + "/localhost.crt");
    tcp::acceptor acceptor{io, {asio::ip::make_address("127.0.0.1"), 0}};
    websocket::stream<beast::ssl_stream<tcp::socket>> peer{io, server_tls};
    beast::flat_buffer buffer;
    int writes_seen = 0, callbacks = 0;
    bool normal_close = false, timed_out = false;
    std::string failure;
    const std::string payload = R"({"channel":"heartbeats"})";
    asio::steady_timer deadline{io, std::chrono::seconds(5)};
    deadline.async_wait([&](Error ec) {
        if (!ec) {
            timed_out = true;
            io.stop();
        }
    });
    std::function<void()> receive;
    receive = [&] {
        peer.async_read(buffer, [&](Error ec, std::size_t) {
            if (ec == websocket::error::closed) {
                normal_close = peer.reason().code == websocket::close_code::normal;
                deadline.cancel();
                return;
            }
            REQUIRE_FALSE(ec);
            REQUIRE(writes_seen < 2);
            CHECK(beast::buffers_to_string(buffer.data()) == subscriptions[writes_seen]);
            ++writes_seen;
            buffer.consume(buffer.size());
            if (writes_seen < 2)
                receive();
            else
                peer.async_write(asio::buffer(payload), [&](Error ec, std::size_t) {
                    REQUIRE_FALSE(ec);
                    receive();
                });
        });
    };
    acceptor.async_accept(peer.next_layer().next_layer(), [&](Error ec) {
        REQUIRE_FALSE(ec);
        peer.next_layer().async_handshake(asio::ssl::stream_base::server, [&](Error ec) {
            REQUIRE_FALSE(ec);
            peer.async_accept([&](Error ec) {
                REQUIRE_FALSE(ec);
                receive();
            });
        });
    });
    CoinbaseWebSocketClient* active = nullptr;
    CoinbaseWebSocketClient client{io,
                                   client_tls,
                                   [&](auto raw, auto wall, auto mono) {
                                       ++callbacks;
                                       CHECK(raw == payload);
                                       CHECK(wall.nanoseconds > 0);
                                       CHECK(mono.nanoseconds > 0);
                                       if (request_shutdown)
                                           asio::post(io, [&] { active->close(); });
                                       return request_shutdown;
                                   },
                                   [&](auto error) {
                                       failure = error;
                                       deadline.cancel();
                                   },
                                   "localhost",
                                   std::to_string(acceptor.local_endpoint().port())};
    active = &client;
    client.connect();
    io.run();
    CHECK_FALSE(timed_out);
    CHECK(failure.empty());
    CHECK(writes_seen == 2);
    CHECK(callbacks == 1);
    CHECK(normal_close);
}
TEST_CASE("connection errors are surfaced once without delivering messages") {
    asio::io_context io;
    asio::ssl::context ssl{asio::ssl::context::tls_client};
    // Bound but not listening: no external service or DNS availability required.
    tcp::socket bound{io};
    bound.open(tcp::v4());
    bound.bind({asio::ip::make_address("127.0.0.1"), 0});
    const auto port = bound.local_endpoint().port();
    int errors = 0, messages = 0;
    CoinbaseWebSocketClient client{io,
                                   ssl,
                                   [&](auto, auto, auto) {
                                       ++messages;
                                       return true;
                                   },
                                   [&](auto error) {
                                       ++errors;
                                       CHECK(error.find("connect:") == 0);
                                   },
                                   "127.0.0.1",
                                   std::to_string(port)};
    client.connect();
    io.run();
    CHECK(errors == 1);
    CHECK(messages == 0);
}
