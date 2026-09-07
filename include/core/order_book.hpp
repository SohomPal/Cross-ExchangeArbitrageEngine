#pragma once

#include "core/book_state.hpp"
#include "core/market_event.hpp"

#include <functional>
#include <map>

namespace core {

using BidLevels = std::map<PriceTicks, QuantityAtoms, std::greater<PriceTicks>>;
using AskLevels = std::map<PriceTicks, QuantityAtoms, std::less<PriceTicks>>;

class OrderBook {
public:
    OrderBook(Venue venue, Instrument instrument);

    // Rejection preserves levels, sequence, and state. Snapshots reject duplicate
    // (side, price) entries; updates process repeated entries in event order.
    bool apply(const BookSnapshot& snapshot);
    bool apply(const BookUpdate& update);
    void mark_initializing() { state_ = BookState::Initializing; }
    void mark_stale() { state_ = BookState::Stale; }
    void mark_resyncing() { state_ = BookState::Resyncing; }
    void mark_disconnected() { state_ = BookState::Disconnected; }
    void invalidate() { state_ = BookState::Invalid; }

    [[nodiscard]] Venue venue() const;
    [[nodiscard]] Instrument instrument() const;
    [[nodiscard]] BookState state() const;
    [[nodiscard]] std::optional<BookLevel> best_bid() const;
    [[nodiscard]] std::optional<BookLevel> best_ask() const;
    [[nodiscard]] const BidLevels& bids() const;
    [[nodiscard]] const AskLevels& asks() const;
    [[nodiscard]] bool has_two_sided_market() const;
    [[nodiscard]] std::optional<std::uint64_t> last_sequence() const;

private:
    Venue venue_;
    Instrument instrument_;
    BookState state_{BookState::Initializing};
    BidLevels bids_;
    AskLevels asks_;
    std::optional<std::uint64_t> last_sequence_;
};

} // namespace core
