#pragma once
// NBA / WNBA — sportsdataverse ESPN player boxscore CSVs
//
// Replaces an earlier attempt at stats.nba.com/stats.wnba.com directly.
// That endpoint is real but notoriously flaky — bot protection and
// timeouts even with correct headers are a known, common problem with it
// (confirmed via multiple independent bug reports during development).
//
// This is a better source: sportsdataverse (the same organization behind
// nflverse) publishes per-season ESPN-derived player box scores as GitHub
// release assets. Verified directly during development, not assumed from
// docs — fetched the real 2026 files for both leagues, confirmed identical
// schema, confirmed real current-season rows (including real home/away and
// opponent fields neither ESPN's live site API nor stats.nba.com's
// MATCHUP-string format gave directly).
//
// One file per season (~2MB WNBA, ~18MB NBA) rather than one file for all
// history like NFL's — small enough this re-downloads once per league at
// startup, same caching pattern as NflverseLoader.h.
#include "Types.h"
#include "NetClient.h"
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <map>

struct HoopBoxRow {
    std::string athleteDisplayName;
    std::string teamAbbrev;
    std::string opponentAbbrev;
    std::string gameDate; // "2026-06-13"
    int season = 0;
    int seasonType = 0; // ESPN convention: 1 preseason, 2 regular season, 3 postseason
    bool home = false;
    double points = 0, rebounds = 0, assists = 0;
};

namespace HoopRLoader {

    // Same minimal CSV splitter as NflverseLoader.h — duplicated rather
    // than shared to keep each loader file self-contained and avoid
    // touching the already-working NFL loader while adding this.
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

    // league: "nba" or "wnba" (lowercase, matches the release tag naming).
    // season: e.g. 2026 — ESPN/sportsdataverse label a season by its ending
    // year, so NBA's 2025-26 season and WNBA's calendar-year 2026 season
    // are both just "2026" here.
    inline std::vector<HoopBoxRow> fetchPlayerBoxScores(const std::string& league, int season) {
        std::string url = "https://github.com/sportsdataverse/sportsdataverse-data/releases/download/espn_"
            + league + "_player_boxscores/player_box_" + std::to_string(season) + ".csv";
        std::string csv = NetClient::httpGet(url);

        std::vector<HoopBoxRow> out;
        std::istringstream stream(csv);
        std::string line;
        if (!std::getline(stream, line)) throw std::runtime_error(league + " boxscore CSV: empty response");
        std::vector<std::string> headers = splitCsvLine(line);
        std::map<std::string, int> col;
        for (size_t i = 0; i < headers.size(); i++) col[headers[i]] = (int)i;

        auto need = [&](const char* name) {
            auto it = col.find(name);
            if (it == col.end()) throw std::runtime_error(league + " boxscore CSV: missing column " + name);
            return it->second;
        };
        int cName = need("athlete_display_name"), cTeam = need("team_abbreviation");
        int cOpp = need("opponent_team_abbreviation"), cDate = need("game_date");
        int cSeason = need("season"), cType = need("season_type"), cHome = need("home_away");
        int cPts = need("points"), cReb = need("rebounds"), cAst = need("assists");
        int cDnp = need("did_not_play");

        while (std::getline(stream, line)) {
            if (line.empty()) continue;
            std::vector<std::string> f = splitCsvLine(line);
            if ((int)f.size() <= cAst) continue; // malformed/short row — skip rather than crash
            if (f[cType] != "2") continue; // regular season only, matches the app's other categories
            if (f[cDnp] == "true") continue; // dressed but didn't play — not a real game log entry

            HoopBoxRow row;
            row.athleteDisplayName = f[cName];
            row.teamAbbrev = f[cTeam];
            row.opponentAbbrev = f[cOpp];
            row.gameDate = f[cDate].substr(0, 10);
            row.season = (int)toDouble(f[cSeason]);
            row.seasonType = (int)toDouble(f[cType]);
            row.home = f[cHome] == "home";
            row.points = toDouble(f[cPts]);
            row.rebounds = toDouble(f[cReb]);
            row.assists = toDouble(f[cAst]);
            out.push_back(std::move(row));
        }
        return out;
    }

    // Suffix-aware last-name extraction now lives in Types.h as
    // lastNameKeyOf() — shared with Player::lastNameKey() and
    // NflverseLoader.h so the fix (stripping Jr./Sr./II/III/IV before
    // taking the surname) only had to happen in one place.

    inline std::vector<GameEntry> lookupGameLog(const std::vector<HoopBoxRow>& all, const Player& player, const StatCategory& cat) {
        std::string key = player.lastNameKey();
        std::vector<const HoopBoxRow*> rows;
        for (const auto& r : all) if (lastNameKeyOf(r.athleteDisplayName) == key) rows.push_back(&r);
        if (rows.empty()) throw std::runtime_error("boxscore data: player not found: " + player.name);

        std::sort(rows.begin(), rows.end(), [](const HoopBoxRow* a, const HoopBoxRow* b) { return a->gameDate < b->gameDate; });

        std::vector<GameEntry> games;
        for (const auto* r : rows) {
            double value;
            if (cat.key == "pts") value = r->points;
            else if (cat.key == "reb") value = r->rebounds;
            else if (cat.key == "ast") value = r->assists;
            else if (cat.key == "pra") value = r->points + r->rebounds + r->assists;
            else throw std::runtime_error("boxscore data: stat not mapped: " + cat.key);

            GameEntry entry;
            // gameDate is already "YYYY-MM-DD" — reuse NetClient's ISO
            // reformatter for the short "Aug 14" style the rest of the
            // chart uses, and its sort-key helper for chronological sort.
            entry.date = NetClient::reformatDate(r->gameDate);
            entry.opponent = r->opponentAbbrev.empty() ? "???" : r->opponentAbbrev;
            entry.home = r->home;
            entry.hasValue = true;
            entry.value = value;
            entry.sortKey = NetClient::isoToSortKey(r->gameDate);
            games.push_back(entry);
        }
        return games;
    }
}
