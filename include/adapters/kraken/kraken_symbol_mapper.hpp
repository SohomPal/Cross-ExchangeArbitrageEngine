#pragma once
#include "core/instrument.hpp"
#include <string_view>
namespace adapters::kraken {
core::Instrument map_symbol(std::string_view symbol);
}
