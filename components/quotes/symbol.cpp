#include "quotes/symbol.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace tab5::quotes {
namespace {

std::string trim(const std::string& input)
{
    auto begin = input.begin();
    while (begin != input.end() && std::isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }

    auto end = input.end();
    while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }

    return std::string(begin, end);
}

std::string upper(std::string input)
{
    std::transform(input.begin(), input.end(), input.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return input;
}

bool valid_ticker(const std::string& ticker)
{
    if (ticker.empty() || ticker.size() > 16) {
        return false;
    }

    for (char ch : ticker) {
        const bool ok = std::isalnum(static_cast<unsigned char>(ch)) || ch == '.' || ch == '-';
        if (!ok) {
            return false;
        }
    }

    return ticker.front() != '.' && ticker.back() != '.' && ticker.front() != '-' && ticker.back() != '-';
}

} // namespace

std::string to_string(MarketRegion region)
{
    switch (region) {
    case MarketRegion::Us:
        return "US";
    case MarketRegion::Hk:
        return "HK";
    case MarketRegion::Shanghai:
        return "SH";
    case MarketRegion::Shenzhen:
        return "SZ";
    }
    return "US";
}

std::optional<MarketRegion> parse_region(const std::string& value)
{
    const std::string normalized = upper(trim(value));
    if (normalized == "US") {
        return MarketRegion::Us;
    }
    if (normalized == "HK") {
        return MarketRegion::Hk;
    }
    if (normalized == "SH") {
        return MarketRegion::Shanghai;
    }
    if (normalized == "SZ") {
        return MarketRegion::Shenzhen;
    }
    return std::nullopt;
}

SecuritySymbol::SecuritySymbol(std::string ticker, MarketRegion region)
    : ticker_(upper(trim(std::move(ticker))))
    , region_(region)
{
}

std::optional<SecuritySymbol> SecuritySymbol::parse(const std::string& input)
{
    const std::string normalized = upper(trim(input));
    const auto dot = normalized.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot == normalized.size() - 1) {
        return std::nullopt;
    }

    const std::string ticker = normalized.substr(0, dot);
    const std::string region_text = normalized.substr(dot + 1);
    const auto region = parse_region(region_text);
    if (!region.has_value() || !valid_ticker(ticker)) {
        return std::nullopt;
    }

    return SecuritySymbol(ticker, *region);
}

std::string SecuritySymbol::value() const
{
    if (ticker_.empty()) {
        return {};
    }
    return ticker_ + "." + to_string(region_);
}

bool SecuritySymbol::operator==(const SecuritySymbol& other) const
{
    return ticker_ == other.ticker_ && region_ == other.region_;
}

bool SecuritySymbol::operator<(const SecuritySymbol& other) const
{
    return value() < other.value();
}

Watchlist::Watchlist(std::vector<SecuritySymbol> symbols)
{
    for (const auto& symbol : symbols) {
        add(symbol);
    }
}

WatchlistAddResult Watchlist::add(const SecuritySymbol& symbol)
{
    if (symbol.empty()) {
        return WatchlistAddResult::Invalid;
    }
    if (contains(symbol)) {
        return WatchlistAddResult::AlreadyExists;
    }
    if (symbols_.size() >= kMaxSymbols) {
        return WatchlistAddResult::Full;
    }

    symbols_.push_back(symbol);
    return WatchlistAddResult::Added;
}

bool Watchlist::remove(const SecuritySymbol& symbol)
{
    const auto before = symbols_.size();
    symbols_.erase(std::remove(symbols_.begin(), symbols_.end(), symbol), symbols_.end());
    return symbols_.size() != before;
}

bool Watchlist::move(std::size_t from_index, std::size_t to_index)
{
    if (from_index >= symbols_.size() || to_index >= symbols_.size() || from_index == to_index) {
        return false;
    }

    auto symbol = symbols_[from_index];
    symbols_.erase(symbols_.begin() + static_cast<std::ptrdiff_t>(from_index));
    symbols_.insert(symbols_.begin() + static_cast<std::ptrdiff_t>(to_index), symbol);
    return true;
}

bool Watchlist::contains(const SecuritySymbol& symbol) const
{
    return std::find(symbols_.begin(), symbols_.end(), symbol) != symbols_.end();
}

void Watchlist::clear()
{
    symbols_.clear();
}

std::string Watchlist::serialize() const
{
    std::ostringstream out;
    for (std::size_t i = 0; i < symbols_.size(); ++i) {
        if (i != 0) {
            out << '\n';
        }
        out << symbols_[i].value();
    }
    return out.str();
}

Watchlist Watchlist::deserialize(const std::string& serialized)
{
    Watchlist watchlist;
    std::istringstream in(serialized);
    std::string line;

    while (std::getline(in, line)) {
        if (auto symbol = SecuritySymbol::parse(line)) {
            watchlist.add(*symbol);
        }
    }

    return watchlist;
}

} // namespace tab5::quotes
