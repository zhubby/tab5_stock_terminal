#pragma once

#include "quotes/symbol.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tab5::quotes {

struct QuoteSnapshot {
    SecuritySymbol symbol;
    std::optional<double> last_price;
    std::optional<double> previous_close;
    std::optional<double> open;
    std::optional<double> high;
    std::optional<double> low;
    std::optional<double> volume;
    std::optional<double> turnover;
    std::int64_t timestamp_ms { 0 };
    std::int64_t received_at_ms { 0 };
    std::uint64_t sequence { 0 };
    std::string trade_status;
    std::string session;
    bool stale { false };

    std::optional<double> change() const;
    std::optional<double> change_percent() const;
};

struct QuoteDelta {
    SecuritySymbol symbol;
    std::optional<double> last_price;
    std::optional<double> previous_close;
    std::optional<double> open;
    std::optional<double> high;
    std::optional<double> low;
    std::optional<double> volume;
    std::optional<double> turnover;
    std::optional<std::int64_t> timestamp_ms;
    std::optional<std::int64_t> received_at_ms;
    std::optional<std::uint64_t> sequence;
    std::optional<std::string> trade_status;
    std::optional<std::string> session;
};

QuoteDelta merge_delta(const QuoteDelta& base, const QuoteDelta& update);

class QuoteStore {
public:
    void set_watchlist(const Watchlist& watchlist);
    void apply_snapshot(const QuoteSnapshot& snapshot);
    bool apply_delta(const QuoteDelta& delta);
    bool mark_stale_older_than(std::int64_t now_ms, std::int64_t stale_after_ms);
    void mark_all_stale();
    void clear();

    std::optional<QuoteSnapshot> get(const SecuritySymbol& symbol) const;
    std::vector<QuoteSnapshot> ordered_snapshots() const;
    std::size_t size() const { return snapshots_.size(); }

private:
    std::vector<SecuritySymbol> order_;
    std::map<std::string, QuoteSnapshot> snapshots_;
};

} // namespace tab5::quotes
