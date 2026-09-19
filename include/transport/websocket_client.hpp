#pragma once
#include "core/timestamp.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
namespace transport {
// Run all methods and callbacks on the single io_context thread.
class WebSocketClient {
  public:
    using MessageHandler = std::function<bool(std::string, core::ReceiveWallTimestamp,
                                              core::ReceiveMonotonicTimestamp)>;
    using ErrorHandler = std::function<void(std::string_view)>;
    WebSocketClient(boost::asio::io_context&, boost::asio::ssl::context&, MessageHandler,
                    ErrorHandler, std::string host, std::string port,
                    std::function<void()> connected, std::string target,
                    std::vector<std::string> requests);
    ~WebSocketClient();
    void connect();
    void close();
    // Cancel immediately during recovery, including an outstanding handshake/read.
    void abort();

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace transport
