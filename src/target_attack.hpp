#pragma once

#include <cstdint>

namespace funrounds::native::detail {
bool ShutdownTargetAttack() noexcept;
bool HasNativeWeaponCommand() noexcept;
std::int32_t NativeWeaponCommandSlot(std::int32_t pawn) noexcept;
bool ShutdownNativeWeaponCommand() noexcept;
}
