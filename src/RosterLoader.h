#pragma once
// Full-league roster loading — the "every player in the league" fix.
//
// SportsData.h ships each league with a 5-player seed list so the app has
// something to show the instant it launches. This file replaces that seed
// list with the real, full roster once it's fetched on a background
// thread. If the fetch fails for any reason, the seed list stays in place
// — the app never ends up with an empty player list.
#include "Types.h"
#include "NetClient.h"
#include <vector>
#include <string>
#include <map>
#include <stdexcept>
#include <algorithm>

namespace RosterLoader {

    inline std::string initialsFrom(const std::string& fullName) {
        std::string out;
        bool atWordStart = true;
        for (char c : fullName) {
            if (c == ' ' || c == '.') { atWordStart = true; continue; }
            if (atWordStart && std::isalpha((unsigned char)c)) {
                out += (char)std::toupper((unsigned char)c);
                atWordStart = false;
                if (out.size() >= 2) break;
            }
        }
        return out.empty() ? "?" : out;
    }

    // playerId -> gamesStarted this season, from one bulk pitching-stats
    // request (not per-player) — used to tell starters from pure relievers.
    // A pitcher with 0 starts is excluded from the roster entirely rather
    // than mislabeled; a pitcher with 1+ starts is tagged "SP" even if they
    // also came out of the bullpen sometimes (true two-role "bulk" arms are
    // a real edge case MLB's own site doesn't cleanly separate either — a
    // start-count threshold is the same imperfect heuristic most prop
    // sites use).
    inline std::map<int, int> fetchMlbGamesStartedByPlayer(int season = 2026) {
        std::map<int, int> out;
        json res = json::parse(NetClient::httpGet(
            "https://statsapi.mlb.com/api/v1/stats?stats=season&group=pitching&season="
            + std::to_string(season) + "&sportId=1&limit=2000"));
        if (!res.contains("stats") || res["stats"].empty()) return out;
        for (auto& split : res["stats"][0]["splits"]) {
            if (!split.contains("player") || !split.contains("stat")) continue;
            int playerId = split["player"].value("id", -1);
            int gs = split["stat"].value("gamesStarted", 0);
            if (playerId != -1) out[playerId] = gs;
        }
        return out;
    }

    // One request for every active MLB player, then one more for team
    // id -> abbreviation so each player's currentTeam can be labeled the
    // same way the rest of the app already labels teams. Pitchers with no
    // starts this season (pure relievers) are dropped; the rest are
    // tagged role="SP" or role="Hitter" for the tab split in main.cpp.
    inline std::vector<Player> fetchMlbAllPlayers(int season = 2026) {
        json teamsRes = json::parse(NetClient::httpGet("https://statsapi.mlb.com/api/v1/teams?sportId=1"));
        std::map<int, std::string> abbrevByTeamId;
        for (auto& t : teamsRes["teams"]) abbrevByTeamId[t.value("id", -1)] = t.value("abbreviation", "");

        std::map<int, int> gamesStartedByPlayer = fetchMlbGamesStartedByPlayer(season);

        json playersRes = json::parse(NetClient::httpGet(
            "https://statsapi.mlb.com/api/v1/sports/1/players?season=" + std::to_string(season)));
        if (!playersRes.contains("people")) throw std::runtime_error("MLB players endpoint returned no people");

        std::vector<Player> out;
        for (auto& p : playersRes["people"]) {
            if (!p.contains("currentTeam")) continue; // skip free agents / minors-only entries
            int teamId = p["currentTeam"].value("id", -1);
            auto it = abbrevByTeamId.find(teamId);
            if (it == abbrevByTeamId.end()) continue;

            std::string posAbbrev = p["primaryPosition"].value("abbreviation", "");
            bool isPitcher = (posAbbrev == "P");
            int playerId = p.value("id", 0);

            std::string role;
            if (isPitcher) {
                auto gsIt = gamesStartedByPlayer.find(playerId);
                int gamesStarted = (gsIt == gamesStartedByPlayer.end()) ? 0 : gsIt->second;
                if (gamesStarted <= 0) continue; // pure reliever — excluded from the roster entirely
                role = "SP";
            } else {
                role = "Hitter";
            }

            Player pl;
            pl.id = "mlb-" + std::to_string(playerId);
            pl.name = p.value("fullName", "");
            pl.team = it->second;
            pl.position = posAbbrev;
            pl.initials = initialsFrom(pl.name);
            pl.sport = "MLB";
            pl.role = role;
            if (!pl.name.empty()) out.push_back(pl);
        }
        return out;
    }

    // NHL has no single "every player" endpoint, so this loops each team's
    // current roster (one request per team, 32 total) and merges them.
    // Goalies are skipped: none of the NHL stat categories in
    // SportsData.h (shots on goal / points / goals / blocked shots) are
    // meaningful goalie props.
    inline std::vector<Player> fetchNhlAllPlayers(const std::vector<std::string>& teams, std::vector<std::string>* failedTeams = nullptr) {
        std::vector<Player> out;
        for (const std::string& team : teams) {
            try {
                json roster = json::parse(NetClient::httpGet("https://api-web.nhle.com/v1/roster/" + team + "/current"));
                for (const char* group : {"forwards", "defensemen"}) {
                    if (!roster.contains(group)) continue;
                    for (auto& p : roster[group]) {
                        Player pl;
                        pl.id = "nhl-" + std::to_string(p.value("id", 0));
                        std::string first = p["firstName"].value("default", "");
                        std::string last = p["lastName"].value("default", "");
                        pl.name = first + " " + last;
                        pl.team = team;
                        pl.position = p.value("positionCode", "");
                        pl.initials = initialsFrom(pl.name);
                        pl.sport = "NHL";
                        if (!last.empty()) out.push_back(pl);
                    }
                }
            } catch (...) {
                // One team's roster failing shouldn't drop the other 31 —
                // but silently swallowing it made "some players are
                // missing" impossible to diagnose. Recording which team
                // failed at least makes the gap visible instead of just
                // absent.
                if (failedTeams) failedTeams->push_back(team);
                continue;
            }
        }
        return out;
    }

    // ESPN's site API uses its own team slugs that occasionally diverge
    // from the standard 3-letter abbreviations used elsewhere in this app
    // (Golden State is "gs" not "gsw", New Orleans is "no" not "nop", ...).
    // This table covers the known mismatches for NBA/WNBA; anything not
    // listed falls back to lowercasing the standard abbreviation, which is
    // correct for the majority of teams and all of NFL.
    inline std::string espnSlug(const std::string& sportKey, const std::string& abbrev) {
        static const std::map<std::string, std::string> nbaOverrides = {
            {"GSW","gs"}, {"NOP","no"}, {"NYK","ny"}, {"SAS","sa"}, {"UTA","utah"}, {"WAS","wsh"},
        };
        static const std::map<std::string, std::string> wnbaOverrides = {
            // "CON","GSV","LAS","WAS" verified-pattern overrides from
            // before. Adding NYL->ny and LVA->lv here too: New York and
            // Las Vegas are very likely single-word-city slugs the same
            // way NBA's Knicks are "ny" not "nyk" (see nbaOverrides
            // above) — but unlike the GitHub-hosted data sources in this
            // app, I can't reach site.api.espn.com from my own sandbox to
            // confirm this directly, so treat this as a probable fix, not
            // a verified one. It can't be the sole explanation if every
            // single WNBA team is failing, since only these two would be
            // affected — the real answer will come from the per-team
            // failure detail now surfaced in the Rosters status line.
            {"CON","conn"}, {"GSV","gs"}, {"LAS","la"}, {"WAS","wsh"}, {"NYL","ny"}, {"LVA","lv"},
        };
        const std::map<std::string, std::string>* overrides =
            sportKey == "NBA" ? &nbaOverrides : sportKey == "WNBA" ? &wnbaOverrides : nullptr;
        if (overrides) {
            auto it = overrides->find(abbrev);
            if (it != overrides->end()) return it->second;
        }
        std::string lower = abbrev;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return lower;
    }

    // Generic ESPN site-API roster loop for the leagues with no per-game
    // log support anyway (NBA/WNBA/NFL) — stats for these still come from
    // SampleGenerator, but the player list itself becomes real and
    // complete instead of 5 stars per league.
    inline std::vector<Player> fetchEspnAllPlayers(const std::string& sportKey, const std::string& espnPath,
                                                     const std::vector<std::string>& teams, std::vector<std::string>* failedTeams = nullptr) {
        std::vector<Player> out;
        for (const std::string& team : teams) {
            try {
                std::string slug = espnSlug(sportKey, team);
                json res = json::parse(NetClient::httpGet(
                    "https://site.api.espn.com/apis/site/v2/sports/" + espnPath + "/teams/" + slug + "/roster"));
                if (!res.contains("athletes")) { if (failedTeams) failedTeams->push_back(team + " (no athletes field)"); continue; }

                // ESPN returns "athletes" in one of two shapes depending on
                // sport/team, and the previous version only handled one of
                // them: either an array of position-group objects each with
                // an "items" array of athletes, OR (sometimes) a flat array
                // of athlete objects directly. Assuming only the grouped
                // shape meant any team ESPN answered with the flat shape
                // silently contributed zero players — no exception, no
                // entry in failedTeams, just quietly missing. Handling both
                // explicitly instead of guessing wrong for one of them.
                size_t beforeCount = out.size();
                for (auto& entry : res["athletes"]) {
                    std::vector<const json*> athletes;
                    if (entry.is_object() && entry.contains("items") && entry["items"].is_array()) {
                        for (auto& a : entry["items"]) athletes.push_back(&a);
                    } else if (entry.is_object() && (entry.contains("displayName") || entry.contains("id"))) {
                        athletes.push_back(&entry); // "athletes" was already a flat list
                    }
                    for (const json* aPtr : athletes) {
                        const json& a = *aPtr;
                        Player pl;
                        pl.id = sportKey + "-" + std::to_string(a.value("id", 0));
                        pl.name = a.value("displayName", "");
                        pl.team = team;
                        pl.position = a.contains("position") ? a["position"].value("abbreviation", "") : "";
                        pl.initials = initialsFrom(pl.name);
                        pl.sport = sportKey;
                        if (!pl.name.empty()) out.push_back(pl);
                    }
                }
                if (out.size() == beforeCount && failedTeams) {
                    failedTeams->push_back(team + " (0 players parsed from response)");
                }
            } catch (...) {
                // Same visibility fix as NHL above — record which team
                // failed instead of just silently coming up short.
                if (failedTeams) failedTeams->push_back(team);
                continue;
            }
        }
        return out;
    }

    // Dispatches to the right loader for a given league. Throws if the
    // result would be empty (caller keeps the seed list in that case).
    // failedTeams, if provided, is filled with any team abbreviations
    // whose individual roster fetch failed — the league still loads with
    // whatever succeeded, but the caller can now tell the person which
    // teams' players might be missing instead of the gap being silent.
    inline std::vector<Player> loadFullRoster(const SportConfig& sport, std::vector<std::string>* failedTeams = nullptr) {
        std::vector<Player> result;
        if (sport.name == "MLB") result = fetchMlbAllPlayers();
        else if (sport.name == "NHL") result = fetchNhlAllPlayers(sport.teams, failedTeams);
        else if (!sport.espnPath.empty()) result = fetchEspnAllPlayers(sport.name, sport.espnPath, sport.teams, failedTeams);
        else throw std::runtime_error("no roster loader configured for " + sport.name);

        if (result.empty()) throw std::runtime_error("full roster fetch for " + sport.name + " returned no players");
        std::sort(result.begin(), result.end(), [](const Player& a, const Player& b) { return a.name < b.name; });
        return result;
    }
}
