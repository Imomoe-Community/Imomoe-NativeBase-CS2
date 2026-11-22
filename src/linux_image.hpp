// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

namespace funrounds::native::detail {

inline constexpr std::uint32_t kPtLoad = 1;
inline constexpr std::uint32_t kPfRead = 4;

template <typename ProgramHeader>
bool ContainsReadableImageRange(const ProgramHeader* headers, std::size_t count,
                                std::uint64_t rva, std::size_t length) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        const ProgramHeader& header = headers[index];
        if (header.p_type == kPtLoad && (header.p_flags & kPfRead) != 0 &&
            rva >= header.p_vaddr && length <= header.p_memsz &&
            rva - header.p_vaddr <= header.p_memsz - length) {
            return true;
        }
    }
    return false;
}

} // namespace funrounds::native::detail
