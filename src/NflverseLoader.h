#pragma once
// NFL — nflverse player_stats.csv
//
// ESPN's site API (used for NFL rosters) has no per-game log endpoint, so
// it never could power the chart — that's the "not working" API this
// replaces. nflverse (https://github.com/nflverse) is a community-
// maintained, MIT/CC0-licensed project that republishes real official NFL
// stats derived from nflfastR's play-by-play data. It's not a live query
// API — it's one big CSV covering every player, every week, back to 1999
// — downloaded and verified to exist and match this exact schema via a
// direct byte-range fetch during development (not guessed from docs).
//
// Because it's one big file rather than a per-player endpoint, this is
// downloaded ONCE at startup (~33MB) and kept in memory for the rest of
// the session — see loadBackgroundData() in main.cpp — rather than
// re-fetched on every player switch like MLB/NHL/NBA/WNBA.
#include "Types.h"
#include "NetClient.h"
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <map>

struct NflStatRow {
    std::string playerDisplayName;
    std::string position;
    std::string team;
    int season = 0;
    int week = 0;
    std::string seasonType;
    std::string opponentTeam;
    double receptions = 0, receivingYards = 0, receivingTds = 0;
    double carries = 0, rushingYards = 0, rushingTds = 0;
};

namespace NflverseLoader {

    // Minimal RFC4180-ish CSV line splitter. The dataset's fields were
    // checked directly (see NetClient comment above) and don't contain
    // embedded commas/quotes in the columns this app reads, but this
    // handles quoted fields defensively anyway rather than assuming that
    // holds for every row across 25+ years of data.
    inline std::vector<std::string> splitCsvLine(const std::string& line) {
        std::vector<std::string> out;
        std::string field;
        bool inQuotes = false;
        for (size_t i = 0; i < line.size(); i++) {
            char c = line[i];
            if (inQuotes) {
                if (c == '"') {
                    if (i + 1 < line.size() && line[i + 1] == '"') { field += '"'; i++; }
                    else inQuotes = false;
                } else field += c;
            } else {
                if (c == '"') inQuotes = true;
                else if (c == ',') { out.push_back(field); field.clear(); }
                else field += c;
            }
        }
        out.push_back(field);
        return out;
    }

    inline double toDouble(const std::string& s) {
        if (s.empty()) return 0.0;
        try { return std::stod(s); } catch (...) { return 0.0; }
    }

    // Downloads and parses the full nflverse player_stats.csv, keeping only
    // skill-position (QB/RB/WR/TE) regular-season rows — the ones this app
    // actually has categories for. Filtering here rather than keeping every
    // row (kickers, defensive stats, preseason, etc.) keeps memory
    // reasonable given this is ~500k+ total rows across 25+ seasons.
    inline std::vector<NflStatRow> fetchAllPlayerStats() {
        std::string csv = NetClient::httpGet(
            "https://github.com/nflverse/nflverse-data/releases/download/player_stats/player_stats.csv");

        std::vector<NflStatRow> out;
        std::istringstream stream(csv);
        std::string line;

        if (!std::getline(stream, line)) throw std::runtime_error("nflverse CSV: empty response");
        std::vector<std::string> headers = splitCsvLine(line);
        std::map<std::string, int> col;
        for (size_t i = 0; i < headers.size(); i++) col[headers[i]] = (int)i;

        auto need = [&](const char* name) {
            auto it = col.find(name);
            if (it == col.end()) throw std::runtime_error(std::string("nflverse CSV: missing column ") + name);
            return it->second;
        };
        int cName = need("player_display_name"), cPos = need("position"), cTeam = need("recent_team");
        int cSeason = need("season"), cWeek = need("week"), cType = need("season_type"), cOpp = need("opponent_team");
        int cRec = need("receptions"), cRecYds = need("receiving_yards"), cRecTd = need("receiving_tds");
        int cCarries = need("carries"), cRushYds = need("rushing_yards"), cRushTd = need("rushing_tds");

        while (std::getline(stream, line)) {
            if (line.empty()) continue;
            std::vector<std::string> f = splitCsvLine(line);
            if ((int)f.size() <= cRushTd) continue; // malformed/short row — skip rather than crash

            const std::string& pos = f[cPos];
            if (pos != "QB" && pos != "RB" && pos != "WR" && pos != "TE") continue;
            if (f[cType] != "REG") continue; // regular season only — matches what the app's prop lines assume

            NflStatRow row;
            row.playerDisplayName = f[cName];
            row.position = pos;
            row.team = f[cTeam];
            row.season = (int)toDouble(f[cSeason]);
            row.week = (int)toDouble(f[cWeek]);
            row.seasonType = f[cType];
            row.opponentTeam = f[cOpp];
            row.receptions = toDouble(f[cRec]);
            row.receivingYards = toDouble(f[cRecYds]);
            row.receivingTds = toDouble(f[cRecTd]);
            row.carries = toDouble(f[cCarries]);
            row.rushingYards = toDouble(f[cRushYds]);
            row.rushingTds = toDouble(f[cRushTd]);
            out.push_back(std::move(row));
        }
        return out;
    }

    // Suffix-aware last-name extraction now lives in Types.h as
    // lastNameKeyOf() — shared with Player::lastNameKey() and HoopRLoader.h
    // so the fix (stripping Jr./Sr./II/III/IV before taking the surname)
    // only had to happen in one place.

    // Finds this player's most recent season in the dataset (rather than
    // assuming "this year" — as of a given run, the current NFL season may
    // not have started yet, so "most recent season this player has any
    // rows for" self-adjusts across the calendar without a hardcoded year).
    inline std::vector<GameEntry> lookupGameLog(const std::vector<NflStatRow>& all, const Player& player, const StatCategory& cat) {
        std::string key = player.lastNameKey();
        int bestSeason = -1;
        for (const auto& r : all) {
            if (lastNameKeyOf(r.playerDisplayName) == key && r.season > bestSeason) bestSeason = r.season;
        }
        if (bestSeason == -1) throw std::runtime_error("nflverse: player not found: " + player.name);

        std::vector<const NflStatRow*> rows;
        for (const auto& r : all) {
            if (lastNameKeyOf(r.playerDisplayName) == key && r.season == bestSeason) rows.push_back(&r);
        }
        std::sort(rows.begin(), rows.end(), [](const NflStatRow* a, const NflStatRow* b) { return a->week < b->week; });

        std::vector<GameEntry> games;
        for (const auto* r : rows) {
            double value;
            if (cat.key == "recyds") value = r->receivingYards;
            else if (cat.key == "rec") value = r->receptions;
            else if (cat.key == "rushyds") value = r->rushingYards;
            else if (cat.key == "td") value = r->receivingTds + r->rushingTds;
            else throw std::runtime_error("nflverse: stat not mapped: " + cat.key);

            GameEntry entry;
            entry.date = "Wk " + std::to_string(r->week);
            entry.opponent = r->opponentTeam.empty() ? "???" : r->opponentTeam;
            // This dataset doesn't include a home/away column, so it can't
            // be determined from this source — defaulting to true (shown
            // as "vs") rather than fabricating an away/home split that
            // isn't backed by real data.
            entry.home = true;
            entry.hasValue = true;
            entry.value = value;
            entry.sortKey = r->season * 100 + r->week;
            games.push_back(entry);
        }
        if (games.empty()) throw std::runtime_error("nflverse: no regular-season rows for " + player.name);
        return games;
    }
}
