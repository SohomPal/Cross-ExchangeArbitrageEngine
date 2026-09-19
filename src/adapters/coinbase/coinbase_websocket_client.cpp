#include "adapters/coinbase/coinbase_websocket_client.hpp"
#include "adapters/coinbase/coinbase_subscription.hpp"
namespace adapters::coinbase {
CoinbaseWebSocketClient::CoinbaseWebSocketClient(boost::asio::io_context& io,
                                                 boost::asio::ssl::context& ssl,
                                                 MessageHandler message, ErrorHandler error,
                                                 std::string host, std::string port,
                                                 std::function<void()> connected)
    : WebSocketClient(io, ssl, std::move(message), std::move(error), std::move(host),
                      std::move(port), std::move(connected), "/",
                      {std::string(subscriptions[0]), std::string(subscriptions[1])}) {}
} // namespace adapters::coinbase
