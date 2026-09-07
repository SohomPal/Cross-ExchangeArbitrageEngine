#pragma once
#include "core/timestamp.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
namespace adapters::coinbase {
// Run all methods and callbacks on the single io_context thread.
class CoinbaseWebSocketClient {
  public:
    using MessageHandler = std::function<bool(std::string_view, core::ReceiveWallTimestamp,
                                              core::ReceiveMonotonicTimestamp)>;
    using ErrorHandler = std::function<void(std::string_view)>;
    CoinbaseWebSocketClient(boost::asio::io_context&, boost::asio::ssl::context&, MessageHandler,
                            ErrorHandler, std::string host = "advanced-trade-ws.coinbase.com",
                            std::string port = "443");
    ~CoinbaseWebSocketClient();
    void connect();
    void close();

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace adapters::coinbase
