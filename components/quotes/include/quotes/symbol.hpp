#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace tab5::quotes {

enum class MarketRegion {
    Us,
    Hk,
    Shanghai,
    Shenzhen,
};

std::string to_string(MarketRegion region);
std::optional<MarketRegion> parse_region(const std::string& value);

class SecuritySymbol {
public:
    SecuritySymbol() = default;
    SecuritySymbol(std::string ticker, MarketRegion region);

    static std::optional<SecuritySymbol> parse(const std::string& input);

    const std::string& ticker() const { return ticker_; }
    MarketRegion region() const { return region_; }
    std::string value() const;
    bool empty() const { return ticker_.empty(); }

    bool operator==(const SecuritySymbol& other) const;
    bool operator<(const SecuritySymbol& other) const;

private:
    std::string ticker_;
    MarketRegion region_ { MarketRegion::Us };
};

enum class WatchlistAddResult {
    Added,
    AlreadyExists,
    Full,
    Invalid,
};

class Watchlist {
public:
    static constexpr std::size_t kMaxSymbols = 500;

    Watchlist() = default;
    explicit Watchlist(std::vector<SecuritySymbol> symbols);

    WatchlistAddResult add(const SecuritySymbol& symbol);
    bool remove(const SecuritySymbol& symbol);
    bool move(std::size_t from_index, std::size_t to_index);
    bool contains(const SecuritySymbol& symbol) const;
    void clear();

    std::size_t size() const { return symbols_.size(); }
    bool empty() const { return symbols_.empty(); }
    const std::vector<SecuritySymbol>& symbols() const { return symbols_; }

    std::string serialize() const;
    static Watchlist deserialize(const std::string& serialized);

private:
    std::vector<SecuritySymbol> symbols_;
};

} // namespace tab5::quotes
