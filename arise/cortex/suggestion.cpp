#include "cortex/suggestion.hpp"

#include <ctime>

using namespace std::chrono;

namespace arise {

const char* tierToString(Tier t) {
    switch (t) {
        case Tier::Silent:  return "silent";
        case Tier::Ambient: return "ambient";
        case Tier::Active:  return "active";
        case Tier::Urgent:  return "urgent";
    }
    return "ambient";
}

std::optional<Tier> tierFromString(std::string_view s) {
    if (s == "silent")  return Tier::Silent;
    if (s == "ambient") return Tier::Ambient;
    if (s == "active")  return Tier::Active;
    if (s == "urgent")  return Tier::Urgent;
    return std::nullopt;
}

int tierRank(Tier t) {
    switch (t) {
        case Tier::Silent:  return 0;
        case Tier::Ambient: return 1;
        case Tier::Active:  return 2;
        case Tier::Urgent:  return 3;
    }
    return 0;
}

const char* gateOutcomeToString(GateOutcome o) {
    switch (o) {
        case GateOutcome::Pass:                  return "pass";
        case GateOutcome::BlockedRateLimit:      return "blocked_rate_limit";
        case GateOutcome::BlockedQuietHours:     return "blocked_quiet_hours";
        case GateOutcome::BlockedCategoryMuted:  return "blocked_category_muted";
        case GateOutcome::BlockedSilent:         return "blocked_silent";
    }
    return "blocked_rate_limit";
}

SuggestionGate::SuggestionGate(Config cfg) : cfg_(std::move(cfg)) {}

bool SuggestionGate::isInQuietHours(system_clock::time_point t) const {
    if (!cfg_.quiet_hours_enabled) return false;
    std::time_t tt = system_clock::to_time_t(t);
    std::tm tm_buf{};
    localtime_r(&tt, &tm_buf);
    int h = tm_buf.tm_hour;

    int s = cfg_.quiet_start_hour;
    int e = cfg_.quiet_end_hour;
    if (s == e) return false;            // disabled by equal bounds
    if (s < e) return h >= s && h < e;   // doesn't wrap midnight
    return h >= s || h < e;              // wraps midnight (default)
}

GateOutcome SuggestionGate::check(const Suggestion& s,
                                  system_clock::time_point now) const {
    if (s.tier == Tier::Silent) return GateOutcome::BlockedSilent;

    // Category mute first — applies to all non-urgent tiers.
    if (s.tier != Tier::Urgent) {
        auto it = muted_.find(s.category);
        if (it != muted_.end() && now < it->second.until) {
            return GateOutcome::BlockedCategoryMuted;
        }
    }

    // Quiet hours: only Urgent passes.
    if (s.tier != Tier::Urgent && isInQuietHours(now)) {
        return GateOutcome::BlockedQuietHours;
    }

    // Per-tier rate limit.
    auto interval_for = [&](Tier t) -> seconds {
        switch (t) {
            case Tier::Ambient: return cfg_.min_interval_ambient;
            case Tier::Active:  return cfg_.min_interval_active;
            case Tier::Urgent:  return cfg_.min_interval_urgent;
            default:            return seconds{0};
        }
    };
    auto last_for = [&](Tier t) {
        switch (t) {
            case Tier::Ambient: return last_fired_ambient_;
            case Tier::Active:  return last_fired_active_;
            case Tier::Urgent:  return last_fired_urgent_;
            default:            return system_clock::time_point{};
        }
    };

    auto last = last_for(s.tier);
    auto need = interval_for(s.tier);
    if (need.count() > 0 && last.time_since_epoch().count() != 0) {
        if (now - last < need) return GateOutcome::BlockedRateLimit;
    }
    return GateOutcome::Pass;
}

void SuggestionGate::noteFired(Tier t, system_clock::time_point now) {
    switch (t) {
        case Tier::Silent:  return;
        case Tier::Ambient: last_fired_ambient_ = now; return;
        case Tier::Active:  last_fired_active_  = now; return;
        case Tier::Urgent:  last_fired_urgent_  = now; return;
    }
}

void SuggestionGate::muteCategory(const std::string& category,
                                  seconds duration) {
    if (category.empty()) return;
    if (duration.count() <= 0) {
        muted_.erase(category);
        return;
    }
    muted_[category] = MuteEntry{ system_clock::now() + duration };
}

void SuggestionGate::setConsecutiveRejects(const std::string& category, int n) {
    if (category.empty()) return;
    if (n <= 0) {
        consecutive_rejects_.erase(category);
    } else {
        consecutive_rejects_[category] = n;
    }
    // Auto-engage mute when threshold crossed.
    if (n >= cfg_.mute_after_rejects) {
        muteCategory(category, cfg_.mute_window);
    }
}

int SuggestionGate::consecutiveRejects(const std::string& category) const {
    auto it = consecutive_rejects_.find(category);
    return it == consecutive_rejects_.end() ? 0 : it->second;
}

system_clock::time_point SuggestionGate::lastFiredAt(Tier t) const {
    switch (t) {
        case Tier::Ambient: return last_fired_ambient_;
        case Tier::Active:  return last_fired_active_;
        case Tier::Urgent:  return last_fired_urgent_;
        default:            return {};
    }
}

} // namespace arise
