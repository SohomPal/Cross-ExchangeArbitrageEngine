#include "adapters/kraken/kraken_symbol_mapper.hpp"
#include <stdexcept>
namespace adapters::kraken {
core::Instrument map_symbol(std::string_view symbol) {
    if (symbol != "BTC/USD")
        throw std::invalid_argument("unsupported Kraken symbol");
    return core::Instrument::BTC_USD;
}
} // namespace adapters::kraken
