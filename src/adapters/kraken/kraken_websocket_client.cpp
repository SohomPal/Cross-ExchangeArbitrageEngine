#include "adapters/kraken/kraken_websocket_client.hpp"
#include "adapters/kraken/kraken_subscription.hpp"
namespace adapters::kraken {
KrakenWebSocketClient::KrakenWebSocketClient(boost::asio::io_context& io,
                                             boost::asio::ssl::context& ssl, MessageHandler message,
                                             ErrorHandler error, std::function<void()> connected,
                                             std::size_t depth)
    : WebSocketClient(io, ssl, std::move(message), std::move(error), "ws.kraken.com", "443",
                      std::move(connected), "/v2", {subscription(depth)}) {}
} // namespace adapters::kraken
