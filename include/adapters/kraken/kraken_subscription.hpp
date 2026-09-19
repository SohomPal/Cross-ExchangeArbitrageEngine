#pragma once
#include <cstddef>
#include <string>
namespace adapters::kraken {
void validate_depth(std::size_t depth);
std::string subscription(std::size_t depth = 100);
} // namespace adapters::kraken
