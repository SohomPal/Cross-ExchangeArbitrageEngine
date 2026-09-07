#include "adapters/coinbase/coinbase_websocket_client.hpp"
#include "adapters/coinbase/coinbase_subscription.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>

namespace adapters::coinbase {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using Error = boost::system::error_code;
struct CoinbaseWebSocketClient::Impl : std::enable_shared_from_this<Impl> {
    std::string host, port;
    tcp::resolver resolver;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws;
    beast::flat_buffer buffer;
    MessageHandler message;
    ErrorHandler error;
    bool started{false}, stopping{false}, writing{false}, closing{false};
    Impl(asio::io_context& io, asio::ssl::context& ssl, MessageHandler m, ErrorHandler e,
         std::string h, std::string p)
        : host(std::move(h)), port(std::move(p)), resolver(io), ws(io, ssl), message(std::move(m)),
          error(std::move(e)) {}
    void abort_socket() {
        Error ignored;
        beast::get_lowest_layer(ws).socket().close(ignored);
    }
    void fail(std::string_view op, Error ec) {
        if (stopping)
            return;
        stopping = true;
        resolver.cancel();
        abort_socket();
        error(std::string(op) + ": " + ec.message());
    }
    void connect() {
        if (started || stopping)
            return;
        started = true;
        ws.next_layer().set_verify_mode(asio::ssl::verify_peer);
        ws.next_layer().set_verify_callback(asio::ssl::host_name_verification(host));
        if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host.c_str())) {
            fail("SNI",
                 Error(static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category()));
            return;
        }
        resolver.async_resolve(host, port, [self = shared_from_this()](Error ec, auto endpoints) {
            if (self->stopping)
                return;
            if (ec)
                return self->fail("resolve", ec);
            beast::get_lowest_layer(self->ws).expires_after(std::chrono::seconds(30));
            beast::get_lowest_layer(self->ws).async_connect(endpoints, [self](Error ec, auto) {
                if (self->stopping)
                    return;
                if (ec)
                    return self->fail("connect", ec);
                self->ws.next_layer().async_handshake(
                    asio::ssl::stream_base::client, [self](Error ec) {
                        if (self->stopping)
                            return;
                        if (ec)
                            return self->fail("TLS handshake", ec);
                        beast::get_lowest_layer(self->ws).expires_never();
                        self->ws.set_option(
                            websocket::stream_base::timeout::suggested(beast::role_type::client));
                        self->ws.async_handshake(self->host, "/", [self](Error ec) {
                            if (self->stopping)
                                return;
                            if (ec)
                                return self->fail("WebSocket handshake", ec);
                            self->ws.text(true);
                            self->subscribe(0);
                        });
                    });
            });
        });
    }
    void subscribe(std::size_t index) {
        writing = true;
        ws.async_write(asio::buffer(subscriptions[index]),
                       [self = shared_from_this(), index](Error ec, std::size_t) {
                           self->writing = false;
                           if (self->stopping)
                               return self->close();
                           if (ec)
                               return self->fail("subscribe", ec);
                           if (index + 1 < subscriptions.size())
                               self->subscribe(index + 1);
                           else
                               self->read();
                       });
    }
    void read() {
        if (stopping)
            return;
        ws.async_read(buffer, [self = shared_from_this()](Error ec, std::size_t) {
            if (ec) {
                if (!self->stopping)
                    self->fail("read", ec);
                return;
            }
            using namespace std::chrono;
            const core::ReceiveWallTimestamp wall{
                duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count()};
            const core::ReceiveMonotonicTimestamp mono{
                duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count()};
            const auto payload = beast::buffers_to_string(self->buffer.data());
            const bool proceed = self->message(payload, wall, mono);
            self->buffer.consume(self->buffer.size());
            if (!proceed)
                self->close();
            else
                self->read();
        });
    }
    void close() {
        stopping = true;
        resolver.cancel();
        if (writing || closing)
            return;
        if (!ws.is_open())
            return abort_socket();
        closing = true;
        ws.async_close(websocket::close_code::normal, [self = shared_from_this()](Error ec) {
            self->abort_socket();
            if (ec && ec != websocket::error::closed && ec != asio::error::operation_aborted)
                self->error("close: " + ec.message());
        });
    }
};
CoinbaseWebSocketClient::CoinbaseWebSocketClient(asio::io_context& io, asio::ssl::context& ssl,
                                                 MessageHandler m, ErrorHandler e, std::string host,
                                                 std::string port)
    : impl_(std::make_shared<Impl>(io, ssl, std::move(m), std::move(e), std::move(host),
                                   std::move(port))) {}
CoinbaseWebSocketClient::~CoinbaseWebSocketClient() = default;
void CoinbaseWebSocketClient::connect() { impl_->connect(); }
void CoinbaseWebSocketClient::close() { impl_->close(); }
} // namespace adapters::coinbase
