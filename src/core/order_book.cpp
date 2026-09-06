#include "core/order_book.hpp"

namespace core {
namespace {

bool valid_level(const BookLevel& level, bool snapshot) {
    return (level.side == Side::Bid || level.side == Side::Ask) &&
           level.price.raw() > 0 &&
           (snapshot ? level.quantity.raw() > 0 : level.quantity.raw() >= 0);
}

template <typename Levels>
void apply_change(Levels& levels, const BookLevel& change) {
    if (change.quantity.raw() == 0) {
        levels.erase(change.price);
    } else {
        levels.insert_or_assign(change.price, change.quantity);
    }
}

} // namespace

OrderBook::OrderBook(Venue venue, Instrument instrument)
    : venue_(venue), instrument_(instrument) {}

bool OrderBook::apply(const BookSnapshot& snapshot) {
    if (snapshot.venue != venue_ || snapshot.instrument != instrument_) {
        return false;
    }

    BidLevels new_bids;
    AskLevels new_asks;
    for (const auto& level : snapshot.levels) {
        if (!valid_level(level, true)) {
            return false;
        }
        const bool inserted = level.side == Side::Bid
            ? new_bids.emplace(level.price, level.quantity).second
            : new_asks.emplace(level.price, level.quantity).second;
        if (!inserted) {
            return false;
        }
    }

    bids_.swap(new_bids);
    asks_.swap(new_asks);
    last_sequence_ = snapshot.sequence;
    state_ = BookState::Valid;
    return true;
}

bool OrderBook::apply(const BookUpdate& update) {
    if (state_ != BookState::Valid || update.venue != venue_ ||
        update.instrument != instrument_) {
        return false;
    }

    // Stage the entire batch, including allocations, before committing it.
    auto new_bids = bids_;
    auto new_asks = asks_;
    for (const auto& change : update.changes) {
        if (!valid_level(change, false)) {
            return false;
        }
        if (change.side == Side::Bid) {
            apply_change(new_bids, change);
        } else {
            apply_change(new_asks, change);
        }
    }

    bids_.swap(new_bids);
    asks_.swap(new_asks);
    last_sequence_ = update.sequence;
    return true;
}

Venue OrderBook::venue() const { return venue_; }
Instrument OrderBook::instrument() const { return instrument_; }
BookState OrderBook::state() const { return state_; }
const BidLevels& OrderBook::bids() const { return bids_; }
const AskLevels& OrderBook::asks() const { return asks_; }

std::optional<BookLevel> OrderBook::best_bid() const {
    if (bids_.empty()) {
        return std::nullopt;
    }
    const auto& [price, quantity] = *bids_.begin();
    return BookLevel{Side::Bid, price, quantity};
}

std::optional<BookLevel> OrderBook::best_ask() const {
    if (asks_.empty()) {
        return std::nullopt;
    }
    const auto& [price, quantity] = *asks_.begin();
    return BookLevel{Side::Ask, price, quantity};
}

bool OrderBook::has_two_sided_market() const {
    return !bids_.empty() && !asks_.empty();
}

std::optional<std::uint64_t> OrderBook::last_sequence() const {
    return last_sequence_;
}

} // namespace core
