// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "engine.hpp"

#include <cstring>
#include <map>
#include <set>
#include <string>

#include <entity2/entityidentity.h>
#include <entity2/entitynetwork.h>

namespace funrounds::native::detail {
namespace {

constexpr char kSchemaInterface[] = "SchemaSystem_001";
constexpr char kServerModule[] = S2SDK_LIBRARY_PREFIX "server" S2SDK_LIBRARY_SUFFIX;
constexpr char kGamedataPath[] = "gamedata/imomoe-native-base.jsonc";

s2sdk::ConfigId g_gameConfig = 0;
ISchemaSystem* g_schemaSystem = nullptr;
ISchemaSystemTypeScope* g_serverSchema = nullptr;

template <typename T>
using NamedValues = std::map<std::string, T, std::less<>>;

struct DeclaredField {
    SchemaField field{};
    const CSchemaType* type{};
};

NamedValues<void*> g_signatures;
NamedValues<void*> g_vtables;
NamedValues<int> g_offsets;
NamedValues<NamedValues<DeclaredField>> g_schemaFields;
NamedValues<NamedValues<int>> g_sdkSchemaOffsets;
std::set<std::string, std::less<>> g_reportedProbes;

void ReportProbeFailure(std::source_location caller, std::string_view reason,
                        std::string_view key, std::string_view member = {}) noexcept {
    if (!__s2sdk_PrintToServer) return;
    try {
        std::string_view module(caller.file_name());
        if (const auto separator = module.find_last_of("/\\"); separator != std::string_view::npos) {
            module.remove_prefix(separator + 1);
        }
        module = module.substr(0, module.find_last_of('.'));
        std::string message("[Imomoe NativeBase] probe failed: ");
        message.append(module).append(" ").append(reason).append(key);
        if (!member.empty()) message.append("::").append(member);
        if (g_reportedProbes.insert(message).second) {
            s2sdk::PrintToServer(plg::string(message));
        }
    } catch (...) {
        // A diagnostic must not change the probe result at a noexcept boundary.
    }
}

template <typename T, typename Lookup>
T ProbeGamedata(NamedValues<T>& values, Lookup lookup, std::string_view key,
                T missing, std::source_location caller) noexcept {
    if (!g_gameConfig || !lookup) return missing;
    try {
        auto entry = values.find(key);
        if (entry == values.end()) {
            auto value = lookup(g_gameConfig, plg::string(key.data(), key.size()));
            entry = values.emplace(key, value).first;
        }
        if (entry->second != missing) return entry->second;
        ReportProbeFailure(caller, "missing gamedata:", key);
    } catch (...) {
        return missing;
    }
    return missing;
}

DeclaredField FindDeclaredField(const char* className, const char* fieldName) noexcept {
    if (!g_serverSchema) return {};
    try {
        if (const auto c = g_schemaFields.find(className); c != g_schemaFields.end()) {
            if (const auto field = c->second.find(fieldName); field != c->second.end()) {
                return field->second;
            }
        }
        const auto* classInfo = g_serverSchema->FindDeclaredClass(className).Get();
        if (!classInfo || classInfo->m_nSize < 0 || !classInfo->m_pFields) return {};
        for (std::uint16_t i = 0; i < classInfo->m_nFieldCount; ++i) {
            const auto& candidate = classInfo->m_pFields[i];
            if (!candidate.m_pszName || std::strcmp(candidate.m_pszName, fieldName) != 0 || !candidate.m_pType) {
                continue;
            }
            int size = 0;
            std::uint8_t alignment = 0;
            const auto offset = candidate.m_nSingleInheritanceOffset;
            if (offset < 0 || offset > classInfo->m_nSize ||
                !candidate.m_pType->GetSizeAndAlignment(size, alignment) ||
                size < 0 || size > classInfo->m_nSize - offset) {
                return {};
            }
            const DeclaredField resolved{{offset, static_cast<std::size_t>(size)}, candidate.m_pType};
            g_schemaFields[className].emplace(fieldName, resolved);
            return resolved;
        }
    } catch (...) {
        return {};
    }
    // Schema may not be ready when a capability is first queried. Cache only verified metadata.
    return {};
}

bool ImportsAvailable() noexcept {
    return __s2sdk_QueryInterface != nullptr &&
           __s2sdk_LoadGameConfigFile != nullptr &&
           __s2sdk_CloseGameConfigFile != nullptr &&
           __s2sdk_GetGameConfigOffset != nullptr &&
           __s2sdk_GetGameConfigSignature != nullptr &&
           __s2sdk_GetGameConfigVTable != nullptr &&
           __s2sdk_EntHandleToEntPointer != nullptr &&
           __s2sdk_EntPointerToEntHandle != nullptr &&
           __s2sdk_IsEntityPlayerPawn != nullptr &&
           __s2sdk_IsEntityPlayerController != nullptr;
}

} // namespace

bool EnsureEngine() {
    if (!ImportsAvailable()) {
        return false;
    }

    if (g_gameConfig == 0) {
        plg::string configPath = plg::GetPlugin()->GetLocation();
        configPath += '/';
        configPath += kGamedataPath;
        g_gameConfig = s2sdk::LoadGameConfigFile({configPath});
        if (g_gameConfig == 0) {
            ReportProbeFailure(std::source_location::current(), "missing gamedata:", kGamedataPath);
            return false;
        }
    }

    if (!g_schemaSystem) {
        g_schemaSystem = static_cast<ISchemaSystem*>(s2sdk::QueryInterface("schemasystem", kSchemaInterface));
    }
    if (!g_schemaSystem || !g_schemaSystem->SchemaSystemIsReady()) {
        return false;
    }

    if (!g_serverSchema) {
        g_serverSchema = g_schemaSystem->FindTypeScopeForModule(kServerModule);
    }
    return g_serverSchema != nullptr;
}

void ShutdownEngine() noexcept {
    g_reportedProbes.clear();
    g_schemaFields.clear();
    g_sdkSchemaOffsets.clear();
    g_offsets.clear();
    g_vtables.clear();
    g_signatures.clear();
    if (g_gameConfig != 0 && __s2sdk_CloseGameConfigFile) {
        s2sdk::CloseGameConfigFile(g_gameConfig);
    }
    g_gameConfig = 0;
    g_serverSchema = nullptr;
    g_schemaSystem = nullptr;
}

void* ProbeSignature(SignatureKey key, std::source_location caller) noexcept {
    return ProbeGamedata(g_signatures, __s2sdk_GetGameConfigSignature, key.name(), static_cast<void*>(nullptr), caller);
}

void* ProbeVTable(VTableKey key, std::source_location caller) noexcept {
    return ProbeGamedata(g_vtables, __s2sdk_GetGameConfigVTable, key.name(), static_cast<void*>(nullptr), caller);
}

int ProbeOffset(OffsetKey key, std::source_location caller) noexcept {
    const auto offset = ProbeGamedata(g_offsets, __s2sdk_GetGameConfigOffset, key.name(), -1, caller);
    if (offset < -1) InvalidGamedata(key.name(), caller);
    return offset;
}

void InvalidGamedata(std::string_view key, std::source_location caller) noexcept {
    ReportProbeFailure(caller, "invalid gamedata:", key);
}

ISchemaSystemTypeScope* ServerSchema() noexcept {
    return g_serverSchema;
}

void MissingSchema(std::string_view className, std::string_view field, std::source_location caller) noexcept {
    // An uninitialized type scope is a readiness condition, not a missing field.
    if (g_serverSchema) ReportProbeFailure(caller, "missing schema:", className, field);
}

int ProbeSchemaOffset(std::string_view className, std::string_view field, bool required,
                      std::source_location caller) noexcept {
    if (!g_serverSchema || !__s2sdk_GetSchemaOffset) return -1;
    try {
        auto c = g_sdkSchemaOffsets.find(className);
        if (c == g_sdkSchemaOffsets.end()) c = g_sdkSchemaOffsets.try_emplace(std::string(className)).first;
        auto entry = c->second.find(field);
        if (entry == c->second.end()) {
            const auto offset = s2sdk::GetSchemaOffset(plg::string(className.data(), className.size()),
                                                       plg::string(field.data(), field.size()));
            entry = c->second.emplace(field, offset).first;
        }
        if (entry->second >= 0) return entry->second;
        if (required) MissingSchema(className, field, caller);
    } catch (...) {
        return -1;
    }
    return -1;
}

bool ResolveSchemaField(const char* className, const char* fieldName, std::size_t expectedSize,
                        SchemaField& result, std::source_location caller) noexcept {
    const auto resolved = FindDeclaredField(className, fieldName);
    if (resolved.type && (expectedSize == 0 || resolved.field.size == expectedSize)) {
        result = resolved.field;
        return true;
    }
    MissingSchema(className, fieldName, caller);
    return false;
}

bool ResolveSchemaHandle(const char* className, const char* fieldName, std::source_location caller) noexcept {
    const auto resolved = FindDeclaredField(className, fieldName);
    if (resolved.type && resolved.field.size == sizeof(std::uint32_t) &&
        resolved.type->m_eTypeCategory == SCHEMA_TYPE_ATOMIC) {
        if (const char* name = resolved.type->m_sTypeName.Get(); name && std::string_view(name).starts_with("CHandle<")) {
            return true;
        }
    }
    MissingSchema(className, fieldName, caller);
    return false;
}

bool ResolvePlayerPawnComponentChain(SchemaField& result, std::source_location caller) noexcept {
    constexpr auto className = "CPlayerPawnComponent";
    constexpr auto fieldName = "__m_pChainEntity";
    const auto resolved = FindDeclaredField(className, fieldName);
    if (resolved.type && resolved.field.size == sizeof(CNetworkVarChainer) &&
        resolved.type->m_eTypeCategory == SCHEMA_TYPE_DECLARED_CLASS) {
        const auto* chainClass = static_cast<const CSchemaType_DeclaredClass*>(resolved.type)->m_pClassInfo;
        if (chainClass && chainClass->m_pszName && std::strcmp(chainClass->m_pszName, "CNetworkVarChainer") == 0 &&
            chainClass->m_nSize == static_cast<int>(sizeof(CNetworkVarChainer))) {
            result = resolved.field;
            return true;
        }
    }
    MissingSchema(className, fieldName, caller);
    return false;
}

void* EntityFromHandle(std::int32_t handle) noexcept {
    if (handle == -1 || !__s2sdk_EntHandleToEntPointer) {
        return nullptr;
    }
    return s2sdk::EntHandleToEntPointer(handle);
}

std::int32_t HandleFromEntity(void* entity) noexcept {
    if (!entity || !__s2sdk_EntPointerToEntHandle) {
        return -1;
    }
    return s2sdk::EntPointerToEntHandle(entity);
}

bool IsPlayerPawn(std::int32_t handle) noexcept {
    return handle != -1 && __s2sdk_IsEntityPlayerPawn && s2sdk::IsEntityPlayerPawn(handle);
}

bool IsPlayerController(std::int32_t handle) noexcept {
    return handle != -1 && __s2sdk_IsEntityPlayerController && s2sdk::IsEntityPlayerController(handle);
}

} // namespace funrounds::native::detail
