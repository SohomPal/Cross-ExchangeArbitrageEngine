#pragma once
#include <array>
#include <string_view>
namespace adapters::coinbase {
inline constexpr std::array<std::string_view, 2> subscriptions{
    R"({"type":"subscribe","channel":"level2","product_ids":["BTC-USD"]})",
    R"({"type":"subscribe","channel":"heartbeats"})"};
}
