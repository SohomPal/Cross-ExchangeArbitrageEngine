#pragma once
#include "transport/websocket_client.hpp"
namespace adapters::coinbase {
class CoinbaseWebSocketClient : public transport::WebSocketClient {
  public:
    CoinbaseWebSocketClient(boost::asio::io_context&, boost::asio::ssl::context&, MessageHandler,
                            ErrorHandler, std::string host = "advanced-trade-ws.coinbase.com",
                            std::string port = "443", std::function<void()> connected = {});
};
} // namespace adapters::coinbase
