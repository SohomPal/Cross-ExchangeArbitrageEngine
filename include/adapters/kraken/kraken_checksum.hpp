#pragma once
#include "adapters/kraken/kraken_book_message.hpp"
#include <map>
namespace adapters::kraken {
std::string checksum_digits(std::string_view token);
class KrakenChecksumBook {
  public:
    void replace_snapshot(const std::vector<KrakenLevelChange>&);
    void apply(const KrakenLevelChange&);
    void truncate(std::size_t depth);
    std::uint32_t checksum() const;
    std::vector<core::BookLevel> levels() const;

  private:
    std::map<core::PriceTicks, KrakenLevelChange, std::greater<core::PriceTicks>> bids_;
    std::map<core::PriceTicks, KrakenLevelChange> asks_;
};
} // namespace adapters::kraken
