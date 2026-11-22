// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstddef>
#include <string_view>

namespace funrounds::native::detail {

// Keep lookup keys statically discoverable, including those stored in tables.
enum class GamedataSection { Signature, Offset, VTable };

template <GamedataSection Section>
class GamedataKey {
public:
    template <std::size_t Size>
    explicit consteval GamedataKey(const char (&literal)[Size]) : name_(literal, Size - 1) {}

    constexpr std::string_view name() const noexcept { return name_; }

private:
    std::string_view name_;
};

using SignatureKey = GamedataKey<GamedataSection::Signature>;
using OffsetKey = GamedataKey<GamedataSection::Offset>;
using VTableKey = GamedataKey<GamedataSection::VTable>;

} // namespace funrounds::native::detail
