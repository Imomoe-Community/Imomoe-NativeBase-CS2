// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string_view>

#include "standard_grenade_policy.hpp"

namespace {

void require(bool condition, const std::source_location& location = std::source_location::current()) {
    if (!condition) {
        std::cerr << "standard grenade policy check failed at line " << location.line() << '\n';
        std::abort();
    }
}

} // namespace

int main() {
    using namespace funrounds::native::grenade;

    require(ValidKind(1));
    require(ValidKind(2));
    require(ValidKind(3));
    require(ValidKind(4));
    require(!ValidKind(-1));
    require(!ValidKind(0));
    require(ValidKind(5));
    require(ValidKind(6));
    require(!ValidKind(7));
    require(ProjectileClass(Kind::Decoy) == "decoy_projectile");
    require(!VTableName(Kind::Decoy));
    require(VTableName(Kind::Flash).value().name() == "CFlashbangProjectile");
    require(WeaponClass(Kind::Incendiary) == "weapon_incgrenade");
    require(UsesDetonationHold(Kind::Decoy) && UsesDetonationHold(Kind::Smoke));
    require(UsesDetonationHold(Kind::Molotov) && UsesDetonationHold(Kind::Incendiary));
    require(!UsesDetonationHold(Kind::He) && !UsesDetonationHold(Kind::Flash));
    require(ProjectileClass(Kind::Smoke) == "smokegrenade_projectile");
    require(ProjectileClass(Kind::Molotov) == "molotov_projectile");
    require(ProjectileClass(Kind::Incendiary) == "molotov_projectile");
    require(ProjectileClass(Kind::He) == "hegrenade_projectile");
    require(VTableName(Kind::Smoke).value().name() == "CSmokeGrenadeProjectile");
    require(VTableName(Kind::Molotov).value().name() == "CMolotovProjectile");
    require(VTableName(Kind::Incendiary).value().name() == "CMolotovProjectile");
    require(VTableName(Kind::He).value().name() == "CHEGrenadeProjectile");
    require(ProjectileClass(static_cast<Kind>(99)).empty());
    require(!VTableName(static_cast<Kind>(99)));
    require(ResultValue(RequestResult::Failed) == -1);
    require(ResultValue(RequestResult::AlreadyStartedOrGone) == 0);
    require(ResultValue(RequestResult::Submitted) == 1);
    require(StateValue(StateResult::Failed) == -1);
    require(StateValue(StateResult::NotStarted) == 0);
    require(StateValue(StateResult::StartedOrGone) == 1);
    return 0;
}
