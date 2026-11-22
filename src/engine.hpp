// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <source_location>
#include <string_view>

#include <schemasystem/schemasystem.h>

#include <s2sdk/s2sdk.hpp>

#include "gamedata_key.hpp"

namespace funrounds::native::detail {

struct SchemaField {
    std::int32_t offset;
    std::size_t size;
};

bool EnsureEngine();
void ShutdownEngine() noexcept;

void* ProbeSignature(SignatureKey key, std::source_location caller = std::source_location::current()) noexcept;
void* ProbeVTable(VTableKey key, std::source_location caller = std::source_location::current()) noexcept;
int ProbeOffset(OffsetKey key, std::source_location caller = std::source_location::current()) noexcept;
void InvalidGamedata(std::string_view key, std::source_location caller = std::source_location::current()) noexcept;

ISchemaSystemTypeScope* ServerSchema() noexcept;
void MissingSchema(std::string_view className, std::string_view field,
                   std::source_location caller = std::source_location::current()) noexcept;
int ProbeSchemaOffset(std::string_view className, std::string_view field, bool required = true,
                      std::source_location caller = std::source_location::current()) noexcept;
bool ResolveSchemaField(const char* className, const char* fieldName, std::size_t expectedSize, SchemaField& field,
                        std::source_location caller = std::source_location::current()) noexcept;
bool ResolveSchemaHandle(const char* className, const char* fieldName,
                         std::source_location caller = std::source_location::current()) noexcept;
bool ResolvePlayerPawnComponentChain(SchemaField& field,
                                     std::source_location caller = std::source_location::current()) noexcept;

void* EntityFromHandle(std::int32_t handle) noexcept;
std::int32_t HandleFromEntity(void* entity) noexcept;
bool IsPlayerPawn(std::int32_t handle) noexcept;
bool IsPlayerController(std::int32_t handle) noexcept;

} // namespace funrounds::native::detail
