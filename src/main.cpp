// PropDash (C++) — SDL2 + Dear ImGui + ImPlot
//
// Same app as the earlier React and JavaFX versions: search a player across
// 5 leagues, pick a stat category, see a per-game bar chart against a prop
// line. MLB and NHL pull real game logs over HTTP (libcurl); NBA/WNBA/NFL
// use generated sample data, same reason as before — ESPN's API has no
// per-game log endpoint without looping per-event boxscores.
//
// Player lists start as a 5-name seed per league and are replaced with the
// full league roster by a background fetch (RosterLoader.h) shortly after
// launch — see AppState::players below.

#include <SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "implot.h"

#include "Types.h"
#include "SportsData.h"
#include "SampleGenerator.h"
#include "NetClient.h"
#include "RosterLoader.h"
#include "NflverseLoader.h"
#include "HoopRLoader.h"

#include <atomic>
#include <thread>
#include <mutex>
#include <map>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------

enum class LiveStatus { Idle, Loading, Live, Sample };

// GameLog is the original bar-chart view. The other four correspond to the
// sections in the reference dashboard screenshots (Matchup Analyzer, Pitch
// Mix, Official Opposing Lineup, Bullpen). Matchup has real data behind it
// (the MLB team-K% panel below); the other three are placeholders that say
// so honestly rather than showing invented numbers — wiring them up for
// real would mean a live per-pitch-type/lineup/bullpen feed this rewrite
// doesn't have yet.
enum class ViewTab { GameLog, Matchup, PitchMix, Lineup, Bullpen };

enum class MatchupFilter { All, Favorable, Challenging };

// MLB only: which half of the roster/category-list is showing. Ignored for
// every other league (they only ever have one category list, one role).
enum class MlbPlayerType { StartingPitcher, Hitter };

struct AppState {
    std::string sportKey = "MLB";
    int categoryIndex = 0;
    Player selectedPlayer;   // the actual selected player, not an index — see note below
    int gamesShown = 20;
    ViewTab viewTab = ViewTab::GameLog;
    MatchupFilter matchupFilter = MatchupFilter::All;
    MlbPlayerType mlbPlayerType = MlbPlayerType::StartingPitcher; // MLB only

    char searchBuf[128] = "";

    // Shared between the UI thread and the background fetch thread.
    std::mutex dataMutex;
    std::vector<GameEntry> games;      // the currently-displayed log (live or sample)
    std::atomic<LiveStatus> status{LiveStatus::Idle};
    std::atomic<int> requestId{0};     // bumped on every player/category switch; stale
                                        // background results are discarded by comparing
                                        // against this when they land.
    std::string statusMessage;

    // Full-league rosters. Starts as a copy of each league's 5-player seed
    // list (SportsData.h) and is swapped out per-sport as RosterLoader
    // finishes each league in the background. Storing the *selected*
    // player as a value (above) rather than an index into this map means a
    // roster swap-in mid-session can never silently point playerIndex at
    // the wrong person.
    std::mutex rosterMutex;
    std::map<std::string, std::vector<Player>> players;
    std::map<std::string, std::string> rosterStatus; // "" once a league's full roster is loaded

    // MLB team strikeout-rate percentiles, powering the opponent panel and
    // the matchup difficulty filter for the Strikeouts category.
    std::mutex matchupMutex;
    std::map<std::string, MatchupStat> mlbTeamKRates;
    bool matchupLoaded = false;
    bool matchupLoadFailed = false;
    std::string matchupError; // populated on failure so the UI can show *why*, not just "unavailable"

    // NFL, NBA, and WNBA all use the same pattern: no real per-player
    // endpoint exists, so a bulk per-season/all-time file is downloaded
    // once at startup and kept here for the rest of the session, rather
    // than re-fetched on every player/category switch like MLB/NHL.
    // NFL uses NflverseLoader (one all-time CSV); NBA/WNBA use
    // HoopRLoader (one CSV per current season, same schema for both).
    std::mutex nflMutex;
    std::vector<NflStatRow> nflStats;
    bool nflStatsLoaded = false;
    bool nflStatsFailed = false;
    std::string nflStatsError;

    std::mutex hoopMutex;
    std::map<std::string, std::vector<HoopBoxRow>> hoopStats; // "NBA" / "WNBA" -> rows
    std::map<std::string, bool> hoopStatsLoaded;
    std::map<std::string, std::string> hoopStatsError;

    AppState() {
        for (const auto& key : sportOrder()) players[key] = sportsData().at(key).players;
        selectedPlayer = players["MLB"][0];
    }
};

static const SportConfig& currentSport(AppState& s) { return sportsData().at(s.sportKey); }

// MLB shows two independent category lists (pitcher stats vs. hitter
// stats); every other league only ever has one. This is the single place
// that decides which list is "current" so category-index lookups, tab
// rendering, and the reset-on-switch logic all agree with each other.
static const std::vector<StatCategory>& currentCategoryList(AppState& s) {
    const SportConfig& sport = currentSport(s);
    if (s.sportKey == "MLB" && s.mlbPlayerType == MlbPlayerType::Hitter) return sport.hitterCategories;
    return sport.categories;
}
static const StatCategory& currentCategory(AppState& s) {
    const auto& list = currentCategoryList(s);
    int idx = std::min(s.categoryIndex, (int)list.size() - 1);
    return list[std::max(0, idx)];
}
static const Player& currentPlayer(AppState& s) { return s.selectedPlayer; }

// Kicks off (or retries) the MLB team-strikeout-rate fetch on its own —
// separate from loadBackgroundData so the UI can call this again later if
// the first attempt failed, rather than only ever getting one shot at it
// on startup.
static void requestMatchupData(AppState& state) {
    {
        std::lock_guard<std::mutex> lock(state.matchupMutex);
        state.matchupLoadFailed = false;
        state.matchupError.clear();
    }
    std::thread([&state]() {
        try {
            auto rates = NetClient::fetchMlbTeamKRates();
            std::lock_guard<std::mutex> lock(state.matchupMutex);
            state.mlbTeamKRates = std::move(rates);
            state.matchupLoaded = true;
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(state.matchupMutex);
            state.matchupLoadFailed = true;
            state.matchupError = e.what();
        } catch (...) {
            std::lock_guard<std::mutex> lock(state.matchupMutex);
            state.matchupLoadFailed = true;
            state.matchupError = "unknown error";
        }
    }).detach();
}

// Kicks off the one-time background load of full-league rosters (all 5
// leagues) plus MLB team strikeout rates. Both degrade gracefully: on
// failure the seed list / "not available" state just stays in place.
static void loadBackgroundData(AppState& state) {
    std::thread([&state]() {
        for (const auto& key : sportOrder()) {
            const SportConfig& sport = sportsData().at(key);
            // Declared outside the try block on purpose: RosterLoader::
            // loadFullRoster() still populates this even when it ultimately
            // throws (e.g. every team came back with 0 players). If this
            // were declared inside the try, stack unwinding would destroy
            // it before the catch block could see it — which is exactly
            // why a total-failure case was only ever showing the generic
            // "returned no players" message instead of which teams failed
            // and why.
            std::vector<std::string> failedTeams;
            try {
                auto full = RosterLoader::loadFullRoster(sport, &failedTeams);
                std::lock_guard<std::mutex> lock(state.rosterMutex);
                state.players[key] = std::move(full);
                if (failedTeams.empty()) {
                    state.rosterStatus[key] = "";
                } else {
                    std::string list;
                    for (size_t i = 0; i < failedTeams.size(); i++) list += (i ? ", " : "") + failedTeams[i];
                    state.rosterStatus[key] = "loaded, but " + std::to_string(failedTeams.size())
                        + " team(s) failed and are missing players: " + list;
                }
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(state.rosterMutex);
                std::string detail = std::string("showing seed list (") + e.what() + ")";
                if (!failedTeams.empty()) {
                    std::string list;
                    for (size_t i = 0; i < failedTeams.size(); i++) list += (i ? ", " : "") + failedTeams[i];
                    detail += " \xE2\x80\x94 per-team detail: " + list;
                }
                state.rosterStatus[key] = detail;
            }
        }
    }).detach();

    requestMatchupData(state);

    std::thread([&state]() {
        try {
            auto rows = NflverseLoader::fetchAllPlayerStats();
            std::lock_guard<std::mutex> lock(state.nflMutex);
            state.nflStats = std::move(rows);
            state.nflStatsLoaded = true;
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(state.nflMutex);
            state.nflStatsFailed = true;
            state.nflStatsError = e.what();
        }
    }).detach();

    for (const std::string& league : {"nba", "wnba"}) {
        std::thread([&state, league]() {
            std::string key = league == "nba" ? "NBA" : "WNBA";
            try {
                auto rows = HoopRLoader::fetchPlayerBoxScores(league, 2026);
                std::lock_guard<std::mutex> lock(state.hoopMutex);
                state.hoopStats[key] = std::move(rows);
                state.hoopStatsLoaded[key] = true;
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(state.hoopMutex);
                state.hoopStatsLoaded[key] = false;
                state.hoopStatsError[key] = e.what();
            }
        }).detach();
    }
}

// Kicks off a background fetch for the given player/category. Always
// resolves — either with real data or generated sample data (any
// failure) — and stamps the result with the requestId it was launched
// under so late results from an abandoned switch don't clobber a newer
// selection.
static void requestData(AppState& state) {
    int myRequest = ++state.requestId;
    state.status = LiveStatus::Loading;

    const SportConfig sport = currentSport(state);
    const StatCategory cat = currentCategory(state);
    const Player player = currentPlayer(state);

    std::thread([&state, myRequest, sport, cat, player]() {
        std::vector<GameEntry> result;
        LiveStatus finalStatus;
        std::string message;

        bool tryLive = sport.liveSupported;
        if (tryLive) {
            try {
                std::string sourceLabel;
                if (sport.name == "MLB") { result = NetClient::fetchMlbGameLog(player, cat); sourceLabel = "statsapi.mlb.com"; }
                else if (sport.name == "NHL") { result = NetClient::fetchNhlGameLog(player, cat); sourceLabel = "api-web.nhle.com"; }
                else if (sport.name == "NBA" || sport.name == "WNBA") {
                    std::lock_guard<std::mutex> lock(state.hoopMutex);
                    bool loaded = state.hoopStatsLoaded.count(sport.name) && state.hoopStatsLoaded[sport.name];
                    if (!loaded) {
                        std::string err = state.hoopStatsError.count(sport.name) ? state.hoopStatsError[sport.name] : "";
                        throw std::runtime_error(err.empty() ? "boxscore data still downloading" : ("boxscore data failed to load: " + err));
                    }
                    result = HoopRLoader::lookupGameLog(state.hoopStats[sport.name], player, cat);
                    sourceLabel = "sportsdataverse (github.com/sportsdataverse)";
                }
                else if (sport.name == "NFL") {
                    std::lock_guard<std::mutex> lock(state.nflMutex);
                    if (!state.nflStatsLoaded) {
                        throw std::runtime_error(state.nflStatsFailed
                            ? ("nflverse data failed to load: " + state.nflStatsError)
                            : "nflverse data still downloading (~33MB, one-time)");
                    }
                    result = NflverseLoader::lookupGameLog(state.nflStats, player, cat);
                    sourceLabel = "nflverse (github.com/nflverse)";
                } else throw std::runtime_error("no live client for " + sport.name);

                // Real "next scheduled game" lookup for MLB/NHL. NBA/WNBA/NFL
                // still fall back to an honest "TBD" placeholder here — their
                // schedule endpoints aren't wired up yet, a known remaining
                // gap rather than something silently faked.
                GameEntry upcoming;
                bool gotRealUpcoming = false;
                try {
                    if (sport.name == "MLB") { upcoming = NetClient::fetchMlbNextGame(player); gotRealUpcoming = true; }
                    else if (sport.name == "NHL") { upcoming = NetClient::fetchNhlNextGame(player); gotRealUpcoming = true; }
                } catch (...) { /* fall through to TBD below */ }
                if (!gotRealUpcoming) {
                    upcoming.date = "Next";
                    upcoming.opponent = "TBD";
                    upcoming.home = true;
                    upcoming.hasValue = false;
                }
                result.push_back(upcoming);

                finalStatus = LiveStatus::Live;
                message = "Live game log \xC2\xB7 " + sourceLabel;
            } catch (const std::exception& e) {
                result = SampleGenerator::genLog(player.id + "-" + cat.key, cat.mean, cat.variance, sport.teams, 20, sport.intervalDays);
                finalStatus = LiveStatus::Sample;
                message = "Live fetch failed (" + std::string(e.what()) + ") \xE2\x80\x94 showing sample data";
            }
        } else {
            result = SampleGenerator::genLog(player.id + "-" + cat.key, cat.mean, cat.variance, sport.teams, 20, sport.intervalDays);
            finalStatus = LiveStatus::Sample;
            message = "Sample game log";
        }

        if (state.requestId.load() != myRequest) return; // a newer selection superseded this one
        {
            std::lock_guard<std::mutex> lock(state.dataMutex);
            state.games = std::move(result);
            state.statusMessage = message;
        }
        state.status = finalStatus;
    }).detach();
}

// ---------------------------------------------------------------------------
// UI helpers
// ---------------------------------------------------------------------------

static ImVec4 hexToColor(const std::string& hex, float alpha = 1.0f) {
    unsigned int r, g, b;
    std::sscanf(hex.c_str() + 1, "%02x%02x%02x", &r, &g, &b);
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, alpha);
}

// True only for the one stat/league combo that currently has real opponent
// data behind it (MLB team strikeout rate vs. a pitcher's K prop). Every
// other combo shows an honest "not available yet" note instead of a made
// up number — see the Matchup view and the Filters panel below.
//
// Reads matchupLoaded under matchupMutex: it's written by the background
// fetch thread under that same lock, and reading it unlocked here was a
// real data race (the flag and the map it gates could be observed
// inconsistently mid-write). Bools rarely tear on real hardware, but this
// is undefined behavior regardless of whether it happens to crash — fixed
// properly rather than left as a "probably fine in practice" race.
static bool matchupDataAvailable(AppState& state) {
    if (state.sportKey != "MLB" || currentCategory(state).key != "k") return false;
    std::lock_guard<std::mutex> lock(state.matchupMutex);
    return state.matchupLoaded;
}

// Looks up one team's K% matchup stat under lock. Returns nullopt if the
// data isn't loaded yet or the team isn't found.
static std::optional<MatchupStat> lookupMatchupStat(AppState& state, const std::string& team) {
    std::lock_guard<std::mutex> lock(state.matchupMutex);
    auto it = state.mlbTeamKRates.find(team);
    if (it == state.mlbTeamKRates.end()) return std::nullopt;
    return it->second;
}

// Keeps only games whose opponent falls on the requested side of the
// matchup-difficulty split. A no-op (returns the input unchanged) whenever
// matchup data isn't available for the current sport/category — the
// Filters panel disables the radio buttons in that case so this should
// only ever be called with All in that situation anyway.
static std::vector<GameEntry> filterByMatchup(AppState& state, const std::vector<GameEntry>& games) {
    if (state.matchupFilter == MatchupFilter::All || !matchupDataAvailable(state)) return games;
    std::lock_guard<std::mutex> lock(state.matchupMutex);
    std::vector<GameEntry> out;
    for (const auto& g : games) {
        auto it = state.mlbTeamKRates.find(g.opponent);
        if (it == state.mlbTeamKRates.end()) continue; // unranked opponent — exclude rather than guess
        bool favorable = it->second.percentile >= 60;   // high team K% = tougher on the batter = good for a K prop
        bool challenging = it->second.percentile <= 40;
        if (state.matchupFilter == MatchupFilter::Favorable && favorable) out.push_back(g);
        else if (state.matchupFilter == MatchupFilter::Challenging && challenging) out.push_back(g);
    }
    return out;
}

static void applyDarkTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.043f, 0.055f, 0.075f, 1.0f);   // #0B0E13
    c[ImGuiCol_ChildBg] = ImVec4(0.051f, 0.063f, 0.086f, 1.0f);    // #0D1015
    c[ImGuiCol_FrameBg] = ImVec4(0.082f, 0.102f, 0.133f, 1.0f);    // #151A22
    c[ImGuiCol_Border] = ImVec4(0.137f, 0.165f, 0.212f, 1.0f);     // #232A36
    c[ImGuiCol_Text] = ImVec4(0.929f, 0.941f, 0.957f, 1.0f);       // #EDF0F4
    c[ImGuiCol_TextDisabled] = ImVec4(0.486f, 0.525f, 0.596f, 1.0f); // #7C8698
    c[ImGuiCol_Header] = ImVec4(0.106f, 0.126f, 0.161f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.137f, 0.165f, 0.212f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.051f, 0.063f, 0.086f, 1.0f);
    c[ImGuiCol_TabHovered] = ImVec4(0.137f, 0.165f, 0.212f, 1.0f);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int, char**) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "PropDash",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 860,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );
    // The 3-column layout below reserves a fixed 120px for the league
    // sidebar and 260px for the filters panel. If the window gets resized
    // smaller than that combined width, the middle column's computed
    // width goes negative, which produces an inverted/invalid clip
    // rectangle deep in ImGui's draw code — the actual cause of the
    // ClipRect assertion. Clamping the minimum window size here removes
    // the possibility at the source, on top of the defensive clamp added
    // to the width math itself below.
    SDL_SetWindowMinimumSize(window, 700, 500);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    applyDarkTheme();

    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    AppState state;
    requestData(state);       // initial game-log load
    loadBackgroundData(state); // kicks off full-roster + matchup-stat fetches

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("PropDash", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

        // Everything that builds this frame's content is wrapped in
        // try/catch. An uncaught C++ exception (std::out_of_range from a
        // map .at(), std::invalid_argument from a stoi, etc.) would
        // otherwise propagate out of the render loop and terminate the
        // whole app — a genuine crash, not just a visual glitch. Catching
        // it here means a bad frame shows a red error line instead of
        // closing the window, and clears the matchup filter (the one
        // piece of state most likely to be involved) so the next frame
        // has a chance to recover cleanly instead of hitting the same
        // exception again immediately.
        try {
            const SportConfig& sport = currentSport(state);
            const StatCategory& cat = currentCategory(state);
            const Player& player = currentPlayer(state);
            ImVec4 accent = hexToColor(sport.accentHex);

            ImGui::Columns(3, "layout", false);
            {
                // Belt-and-suspenders on top of SDL_SetWindowMinimumSize
                // above: even if window width somehow comes in smaller
                // than expected (e.g. a transient value during a resize
                // event, or a future layout tweak that changes the fixed
                // widths below), never hand ImGui a negative column width
                // — that's what produced the ClipRect assertion.
                float winW = ImGui::GetWindowWidth();
                float leftW = 120.0f;
                float rightW = 260.0f;
                float midW = std::max(200.0f, winW - leftW - rightW);
                ImGui::SetColumnWidth(0, leftW);
                ImGui::SetColumnWidth(1, midW);
            }

            // ---------------- Far-left column: league sidebar ----------------
            //
            // A real vertical tab list, not the horizontal row this used to
            // be — lives in its own column so it reads as permanent
            // navigation rather than part of the scrolling main content.
            // Picking a league scopes everything to its right (the always-
            // visible player list, search, categories, chart) to that
            // league only.
            {
                ImGui::TextDisabled("LEAGUE");
                ImGui::Spacing();
                for (const auto& sKey : sportOrder()) {
                    bool active = state.sportKey == sKey;
                    ImVec4 tabColor = hexToColor(sportsData().at(sKey).accentHex);
                    if (active) {
                        ImGui::PushStyleColor(ImGuiCol_Text, tabColor);
                        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(tabColor.x, tabColor.y, tabColor.z, 0.15f));
                    }
                    if (ImGui::Selectable(sKey.c_str(), active, 0, ImVec2(0, 26))) {
                        if (!active) {
                            state.sportKey = sKey;
                            state.categoryIndex = 0;
                            state.matchupFilter = MatchupFilter::All;
                            if (sKey == "MLB") state.mlbPlayerType = MlbPlayerType::StartingPitcher;
                            std::lock_guard<std::mutex> lock(state.rosterMutex);
                            const auto& roster = state.players[sKey];
                            const Player* pick = roster.empty() ? nullptr : &roster[0];
                            if (sKey == "MLB") {
                                for (const auto& p : roster) if (p.role == "SP") { pick = &p; break; }
                            }
                            if (pick) state.selectedPlayer = *pick;
                            requestData(state);
                        }
                    }
                    if (active) ImGui::PopStyleColor(2);
                }
            }

            ImGui::NextColumn();

        // ---------------- Main column: role toggle, search, browsable roster, view tabs, chart ----------------

        // MLB-only: Starting Pitchers / Hitters. Switches both which
        // category list applies (pitcher stats vs. hitter stats) and which
        // half of the roster below matches against.
        if (state.sportKey == "MLB") {
            const char* label0 = "Starting Pitchers";
            const char* label1 = "Hitters";
            bool spActive = state.mlbPlayerType == MlbPlayerType::StartingPitcher;
            if (spActive) ImGui::PushStyleColor(ImGuiCol_Text, accent);
            if (ImGui::Selectable(label0, spActive, 0, ImVec2(ImGui::CalcTextSize(label0).x + 10, 0)) && !spActive) {
                state.mlbPlayerType = MlbPlayerType::StartingPitcher;
                state.categoryIndex = 0;
                std::lock_guard<std::mutex> lock(state.rosterMutex);
                for (const auto& p : state.players["MLB"]) if (p.role == "SP") { state.selectedPlayer = p; break; }
                requestData(state);
            }
            if (spActive) ImGui::PopStyleColor();
            ImGui::SameLine(0, 14);
            bool hitActive = state.mlbPlayerType == MlbPlayerType::Hitter;
            if (hitActive) ImGui::PushStyleColor(ImGuiCol_Text, accent);
            if (ImGui::Selectable(label1, hitActive, 0, ImVec2(ImGui::CalcTextSize(label1).x + 10, 0)) && !hitActive) {
                state.mlbPlayerType = MlbPlayerType::Hitter;
                state.categoryIndex = 0;
                std::lock_guard<std::mutex> lock(state.rosterMutex);
                for (const auto& p : state.players["MLB"]) if (p.role == "Hitter") { state.selectedPlayer = p; break; }
                requestData(state);
            }
            if (hitActive) ImGui::PopStyleColor();
            ImGui::NewLine();
        }

        ImGui::PushItemWidth(-1);
        std::string searchHint = "Search " + state.sportKey + " players"
            + (state.sportKey == "MLB" ? (state.mlbPlayerType == MlbPlayerType::StartingPitcher ? " (starting pitchers)" : " (hitters)") : "");
        ImGui::InputTextWithHint("##search", searchHint.c_str(), state.searchBuf, sizeof(state.searchBuf));
        ImGui::PopItemWidth();

        // Search results only, not a persistent browsable list — nothing
        // shows until you type. Scoped to the selected league only (and,
        // for MLB, the selected role only): switching to NHL never shows
        // an NBA player, and Starting Pitchers never shows a reliever
        // (excluded from the roster entirely, upstream) or a hitter.
        if (strlen(state.searchBuf) > 0) {
            std::string q = state.searchBuf;
            std::transform(q.begin(), q.end(), q.begin(), ::tolower);
            ImGui::BeginChild("results", ImVec2(0, 140), true);
            {
                // Full-league rosters live under rosterMutex since a
                // background thread may be swapping a seed list out for
                // the real roster mid-frame.
                std::lock_guard<std::mutex> lock(state.rosterMutex);
                const std::vector<Player>& roster = state.players[state.sportKey];
                for (const Player& p : roster) {
                    if (state.sportKey == "MLB") {
                        bool wantSP = state.mlbPlayerType == MlbPlayerType::StartingPitcher;
                        if (wantSP && p.role != "SP") continue;
                        if (!wantSP && p.role != "Hitter") continue;
                    }
                    std::string nameLower = p.name, teamLower = p.team;
                    std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
                    std::transform(teamLower.begin(), teamLower.end(), teamLower.begin(), ::tolower);
                    if (nameLower.find(q) == std::string::npos && teamLower.find(q) == std::string::npos) continue;

                    std::string label = p.name + "   " + p.team + " \xC2\xB7 " + p.position + "##" + state.sportKey + p.id;
                    if (ImGui::Selectable(label.c_str())) {
                        state.selectedPlayer = p;
                        state.searchBuf[0] = '\0';
                        requestData(state);
                    }
                }
            }
            ImGui::EndChild();
        }

        ImGui::Spacing();

        // View tabs (GameLog / Matchup / Pitch Mix / Lineup / Bullpen) —
        // which section of the dashboard the left column is showing right
        // now. The per-stat category tabs live in the right sidebar (see
        // "Stat" section below), next to Filters.
        //
        // Matchup / Pitch Mix / Opposing Lineup / Bullpen are pitcher-
        // specific analysis views (built around a starting pitcher's
        // opponent, not a general prop concept), so they only make sense
        // for MLB Starting Pitchers. Every other league, and MLB Hitters,
        // only gets Game Log — showing an "Opposing Lineup" tab for an NBA
        // player or a hitter wouldn't correspond to anything real.
        {
            bool showPitcherTabs = state.sportKey == "MLB" && state.mlbPlayerType == MlbPlayerType::StartingPitcher;

            static const std::pair<ViewTab, const char*> pitcherViews[] = {
                {ViewTab::GameLog, "Game Log"}, {ViewTab::Matchup, "Matchup"},
                {ViewTab::PitchMix, "Pitch Mix"}, {ViewTab::Lineup, "Opposing Lineup"},
                {ViewTab::Bullpen, "Bullpen"},
            };
            static const std::pair<ViewTab, const char*> gameLogOnly[] = {
                {ViewTab::GameLog, "Game Log"},
            };
            const auto* views = showPitcherTabs ? pitcherViews : gameLogOnly;
            size_t viewCount = showPitcherTabs ? (sizeof(pitcherViews) / sizeof(pitcherViews[0]))
                                                : (sizeof(gameLogOnly) / sizeof(gameLogOnly[0]));

            // If a previous MLB-pitcher session left viewTab on something
            // other than Game Log and the context no longer supports it
            // (switched leagues, switched to Hitters), snap back rather
            // than silently rendering content for a tab that isn't shown.
            if (!showPitcherTabs && state.viewTab != ViewTab::GameLog) state.viewTab = ViewTab::GameLog;

            for (size_t i = 0; i < viewCount; i++) {
                ViewTab tab = views[i].first;
                const char* label = views[i].second;
                bool active = state.viewTab == tab;
                if (active) ImGui::PushStyleColor(ImGuiCol_Text, accent);
                if (ImGui::Selectable(label, active, 0, ImVec2(ImGui::CalcTextSize(label).x + 4, 0))) state.viewTab = tab;
                if (active) ImGui::PopStyleColor();
                ImGui::SameLine(0, 20);
            }
            ImGui::NewLine();
        }
        ImGui::Separator();
        ImGui::Spacing();

        // Metrics + chart, computed from a locked copy of the games list.
        // Fetched here (above the header) so the header itself can show
        // "Next: vs/at TEAM" regardless of which view tab is showing —
        // previously that line only existed inside the Game Log tab's own
        // panel, so switching to Matchup/Pitch Mix/etc. for a non-MLB
        // league lost it entirely.
        std::vector<GameEntry> gamesCopy;
        {
            std::lock_guard<std::mutex> lock(state.dataMutex);
            gamesCopy = state.games;
        }

        // Player header
        ImGui::PushFont(io.Fonts->Fonts[0]);
        ImGui::TextColored(ImVec4(1,1,1,1), "%s  %s", player.name.c_str(), player.position.c_str());
        ImGui::PopFont();
        ImGui::TextColored(ImVec4(0.486f, 0.525f, 0.596f, 1.0f), "%s \xC2\xB7 %s", player.team.c_str(), sport.name.c_str());
        ImGui::TextColored(accent, "%.1f %s line", cat.propLine(), cat.label.c_str());
        if (!gamesCopy.empty()) {
            const GameEntry& next = gamesCopy.back(); // always the upcoming placeholder — see requestData()
            ImGui::TextColored(ImVec4(0.486f, 0.525f, 0.596f, 1.0f), "Next: %s %s",
                next.home ? "vs" : "@", next.opponent.c_str());
        }

        ImGui::Spacing();

        // Status row
        {
            LiveStatus st = state.status.load();
            ImVec4 dotColor = st == LiveStatus::Live ? ImVec4(0.37f, 0.88f, 0.48f, 1) :
                               st == LiveStatus::Loading ? ImVec4(0.486f, 0.525f, 0.596f, 1) :
                               ImVec4(0.886f, 0.294f, 0.290f, 1);
            ImGui::TextColored(dotColor, "\xE2\x97\x8F");
            ImGui::SameLine();
            std::lock_guard<std::mutex> lock(state.dataMutex);
            if (st == LiveStatus::Loading) ImGui::TextColored(ImVec4(0.486f,0.525f,0.596f,1), "Fetching live data...");
            else ImGui::TextColored(ImVec4(0.486f,0.525f,0.596f,1), "%s", state.statusMessage.c_str());
        }

        ImGui::Spacing();

        if (state.viewTab != ViewTab::GameLog) {
            // ---------------- Alternate views ----------------
            if (state.viewTab == ViewTab::Matchup) {
                GameEntry upcomingForMatchup = gamesCopy.empty() ? GameEntry{} : gamesCopy.back();
                ImGui::TextColored(ImVec4(1,1,1,1), "Matchup Analyzer");
                ImGui::TextDisabled("Opponent context for the active prop, %s %s",
                    upcomingForMatchup.home ? "vs" : "@", upcomingForMatchup.opponent.c_str());
                ImGui::Spacing();
                if (matchupDataAvailable(state)) {
                    auto stat = lookupMatchupStat(state, upcomingForMatchup.opponent);
                    if (stat) {
                        bool favorable = stat->percentile >= 60;
                        bool challenging = stat->percentile <= 40;
                        ImGui::Text("%s team K%% (hitting): %.1f%%", upcomingForMatchup.opponent.c_str(), stat->value);
                        ImGui::ProgressBar(stat->percentile / 100.0f, ImVec2(-1, 0),
                            (std::to_string(stat->percentile) + " pct").c_str());
                        if (favorable) ImGui::TextColored(ImVec4(0.37f,0.88f,0.48f,1), "Favorable matchup for a strikeout prop \xE2\x80\x94 this lineup strikes out more than most.");
                        else if (challenging) ImGui::TextColored(ImVec4(0.886f,0.29f,0.29f,1), "Challenging matchup for a strikeout prop \xE2\x80\x94 this lineup makes contact more than most.");
                        else ImGui::TextDisabled("Roughly average matchup \xE2\x80\x94 no strong edge either way.");
                    } else {
                        ImGui::TextDisabled("No ranked data for %s yet.", upcomingForMatchup.opponent.c_str());
                    }
                } else {
                    bool isMlbK = state.sportKey == "MLB" && cat.key == "k";
                    std::lock_guard<std::mutex> lock(state.matchupMutex);
                    if (!isMlbK) {
                        ImGui::TextDisabled("Live matchup data currently covers MLB pitcher strikeouts only.");
                        ImGui::TextDisabled("Switch to a starting pitcher's Strikeouts tab to see it in action.");
                    } else if (state.matchupLoadFailed) {
                        ImGui::TextColored(ImVec4(0.886f,0.29f,0.29f,1), "Matchup data failed to load: %s", state.matchupError.c_str());
                        if (ImGui::Button("Retry")) requestMatchupData(state);
                    } else {
                        ImGui::TextDisabled("Loading team strikeout-rate data...");
                    }
                }
            } else {
                const char* title = state.viewTab == ViewTab::PitchMix ? "Pitch Mix"
                                   : state.viewTab == ViewTab::Lineup ? "Official Opposing Lineup" : "Bullpen";
                ImGui::TextColored(ImVec4(1,1,1,1), "%s", title);
                ImGui::Spacing();
                ImGui::TextDisabled("Not wired up to a live feed in this build yet.");
                ImGui::TextDisabled("This needs a per-pitch-type / lineup / bullpen data source beyond");
                ImGui::TextDisabled("the game-log endpoints this rewrite currently uses \xE2\x80\x94 tracked as a");
                ImGui::TextDisabled("follow-up rather than shown here with placeholder numbers.");
            }
        } else if (!gamesCopy.empty()) {
            std::vector<GameEntry> history(gamesCopy.begin(), gamesCopy.end() - 1);
            GameEntry upcoming = gamesCopy.back();

            // Next Matchup panel — shown inline in Game Log itself rather
            // than only in the separate Matchup tab, since "what's the
            // opponent and what do they look like" is core context for
            // reading the chart below it, not a side detail you should
            // have to go find on another tab.
            {
                ImGui::BeginChild("nextMatchup", ImVec2(0, 90), ImGuiChildFlags_Border);
                ImGui::TextColored(accent, "NEXT: %s %s", upcoming.home ? "vs" : "@", upcoming.opponent.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", upcoming.date.c_str());

                bool isMlbK = state.sportKey == "MLB" && cat.key == "k";
                if (isMlbK) {
                    auto stat = lookupMatchupStat(state, upcoming.opponent);
                    if (stat) {
                        bool favorable = stat->percentile >= 60;
                        bool challenging = stat->percentile <= 40;
                        ImGui::Text("%s team K%% (hitting): %.1f%% \xE2\x80\x94 %d%s percentile league-wide",
                            upcoming.opponent.c_str(), stat->value, stat->percentile,
                            (stat->percentile % 10 == 1 && stat->percentile != 11) ? "st" :
                            (stat->percentile % 10 == 2 && stat->percentile != 12) ? "nd" :
                            (stat->percentile % 10 == 3 && stat->percentile != 13) ? "rd" : "th");
                        if (favorable) ImGui::TextColored(ImVec4(0.37f,0.88f,0.48f,1), "Favorable matchup for this K prop \xE2\x80\x94 this lineup strikes out more than most.");
                        else if (challenging) ImGui::TextColored(ImVec4(0.886f,0.29f,0.29f,1), "Challenging matchup for this K prop \xE2\x80\x94 this lineup makes contact more than most.");
                        else ImGui::TextDisabled("Roughly average matchup \xE2\x80\x94 no strong edge either way.");
                    } else {
                        std::lock_guard<std::mutex> lock(state.matchupMutex);
                        if (state.matchupLoadFailed) {
                            ImGui::TextColored(ImVec4(0.886f,0.29f,0.29f,1), "Opponent data failed to load: %s", state.matchupError.c_str());
                            if (ImGui::Button("Retry##inline")) requestMatchupData(state);
                        } else if (!state.matchupLoaded) {
                            ImGui::TextDisabled("Loading opponent strikeout-rate data...");
                        } else {
                            ImGui::TextDisabled("No ranked data for %s.", upcoming.opponent.c_str());
                        }
                    }
                } else {
                    ImGui::TextDisabled("Opponent stat context is available for MLB pitcher Strikeouts right now.");
                }
                ImGui::EndChild();
            }
            ImGui::Spacing();

            int start = std::max(0, (int)history.size() - state.gamesShown);
            std::vector<GameEntry> visible(history.begin() + start, history.end());
            visible = filterByMatchup(state, visible);

            double line = cat.propLine();
            int hits = 0;
            double sum = 0;
            for (auto& g : visible) { if (g.value > line) hits++; sum += g.value; }
            double hitRate = visible.empty() ? 0 : (100.0 * hits / visible.size());
            double seasonAvg = visible.empty() ? 0 : sum / visible.size();
            int last5Start = std::max(0, (int)visible.size() - 5);
            double recentSum = 0;
            for (int i = last5Start; i < (int)visible.size(); i++) recentSum += visible[i].value;
            double recentAvg = (visible.size() - last5Start) > 0 ? recentSum / (visible.size() - last5Start) : 0;
            double trend = recentAvg - seasonAvg;

            ImGui::Columns(3, "metrics", false);
            ImGui::TextColored(ImVec4(0.486f,0.525f,0.596f,1), "HIT RATE");
            ImGui::TextColored(hitRate >= 50 ? ImVec4(0.37f,0.88f,0.48f,1) : ImVec4(0.886f,0.29f,0.29f,1), "%.0f%% (%d/%d)", hitRate, hits, (int)visible.size());
            ImGui::NextColumn();
            ImGui::TextColored(ImVec4(0.486f,0.525f,0.596f,1), "SEASON AVG");
            ImGui::Text("%.1f", seasonAvg);
            ImGui::NextColumn();
            ImGui::TextColored(ImVec4(0.486f,0.525f,0.596f,1), "L5 TREND");
            ImGui::TextColored(trend >= 0 ? ImVec4(0.37f,0.88f,0.48f,1) : ImVec4(0.886f,0.29f,0.29f,1), "%s%.1f", trend >= 0 ? "+" : "", trend);
            ImGui::Columns(1);

            ImGui::Spacing();

            // Chart: two bar series (over/under the line), NAN gaps so only
            // the matching color renders at each x position, plus a dashed
            // horizontal line for the prop line itself.
            std::vector<GameEntry> plotted = visible;
            plotted.push_back(upcoming);
            int n = (int)plotted.size();

            std::vector<double> xs(n), overVals(n), underVals(n), lineY(n);
            std::vector<std::string> tickLabels(n);
            for (int i = 0; i < n; i++) {
                xs[i] = i;
                lineY[i] = line;
                if (!plotted[i].hasValue) {
                    overVals[i] = NAN;
                    underVals[i] = NAN;
                } else if (plotted[i].value > line) {
                    overVals[i] = plotted[i].value;
                    underVals[i] = NAN;
                } else {
                    overVals[i] = NAN;
                    underVals[i] = plotted[i].value;
                }
                tickLabels[i] = (plotted[i].home ? "vs " : "@") + plotted[i].opponent + "\n" + plotted[i].date;
            }
            std::vector<const char*> tickPtrs;
            for (auto& s : tickLabels) tickPtrs.push_back(s.c_str());

            if (n < 2) {
                // Only the "upcoming" placeholder is left — the matchup
                // filter excluded every real game. Rendering ImPlot with a
                // single point sends v_min==v_max into SetupAxisTicks,
                // which divides by (n_ticks - 1) internally — a real
                // division-by-zero inside ImPlot's own code, not something
                // a C++ try/catch can intercept, and the actual root cause
                // of the Favorable/Challenging crash. Showing a message
                // instead of the chart avoids ever calling ImPlot with a
                // degenerate 1-point range.
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.886f,0.29f,0.29f,1), "No games match this matchup filter in the selected window.");
                ImGui::TextDisabled("Try \"All games\", or show more games shown in Filters.");
            } else if (ImPlot::BeginPlot("##chart", ImVec2(-1, 320), ImPlotFlags_NoLegend)) {
                ImPlot::SetupAxes("Game", cat.label.c_str());
                ImPlot::SetupAxisTicks(ImAxis_X1, 0, n - 1, n, tickPtrs.data());
                ImPlot::SetupAxisLimits(ImAxis_X1, -0.5, n - 0.5, ImGuiCond_Always);

                ImPlot::SetNextLineStyle(ImVec4(0.949f, 0.722f, 0.294f, 1.0f), 1.5f); // #F2B84B
                ImPlot::PlotLine("Line", xs.data(), lineY.data(), n);

                ImPlot::SetNextFillStyle(ImVec4(0.37f, 0.88f, 0.48f, 1.0f));
                ImPlot::PlotBars("Over", xs.data(), overVals.data(), n, 0.6);

                ImPlot::SetNextFillStyle(ImVec4(0.886f, 0.29f, 0.29f, 1.0f));
                ImPlot::PlotBars("Under", xs.data(), underVals.data(), n, 0.6);

                ImPlot::EndPlot();
            }

            ImGui::TextColored(ImVec4(0.3f,0.6f,1,1), " "); // spacing
            ImGui::TextColored(ImVec4(0.37f,0.88f,0.48f,1), "\xE2\x96\xA0"); ImGui::SameLine(); ImGui::TextDisabled("Over line   ");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.886f,0.29f,0.29f,1), "\xE2\x96\xA0"); ImGui::SameLine(); ImGui::TextDisabled("Under line");
        } else {
            ImGui::TextDisabled("Loading...");
        }

        ImGui::TextColored(ImVec4(0.29f,0.32f,0.38f,1), "PROPDASH C++ \xC2\xB7 live game logs for all 5 leagues \xE2\x80\x94 sample data only on fetch failure");

        // ---------------- Right column: stat tabs + filters ----------------

        ImGui::NextColumn();

        // Stat category tabs — moved here from the top of the left column
        // so they sit directly above (to the right of) the Filters panel.
        // Uses currentCategoryList() rather than sport.categories directly
        // so MLB's Hitters tab shows hitter stats, not pitcher stats.
        ImGui::TextColored(ImVec4(1,1,1,1), "Stat");
        ImGui::Separator();
        ImGui::Spacing();
        const std::vector<StatCategory>& catList = currentCategoryList(state);
        for (size_t i = 0; i < catList.size(); i++) {
            bool active = (int)i == state.categoryIndex;
            if (active) ImGui::PushStyleColor(ImGuiCol_Text, accent);
            if (ImGui::Selectable(catList[i].label.c_str(), active)) {
                if (!active) {
                    state.categoryIndex = (int)i;
                    state.matchupFilter = MatchupFilter::All; // stays valid only per-category
                    requestData(state);
                }
            }
            if (active) ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1,1,1,1), "Filters");
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextDisabled("Games shown");
        if (ImGui::RadioButton("Last 10 games", state.gamesShown == 10)) state.gamesShown = 10;
        if (ImGui::RadioButton("Last 20 games", state.gamesShown == 20)) state.gamesShown = 20;

        ImGui::Spacing();
        ImGui::TextDisabled("Matchup difficulty");
        // Real BeginDisabled/EndDisabled instead of a manual alpha-push +
        // "ignore the click" hack. The old version still let RadioButton
        // fully process the click (it just discarded the result), which
        // meant three interactive widgets sharing IDs/active-item state
        // every frame whether or not the data backing them existed —
        // BeginDisabled actually removes them from hit-testing, which is
        // the correct fix rather than a cosmetic one.
        bool avail = matchupDataAvailable(state);
        ImGui::BeginDisabled(!avail);
        if (ImGui::RadioButton("All games##mf", state.matchupFilter == MatchupFilter::All)) state.matchupFilter = MatchupFilter::All;
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.37f,0.88f,0.48f,1));
        if (ImGui::RadioButton("Favorable only##mf", state.matchupFilter == MatchupFilter::Favorable)) state.matchupFilter = MatchupFilter::Favorable;
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.886f,0.29f,0.29f,1));
        if (ImGui::RadioButton("Challenging only##mf", state.matchupFilter == MatchupFilter::Challenging)) state.matchupFilter = MatchupFilter::Challenging;
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        if (!avail) {
            ImGui::TextDisabled("(MLB Strikeouts only, for now)");
        } else if (state.matchupFilter != MatchupFilter::All) {
            ImGui::TextDisabled("Filtering games by opponent K%% rank");
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Rosters");
        {
            std::lock_guard<std::mutex> lock(state.rosterMutex);
            auto it = state.rosterStatus.find(state.sportKey);
            if (it == state.rosterStatus.end()) ImGui::TextDisabled("Loading full %s roster...", sport.name.c_str());
            else if (it->second.empty()) ImGui::TextColored(ImVec4(0.37f,0.88f,0.48f,1), "Full roster loaded (%d players)", (int)state.players[state.sportKey].size());
            else ImGui::TextColored(ImVec4(0.886f,0.29f,0.29f,1), "%s", it->second.c_str());
        }

        ImGui::Columns(1);
        } catch (const std::exception& e) {
            // Reset columns state in case the exception hit mid-layout —
            // Columns(1) is always safe to call even if already at 1
            // column, so this can't itself throw or misbehave.
            ImGui::Columns(1);
            state.matchupFilter = MatchupFilter::All;
            ImGui::TextColored(ImVec4(0.886f, 0.29f, 0.29f, 1), "Render error (recovered): %s", e.what());
        }
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 11, 14, 19, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
