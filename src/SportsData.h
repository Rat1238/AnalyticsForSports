#pragma once
#include "Types.h"
#include <map>
#include <vector>
#include <string>

// Returns the 5 leagues in a fixed, stable order (map iteration order isn't
// guaranteed, so callers that need tab order should use this instead of
// iterating SportsData::all() directly).
inline const std::vector<std::string>& sportOrder() {
    static std::vector<std::string> order = { "MLB", "NBA", "WNBA", "NHL", "NFL" };
    return order;
}

inline const std::map<std::string, SportConfig>& sportsData() {
    static std::map<std::string, SportConfig> data = {
        { "MLB", SportConfig{
            "MLB", "#F2B84B", 2,
            {
                {"k", "Strikeouts", 6.5, 2.4},
                {"outs", "Pitcher Outs", 16.5, 3},
                {"er", "Earned Runs", 2.2, 1.4},
                {"hits", "Hits Allowed", 5, 2},
                {"bb", "Pitcher Walks", 1.9, 1.1},
            },
            {
                // Seed list, shown instantly on launch and replaced by the
                // full roster once RosterLoader finishes (see
                // RosterLoader.h). Kept as the permanent fallback if that
                // fetch fails (offline, statsapi down, etc). Includes both
                // roles so the Starting Pitchers / Hitters tabs both have
                // something to show before the live roster load completes.
                {"m1", "Chase Burns", "CIN", "RHP", "CB", "MLB", "SP"},
                {"m2", "Paul Skenes", "PIT", "RHP", "PS", "MLB", "SP"},
                {"m3", "Tarik Skubal", "DET", "LHP", "TS", "MLB", "SP"},
                {"m4", "Gerrit Cole", "NYY", "RHP", "GC", "MLB", "SP"},
                {"m5", "Zack Wheeler", "PHI", "RHP", "ZW", "MLB", "SP"},
                {"mh1", "Aaron Judge", "NYY", "OF", "AJ", "MLB", "Hitter"},
                {"mh2", "Juan Soto", "NYM", "OF", "JS", "MLB", "Hitter"},
                {"mh3", "Bobby Witt Jr.", "KC", "SS", "BW", "MLB", "Hitter"},
                {"mh4", "Shohei Ohtani", "LAD", "DH", "SO", "MLB", "Hitter"},
                {"mh5", "Corbin Carroll", "ARI", "OF", "CC", "MLB", "Hitter"},
            },
            {"ARI","ATL","BAL","BOS","CHC","CWS","CIN","CLE","COL","DET",
             "HOU","KC","LAA","LAD","MIA","MIL","MIN","NYM","NYY","OAK",
             "PHI","PIT","SD","SF","SEA","STL","TB","TEX","TOR","WSH"},
            true,
            "", // MLB uses statsapi.mlb.com directly, not ESPN
            {
                // Hitter categories — separate hydrate group ("hitting")
                // and separate stat-field mapping in NetClient::fetchMlbGameLog.
                {"bhits", "Hits", 1.1, 0.9, "hitting"},
                {"hr", "Home Runs", 0.35, 0.4, "hitting"},
                {"rbi", "RBIs", 0.7, 0.7, "hitting"},
                {"tb", "Total Bases", 1.6, 1.1, "hitting"},
                {"hrr", "Hits+Runs", 1.8, 1.2, "hitting"},
            }
        }},
        { "NBA", SportConfig{
            "NBA", "#FF6B4A", 3,
            {
                {"pts", "Points", 26, 8},
                {"reb", "Rebounds", 7, 3},
                {"ast", "Assists", 6, 3},
                {"pra", "Pts+Reb+Ast", 38, 10},
            },
            {
                {"n1", "J. Tatum", "BOS", "F", "JT", "NBA"},
                {"n2", "S. Gilgeous-Alexander", "OKC", "G", "SGA", "NBA"},
                {"n3", "L. Doncic", "LAL", "G", "LD", "NBA"},
                {"n4", "G. Antetokounmpo", "MIL", "F", "GA", "NBA"},
                {"n5", "A. Edwards", "MIN", "G", "AE", "NBA"},
            },
            {"ATL","BOS","BKN","CHA","CHI","CLE","DAL","DEN","DET","GSW",
             "HOU","IND","LAC","LAL","MEM","MIA","MIL","MIN","NOP","NYK",
             "OKC","ORL","PHI","PHX","POR","SAC","SAS","TOR","UTA","WAS"},
            true, // live via stats.nba.com (NetClient::fetchNbaFamilyGameLog)
            "basketball/nba" // ESPN still used for the roster list, just not the game log
        }},
        { "WNBA", SportConfig{
            "WNBA", "#FF5FA2", 3,
            {
                {"pts", "Points", 18, 6},
                {"reb", "Rebounds", 7, 3},
                {"ast", "Assists", 5, 2.5},
                {"pra", "Pts+Reb+Ast", 28, 8},
            },
            {
                {"w1", "A'ja Wilson", "LVA", "F", "AW", "WNBA"},
                {"w2", "Caitlin Clark", "IND", "G", "CC", "WNBA"},
                {"w3", "Breanna Stewart", "NYL", "F", "BS", "WNBA"},
                {"w4", "Sabrina Ionescu", "NYL", "G", "SI", "WNBA"},
                {"w5", "Napheesa Collier", "MIN", "F", "NC", "WNBA"},
            },
            {"ATL","CHI","CON","DAL","GSV","IND","LVA","MIN","NYL","PHX","SEA","WAS","LAS"},
            true, // live via stats.wnba.com (NetClient::fetchNbaFamilyGameLog)
            "basketball/wnba"
        }},
        { "NHL", SportConfig{
            "NHL", "#4FD1FF", 3,
            {
                {"sog", "Shots on Goal", 3.2, 1.3},
                {"pts", "Points", 1.1, 0.9},
                {"goals", "Goals", 0.5, 0.5},
                {"blk", "Blocked Shots", 1.5, 1},
            },
            {
                {"h1", "A. Matthews", "TOR", "C", "AM", "NHL"},
                {"h2", "C. McDavid", "EDM", "C", "CM", "NHL"},
                {"h3", "N. MacKinnon", "COL", "C", "NM", "NHL"},
                {"h4", "D. Pastrnak", "BOS", "RW", "DP", "NHL"},
                {"h5", "K. Kaprizov", "MIN", "LW", "KK", "NHL"},
            },
            {"ANA","BOS","BUF","CGY","CAR","CHI","COL","CBJ","DAL","DET",
             "EDM","FLA","LAK","MIN","MTL","NSH","NJD","NYI","NYR","OTT",
             "PHI","PIT","SJS","SEA","STL","TBL","TOR","UTA","VAN","VGK",
             "WSH","WPG"},
            true,
            "" // NHL uses api-web.nhle.com directly, not ESPN
        }},
        { "NFL", SportConfig{
            "NFL", "#5FE07A", 7,
            {
                {"recyds", "Receiving Yards", 78, 25},
                {"rec", "Receptions", 5.5, 1.8},
                {"rushyds", "Rush Yards", 58, 22},
                {"td", "Touchdowns", 0.8, 0.7},
            },
            {
                {"f1", "J. Chase", "CIN", "WR", "JC", "NFL"},
                {"f2", "C. Lamb", "DAL", "WR", "CL", "NFL"},
                {"f3", "T. Kelce", "KC", "TE", "TK", "NFL"},
                {"f4", "S. Barkley", "PHI", "RB", "SB", "NFL"},
                {"f5", "P. Nacua", "LAR", "WR", "PN", "NFL"},
            },
            {"ARI","ATL","BAL","BUF","CAR","CHI","CIN","CLE","DAL","DEN",
             "DET","GB","HOU","IND","JAX","KC","LV","LAC","LAR","MIA",
             "MIN","NE","NO","NYG","NYJ","PHI","PIT","SF","SEA","TB",
             "TEN","WSH"},
            true, // live via the nflverse player_stats.csv cache (see NflverseLoader.h)
            "football/nfl"
        }},
    };
    return data;
}
