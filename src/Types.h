#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cctype>
#include <cmath>
#include <algorithm>

// Lowercase last name, stripped of punctuation, with trailing generational
// suffixes (Jr./Sr./II/III/IV/V) removed first — shared by Player::
// lastNameKey() below and every CSV/API loader that needs to match a
// "Vladimir Guerrero Jr." style display name against it. Extracted into
// one function specifically because this logic used to be duplicated
// (with the same bug — matching on "Jr" instead of "Guerrero") in
// NetClient.h, NflverseLoader.h, and HoopRLoader.h independently. A
// single shared implementation means fixing it once actually fixes it
// everywhere, rather than needing the same fix applied in four places
// and hoping none get missed.
inline std::string lastNameKeyOf(const std::string& fullName) {
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : fullName) {
        if (c == ' ') { if (!cur.empty()) { tokens.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) tokens.push_back(cur);

    auto normalize = [](const std::string& s) {
        std::string out;
        for (char c : s) if (std::isalpha((unsigned char)c)) out += (char)std::tolower((unsigned char)c);
        return out;
    };

    while (!tokens.empty()) {
        std::string norm = normalize(tokens.back());
        if (norm == "jr" || norm == "sr" || norm == "ii" || norm == "iii" || norm == "iv" || norm == "v") {
            tokens.pop_back();
        } else break;
    }
    if (tokens.empty()) return normalize(fullName); // degenerate fallback (e.g. mononym)

    std::string out;
    for (char c : tokens.back()) {
        if (std::isalpha((unsigned char)c) || c == '\'' || c == '-') out += (char)std::tolower((unsigned char)c);
    }
    return out;
}

struct Player {
    std::string id;
    std::string name;
    std::string team;
    std::string position;
    std::string initials;
    std::string sport;
    // MLB only: "SP" (starting pitcher) or "Hitter". Empty/unused for the
    // other four leagues. Pure relief pitchers are excluded entirely
    // upstream (RosterLoader.h) rather than given a role here — the app
    // never shows an "RP" tab.
    std::string role;

    // Lowercase last name, stripped of punctuation and trailing
    // generational suffixes — see lastNameKeyOf() above, which this
    // delegates to so there's exactly one implementation of this logic
    // in the whole app.
    std::string lastNameKey() const { return lastNameKeyOf(name); }
};

struct StatCategory {
    std::string key;
    std::string label;
    double mean;
    double variance;
    // MLB only: which hydrate group (and which stat fields) this category
    // pulls from — "pitching" or "hitting". Irrelevant for the other
    // leagues, defaults to "pitching" so existing category lists that
    // don't set it (NBA/WNBA/NHL/NFL) keep compiling and behaving exactly
    // as before.
    std::string statGroup = "pitching";

    double propLine() const {
        return std::floor(mean) + 0.5;
    }
};

struct GameEntry {
    std::string date;
    std::string opponent;
    bool home = true;
    bool hasValue = false; // false = upcoming game, no result yet
    double value = 0.0;

    // yyyymmdd, used ONLY for sorting. APIs don't agree on game-log order
    // (MLB gives oldest-first, NHL gives newest-first) so every fetcher
    // fills this in and the caller sorts ascending before display — that's
    // what makes "most recent games" reliable regardless of source order.
    long sortKey = 0;
};

// A single opponent's standing on some stat, used to color a game as a
// favorable or challenging matchup for the active prop.
struct MatchupStat {
    std::string team;      // abbreviation
    double value = 0.0;    // raw stat, e.g. team K% against
    int percentile = 0;    // 1-99 among league teams, higher = tougher for the batter/pitcher's opponent
};

struct SportConfig {
    std::string name;
    std::string accentHex;
    int intervalDays;
    std::vector<StatCategory> categories;        // MLB: pitcher categories. Other leagues: their only category list.
    std::vector<Player> players;   // seed list — shown until the full-league roster finishes loading
    std::vector<std::string> teams; // every team abbreviation in the league
    bool liveSupported; // true for MLB/NHL, false for ESPN-backed leagues (no per-game log endpoint)
    // ESPN site-API path segment for this league, e.g. "basketball/nba".
    // Empty for MLB/NHL, which use their own dedicated roster APIs instead.
    std::string espnPath;
    // MLB only, appended at the end (not inserted earlier) so every
    // existing positional brace-init in SportsData.h for NBA/WNBA/NHL/NFL
    // keeps binding to the right fields — inserting a member in the
    // middle would silently shift all of those.
    std::vector<StatCategory> hitterCategories;
};
