// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <cstdlib>
#include <iostream>
#include <source_location>

#include "team_flash_policy.hpp"

namespace {

void Require(bool condition, const std::source_location& location = std::source_location::current()) {
    if (!condition) {
        std::cerr << "Team flash policy check failed at line " << location.line() << '\n';
        std::abort();
    }
}

} // namespace

int main() {
    using namespace funrounds::native::team_flash;
    constexpr Attribution t{0, 0x00020001, 2};
    constexpr Attribution ct{1, 0x80020002, 3};
    constexpr std::uint32_t oldFlash = 0x00040051, newFlash = 0x80040051, concurrent = 0x00040052;
    AttributionTable table;
    table.Record(oldFlash, t);
    table.Record(concurrent, t);

    Require(!table.Find(newFlash));
    Require(ShouldVetoTeamFlash(false, 2, 2, 0x00020003, table.Find(oldFlash)));
    table.Record(newFlash, ct);
    table.Erase(oldFlash);
    Require(!table.Find(oldFlash));
    Require(ShouldVetoTeamFlash(false, 2, 3, 0x00020003, table.Find(newFlash)));
    Require(ShouldVetoTeamFlash(false, 2, 2, 0x00020003, table.Find(concurrent)));

    Require(!ShouldVetoTeamFlash(true, 2, 3, 0x00020003, ct));
    Require(!ShouldVetoTeamFlash(false, 2, 2, 0x00020003, ct));
    Require(!ShouldVetoTeamFlash(false, 1, 3, 0x00040002, ct));
    Require(!ShouldVetoTeamFlash(false, 4, 3, ct.throwerPawnHandle, ct));
    Require(!ShouldVetoTeamFlash(false, 2, 1, 0x00020003, Attribution{1, ct.throwerPawnHandle, 1}));
    Require(!ShouldVetoTeamFlash(false, 2, 3, 0x00020003, Attribution{1, ct.throwerPawnHandle, 0}));
    Require(!ShouldVetoTeamFlash(false, 2, 3, 0x00020003, std::nullopt));

    table.Record(newFlash, t);
    Require(ShouldVetoTeamFlash(false, 2, 2, 0x00020003, table.Find(newFlash)));
    table.Clear();
    Require(!table.Find(newFlash) && !table.Find(concurrent));
}
