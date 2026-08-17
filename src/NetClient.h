#pragma once
#include "Types.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <stdexcept>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <algorithm>
#include <utility>

using json = nlohmann::json;

namespace NetClient {

    inline size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        ((std::string*)userp)->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    // Blocking GET. Called from a background std::thread — never call this
    // from the UI thread, it would freeze the render loop.
    // MSYS2-built curl often can't find its CA bundle automatically on
    // Windows, causing "Problem with the SSL CA cert" even though the
    // request itself would otherwise succeed. Try a few common install
    // locations before falling back to the (broken) default.
    inline void applyCaBundle(CURL* curl) {
        static const char* candidates[] = {
            "C:/msys64/mingw64/ssl/certs/ca-bundle.crt",
            "C:/msys64/usr/ssl/certs/ca-bundle.crt",
            "C:/msys64/mingw64/etc/ssl/certs/ca-bundle.crt",
        };
        if (const char* env = std::getenv("CURL_CA_BUNDLE")) {
            curl_easy_setopt(curl, CURLOPT_CAINFO, env);
            return;
        }
        for (const char* path : candidates) {
            std::ifstream f(path);
            if (f.good()) {
                curl_easy_setopt(curl, CURLOPT_CAINFO, path);
                return;
            }
        }
        // None found — leave curl's default in place; it'll likely still
        // fail, but this avoids silently pointing at a nonexistent file.
    }

    inline std::string httpGet(const std::string& url) {
        CURL* curl = curl_easy_init();
        if (!curl) throw std::runtime_error("curl_easy_init failed");
        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        applyCaBundle(curl);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "propdash-cpp/1.0");
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_easy_cleanup(curl);
        if (res != CURLE_OK) throw std::runtime_error(std::string("curl error: ") + curl_easy_strerror(res));
        if (httpCode != 200) throw std::runtime_error("HTTP " + std::to_string(httpCode) + " from " + url);
        return response;
    }

    // stats.nba.com / stats.wnba.com reject requests that don't look like
    // they came from a browser hitting the site itself — no API key exists,
    // it's just picky about headers. This isn't guesswork: the header set
    // below is the same one used across several independent open-source
    // clients (nba_api, wehoop) that reverse-engineered it, cross-checked
    // against each other rather than taken from a single source.
    inline std::string httpGetWithHeaders(const std::string& url, const std::vector<std::string>& headers, long timeoutSec = 20L) {
        CURL* curl = curl_easy_init();
        if (!curl) throw std::runtime_error("curl_easy_init failed");
        std::string response;
        struct curl_slist* headerList = nullptr;
        for (const auto& h : headers) headerList = curl_slist_append(headerList, h.c_str());
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        applyCaBundle(curl);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_slist_free_all(headerList);
        curl_easy_cleanup(curl);
        if (res != CURLE_OK) throw std::runtime_error(std::string("curl error: ") + curl_easy_strerror(res));
        if (httpCode != 200) throw std::runtime_error("HTTP " + std::to_string(httpCode) + " from " + url);
        return response;
    }

    inline std::string urlEncode(const std::string& s) {
        CURL* curl = curl_easy_init();
        char* out = curl_easy_escape(curl, s.c_str(), (int)s.length());
        std::string result(out);
        curl_free(out);
        curl_easy_cleanup(curl);
        return result;
    }

    inline int ipToOuts(const std::string& ip) {
        if (ip.empty()) return 0;
        size_t dot = ip.find('.');
        int whole = std::stoi(ip.substr(0, dot));
        int partial = (dot == std::string::npos) ? 0 : std::stoi(ip.substr(dot + 1));
        return whole * 3 + partial;
    }

    // Real wall-clock date (not SampleGenerator's fixed fictional "today")
    // — used for schedule lookups, which need to query against whatever
    // date it actually is when the app runs.
    inline std::string todayIso() {
        std::time_t t = std::time(nullptr);
        std::tm* utc = std::gmtime(&t);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday);
        return buf;
    }
    inline std::string addDaysIso(const std::string& iso, int days) {
        std::tm tmv{};
        int y, m, d;
        std::sscanf(iso.c_str(), "%d-%d-%d", &y, &m, &d);
        tmv.tm_year = y - 1900; tmv.tm_mon = m - 1; tmv.tm_mday = d; tmv.tm_hour = 12;
        std::time_t t = std::mktime(&tmv) + (std::time_t)days * 86400;
        std::tm* r = std::localtime(&t);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", r->tm_year + 1900, r->tm_mon + 1, r->tm_mday);
        return buf;
    }

    // Converts "2026-08-14" -> "Aug 14"
    inline std::string reformatDate(const std::string& iso) {
        static const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
        if (iso.size() < 10) return iso;
        int mon = std::stoi(iso.substr(5, 2));
        std::string day = iso.substr(8, 2);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%s %s", months[mon - 1], day.c_str());
        return std::string(buf);
    }

    // "2026-08-14" or "2026-08-14T23:10:00Z" -> 20260814, for sorting.
    // MLB's gameLog is oldest-first but NHL's is newest-first, so every
    // fetcher below fills this in and the caller sorts ascending — that's
    // the actual fix for games not appearing in "most recent" order.
    inline long isoToSortKey(const std::string& iso) {
        if (iso.size() < 10) return 0;
        try {
            return std::stol(iso.substr(0, 4) + iso.substr(5, 2) + iso.substr(8, 2));
        } catch (...) { return 0; }
    }

    // Throws on any failure — caller falls back to sample data.
    inline std::vector<GameEntry> fetchMlbGameLog(const Player& player, const StatCategory& cat) {
        json teamsRes = json::parse(httpGet("https://statsapi.mlb.com/api/v1/teams?sportId=1"));
        int teamId = -1;
        for (auto& t : teamsRes["teams"]) {
            if (t.value("abbreviation", "") == player.team) { teamId = t["id"].get<int>(); break; }
        }
        if (teamId == -1) throw std::runtime_error("MLB team not found: " + player.team);

        json rosterRes = json::parse(httpGet("https://statsapi.mlb.com/api/v1/teams/" + std::to_string(teamId) + "/roster"));
        int personId = -1;
        for (auto& r : rosterRes["roster"]) {
            std::string full = r["person"].value("fullName", "");
            if (lastNameKeyOf(full) == player.lastNameKey()) { personId = r["person"]["id"].get<int>(); break; }
        }
        if (personId == -1) throw std::runtime_error("MLB player not found on roster: " + player.name);

        // id -> abbreviation, built from the same /teams response used for
        // the team lookup above. MLB's per-game gameLog "opponent" object
        // does NOT reliably include an "abbreviation" field the way the
        // team-list endpoint does — only "id" and "name" — so resolving by
        // id here is what actually guarantees the opponent string matches
        // the abbreviation format fetchMlbTeamKRates() uses as its map
        // keys. Falling back to the full team name (the old behavior)
        // meant the matchup filter's map lookup silently failed for every
        // single game, every time — not an occasional edge case.
        std::map<int, std::string> abbrevById;
        for (auto& t : teamsRes["teams"]) abbrevById[t.value("id", -1)] = t.value("abbreviation", "");

        std::string hydrate = urlEncode("stats(group=[" + cat.statGroup + "],type=[gameLog],season=2026)");
        json personRes = json::parse(httpGet("https://statsapi.mlb.com/api/v1/people/" + std::to_string(personId) + "?hydrate=" + hydrate));
        auto& people = personRes["people"];
        if (people.empty() || !people[0].contains("stats")) throw std::runtime_error("MLB no stats for player");

        const json* splits = nullptr;
        for (auto& sg : people[0]["stats"]) {
            if (sg["type"].value("displayName", "") == "gameLog") { splits = &sg["splits"]; break; }
        }
        if (!splits || splits->empty()) throw std::runtime_error("MLB no game log entries");

        std::vector<GameEntry> games;
        for (auto& g : *splits) {
            const json& stat = g["stat"];
            double value;
            if (cat.statGroup == "pitching") {
                if (cat.key == "k") value = stat.value("strikeOuts", 0);
                else if (cat.key == "outs") value = ipToOuts(stat.value("inningsPitched", std::string("0")));
                else if (cat.key == "er") value = stat.value("earnedRuns", 0);
                else if (cat.key == "hits") value = stat.value("hits", 0);
                else if (cat.key == "bb") value = stat.value("baseOnBalls", 0);
                else throw std::runtime_error("MLB pitching stat not mapped: " + cat.key);
            } else { // "hitting"
                if (cat.key == "bhits") value = stat.value("hits", 0);
                else if (cat.key == "hr") value = stat.value("homeRuns", 0);
                else if (cat.key == "rbi") value = stat.value("rbi", 0);
                else if (cat.key == "tb") value = stat.value("totalBases", 0);
                else if (cat.key == "hrr") value = stat.value("hits", 0) + stat.value("runs", 0);
                else throw std::runtime_error("MLB hitting stat not mapped: " + cat.key);
            }

            GameEntry entry;
            std::string iso = g.value("date", "");
            entry.date = reformatDate(iso);
            entry.sortKey = isoToSortKey(iso);
            int oppId = g["opponent"].value("id", -1);
            auto abbrevIt = abbrevById.find(oppId);
            entry.opponent = (abbrevIt != abbrevById.end() && !abbrevIt->second.empty())
                ? abbrevIt->second
                : g["opponent"].value("name", "???"); // last-resort fallback if id lookup somehow misses
            entry.home = g.value("isHome", false);
            entry.hasValue = true;
            entry.value = value;
            games.push_back(entry);
        }
        std::sort(games.begin(), games.end(), [](const GameEntry& a, const GameEntry& b) { return a.sortKey < b.sortKey; });
        return games;
    }

    // Team-level hitting strikeout rate for every MLB club in one request —
    // used to flag a game's opponent as a "favorable" or "challenging"
    // strikeout matchup (a high team K% against is favorable for the
    // pitcher's strikeout prop, and vice versa).
    inline std::map<std::string, MatchupStat> fetchMlbTeamKRates(int season = 2026) {
        json res = json::parse(httpGet(
            "https://statsapi.mlb.com/api/v1/teams/stats?stats=season&group=hitting&season="
            + std::to_string(season) + "&sportIds=1"));

        std::map<std::string, MatchupStat> byTeamId;
        // This endpoint's shape nests splits under stats[0].splits[i], each
        // with its own "team" object and "stat" block (strikeOuts, plateAppearances).
        if (!res.contains("stats") || res["stats"].empty()) return {};
        for (auto& split : res["stats"][0]["splits"]) {
            if (!split.contains("team") || !split.contains("stat")) continue;
            int teamId = split["team"].value("id", -1);
            double so = split["stat"].value("strikeOuts", 0.0);
            double pa = split["stat"].value("plateAppearances", 0.0);
            if (teamId == -1 || pa <= 0) continue;
            MatchupStat m;
            m.value = 100.0 * so / pa;
            byTeamId[std::to_string(teamId)] = m;
        }

        // Map team id -> abbreviation so callers can key by the same
        // abbreviations already used throughout Types.h/SportsData.h.
        json teamsRes = json::parse(httpGet("https://statsapi.mlb.com/api/v1/teams?sportId=1"));
        std::map<std::string, MatchupStat> byAbbrev;
        for (auto& t : teamsRes["teams"]) {
            std::string idKey = std::to_string(t.value("id", -1));
            auto it = byTeamId.find(idKey);
            if (it == byTeamId.end()) continue;
            MatchupStat m = it->second;
            m.team = t.value("abbreviation", "");
            byAbbrev[m.team] = m;
        }

        // Percentile-rank each team among the others (higher K% = tougher
        // matchup for the batter = more favorable for a pitcher's K prop).
        std::vector<double> vals;
        for (auto& [k, m] : byAbbrev) vals.push_back(m.value);
        std::sort(vals.begin(), vals.end());
        for (auto& [k, m] : byAbbrev) {
            auto lower = std::lower_bound(vals.begin(), vals.end(), m.value);
            m.percentile = vals.empty() ? 50 : (int)(100.0 * (lower - vals.begin()) / vals.size());
        }
        return byAbbrev;
    }

    // Forward declaration: fetchNhlGameLog (below) delegates to this for the
    // "blk" category since blocked shots need a completely different fetch
    // strategy (per-game boxscores, see the function body further down).
    inline std::vector<GameEntry> fetchNhlBlockedShotsFwd(const Player& player);

    // Shared by fetchNhlGameLog and fetchNhlBlockedShotsFwd: resolves a
    // player to their current NHL player id via the team roster endpoint.
    inline int findNhlPlayerId(const Player& player) {
        json roster = json::parse(httpGet("https://api-web.nhle.com/v1/roster/" + player.team + "/current"));
        int playerId = -1;
        auto tryMatch = [&](const json& arr) {
            for (auto& p : arr) {
                std::string last = p["lastName"].value("default", "");
                std::transform(last.begin(), last.end(), last.begin(), ::tolower);
                if (last == player.lastNameKey()) { playerId = p["id"].get<int>(); return true; }
            }
            return false;
        };
        if (!tryMatch(roster["forwards"])) tryMatch(roster["defensemen"]);
        if (playerId == -1) throw std::runtime_error("NHL player not found on roster: " + player.name);
        return playerId;
    }

    inline std::vector<GameEntry> fetchNhlGameLog(const Player& player, const StatCategory& cat) {
        if (cat.key == "blk") return fetchNhlBlockedShotsFwd(player); // defined below, declared via forward decl

        std::string field;
        if (cat.key == "sog") field = "shots";
        else if (cat.key == "pts") field = "points";
        else if (cat.key == "goals") field = "goals";
        else throw std::runtime_error("NHL stat not mapped: " + cat.key);

        int playerId = findNhlPlayerId(player);

        json log = json::parse(httpGet("https://api-web.nhle.com/v1/player/" + std::to_string(playerId) + "/game-log/20252026/2"));
        if (!log.contains("gameLog") || log["gameLog"].empty()) throw std::runtime_error("NHL no game log entries");

        std::vector<GameEntry> games;
        for (auto& g : log["gameLog"]) {
            GameEntry entry;
            std::string iso = g.value("gameDate", "");
            entry.date = reformatDate(iso);
            entry.sortKey = isoToSortKey(iso);
            entry.opponent = g.value("opponentAbbrev", "???");
            entry.home = g.value("homeRoadFlag", "R") == "H";
            entry.hasValue = true;
            entry.value = g.value(field, 0.0);
            games.push_back(entry);
        }
        std::sort(games.begin(), games.end(), [](const GameEntry& a, const GameEntry& b) { return a.sortKey < b.sortKey; });
        return games;
    }

    // Blocked shots aren't in the skater game-log payload (that endpoint
    // only carries goals/assists/points/shots/toi). The only place the NHL
    // API surfaces a per-game blocked-shot count is each game's boxscore,
    // so this pulls the game list from the same game-log endpoint (for
    // dates/opponents/ids) and then makes one extra request per game to
    // read that player's blockedShots out of the boxscore.
    //
    // That's capped at the most recent kMaxGames games so a full 82-game
    // season doesn't mean 82 extra HTTP round trips on every tab switch —
    // this runs on the background fetch thread already, so it just takes a
    // bit longer rather than freezing the UI, but it's still bounded.
    //
    // NOTE: I could not hit this endpoint live to confirm field names.
    // playerByGameStats.{homeTeam,awayTeam}.{forwards,defense,goalies}[].{playerId,blockedShots}
    // is the documented/observed shape as of ImGui-era NHL API usage — if
    // the real response differs, the catch below just skips that game
    // rather than crashing, and the whole category falls back to sample
    // data if every game fails.
    inline std::vector<GameEntry> fetchNhlBlockedShotsFwd(const Player& player) {
        constexpr int kMaxGames = 30;
        int playerId = findNhlPlayerId(player);

        json log = json::parse(httpGet("https://api-web.nhle.com/v1/player/" + std::to_string(playerId) + "/game-log/20252026/2"));
        if (!log.contains("gameLog") || log["gameLog"].empty()) throw std::runtime_error("NHL no game log entries");

        auto findBlocked = [&](const json& box) -> std::optional<double> {
            if (!box.contains("playerByGameStats")) return std::nullopt;
            const json& pbg = box["playerByGameStats"];
            for (const char* side : {"homeTeam", "awayTeam"}) {
                if (!pbg.contains(side)) continue;
                for (const char* group : {"forwards", "defense", "goalies"}) {
                    if (!pbg[side].contains(group)) continue;
                    for (auto& p : pbg[side][group]) {
                        if (p.value("playerId", -1) == playerId) {
                            return p.value("blockedShots", 0.0);
                        }
                    }
                }
            }
            return std::nullopt;
        };

        std::vector<GameEntry> games;
        int seen = 0;
        for (auto& g : log["gameLog"]) {
            if (seen >= kMaxGames) break;
            seen++;
            long long gameId = g.value("gameId", 0LL);
            if (gameId == 0) continue;
            try {
                json box = json::parse(httpGet("https://api-web.nhle.com/v1/gamecenter/" + std::to_string(gameId) + "/boxscore"));
                auto blocked = findBlocked(box);
                if (!blocked) continue; // couldn't locate this player in the boxscore — skip rather than guess

                GameEntry entry;
                std::string iso = g.value("gameDate", "");
                entry.date = reformatDate(iso);
                entry.sortKey = isoToSortKey(iso);
                entry.opponent = g.value("opponentAbbrev", "???");
                entry.home = g.value("homeRoadFlag", "R") == "H";
                entry.hasValue = true;
                entry.value = *blocked;
                games.push_back(entry);
            } catch (...) { continue; } // one bad boxscore shouldn't kill the whole category
        }
        if (games.empty()) throw std::runtime_error("NHL blocked shots: no boxscores resolved");
        std::sort(games.begin(), games.end(), [](const GameEntry& a, const GameEntry& b) { return a.sortKey < b.sortKey; });
        return games;
    }

    // ---------------------------------------------------------------------
    // Real "next scheduled game" lookups — MLB/NHL's own game-log endpoints
    // only return games already played, which is why the app was showing a
    // hardcoded "TBD" placeholder for the upcoming opponent instead of a
    // real one. These hit each league's schedule endpoint separately to
    // find the next not-yet-final game.
    // ---------------------------------------------------------------------

    inline GameEntry fetchMlbNextGame(const Player& player) {
        json teamsRes = json::parse(httpGet("https://statsapi.mlb.com/api/v1/teams?sportId=1"));
        int teamId = -1;
        for (auto& t : teamsRes["teams"]) {
            if (t.value("abbreviation", "") == player.team) { teamId = t.value("id", -1); break; }
        }
        if (teamId == -1) throw std::runtime_error("MLB team not found for next game: " + player.team);

        std::string start = todayIso();
        std::string end = addDaysIso(start, 21); // 3-week lookahead is plenty for "next game"
        json sched = json::parse(httpGet("https://statsapi.mlb.com/api/v1/schedule?sportId=1&teamId="
            + std::to_string(teamId) + "&startDate=" + start + "&endDate=" + end));
        if (!sched.contains("dates")) throw std::runtime_error("MLB: no schedule dates returned");

        for (auto& d : sched["dates"]) {
            for (auto& g : d["games"]) {
                std::string state = g["status"].value("abstractGameState", "");
                if (state == "Final") continue; // already played — keep looking
                int homeId = g["teams"]["home"]["team"].value("id", -1);
                int awayId = g["teams"]["away"]["team"].value("id", -1);
                bool home = homeId == teamId;
                int oppId = home ? awayId : homeId;
                std::string oppAbbrev = "???";
                for (auto& t : teamsRes["teams"]) {
                    if (t.value("id", -1) == oppId) { oppAbbrev = t.value("abbreviation", ""); break; }
                }
                GameEntry entry;
                std::string gd = g.value("gameDate", "");
                entry.date = reformatDate(gd.size() >= 10 ? gd.substr(0, 10) : gd);
                entry.opponent = oppAbbrev;
                entry.home = home;
                entry.hasValue = false;
                return entry;
            }
        }
        throw std::runtime_error("MLB: no upcoming (non-Final) game found in the next 21 days");
    }

    inline GameEntry fetchNhlNextGame(const Player& player) {
        json sched = json::parse(httpGet("https://api-web.nhle.com/v1/club-schedule/" + player.team + "/week/now"));
        if (!sched.contains("games")) throw std::runtime_error("NHL: no games array in weekly schedule");

        for (auto& g : sched["games"]) {
            std::string state = g.value("gameState", "");
            if (state == "OFF" || state == "FINAL") continue; // already played — keep looking
            bool home = g["homeTeam"].value("abbrev", "") == player.team;
            std::string opp = home ? g["awayTeam"].value("abbrev", "???") : g["homeTeam"].value("abbrev", "???");
            GameEntry entry;
            std::string gd = g.value("gameDate", "");
            entry.date = reformatDate(gd.size() >= 10 ? gd.substr(0, 10) : gd);
            entry.opponent = opp;
            entry.home = home;
            entry.hasValue = false;
            return entry;
        }
        throw std::runtime_error("NHL: no upcoming (non-final) game found in this week's schedule");
    }

}
