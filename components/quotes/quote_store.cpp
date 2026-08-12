#include "quotes/quote_store.hpp"

namespace tab5::quotes {
namespace {

bool has_price(const std::optional<double>& value)
{
    return value.has_value();
}

bool update_is_older(const QuoteDelta& base, const QuoteDelta& update)
{
    if (base.sequence && update.sequence) {
        return *update.sequence < *base.sequence;
    }
    if (!base.sequence && !update.sequence && base.timestamp_ms && update.timestamp_ms) {
        return *update.timestamp_ms < *base.timestamp_ms;
    }
    return false;
}

} // namespace

std::optional<double> QuoteSnapshot::change() const
{
    if (!has_price(last_price) || !has_price(previous_close)) {
        return std::nullopt;
    }
    return *last_price - *previous_close;
}

std::optional<double> QuoteSnapshot::change_percent() const
{
    const auto absolute_change = change();
    if (!absolute_change.has_value() || !previous_close.has_value() || *previous_close == 0.0) {
        return std::nullopt;
    }
    return (*absolute_change / *previous_close) * 100.0;
}

QuoteDelta merge_delta(const QuoteDelta& base, const QuoteDelta& update)
{
    if (update_is_older(base, update)) {
        return base;
    }

    QuoteDelta merged = base;
    if (!update.symbol.empty()) {
        merged.symbol = update.symbol;
    }
    if (update.last_price) {
        merged.last_price = update.last_price;
    }
    if (update.previous_close) {
        merged.previous_close = update.previous_close;
    }
    if (update.open) {
        merged.open = update.open;
    }
    if (update.high) {
        merged.high = update.high;
    }
    if (update.low) {
        merged.low = update.low;
    }
    if (update.volume) {
        merged.volume = update.volume;
    }
    if (update.turnover) {
        merged.turnover = update.turnover;
    }
    if (update.timestamp_ms) {
        merged.timestamp_ms = update.timestamp_ms;
    }
    if (update.received_at_ms) {
        merged.received_at_ms = update.received_at_ms;
    }
    if (update.sequence) {
        merged.sequence = update.sequence;
    }
    if (update.trade_status) {
        merged.trade_status = update.trade_status;
    }
    if (update.session) {
        merged.session = update.session;
    }
    return merged;
}

void QuoteStore::set_watchlist(const Watchlist& watchlist)
{
    order_ = watchlist.symbols();
    for (const auto& symbol : order_) {
        const auto key = symbol.value();
        if (snapshots_.find(key) == snapshots_.end()) {
            QuoteSnapshot snapshot;
            snapshot.symbol = symbol;
            snapshot.stale = true;
            snapshots_[key] = snapshot;
        }
    }
}

void QuoteStore::apply_snapshot(const QuoteSnapshot& snapshot)
{
    if (snapshot.symbol.empty()) {
        return;
    }
    const auto key = snapshot.symbol.value();
    const auto existing = snapshots_.find(key);
    const std::uint64_t sequence =
        snapshot.sequence != 0 || existing == snapshots_.end() ? snapshot.sequence : existing->second.sequence;

    snapshots_[key] = snapshot;
    snapshots_[key].sequence = sequence;
    snapshots_[key].stale = false;
}

bool QuoteStore::apply_delta(const QuoteDelta& delta)
{
    if (delta.symbol.empty()) {
        return false;
    }

    auto& snapshot = snapshots_[delta.symbol.value()];
    if (snapshot.symbol.empty()) {
        snapshot.symbol = delta.symbol;
    }

    if (delta.sequence && snapshot.sequence != 0 && *delta.sequence <= snapshot.sequence) {
        return false;
    }
    if (!delta.sequence && delta.timestamp_ms && *delta.timestamp_ms < snapshot.timestamp_ms) {
        return false;
    }

    if (delta.last_price) {
        snapshot.last_price = delta.last_price;
    }
    if (delta.previous_close) {
        snapshot.previous_close = delta.previous_close;
    }
    if (delta.open) {
        snapshot.open = delta.open;
    }
    if (delta.high) {
        snapshot.high = delta.high;
    }
    if (delta.low) {
        snapshot.low = delta.low;
    }
    if (delta.volume) {
        snapshot.volume = delta.volume;
    }
    if (delta.turnover) {
        snapshot.turnover = delta.turnover;
    }
    if (delta.timestamp_ms) {
        snapshot.timestamp_ms = *delta.timestamp_ms;
    }
    if (delta.received_at_ms) {
        snapshot.received_at_ms = *delta.received_at_ms;
    }
    if (delta.sequence) {
        snapshot.sequence = *delta.sequence;
    }
    if (delta.trade_status) {
        snapshot.trade_status = *delta.trade_status;
    }
    if (delta.session) {
        snapshot.session = *delta.session;
    }
    snapshot.stale = false;
    return true;
}

bool QuoteStore::mark_stale_older_than(std::int64_t now_ms, std::int64_t stale_after_ms)
{
    bool changed = false;
    for (auto& entry : snapshots_) {
        auto& snapshot = entry.second;
        if (snapshot.received_at_ms <= 0 || now_ms - snapshot.received_at_ms > stale_after_ms) {
            if (!snapshot.stale) {
                changed = true;
                snapshot.stale = true;
            }
        }
    }
    return changed;
}

void QuoteStore::mark_all_stale()
{
    for (auto& entry : snapshots_) {
        entry.second.stale = true;
    }
}

void QuoteStore::clear()
{
    order_.clear();
    snapshots_.clear();
}

std::optional<QuoteSnapshot> QuoteStore::get(const SecuritySymbol& symbol) const
{
    const auto found = snapshots_.find(symbol.value());
    if (found == snapshots_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::vector<QuoteSnapshot> QuoteStore::ordered_snapshots() const
{
    std::vector<QuoteSnapshot> rows;
    rows.reserve(order_.empty() ? snapshots_.size() : order_.size());

    if (!order_.empty()) {
        for (const auto& symbol : order_) {
            const auto found = snapshots_.find(symbol.value());
            if (found != snapshots_.end()) {
                rows.push_back(found->second);
            }
        }
        return rows;
    }

    for (const auto& entry : snapshots_) {
        rows.push_back(entry.second);
    }
    return rows;
}

} // namespace tab5::quotes
