// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <source_location>

#include "bighead_policy.hpp"

namespace {

void require(bool condition, const std::source_location& location = std::source_location::current()) {
    if (!condition) {
        std::cerr << "Big Head policy check failed at line " << location.line() << '\n';
        std::abort();
    }
}

} // namespace

int main() {
    using namespace funrounds::native::bighead;

    const auto lease = BeginMutatorBitLease(0xA0U);
    require(lease.owned);
    require(!lease.originallySet);
    require(EnableOwnedMutatorBit(0xA0U, lease) == 0xA1U);
    require(RestoreOwnedMutatorBit(0x21U, lease) == 0x20U);
    require(RestoreOwnedMutatorBit(0x20U, lease) == 0x20U);

    const auto existing = BeginMutatorBitLease(0x81U);
    require(existing.originallySet);
    require(!existing.owned);
    require(EnableOwnedMutatorBit(0x81U, existing) == 0x81U);
    require(EnableOwnedMutatorBit(0x80U, existing) == 0x80U);
    require(RestoreOwnedMutatorBit(0x81U, existing) == 0x81U);

    require(IsValidScale(1.0F));
    require(IsValidScale(2.5F));
    require(!IsValidScale(0.0F));
    require(!IsValidScale(-1.0F));
    require(!IsValidScale(std::numeric_limits<float>::infinity()));
    require(!IsValidScale(std::numeric_limits<float>::quiet_NaN()));
    require(StillOwnsScale(2.0F, 2.0F));
    require(!StillOwnsScale(1.0F, 2.0F));

    return 0;
}
