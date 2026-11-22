// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <cstring>

#include <plugin_export.h>

#include "engine.hpp"

namespace funrounds::native::detail {
namespace {

using AddWin = void (*)(void*, std::uint32_t);

struct RoundScore {
    AddWin addT{};
    AddWin addCt{};
    int stateOffset{};
    SchemaField winner{};
    SchemaField warmup{};
};

const RoundScore* ResolveRoundScore() {
    static RoundScore cached;
    if (cached.addT) return &cached;
    if (!EnsureEngine() || !__s2sdk_GetGameRules || !__s2sdk_TerminateRound) return nullptr;
    RoundScore score;
    score.stateOffset = ProbeOffset(OffsetKey{"FunRounds::RoundScoreState"});
    if (score.stateOffset <= 0 ||
        !ResolveSchemaField("CCSGameRules", "m_iRoundWinStatus", sizeof(std::int32_t), score.winner) ||
        !ResolveSchemaField("CCSGameRules", "m_bWarmupPeriod", sizeof(std::uint8_t), score.warmup)) return nullptr;
    score.addT = reinterpret_cast<AddWin>(ProbeSignature(SignatureKey{"FunRounds::RoundScoreAddT"}));
    score.addCt = reinterpret_cast<AddWin>(ProbeSignature(SignatureKey{"FunRounds::RoundScoreAddCT"}));
    if (!score.addT || !score.addCt) return nullptr;
    cached = score;
    return &cached;
}

bool FinishWin(std::int32_t team, float restartDelay) {
    if (team != 2 && team != 3) return false;
    const auto* score = ResolveRoundScore();
    if (!score) return false;
    auto* rules = static_cast<std::byte*>(s2sdk::GetGameRules());
    if (!rules) return false;
    std::int32_t winner;
    std::uint8_t warmup;
    std::memcpy(&winner, rules + score->winner.offset, sizeof(winner));
    std::memcpy(&warmup, rules + score->warmup.offset, sizeof(warmup));
    if (winner != 0 || warmup != 0) return false;
    const auto addWin = team == 2 ? score->addT : score->addCt;
    addWin(rules + score->stateOffset, 1);
    const auto reason = team == 2 ? s2sdk::CSRoundEndReason::TerroristWin : s2sdk::CSRoundEndReason::CTWin;
    s2sdk::TerminateRound(restartDelay, reason);
    return true;
}

} // namespace
} // namespace funrounds::native::detail

extern "C" PLUGIN_API bool HasNativeRoundScoreSupport() noexcept {
    try {
        return funrounds::native::detail::ResolveRoundScore() != nullptr;
    } catch (...) {
        return false;
    }
}

extern "C" PLUGIN_API bool FinishNativeTeamRound(std::int32_t team, float restartDelay) noexcept {
    try {
        return funrounds::native::detail::FinishWin(team, restartDelay);
    } catch (...) {
        return false;
    }
}
