#pragma once
#include "Types.h"
#include <vector>
#include <string>
#include <random>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cmath>
#include <algorithm>

namespace SampleGenerator {

    // Fixed "today" so the same seed always produces the same log across runs.
    inline std::tm fixedToday() {
        std::tm t{};
        t.tm_year = 2026 - 1900;
        t.tm_mon = 7; // August (0-indexed)
        t.tm_mday = 14;
        return t;
    }

    inline std::string formatDate(std::tm t) {
        char buf[16];
        static const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
        std::snprintf(buf, sizeof(buf), "%s %02d", months[t.tm_mon], t.tm_mday);
        return std::string(buf);
    }

    inline std::tm subtractDays(std::tm base, int days) {
        std::time_t t = std::mktime(&base);
        t -= (std::time_t)days * 86400;
        std::tm* result = std::localtime(&t);
        return *result;
    }

    inline unsigned int hashSeed(const std::string& s) {
        unsigned int h = 2166136261u; // FNV-1a
        for (char c : s) { h ^= (unsigned char)c; h *= 16777619u; }
        return h;
    }

    // Same seed string always yields the same log — switching tabs and back
    // looks stable instead of re-randomizing every render.
    inline std::vector<GameEntry> genLog(const std::string& seedStr, double mean, double variance,
                                          const std::vector<std::string>& opponents, int count, int intervalDays) {
        std::mt19937 rng(hashSeed(seedStr));
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        std::uniform_int_distribution<size_t> oppPick(0, opponents.size() - 1);

        std::vector<GameEntry> games;
        std::tm today = fixedToday();

        for (int i = 0; i < count; i++) {
            std::tm date = subtractDays(today, (long)intervalDays * (count - i));
            double raw = mean + (unit(rng) - 0.5) * 2 * variance;
            double value = std::round(std::max(0.0, raw) * 10) / 10.0;
            GameEntry g;
            g.date = formatDate(date);
            g.opponent = opponents[oppPick(rng)];
            g.home = unit(rng) > 0.5;
            g.hasValue = true;
            g.value = value;
            games.push_back(g);
        }
        // Final entry: the upcoming game, no result yet
        GameEntry upcoming;
        upcoming.date = formatDate(today);
        upcoming.opponent = opponents[oppPick(rng)];
        upcoming.home = unit(rng) > 0.5;
        upcoming.hasValue = false;
        games.push_back(upcoming);
        return games;
    }
}
