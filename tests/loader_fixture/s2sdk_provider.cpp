// Copyright (C) 2026 Mai Ooizumi
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <new>
#include <vector>

#include <plg/plugin.hpp>
#include <plugin_export.h>
#include <polyhook/polyhook.hpp>
#include <s2sdk/s2sdk.hpp>

#include <entity2/entityinstance.h>
#include <entity2/entityidentity.h>
#include <gametrace.h>
#include <mathlib/vectorws.h>
#include <navphysicsinterface.h>
#include <schemasystem/schemasystem.h>
#include <schemalib/schemaenuminfo.h>
#include <tier0/utlleanvector.h>

CSchemaType::~CSchemaType() = default;
ISchemaSystem::~ISchemaSystem() = default;

namespace {

constexpr std::int32_t kChickenHandle = std::bit_cast<std::int32_t>(0x80000016U);
constexpr std::int32_t kStaleChickenHandle = 22;
constexpr std::int32_t kProjectileHandle = std::bit_cast<std::int32_t>(0x90000017U);
constexpr std::int32_t kStaleProjectileHandle = 23;
constexpr std::int32_t kSerialWeaponHandle = std::bit_cast<std::int32_t>(0xA0000012U);
constexpr std::int32_t kIgniteFlameHandle = 24;
constexpr std::int32_t kSmokeGrenadeHandle = std::bit_cast<std::int32_t>(0x00018030U);
constexpr std::int32_t kMolotovGrenadeHandle = std::bit_cast<std::int32_t>(0x00018031U);
constexpr std::int32_t kHeGrenadeHandle = std::bit_cast<std::int32_t>(0x00018033U);

struct FixtureCollisionAttribute {
    std::uint8_t padding;
    std::uint8_t collisionGroup;
};

struct FixtureCollisionProperty {
    std::uint8_t collisionGroup;
    std::uint8_t padding[3];
    FixtureCollisionAttribute collisionAttribute;
};

struct FixtureEntity {
    void** vtable;
    void* weaponServices;
    std::int32_t originalTouchCalls;
    std::int32_t originalTouchChecksum;
    std::uintptr_t touchDigest;
    std::uintptr_t touchOther;
    FixtureCollisionProperty* collisionProperty;
    std::int32_t collisionRulesChangedCalls;
    float bigHeadScale;
    std::int32_t bigHeadResetCalls;
    std::int32_t health{100};
    std::int32_t lifeState{};
    std::int32_t controllerHandle{-1};
    std::int32_t effectEntityHandle{-1};
    std::int32_t attachedPawnHandle{-1};
};

struct FixtureGameRules {
    std::int32_t activeMutators;
};

struct FixtureController {
    void** vtable;
    std::uint32_t playerPawnHandle;
    std::uint32_t currentPawnHandle{};
};

struct FixtureWeaponServices {
    std::uint32_t activeWeaponHandle;
};

struct FixtureWeaponData {
    std::uint32_t weaponType;
};

struct FixtureGrenade {
    void** vtable;
    CUtlSymbolLarge privateVScripts;
    CEntityIdentity* entityIdentity;
    std::int32_t detonateCalls{};
};

#pragma pack(push, 4)
struct FixtureWeapon {
    void** vtable;
    std::uint32_t subclassId;
    FixtureWeaponData* weaponData;
    bool isWeapon;
    std::byte reserved[19];
    FixtureCollisionProperty* collisionProperty;
    std::int32_t collisionRulesChangedCalls;
};
#pragma pack(pop)

struct FixtureDamageInfo {
    std::uint32_t inflictorHandle;
    std::uint32_t attackerHandle;
    std::uint32_t abilityHandle;
    float damage;
    float originalDamage;
    bool shouldBleed;
    bool shouldSpark;
    CGameTrace* trace;
    std::uint64_t damageFlags;
    std::int32_t hitGroupId;
    std::uint32_t damageType;
};

struct FixtureDamageResult {
    void* originatingInfo;
    CUtlLeanVector<int> destructibleHitGroupRequests;
    std::int32_t healthLost;
    std::int32_t healthBefore;
    float damageDealt;
    float preModifiedDamage;
    VectorWS damagePosition;
    std::int32_t totalledHealthLost;
    float totalledDamageDealt;
    float totalledPreModifiedDamage;
    float newDamageAccumulatorValue;
    std::uint64_t damageFlags;
    bool damageSuppressed;
    bool suppressFlinch;
    std::int32_t overrideFlinchHitGroup;
    std::uint8_t unknown[8];
};

#if defined(_WIN32)
constexpr std::size_t kTouchIndex = 151;
constexpr std::size_t kIsWeaponIndex = 179;
constexpr std::size_t kCollisionRulesChangedIndex = 188;
constexpr std::size_t kFixtureIgniteIndex = 266;
#else
constexpr std::size_t kTouchIndex = 150;
constexpr std::size_t kIsWeaponIndex = 178;
constexpr std::size_t kCollisionRulesChangedIndex = 187;
constexpr std::size_t kFixtureIgniteIndex = 267;
#endif
constexpr std::size_t kEntityVtableSize =
    (kCollisionRulesChangedIndex > kFixtureIgniteIndex ? kCollisionRulesChangedIndex : kFixtureIgniteIndex) + 1;
constexpr std::size_t kDetonateIndex = 7;

static_assert(sizeof(FixtureDamageInfo) == 48);
static_assert(sizeof(FixtureDamageResult) == 96);
static_assert(offsetof(FixtureEntity, weaponServices) == 8);
static_assert(offsetof(FixtureEntity, collisionProperty) == 40);
static_assert(offsetof(FixtureEntity, bigHeadScale) == 52);
static_assert(offsetof(FixtureController, playerPawnHandle) == 8);
static_assert(offsetof(FixtureWeapon, weaponData) == 12);
static_assert(offsetof(FixtureWeapon, collisionProperty) == 40);
static_assert(offsetof(FixtureCollisionProperty, collisionAttribute) == 4);
static_assert(sizeof(CUtlSymbolLarge) == sizeof(void*));
static_assert(offsetof(FixtureGrenade, entityIdentity) == sizeof(void*) + sizeof(CUtlSymbolLarge));

std::int32_t g_closedConfigCount = 0;
std::int32_t g_collisionRulesChangedCalls = 0;
std::int32_t g_schemaClassLookups = 0;
std::int32_t g_navTraceCalls = 0;
std::int32_t g_lastEyePositionHandle = -1;
std::int32_t g_lastEyeAngleHandle = -1;
std::int32_t g_damageConstructorCalls = 0;
std::int32_t g_takeDamageCalls = 0;
std::int32_t g_smokeGrenadeHandle = kSmokeGrenadeHandle;
std::uintptr_t g_takeDamageChecksum = 0;
std::uint32_t g_activeConfig = 0;
std::uint32_t g_outerDamageType = 0;
std::int32_t g_outerDamageHitGroup = -1;
std::uint32_t g_outerDamageAbility = 0xffffffffU;
float g_outerDamageAmount = 0.0f;
std::vector<s2sdk::OnMapEndCallback> g_mapEndCallbacks;
bool g_failMutatorWrites{};
bool g_igniteFlamePresent{};
bool g_afterburnClientEnabled{};

void FixtureTouch(void* entity, void* other);
void FixtureSmokeDetonate(void* entity);
void FixtureMolotovDetonate(void* entity);
void FixtureCollisionRulesChanged(void* entity);
void FixtureIgnite(void* pawn);
bool FixtureIsWeapon(void* entity);
bool FixtureNavTraceLine(void* self,
                         const Vector& start,
                         const Vector& end,
                         const CBaseEntity* ignore,
                         std::uint64_t interactsWith,
                         std::uint8_t collisionGroup,
                         std::uint8_t objectSetMask,
                         trace_t* result);
void FixtureDamageConstructor(void* storage,
                              CBaseEntity* inflictor,
                              CBaseEntity* attacker,
                              CBaseEntity* weapon,
                              const Vector* force,
                              const Vector* position,
                              float damage,
                              std::int32_t damageType,
                              std::int32_t customDamage,
                              void* auxiliary);
std::int64_t FixtureTakeDamage(void* victim, void* damageInfo, void* damageResult);

void* g_entityVtable[kEntityVtableSize]{};
void* g_smokeVtable[kDetonateIndex + 1]{};
void* g_molotovVtable[kDetonateIndex + 1]{};
void* g_heVtable[kDetonateIndex + 1]{};
FixtureWeaponServices g_weaponServices{18};
FixtureWeaponData g_weaponData{0};
FixtureCollisionProperty g_pawnCollisionProperty{7, {0, 0, 0}, {0, 11}};
FixtureCollisionProperty g_victimCollisionProperty{8, {0, 0, 0}, {0, 12}};
FixtureEntity g_pawn{g_entityVtable, &g_weaponServices, 0, 0, 0, 0, &g_pawnCollisionProperty, 0, 0.0f, 0};
FixtureEntity g_victim{g_entityVtable, nullptr, 0, 0, 0, 0, &g_victimCollisionProperty, 0, 0.0f, 0};
FixtureEntity g_legacyPawn{g_entityVtable, nullptr, 0, 0, 0, 0, nullptr, 0, 0.0f, 0};
FixtureEntity g_chicken{g_entityVtable, nullptr, 0, 0, 0, 0, nullptr, 0, 0.0f, 0};
FixtureEntity g_projectile{g_entityVtable, nullptr, 0, 0, 0, 0, nullptr, 0, 0.0f, 0};
FixtureEntity g_igniteFlame{g_entityVtable, nullptr, 0, 0, 0, 0, nullptr, 0, 0.0f, 0};
FixtureController g_controller{g_entityVtable, 17, 17};
FixtureGameRules g_gameRules{};
FixtureWeapon g_weapon{g_entityVtable, 0, &g_weaponData, true, {}, nullptr, 0};
FixtureWeapon g_serialWeapon{g_entityVtable, 0, &g_weaponData, true, {}, nullptr, 0};
FixtureWeapon g_nonWeapon{g_entityVtable, 0, &g_weaponData, false, {}, nullptr, 0};
CHitBox g_hitbox;
CGameTrace g_damageTrace;
CEntityIdentity g_smokeIdentity;
CEntityIdentity g_molotovIdentity;
CEntityIdentity g_heIdentity;
FixtureGrenade g_smokeGrenade{g_smokeVtable, {}, &g_smokeIdentity, 0};
FixtureGrenade g_molotovGrenade{g_molotovVtable, {}, &g_molotovIdentity, 0};
FixtureGrenade g_heGrenade{g_heVtable, {}, &g_heIdentity, 0};

void* g_navVtable[3]{};
constexpr std::size_t kNavTraceIndex = 2;

class FixtureSchemaType final : public CSchemaType {
public:
    explicit FixtureSchemaType(int size, std::uint8_t alignment) : size_(size), alignment_(alignment) {}

    bool GetSizeAndAlignment(int& size, std::uint8_t& alignment) const override {
        size = size_;
        alignment = alignment_;
        return true;
    }

    ~FixtureSchemaType() override = default;

private:
    int size_;
    std::uint8_t alignment_;
};

FixtureSchemaType g_typeUint32{sizeof(std::uint32_t), alignof(std::uint32_t)};
FixtureSchemaType g_typeInt32{sizeof(std::int32_t), alignof(std::int32_t)};
FixtureSchemaType g_typeUint64{sizeof(std::uint64_t), alignof(std::uint64_t)};
FixtureSchemaType g_typeFloat{sizeof(float), alignof(float)};
FixtureSchemaType g_typeBool{sizeof(bool), alignof(bool)};
FixtureSchemaType g_typePointer{sizeof(void*), alignof(void*)};
FixtureSchemaType g_typeUint8{sizeof(std::uint8_t), alignof(std::uint8_t)};
FixtureSchemaType g_typeCollisionAttribute{sizeof(FixtureCollisionAttribute), alignof(FixtureCollisionAttribute)};
FixtureSchemaType g_typeHandle{sizeof(std::uint32_t), alignof(std::uint32_t)};

SchemaClassFieldData_t g_damageInfoFields[]{
    {"m_hInflictor", &g_typeUint32, offsetof(FixtureDamageInfo, inflictorHandle), 0, nullptr},
    {"m_hAttacker", &g_typeUint32, offsetof(FixtureDamageInfo, attackerHandle), 0, nullptr},
    {"m_hAbility", &g_typeUint32, offsetof(FixtureDamageInfo, abilityHandle), 0, nullptr},
    {"m_flDamage", &g_typeFloat, offsetof(FixtureDamageInfo, damage), 0, nullptr},
    {"m_flOriginalDamage", &g_typeFloat, offsetof(FixtureDamageInfo, originalDamage), 0, nullptr},
    {"m_bShouldBleed", &g_typeBool, offsetof(FixtureDamageInfo, shouldBleed), 0, nullptr},
    {"m_bShouldSpark", &g_typeBool, offsetof(FixtureDamageInfo, shouldSpark), 0, nullptr},
    {"m_nDamageFlags", &g_typeUint64, offsetof(FixtureDamageInfo, damageFlags), 0, nullptr},
    {"m_iHitGroupId", &g_typeInt32, offsetof(FixtureDamageInfo, hitGroupId), 0, nullptr},
    {"m_bitsDamageType", &g_typeUint32, offsetof(FixtureDamageInfo, damageType), 0, nullptr},
};

SchemaClassFieldData_t g_damageResultFields[]{
    {"m_nHealthLost", &g_typeInt32, offsetof(FixtureDamageResult, healthLost), 0, nullptr},
    {"m_flDamageDealt", &g_typeFloat, offsetof(FixtureDamageResult, damageDealt), 0, nullptr},
    {"m_nTotalledHealthLost", &g_typeInt32, offsetof(FixtureDamageResult, totalledHealthLost), 0, nullptr},
    {"m_flTotalledDamageDealt", &g_typeFloat, offsetof(FixtureDamageResult, totalledDamageDealt), 0, nullptr},
    {"m_bWasDamageSuppressed", &g_typeBool, offsetof(FixtureDamageResult, damageSuppressed), 0, nullptr},
};

SchemaClassFieldData_t g_controllerFields[]{
    {"m_hPlayerPawn", &g_typeUint32, offsetof(FixtureController, playerPawnHandle), 0, nullptr},
};
SchemaClassFieldData_t g_pawnFields[]{
    {"m_pWeaponServices", &g_typePointer, offsetof(FixtureEntity, weaponServices), 0, nullptr},
    {"m_hController", &g_typeHandle, offsetof(FixtureEntity, controllerHandle), 0, nullptr},
};
SchemaClassFieldData_t g_csPawnFields[]{
    {"m_pWeaponServices", &g_typePointer, offsetof(FixtureEntity, weaponServices), 0, nullptr},
    {"m_hController", &g_typeHandle, offsetof(FixtureEntity, controllerHandle), 0, nullptr},
    {"m_flBigHeadScale", &g_typeFloat, offsetof(FixtureEntity, bigHeadScale), 0, nullptr},
};
SchemaClassFieldData_t g_gameRulesFields[]{
    {"m_nActiveMutators", &g_typeInt32, offsetof(FixtureGameRules, activeMutators), 0, nullptr},
};
SchemaClassFieldData_t g_weaponServicesFields[]{
    {"m_hActiveWeapon", &g_typeUint32, offsetof(FixtureWeaponServices, activeWeaponHandle), 0, nullptr},
};
SchemaClassFieldData_t g_entityFields[]{
    {"m_iHealth", &g_typeInt32, offsetof(FixtureEntity, health), 0, nullptr},
    {"m_lifeState", &g_typeInt32, offsetof(FixtureEntity, lifeState), 0, nullptr},
    {"m_hEffectEntity", &g_typeHandle, offsetof(FixtureEntity, effectEntityHandle), 0, nullptr},
    {"m_nSubclassID", &g_typeUint32, offsetof(FixtureWeapon, subclassId), 0, nullptr},
    {"m_pCollision", &g_typePointer, offsetof(FixtureEntity, collisionProperty), 0, nullptr},
};
SchemaClassFieldData_t g_flameFields[]{
    {"m_hEntAttached", &g_typeHandle, offsetof(FixtureEntity, attachedPawnHandle), 0, nullptr},
};
SchemaClassFieldData_t g_collisionPropertyFields[]{
    {"m_collisionAttribute", &g_typeCollisionAttribute, offsetof(FixtureCollisionProperty, collisionAttribute), 0, nullptr},
    {"m_CollisionGroup", &g_typeUint8, offsetof(FixtureCollisionProperty, collisionGroup), 0, nullptr},
};
SchemaClassFieldData_t g_collisionAttributeFields[]{
    {"m_nCollisionGroup", &g_typeUint8, offsetof(FixtureCollisionAttribute, collisionGroup), 0, nullptr},
};
SchemaClassFieldData_t g_weaponDataFields[]{
    {"m_WeaponType", &g_typeUint32, offsetof(FixtureWeaponData, weaponType), 0, nullptr},
};

void* DamageInfoManipulator(SchemaClassManipulatorAction_t action, void* object) {
    if (action == SCHEMA_CLASS_MANIPULATOR_ACTION_DESCTRUCT_IN_PLACE) {
        static_cast<FixtureDamageInfo*>(object)->~FixtureDamageInfo();
    }
    return object;
}

CSchemaClassInfo g_damageInfoClass{};
CSchemaClassInfo g_damageResultClass{};
CSchemaClassInfo g_controllerClass{};
CSchemaClassInfo g_pawnClass{};
CSchemaClassInfo g_csPawnClass{};
CSchemaClassInfo g_gameRulesClass{};
CSchemaClassInfo g_weaponServicesClass{};
CSchemaClassInfo g_entityClass{};
CSchemaClassInfo g_flameClass{};
CSchemaClassInfo g_weaponDataClass{};
CSchemaClassInfo g_collisionPropertyClass{};
CSchemaClassInfo g_collisionAttributeClass{};
CSchemaEnumeratorInfo g_damageTypeValues[11]{};
CSchemaEnumeratorInfo g_damageFlagValues[1]{};
CSchemaEnumeratorInfo g_weaponTypeValues[8]{};
CSchemaEnumInfo g_damageTypesEnum{};
CSchemaEnumInfo g_damageFlagsEnum{};
CSchemaEnumInfo g_weaponTypesEnum{};

void ConfigureClass(CSchemaClassInfo& info,
                    const char* name,
                    int size,
                    std::uint8_t alignment,
                    SchemaClassFieldData_t* fields,
                    std::uint16_t fieldCount,
                    SchemaClassManipulatorFn_t manipulator = nullptr,
                    std::uint32_t flags = 0) {
    info.m_pszName = name;
    info.m_nSize = size;
    info.m_nAlignment = alignment;
    info.m_pFields = fields;
    info.m_nFieldCount = fieldCount;
    info.m_nBaseClassCount = 0;
    info.m_pfnManipulator = manipulator;
    info.m_nFlags1 = flags;
}

void InitializeSchemaMetadata() {
    static bool initialized = false;
    if (initialized) {
        return;
    }
    g_typeHandle.m_sTypeName = "CHandle<CBaseEntity>";
    g_typeHandle.m_eTypeCategory = SCHEMA_TYPE_ATOMIC;
    g_typeHandle.m_eAtomicCategory = SCHEMA_ATOMIC_PLAIN;

    ConfigureClass(g_damageInfoClass,
                   "CTakeDamageInfo",
                   sizeof(FixtureDamageInfo),
                   alignof(FixtureDamageInfo),
                   g_damageInfoFields,
                   static_cast<std::uint16_t>(std::size(g_damageInfoFields)),
                   &DamageInfoManipulator,
                   SCHEMA_CF1_CONSTRUCT_ALLOWED);
    ConfigureClass(g_damageResultClass,
                   "CTakeDamageResult",
                   sizeof(FixtureDamageResult),
                   alignof(FixtureDamageResult),
                   g_damageResultFields,
                   static_cast<std::uint16_t>(std::size(g_damageResultFields)));
    ConfigureClass(g_controllerClass,
                   "CCSPlayerController",
                   sizeof(FixtureController),
                   alignof(FixtureController),
                   g_controllerFields,
                   static_cast<std::uint16_t>(std::size(g_controllerFields)));
    ConfigureClass(g_pawnClass,
                   "CBasePlayerPawn",
                   sizeof(FixtureEntity),
                   alignof(FixtureEntity),
                   g_pawnFields,
                   static_cast<std::uint16_t>(std::size(g_pawnFields)));
    ConfigureClass(g_csPawnClass,
                   "CCSPlayerPawn",
                   sizeof(FixtureEntity),
                   alignof(FixtureEntity),
                   g_csPawnFields,
                   static_cast<std::uint16_t>(std::size(g_csPawnFields)));
    ConfigureClass(g_gameRulesClass,
                   "CCSGameRules",
                   sizeof(FixtureGameRules),
                   alignof(FixtureGameRules),
                   g_gameRulesFields,
                   static_cast<std::uint16_t>(std::size(g_gameRulesFields)));
    ConfigureClass(g_weaponServicesClass,
                   "CPlayer_WeaponServices",
                   sizeof(FixtureWeaponServices),
                   alignof(FixtureWeaponServices),
                   g_weaponServicesFields,
                   static_cast<std::uint16_t>(std::size(g_weaponServicesFields)));
    ConfigureClass(g_entityClass,
                   "CBaseEntity",
                   static_cast<int>(std::max(sizeof(FixtureEntity), sizeof(FixtureWeapon))),
                   alignof(FixtureEntity),
                   g_entityFields,
                   static_cast<std::uint16_t>(std::size(g_entityFields)));
    ConfigureClass(g_flameClass,
                   "CEntityFlame",
                   sizeof(FixtureEntity),
                   alignof(FixtureEntity),
                   g_flameFields,
                   static_cast<std::uint16_t>(std::size(g_flameFields)));
    ConfigureClass(g_collisionPropertyClass,
                   "CCollisionProperty",
                   sizeof(FixtureCollisionProperty),
                   alignof(FixtureCollisionProperty),
                   g_collisionPropertyFields,
                   static_cast<std::uint16_t>(std::size(g_collisionPropertyFields)));
    ConfigureClass(g_collisionAttributeClass,
                   "VPhysicsCollisionAttribute_t",
                   sizeof(FixtureCollisionAttribute),
                   alignof(FixtureCollisionAttribute),
                   g_collisionAttributeFields,
                   static_cast<std::uint16_t>(std::size(g_collisionAttributeFields)));
    ConfigureClass(g_weaponDataClass,
                   "CCSWeaponBaseVData",
                   sizeof(FixtureWeaponData),
                   alignof(FixtureWeaponData),
                   g_weaponDataFields,
                   static_cast<std::uint16_t>(std::size(g_weaponDataFields)));

    auto configureEnum = []<std::size_t N>(CSchemaEnumInfo& info, const char* name, std::uint8_t bytes,
                                           CSchemaEnumeratorInfo (&values)[N],
                                           const std::pair<const char*, std::int64_t> (&items)[N]) {
        info.m_pszName = name;
        info.m_nSize = bytes;
        info.m_nAlignment = bytes;
        info.m_nEnumeratorCount = N;
        info.m_pEnumerators = values;
        for (std::size_t i = 0; i < N; ++i) {
            values[i].m_pszName = items[i].first;
            values[i].m_nValue = items[i].second;
        }
    };
    configureEnum(g_damageTypesEnum, "DamageTypes_t", sizeof(std::uint32_t), g_damageTypeValues, {
        {"DMG_HEADSHOT", 1LL << 30}, {"DMG_BLAST", 0x40}, {"DMG_BLAST_SURFACE", 0x400000}, {"DMG_BURN", 0x8},
        {"DMG_DROWN", 0x4000}, {"DMG_FALL", 0x20}, {"DMG_SLASH", 0x4}, {"DMG_CLUB", 0x80}, {"DMG_SHOCK", 0x100},
        {"DMG_BULLET", 0x2}, {"DMG_BUCKSHOT", 0x1000000},
    });
    configureEnum(g_damageFlagsEnum, "TakeDamageFlags_t", sizeof(std::uint64_t), g_damageFlagValues, {{"DFLAG_RADIUS_DMG", 0x400}});
    configureEnum(g_weaponTypesEnum, "CSWeaponType", sizeof(std::uint32_t), g_weaponTypeValues, {
        {"WEAPONTYPE_KNIFE", 0}, {"WEAPONTYPE_TASER", 8}, {"WEAPONTYPE_PISTOL", 1}, {"WEAPONTYPE_SUBMACHINEGUN", 2},
        {"WEAPONTYPE_RIFLE", 3}, {"WEAPONTYPE_SHOTGUN", 4}, {"WEAPONTYPE_SNIPER_RIFLE", 5}, {"WEAPONTYPE_MACHINEGUN", 6},
    });
    initialized = true;
}

class FixtureSchemaTypeScope final : public ISchemaSystemTypeScope {
public:
    CSchemaClassInfo* InstallSchemaClassBinding(const char*, CSchemaClassInfo*) override { return nullptr; }
    CSchemaEnumInfo* InstallSchemaEnumBinding(const char*, CSchemaEnumInfo*) override { return nullptr; }

    SchemaMetaInfoHandle_t<CSchemaClassInfo> FindDeclaredClass(const char* name) override {
        InitializeSchemaMetadata();
        if (!name) {
            return {};
        }
        for (auto* info : {&g_damageInfoClass,
                           &g_damageResultClass,
                           &g_controllerClass,
                           &g_pawnClass,
                           &g_csPawnClass,
                           &g_gameRulesClass,
                           &g_weaponServicesClass,
                           &g_entityClass,
                           &g_flameClass,
                           &g_weaponDataClass,
                           &g_collisionPropertyClass,
                           &g_collisionAttributeClass}) {
            if (std::strcmp(name, info->m_pszName) == 0) {
                ++g_schemaClassLookups;
                return info;
            }
        }
        return {};
    }

    SchemaMetaInfoHandle_t<CSchemaEnumInfo> FindDeclaredEnum(const char* name) override {
        InitializeSchemaMetadata();
        if (!name) return nullptr;
        for (auto* candidate : {&g_damageTypesEnum, &g_damageFlagsEnum, &g_weaponTypesEnum}) {
            if (std::strcmp(name, candidate->m_pszName) == 0) return candidate;
        }
        return nullptr;
    }

    SchemaMetaInfoHandle_t<CSchemaType_Builtin> FindBuiltinTypeByName(const char*) override { return {}; }
    SchemaMetaInfoHandle_t<CSchemaType_Builtin> Type_Builtin(SchemaBuiltinType_t) override { return {}; }
    SchemaMetaInfoHandle_t<CSchemaType_Ptr> Type_Ptr(CSchemaType*) override { return {}; }
    SchemaMetaInfoHandle_t<CSchemaType_Atomic> Type_Atomic(const char*, std::uint16_t, std::uint8_t) override { return {}; }
    SchemaMetaInfoHandle_t<CSchemaType_Atomic_T> Type_Atomic_T(const char*, std::uint16_t, std::uint8_t, CSchemaType*) override { return {}; }
    SchemaMetaInfoHandle_t<CSchemaType_Atomic_CollectionOfT> Type_Atomic_CollectionOfT(const char*, uint16, uint8, uint16, CSchemaType*, uint64, SchemaCollectionManipulatorFn_t) override { return {}; }
    SchemaMetaInfoHandle_t<CSchemaType_Atomic_TT> Type_Atomic_TT(const char*, std::uint16_t, std::uint8_t, CSchemaType*, CSchemaType*) override { return {}; }
    SchemaMetaInfoHandle_t<CSchemaType_Atomic_I> Type_Atomic_I(const char*, std::uint16_t, std::uint8_t, int) override { return {}; }
    SchemaMetaInfoHandle_t<CSchemaType_DeclaredClass> Type_DeclaredClass(const char*) override { return {}; }
    SchemaMetaInfoHandle_t<CSchemaType_DeclaredEnum> Type_DeclaredEnum(const char*) override { return {}; }
    SchemaMetaInfoHandle_t<CSchemaType_FixedArray> Type_FixedArray(CSchemaType*, int, std::uint16_t, std::uint8_t) override { return {}; }
    SchemaMetaInfoHandle_t<CSchemaType_FixedArray> Type_FixedArray_Multidimensional(CSchemaType*, std::uint16_t, std::uint8_t, ...) override { return {}; }
    SchemaMetaInfoHandle_t<CSchemaType_Bitfield> Type_Bitfield(int) override { return {}; }
    SchemaMetaInfoHandle_t<CSchemaType_Atomic> FindType_Atomic(int) override { return {}; }
    SchemaMetaInfoHandle_t<CSchemaType_Atomic_T> FindType_Atomic_T(int, CSchemaType*) override { return {}; }
    SchemaMetaInfoHandle_t<CSchemaType_Atomic_CollectionOfT> FindType_Atomic_CollectionOfT(int, CSchemaType*, uint64, SchemaCollectionManipulatorFn_t) override { return {}; }
    SchemaMetaInfoHandle_t<CSchemaType_Atomic_TT> FindType_Atomic_TT(int, CSchemaType*, CSchemaType*) override { return {}; }
    SchemaMetaInfoHandle_t<CSchemaType_Atomic_I> FindType_Atomic_I(int, int) override { return {}; }
    CSchemaType_DeclaredClass* FindType_DeclaredClass(const char*) override { return nullptr; }
    CSchemaType_DeclaredEnum* FindType_DeclaredEnum(const char*) override { return nullptr; }
    CSchemaClassInfo* FindRawClassBinding(const char*) override { return nullptr; }
    CSchemaClassInfo* FindRawClassBinding(unsigned int) override { return nullptr; }
    CSchemaEnumInfo* FindRawEnumBinding(const char*) override { return nullptr; }
    CSchemaEnumInfo* FindRawEnumBinding(unsigned int) override { return nullptr; }
    const char* GetScopeName() override { return "server"; }
    bool IsGlobalScope() override { return false; }
    void MarkClassAsRequiringGlobalPromotion(const CSchemaClassInfo*) override {}
    void MarkEnumAsRequiringGlobalPromotion(const CSchemaEnumInfo*) override {}
    void ResolveAtomicInfoThreadsafe(const SchemaAtomicTypeInfo_t**, const char*, int) override {}
    void ResolveEnumInfoThreadsafe(const CSchemaEnumInfo**, const char*) override {}
    void ResolveClassInfoThreadsafe(const CSchemaClassInfo**, const char*) override {}
};

FixtureSchemaTypeScope g_schemaScope;

class FixtureSchemaSystem final : public ISchemaSystem {
public:
    bool Connect(CreateInterfaceFn) override { return true; }
    void Disconnect() override {}
    void* QueryInterface(const char*) override { return nullptr; }
    InitReturnVal_t Init() override { return INIT_OK; }
    void Shutdown() override {}
    void PreShutdown() override {}
    const AppSystemInfo_t* GetDependencies() override { return nullptr; }
    AppSystemTier_t GetTier() override { return static_cast<AppSystemTier_t>(0); }
    void Reconnect(CreateInterfaceFn, const char*) override {}
    bool IsSingleton() override { return true; }
    AppSystemBuildType_t GetBuildType() override { return static_cast<AppSystemBuildType_t>(0); }

    CSchemaSystemTypeScope* GlobalTypeScope() override { return nullptr; }
    CSchemaSystemTypeScope* FindOrCreateTypeScopeForModule(const char*, const char**) override {
        return reinterpret_cast<CSchemaSystemTypeScope*>(&g_schemaScope);
    }
    CSchemaSystemTypeScope* FindTypeScopeForModule(const char*, const char**) override {
        return reinterpret_cast<CSchemaSystemTypeScope*>(&g_schemaScope);
    }
    CSchemaSystemTypeScope* GetTypeScopeForBinding(SchemaTypeScope_t, const char*, const char**) override {
        return reinterpret_cast<CSchemaSystemTypeScope*>(&g_schemaScope);
    }
    bool DefaultTypeScopeIsLocal() override { return false; }
    SchemaMetaInfoHandle_t<CSchemaClassInfo> FindClassByScopedName(const char*) override { return {}; }
    void ScopedNameForClass(const CSchemaClassInfo*, CBufferString&) override {}
    SchemaMetaInfoHandle_t<CSchemaEnumInfo> FindEnumByScopedName(const char*) override { return {}; }
    void ScopedNameForEnum(const CSchemaEnumInfo*, CBufferString&) override {}
    void FindDescendentsOfClass(const CSchemaClassInfo*, int, CUtlVector<const CSchemaClassInfo*>*, bool) override {}
    void LoadSchemaDataForModules(const char**, int) override {}
    const char* GetClassModuleName(const CSchemaClassInfo*) override { return "server"; }
    const char* GetClassProjectName(const CSchemaClassInfo*) override { return "fixture"; }
    const char* GetEnumModuleName(const CSchemaEnumInfo*) override { return "server"; }
    const char* GetEnumProjectName(const CSchemaEnumInfo*) override { return "fixture"; }
    bool SchemaSystemIsReady() override { return true; }
    void VerifySchemaBindingConsistency(bool) override {}
    void CompleteModuleRegistration(const char*) override {}
    void RegisterAtomicType(const SchemaAtomicTypeInfo_t*) override {}
    void PrintSchemaStats(const char*) override {}
    void PrintSchemaMetaStats(const char*) override {}
    void RegisterAtomics(const char*, const char*, CSchemaType**, int, SchemaAtomicTypeInfo_t**) override {}
    void RegisterEnums(const char*, const char*, CSchemaType**, int, CSchemaEnumInfo**) override {}
    bool RegisterClasses(const char*, const char*, CSchemaType**, int, CSchemaClassInfo**, CBufferString*) override { return true; }
    void ValidateClasses(CSchemaClassInfo**) override {}
    bool ConvertOldIntrospectedResourceDataToKV3(void*, void*, void*, CKV3Arena*, const char*) override { return false; }
    void FindClassesByMeta(const char*, SchemaHierarchyIteration_t, CUtlVector<const CSchemaClassInfo*>*) override {}
    SchemaMetaInfoHandle_t<CSchemaType_Builtin> GetSchemaBuiltinType(SchemaBuiltinType_t) override { return {}; }
    ~FixtureSchemaSystem() override = default;
};

FixtureSchemaSystem g_schemaSystem;

FixtureEntity* EntityFromHandle(std::int32_t handle) {
    if (handle == g_smokeGrenadeHandle) return reinterpret_cast<FixtureEntity*>(&g_smokeGrenade);
    if (handle == kMolotovGrenadeHandle) return reinterpret_cast<FixtureEntity*>(&g_molotovGrenade);
    if (handle == kHeGrenadeHandle) return reinterpret_cast<FixtureEntity*>(&g_heGrenade);
    switch (handle) {
        case 17: return &g_pawn;
        case 21: return &g_victim;
        case 26: return &g_legacyPawn;
        case kIgniteFlameHandle: return g_igniteFlamePresent ? &g_igniteFlame : nullptr;
        case kChickenHandle:
        case kStaleChickenHandle: return &g_chicken;
        case kProjectileHandle:
        case kStaleProjectileHandle: return &g_projectile;
        case 20: return reinterpret_cast<FixtureEntity*>(&g_controller);
        case 18: return reinterpret_cast<FixtureEntity*>(&g_weapon);
        case kSerialWeaponHandle: return reinterpret_cast<FixtureEntity*>(&g_serialWeapon);
        case 19: return reinterpret_cast<FixtureEntity*>(&g_nonWeapon);
        default: return nullptr;
    }
}

std::int32_t HandleFromEntity(void* entity) {
    if (entity == &g_smokeGrenade) return g_smokeGrenadeHandle;
    if (entity == &g_molotovGrenade) return kMolotovGrenadeHandle;
    if (entity == &g_heGrenade) return kHeGrenadeHandle;
    if (entity == &g_pawn) return 17;
    if (entity == &g_victim) return 21;
    if (entity == &g_legacyPawn) return 26;
    if (entity == &g_igniteFlame && g_igniteFlamePresent) return kIgniteFlameHandle;
    if (entity == &g_chicken) return kChickenHandle;
    if (entity == &g_projectile) return kProjectileHandle;
    if (entity == &g_controller) return 20;
    if (entity == &g_weapon) return 18;
    if (entity == &g_serialWeapon) return kSerialWeaponHandle;
    if (entity == &g_nonWeapon) return 19;
    return -1;
}

void FixtureTouch(void* entity, void* other) {
    auto* fixture = static_cast<FixtureEntity*>(entity);
    const auto entityValue = reinterpret_cast<std::uintptr_t>(entity);
    const auto otherValue = reinterpret_cast<std::uintptr_t>(other);
    fixture->originalTouchCalls += static_cast<std::int32_t>(entity == other);
    fixture->originalTouchChecksum += entity == other ? 1 : 2;
    fixture->touchDigest = (entityValue << 1U) ^ otherValue ^ (fixture->touchDigest + 0x9E3779B97F4A7C15ULL);
    fixture->touchOther = otherValue;
}

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void FixtureSmokeDetonate(void* entity) {
    auto* const grenade = static_cast<FixtureGrenade*>(entity);
    if (grenade->entityIdentity != nullptr && grenade->entityIdentity->m_EHandle.IsValid()) {
        ++grenade->detonateCalls;
    }
}

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void FixtureMolotovDetonate(void* entity) {
    auto* const grenade = static_cast<FixtureGrenade*>(entity);
    if (grenade->entityIdentity != nullptr && grenade->entityIdentity->m_EHandle.IsValid()) {
        grenade->detonateCalls += 2;
    }
}

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void FixtureBigHeadScaleReset(void* pawn) {
    auto* const fixture = static_cast<FixtureEntity*>(pawn);
    fixture->bigHeadScale = 1.0f;
    fixture->bigHeadResetCalls += static_cast<std::int32_t>(pawn != nullptr);
}

void FixtureCollisionRulesChanged(void* entity) {
    auto* callCount = reinterpret_cast<std::int32_t*>(static_cast<std::byte*>(entity) +
                                                      offsetof(FixtureEntity, collisionRulesChangedCalls));
    ++*callCount;
    ++g_collisionRulesChangedCalls;
}

void FixtureIgnite(void* pawn) {
    if (pawn != &g_pawn) {
        return;
    }
    g_igniteFlamePresent = true;
    g_igniteFlame.attachedPawnHandle = 17;
    g_pawn.effectEntityHandle = kIgniteFlameHandle;
}

bool FixtureIsWeapon(void* entity) {
    return static_cast<FixtureWeapon*>(entity)->isWeapon;
}

bool FixtureNavTraceLine(void*,
                         const Vector&,
                         const Vector&,
                         const CBaseEntity*,
                         std::uint64_t,
                         std::uint8_t,
                         std::uint8_t,
                         trace_t* result) {
    ++g_navTraceCalls;
    if (!result) {
        return false;
    }
    result->m_pEnt = reinterpret_cast<decltype(result->m_pEnt)>(&g_victim);
    result->m_pHitbox = &g_hitbox;
    return true;
}

void FixtureDamageConstructor(void* storage,
                              CBaseEntity* inflictor,
                              CBaseEntity* attacker,
                              CBaseEntity* weapon,
                              const Vector*,
                              const Vector*,
                              float damage,
                              std::int32_t damageType,
                              std::int32_t,
                              void*) {
    ++g_damageConstructorCalls;
    auto* info = ::new (storage) FixtureDamageInfo{};
    info->inflictorHandle = static_cast<std::uint32_t>(HandleFromEntity(inflictor));
    info->attackerHandle = static_cast<std::uint32_t>(HandleFromEntity(attacker));
    info->abilityHandle = static_cast<std::uint32_t>(HandleFromEntity(weapon));
    info->damage = damage;
    info->originalDamage = damage;
    info->shouldBleed = true;
    info->shouldSpark = true;
    info->trace = &g_damageTrace;
    info->damageFlags = 0;
    info->hitGroupId = -1;
    info->damageType = static_cast<std::uint32_t>(damageType);
}

std::int64_t FixtureTakeDamage(void* victim, void* damageInfo, void* damageResult) {
    ++g_takeDamageCalls;
    auto* info = static_cast<FixtureDamageInfo*>(damageInfo);
    auto* result = static_cast<FixtureDamageResult*>(damageResult);
    const auto victimValue = reinterpret_cast<std::uintptr_t>(victim);
    if (!victim || !info || !result) {
        return -1;
    }
    result->healthLost = static_cast<std::int32_t>(info->damage);
    const bool distinctDamageResult = info->damage == 50.0f || info->damage == 9.0f;
    if (info->damage == 50.0f) {
        result->healthLost = 17;
    } else if (info->damage == 9.0f) {
        result->healthLost = 0;
    }
    result->healthBefore = 100;
    result->damageDealt = info->damage;
    result->preModifiedDamage = info->originalDamage;
    result->totalledHealthLost = result->healthLost;
    result->totalledDamageDealt = info->damage;
    result->totalledPreModifiedDamage = info->originalDamage;
    result->newDamageAccumulatorValue = 0.0f;
    result->damageFlags = info->damageFlags;
    result->damageSuppressed = false;
    info->shouldBleed = info->damage > 0.0f;
    info->shouldSpark = info->damage > 1.0f;
    if ((info->damageType & 0x4U) != 0) {
        g_outerDamageType = info->damageType;
        g_outerDamageHitGroup = info->hitGroupId;
        g_outerDamageAbility = info->abilityHandle;
        g_outerDamageAmount = info->damage;
    }
    g_takeDamageChecksum = (g_takeDamageChecksum << 1U) ^ victimValue ^
                           static_cast<std::uintptr_t>(result->healthLost) ^
                           static_cast<std::uintptr_t>(g_takeDamageCalls);
    return distinctDamageResult ? 1 : result->healthLost;
}

plg::PluginResult ValidateProviderShutdown() {
    const bool originalTouchRestored = g_pawn.originalTouchCalls >= 5 &&
                                       g_pawn.originalTouchChecksum == g_pawn.originalTouchCalls &&
                                       g_pawn.touchOther == reinterpret_cast<std::uintptr_t>(&g_pawn) &&
                                       g_pawn.touchDigest != 0;
    const bool grenadeHooksRemoved = __polyhook_FindDetour != nullptr &&
                                     !polyhook::FindDetour(g_smokeVtable[kDetonateIndex]) &&
                                     !polyhook::FindDetour(g_molotovVtable[kDetonateIndex]) &&
                                     !polyhook::FindDetour(g_heVtable[kDetonateIndex]);
    if (g_smokeGrenade.detonateCalls != 2) {
        return plg::PluginResult{"Native unload replayed a deferred smoke Detonate on the recycled entity"};
    }
    if (g_closedConfigCount != 1 || !originalTouchRestored || g_navTraceCalls != 1 ||
        g_lastEyePositionHandle != 17 || g_lastEyeAngleHandle != 17 ||
        g_damageConstructorCalls != 14 || g_takeDamageCalls != 10 || g_takeDamageChecksum == 0 ||
        g_pawnCollisionProperty.collisionGroup != COLLISION_GROUP_DEFAULT ||
        g_pawnCollisionProperty.collisionAttribute.collisionGroup != COLLISION_GROUP_DEFAULT ||
        g_pawn.collisionRulesChangedCalls != 2 || g_collisionRulesChangedCalls != 2 ||
        !g_mapEndCallbacks.empty() || !grenadeHooksRemoved) {
        return plg::PluginResult{"Native loader fixture did not restore hooks and exercise native damage/collision paths"};
    }
    return {};
}

class FixtureProviderPlugin final : public plg::Plugin {
public:
    plg::PluginResult OnPluginStart() override {
        g_smokeVtable[kDetonateIndex] = reinterpret_cast<void*>(&FixtureSmokeDetonate);
        g_molotovVtable[kDetonateIndex] = reinterpret_cast<void*>(&FixtureMolotovDetonate);
        g_heVtable[kDetonateIndex] = reinterpret_cast<void*>(&FixtureSmokeDetonate);
        g_smokeGrenadeHandle = kSmokeGrenadeHandle;
        g_smokeIdentity.m_EHandle = CEntityHandle(static_cast<std::uint32_t>(g_smokeGrenadeHandle));
        g_smokeIdentity.m_flags = EF_NONE;
        g_molotovIdentity.m_EHandle = CEntityHandle(static_cast<std::uint32_t>(kMolotovGrenadeHandle));
        g_molotovIdentity.m_flags = EF_NONE;
        g_heIdentity.m_EHandle = CEntityHandle(static_cast<std::uint32_t>(kHeGrenadeHandle));
        g_heIdentity.m_flags = EF_NONE;
        g_smokeGrenade.detonateCalls = 0;
        g_molotovGrenade.detonateCalls = 0;
        g_heGrenade.detonateCalls = 0;
        g_entityVtable[kTouchIndex] = reinterpret_cast<void*>(&FixtureTouch);
        g_entityVtable[kIsWeaponIndex] = reinterpret_cast<void*>(&FixtureIsWeapon);
        g_entityVtable[kCollisionRulesChangedIndex] = reinterpret_cast<void*>(&FixtureCollisionRulesChanged);
        g_entityVtable[kFixtureIgniteIndex] = reinterpret_cast<void*>(&FixtureIgnite);
        g_closedConfigCount = 0;
        g_collisionRulesChangedCalls = 0;
        g_navTraceCalls = 0;
        g_lastEyePositionHandle = -1;
        g_lastEyeAngleHandle = -1;
        g_damageConstructorCalls = 0;
        g_takeDamageCalls = 0;
        g_takeDamageChecksum = 0;
        g_activeConfig = 0;
        g_gameRules.activeMutators = 0x28;
        g_pawn.bigHeadScale = 1.25f;
        g_pawn.bigHeadResetCalls = 0;
        g_mapEndCallbacks.clear();
        g_outerDamageType = 0;
        g_outerDamageHitGroup = -1;
        g_outerDamageAbility = 0xffffffffU;
        g_outerDamageAmount = 0.0f;
        g_weaponServices.activeWeaponHandle = 18;
        g_weaponData.weaponType = 0;
        g_pawn.originalTouchCalls = 0;
        g_pawn.originalTouchChecksum = 0;
        g_pawn.touchDigest = 0;
        g_pawn.touchOther = 0;
        g_pawn.collisionRulesChangedCalls = 0;
        g_pawnCollisionProperty.collisionGroup = 7;
        g_pawnCollisionProperty.collisionAttribute.collisionGroup = 11;
        g_victimCollisionProperty.collisionGroup = 8;
        g_victimCollisionProperty.collisionAttribute.collisionGroup = 12;
        g_damageTrace.m_pEnt = reinterpret_cast<decltype(g_damageTrace.m_pEnt)>(&g_victim);
        g_damageTrace.m_pHitbox = &g_hitbox;
        g_pawn.health = 100;
        g_pawn.lifeState = 0;
        g_pawn.controllerHandle = 20;
        g_pawn.effectEntityHandle = -1;
        g_controller.playerPawnHandle = 17;
        g_controller.currentPawnHandle = 17;
        g_legacyPawn.health = 0;
        g_legacyPawn.lifeState = 1;
        g_igniteFlame.attachedPawnHandle = -1;
        g_igniteFlamePresent = false;
        g_afterburnClientEnabled = false;
        return {};
    }

    plg::PluginResult OnPluginEnd() override { return ValidateProviderShutdown(); }
};

FixtureProviderPlugin g_plugin;

} // namespace

extern "C" PLUGIN_API void PrintToServer(const plg::string& message) {
    std::fputs(message.c_str(), stdout);
}

extern "C" PLUGIN_API s2sdk::EventHookError HookEvent(const plg::string&,
                                                        s2sdk::EventCallback,
                                                        s2sdk::HookMode) {
    return s2sdk::EventHookError::Okay;
}

extern "C" PLUGIN_API s2sdk::EventHookError UnhookEvent(const plg::string&,
                                                          s2sdk::EventCallback,
                                                          s2sdk::HookMode) {
    return s2sdk::EventHookError::Okay;
}

extern "C" PLUGIN_API std::int32_t GetSchemaOffset(const plg::string& className,
                                                     const plg::string& memberName) {
    return className == "CBasePlayerPawn" && memberName == "m_hController" ? 0 : -1;
}

extern "C" PLUGIN_API bool AddConsoleCommand(const plg::string&,
                                              const plg::string&,
                                              s2sdk::ConVarFlag,
                                              s2sdk::ConCommandCallback,
                                              s2sdk::HookMode) {
    return true;
}

extern "C" PLUGIN_API bool RemoveCommand(const plg::string&, s2sdk::ConCommandCallback) { return true; }

extern "C" PLUGIN_API bool HasConsoleCommandCallbacks(const plg::string&) { return false; }

extern "C" PLUGIN_API std::uint64_t FindConVar(const plg::string&) { return UINT64_MAX; }

extern "C" PLUGIN_API std::int16_t UserMessageFindMessageIdByName(const plg::string&) { return -1; }

extern "C" PLUGIN_API void* QueryInterface(const plg::string& module, const plg::string& name) {
    if (module == "schemasystem" && name == SCHEMASYSTEM_INTERFACE_VERSION) {
        return &g_schemaSystem;
    }
    return nullptr;
}

extern "C" PLUGIN_API s2sdk::ConfigId LoadGameConfigFile(const plg::vector<plg::string>&) {
    g_activeConfig = 1;
    return g_activeConfig;
}

extern "C" PLUGIN_API void CloseGameConfigFile(s2sdk::ConfigId id) {
    if (id == g_activeConfig) {
        ++g_closedConfigCount;
        g_activeConfig = 0;
    }
}

extern "C" PLUGIN_API std::int32_t GetGameConfigOffset(s2sdk::ConfigId, const plg::string& name) {
    if (name == "CNavPhysicsInterface::TraceLineIgnoreEntity") return static_cast<std::int32_t>(kNavTraceIndex);
    if (name == "CBaseEntity::Touch") return static_cast<std::int32_t>(kTouchIndex);
    if (name == "CBaseEntity::IsWeapon") return static_cast<std::int32_t>(kIsWeaponIndex);
    if (name == "CBaseEntity::CollisionRulesChanged") return static_cast<std::int32_t>(kCollisionRulesChangedIndex);
    if (name == "CBaseEntity::IgnitePawn") return static_cast<std::int32_t>(kFixtureIgniteIndex);
    if (name == "CSGrenadeProjectileBase::Detonate") return static_cast<std::int32_t>(kDetonateIndex);
    return -1;
}

extern "C" PLUGIN_API void* GetGameConfigSignature(s2sdk::ConfigId, const plg::string& name) {
    if (name == "CBaseEntity_TakeDamageOld") return reinterpret_cast<void*>(&FixtureTakeDamage);
    if (name == "CTakeDamageInfo::Constructor") return reinterpret_cast<void*>(&FixtureDamageConstructor);
    if (name == "FunRounds::BigHeadScaleReset") return reinterpret_cast<void*>(&FixtureBigHeadScaleReset);
    return nullptr;
}

extern "C" PLUGIN_API void* GetGameConfigVTable(std::uint32_t, const plg::string& name) {
    if (name == "CNavPhysicsInterface") return g_navVtable;
    if (name == "CSmokeGrenadeProjectile") return g_smokeVtable;
    if (name == "CMolotovProjectile") return g_molotovVtable;
    if (name == "CHEGrenadeProjectile") return g_heVtable;
    return nullptr;
}

extern "C" PLUGIN_API void NativeFixtureSetNavAvailable(bool available) {
    g_navVtable[kNavTraceIndex] = available ? reinterpret_cast<void*>(&FixtureNavTraceLine) : nullptr;
}

extern "C" PLUGIN_API std::int32_t EntPointerToEntHandle(void* entity) {
    return HandleFromEntity(entity);
}

extern "C" PLUGIN_API void* EntHandleToEntPointer(std::int32_t handle) {
    return EntityFromHandle(handle);
}

extern "C" PLUGIN_API bool IsEntityPlayerPawn(std::int32_t handle) {
    return handle == 17 || handle == 21 || handle == 26;
}

extern "C" PLUGIN_API bool IsValidEntHandle(std::int32_t handle) {
    return EntityFromHandle(handle) != nullptr;
}

extern "C" PLUGIN_API void* GetGameRules() { return &g_gameRules; }

extern "C" PLUGIN_API std::int64_t GetEntSchema2(void* entity,
                                                  const plg::string& className,
                                                  const plg::string& memberName,
                                                  std::int32_t element) {
    if (entity == &g_gameRules && className == "CCSGameRules" && memberName == "m_nActiveMutators" &&
        element == 0) {
        return g_gameRules.activeMutators;
    }
    if (g_afterburnClientEnabled && entity != nullptr && className == "CBaseEntity" && element == 0) {
        const auto* const fixture = static_cast<const FixtureEntity*>(entity);
        if (memberName == "m_iHealth") {
            return fixture->health;
        }
        if (memberName == "m_lifeState") {
            return fixture->lifeState;
        }
    }
    return 0;
}

extern "C" PLUGIN_API void SetEntSchema2(void* entity,
                                         const plg::string& className,
                                         const plg::string& memberName,
                                         std::int64_t value,
                                         bool,
                                         std::int32_t element) {
    if (entity == &g_gameRules && className == "CCSGameRules" && memberName == "m_nActiveMutators" &&
        element == 0) {
        if (g_failMutatorWrites) {
            return;
        }
        g_gameRules.activeMutators = static_cast<std::int32_t>(value);
    }
}

extern "C" PLUGIN_API double GetEntSchemaFloat2(void* entity,
                                                const plg::string& className,
                                                const plg::string& memberName,
                                                std::int32_t element) {
    if (entity == &g_pawn && className == "CCSPlayerPawn" && memberName == "m_flBigHeadScale" && element == 0) {
        return static_cast<double>(g_pawn.bigHeadScale);
    }
    return 0.0;
}

extern "C" PLUGIN_API void SetEntSchemaFloat2(void* entity,
                                               const plg::string& className,
                                               const plg::string& memberName,
                                               double value,
                                               bool,
                                               std::int32_t element) {
    if (entity == &g_pawn && className == "CCSPlayerPawn" && memberName == "m_flBigHeadScale" && element == 0) {
        g_pawn.bigHeadScale = static_cast<float>(value);
    }
}

extern "C" PLUGIN_API void OnMapEnd_Register(s2sdk::OnMapEndCallback callback) {
    if (callback && std::find(g_mapEndCallbacks.begin(), g_mapEndCallbacks.end(), callback) == g_mapEndCallbacks.end()) {
        g_mapEndCallbacks.push_back(callback);
    }
}

extern "C" PLUGIN_API void OnMapEnd_Unregister(s2sdk::OnMapEndCallback callback) {
    std::erase(g_mapEndCallbacks, callback);
}

extern "C" PLUGIN_API void NativeFixtureFailMutatorWrites(bool fail) {
    g_failMutatorWrites = fail;
}

extern "C" PLUGIN_API void NativeFixtureFireMapEnd() {
    const auto callbacks = g_mapEndCallbacks;
    for (const auto callback : callbacks) {
        if (callback != nullptr) {
            callback();
        }
    }
}

extern "C" PLUGIN_API void NativeFixtureEnableAfterburnClient(bool enabled) {
    g_afterburnClientEnabled = enabled;
    g_controller.playerPawnHandle = enabled ? 26U : 17U;
}

extern "C" PLUGIN_API void RemoveEntity(std::int32_t handle) {
    if (handle == kIgniteFlameHandle && g_igniteFlamePresent) {
        g_igniteFlamePresent = false;
        if (g_pawn.effectEntityHandle == kIgniteFlameHandle) {
            g_pawn.effectEntityHandle = -1;
        }
    }
}

extern "C" PLUGIN_API bool IsEntityPlayerController(std::int32_t handle) {
    return handle == 20;
}

extern "C" PLUGIN_API std::int32_t GetMaxClients() { return g_afterburnClientEnabled ? 1 : 0; }

extern "C" PLUGIN_API std::int32_t GetGameTickCount() { return 0; }

extern "C" PLUGIN_API float GetGameTime() { return 0.0f; }

extern "C" PLUGIN_API bool IsClientInGame(std::int32_t slot) { return g_afterburnClientEnabled && slot == 0; }

extern "C" PLUGIN_API std::uint64_t GetClientButtons(std::int32_t, std::int32_t) { return 0; }

extern "C" PLUGIN_API bool IsClientAlive(std::int32_t slot) {
    if (!g_afterburnClientEnabled || slot != 0) {
        return false;
    }
    const auto* const playerPawn = EntityFromHandle(static_cast<std::int32_t>(g_controller.playerPawnHandle));
    return playerPawn != nullptr && playerPawn->health > 0 && playerPawn->lifeState == 0;
}

extern "C" PLUGIN_API std::int32_t GetClientHealth(std::int32_t slot) {
    if (!g_afterburnClientEnabled || slot != 0) {
        return 0;
    }
    const auto* const playerPawn = EntityFromHandle(static_cast<std::int32_t>(g_controller.currentPawnHandle));
    return playerPawn == nullptr ? 0 : playerPawn->health;
}

extern "C" PLUGIN_API std::int32_t PlayerSlotToEntHandle(std::int32_t slot) {
    return g_afterburnClientEnabled && slot == 0 ? 20 : -1;
}

extern "C" PLUGIN_API void* GetClientPawn(std::int32_t slot) {
    return g_afterburnClientEnabled && slot == 0
               ? EntityFromHandle(static_cast<std::int32_t>(g_controller.currentPawnHandle))
               : nullptr;
}

extern "C" PLUGIN_API bool IsFakeClient(std::int32_t) { return false; }

extern "C" PLUGIN_API std::uint16_t GetWeaponItemDefinition(std::int32_t) { return 0; }

#if defined(_MSC_VER)
#pragma warning(disable : 4190)
#endif
extern "C" PLUGIN_API plg::vector<std::int32_t> GetClientWeapons(std::int32_t) { return {}; }

extern "C" PLUGIN_API plg::string GetEntityClassname(std::int32_t handle) {
    if (handle == kIgniteFlameHandle && g_igniteFlamePresent) return plg::string("entityflame");
    if (handle == g_smokeGrenadeHandle) return plg::string("smokegrenade_projectile");
    if (handle == kMolotovGrenadeHandle) return plg::string("molotov_projectile");
    if (handle == kHeGrenadeHandle) return plg::string("hegrenade_projectile");
    return {};
}
#if defined(_MSC_VER)
#pragma warning(default : 4190)
#endif

extern "C" PLUGIN_API std::int32_t GetEntSchemaEnt2(void* entity,
                                                      const plg::string& className,
                                                      const plg::string& memberName,
                                                      std::int32_t element) {
    if (!g_afterburnClientEnabled || entity == nullptr || element != 0) {
        return -1;
    }
    const auto* const fixture = static_cast<const FixtureEntity*>(entity);
    if (className == "CBasePlayerPawn" && memberName == "m_hController") {
        return fixture->controllerHandle;
    }
    if (className == "CBaseEntity" && memberName == "m_hEffectEntity") {
        return fixture->effectEntityHandle;
    }
    if (className == "CEntityFlame" && memberName == "m_hEntAttached") {
        return fixture->attachedPawnHandle;
    }
    return -1;
}

extern "C" PLUGIN_API void OnClientDisconnect_Register(s2sdk::OnClientDisconnectCallback) {}

extern "C" PLUGIN_API void OnClientDisconnect_Unregister(s2sdk::OnClientDisconnectCallback) {}

extern "C" PLUGIN_API plg::vec3 GetEntityEyePosition(std::int32_t handle) {
    g_lastEyePositionHandle = handle;
    return {0.0f, 0.0f, 0.0f};
}

extern "C" PLUGIN_API plg::vec3 GetEntityEyeAngles(std::int32_t handle) {
    g_lastEyeAngleHandle = handle;
    return {0.0f, 0.0f, 0.0f};
}

extern "C" PLUGIN_API void NativeFixtureFireTouch() {
    using TouchFunction = void (*)(void*, void*);
    reinterpret_cast<TouchFunction>(g_entityVtable[kTouchIndex])(&g_pawn, &g_pawn);
}

extern "C" PLUGIN_API bool NativeFixtureFireStandardGrenadeDetonate(std::int32_t handle) {
    using Detonate = void (*)(void*);
    if (handle == g_smokeGrenadeHandle) {
        reinterpret_cast<Detonate>(g_smokeVtable[kDetonateIndex])(&g_smokeGrenade);
        return true;
    }
    if (handle == kMolotovGrenadeHandle) {
        reinterpret_cast<Detonate>(g_molotovVtable[kDetonateIndex])(&g_molotovGrenade);
        return true;
    }
    if (handle == kHeGrenadeHandle) {
        reinterpret_cast<Detonate>(g_heVtable[kDetonateIndex])(&g_heGrenade);
        return true;
    }
    return false;
}

extern "C" PLUGIN_API std::int32_t NativeFixtureGrenadeDetonateCount(std::int32_t handle) {
    if (handle == g_smokeGrenadeHandle) return g_smokeGrenade.detonateCalls;
    if (handle == kMolotovGrenadeHandle) return g_molotovGrenade.detonateCalls;
    if (handle == kHeGrenadeHandle) return g_heGrenade.detonateCalls;
    return -1;
}

extern "C" PLUGIN_API std::int32_t NativeFixtureReuseSmokeGrenadeHandle() {
    g_smokeIdentity.m_EHandle.IncrementSerialNumber();
    g_smokeGrenadeHandle = std::bit_cast<std::int32_t>(g_smokeIdentity.m_EHandle.ToInt());
    g_smokeIdentity.m_flags = EF_NONE;
    return g_smokeGrenadeHandle;
}

extern "C" PLUGIN_API std::int32_t NativeFixtureTouchCallCount() { return g_pawn.originalTouchCalls; }
extern "C" PLUGIN_API std::int32_t NativeFixtureTouchChecksum() { return g_pawn.originalTouchChecksum; }
extern "C" PLUGIN_API std::int32_t NativeFixtureNavTraceCallCount() { return g_navTraceCalls; }
extern "C" PLUGIN_API std::int32_t NativeFixtureLastEyePositionHandle() { return g_lastEyePositionHandle; }
extern "C" PLUGIN_API std::int32_t NativeFixtureLastEyeAngleHandle() { return g_lastEyeAngleHandle; }
extern "C" PLUGIN_API std::int32_t NativeFixtureDamageConstructorCallCount() { return g_damageConstructorCalls; }
extern "C" PLUGIN_API std::int32_t NativeFixtureTakeDamageCallCount() { return g_takeDamageCalls; }
extern "C" PLUGIN_API std::int32_t NativeFixtureCollisionGroup() {
    return g_pawnCollisionProperty.collisionGroup;
}
extern "C" PLUGIN_API std::int32_t NativeFixtureCollisionAttributeGroup() {
    return g_pawnCollisionProperty.collisionAttribute.collisionGroup;
}
extern "C" PLUGIN_API std::int32_t NativeFixtureCollisionRulesChangedCount() {
    return g_pawn.collisionRulesChangedCalls;
}
extern "C" PLUGIN_API void NativeFixtureSetCollisionState(bool collisionGroupFieldAvailable,
                                                            std::int32_t collisionGroup,
                                                            std::int32_t attributeGroup) {
    g_collisionPropertyClass.m_nFieldCount = static_cast<std::uint16_t>(
        collisionGroupFieldAvailable ? std::size(g_collisionPropertyFields) : std::size(g_collisionPropertyFields) - 1);
    g_pawnCollisionProperty.collisionGroup = static_cast<std::uint8_t>(collisionGroup);
    g_pawnCollisionProperty.collisionAttribute.collisionGroup = static_cast<std::uint8_t>(attributeGroup);
}
extern "C" PLUGIN_API std::int32_t NativeFixtureSchemaClassLookupCount() {
    return g_schemaClassLookups;
}
extern "C" PLUGIN_API std::int32_t NativeFixtureBigHeadResetCallCount() { return g_pawn.bigHeadResetCalls; }
extern "C" PLUGIN_API std::uint32_t NativeFixtureOuterDamageType() { return g_outerDamageType; }
extern "C" PLUGIN_API std::int32_t NativeFixtureOuterDamageHitGroup() { return g_outerDamageHitGroup; }
extern "C" PLUGIN_API std::uint32_t NativeFixtureOuterDamageAbility() { return g_outerDamageAbility; }
extern "C" PLUGIN_API float NativeFixtureOuterDamageAmount() { return g_outerDamageAmount; }

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)

struct FixtureProviderInit {
    FixtureProviderInit() {
        g_navVtable[kNavTraceIndex] = reinterpret_cast<void*>(&FixtureNavTraceLine);
        g_hitbox.m_nGroupId = HITGROUP_HEAD;
        g_damageTrace.m_pEnt = reinterpret_cast<decltype(g_damageTrace.m_pEnt)>(&g_victim);
        g_damageTrace.m_pHitbox = &g_hitbox;
        InitializeSchemaMetadata();
    }
} g_fixtureProviderInit;
