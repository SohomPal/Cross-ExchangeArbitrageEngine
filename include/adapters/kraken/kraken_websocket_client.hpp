#pragma once
#include "transport/websocket_client.hpp"
namespace adapters::kraken {
class KrakenWebSocketClient : public transport::WebSocketClient {
  public:
    KrakenWebSocketClient(boost::asio::io_context&, boost::asio::ssl::context&, MessageHandler,
                          ErrorHandler, std::function<void()> connected = {},
                          std::size_t depth = 100);
};
} // namespace adapters::kraken
