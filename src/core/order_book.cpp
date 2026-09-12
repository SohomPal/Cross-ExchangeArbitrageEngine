#include "core/order_book.hpp"

namespace core {
namespace {

bool valid_level(const BookLevel& level, bool snapshot) {
    return (level.side == Side::Bid || level.side == Side::Ask) &&
           level.price.raw() > 0 &&
           (snapshot ? level.quantity.raw() > 0 : level.quantity.raw() >= 0);
}

// Stage only touched prices, including zero-quantity tombstones. All allocations
// precede commit; node transfer uses equal allocators and nonthrowing comparisons.
bool stage_update(const BookUpdate& update, Venue venue, Instrument instrument,
                  BidLevels& bids, AskLevels& asks) {
    if (update.venue != venue || update.instrument != instrument)
        return false;
    for (const auto& change : update.changes) {
        if (!valid_level(change, false))
            return false;
        if (change.side == Side::Bid)
            bids.insert_or_assign(change.price, change.quantity);
        else
            asks.insert_or_assign(change.price, change.quantity);
    }
    return true;
}
template <typename Levels>
void commit_changes(Levels& levels, Levels& changes) {
    while (!changes.empty()) {
        auto node = changes.extract(changes.begin());
        const auto found = levels.find(node.key());
        if (node.mapped().raw() == 0) {
            if (found != levels.end())
                levels.erase(found);
        } else if (found != levels.end()) {
            found->second = node.mapped();
        } else {
            levels.insert(std::move(node));
        }
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
    BidLevels bids;
    AskLevels asks;
    if (state_ != BookState::Valid ||
        !stage_update(update, venue_, instrument_, bids, asks))
        return false;
    commit_changes(bids_, bids);
    commit_changes(asks_, asks);
    last_sequence_ = update.sequence;
    return true;
}

bool OrderBook::apply(std::span<const MarketEvent> events) {
    if (events.empty())
        return true;
    for (const auto& event : events) {
        if (std::holds_alternative<BookSnapshot>(event)) {
            // Snapshot envelopes are rare and retain transactional replacement.
            auto staged = *this;
            for (const auto& value : events)
                if (!std::visit([&](const auto& e) { return staged.apply(e); }, value))
                    return false;
            *this = std::move(staged);
            return true;
        }
    }
    if (state_ != BookState::Valid)
        return false;
    BidLevels bids;
    AskLevels asks;
    for (const auto& event : events)
        if (!stage_update(std::get<BookUpdate>(event), venue_, instrument_, bids, asks))
            return false;
    commit_changes(bids_, bids);
    commit_changes(asks_, asks);
    last_sequence_ = std::get<BookUpdate>(events.back()).sequence;
    return true;
}

Venue OrderBook::venue() const { return venue_; }
Instrument OrderBook::instrument() const { return instrument_; }
BookState OrderBook::state() const { return state_; }
const BidLevels& OrderBook::bids() const { return bids_; }
const AskLevels& OrderBook::asks() const { return asks_; }

std::optional<BookLevel> OrderBook::best_bid() const {
    if (state_ != BookState::Valid || bids_.empty()) {
        return std::nullopt;
    }
    const auto& [price, quantity] = *bids_.begin();
    return BookLevel{Side::Bid, price, quantity};
}

std::optional<BookLevel> OrderBook::best_ask() const {
    if (state_ != BookState::Valid || asks_.empty()) {
        return std::nullopt;
    }
    const auto& [price, quantity] = *asks_.begin();
    return BookLevel{Side::Ask, price, quantity};
}

bool OrderBook::has_two_sided_market() const {
    return state_ == BookState::Valid && !bids_.empty() && !asks_.empty();
}

std::optional<std::uint64_t> OrderBook::last_sequence() const {
    return last_sequence_;
}

} // namespace core
