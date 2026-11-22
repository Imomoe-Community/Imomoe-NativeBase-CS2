#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
set -euo pipefail

if (($# == 0)); then
  echo "Usage: $0 [--allow-game-runtime-libraries] <ELF shared library> [...]" >&2
  exit 2
fi

check_binary() {
  local binary="$1"
  local allow_game_runtime_libraries="$2"
  local output line soname
  local -a unresolved=()

  if ! output="$(LC_ALL=C ldd -v "$binary" 2>&1)"; then
    printf '%s\n' "$output"
    echo "ldd failed for $binary" >&2
    return 1
  fi
  printf '%s\n' "$output"

  while IFS= read -r line; do
    [[ "$line" == *"not found"* ]] || continue
    line="${line#"${line%%[![:space:]]*}"}"
    soname="${line%%[[:space:]]*}"
    if [[ "$line" == *"=> not found" ]]; then
      case "$soname" in
        libv8.so|libtier0.so|libsteam_api.so)
          if [[ "$allow_game_runtime_libraries" == 1 ]]; then continue; fi
          ;;
      esac
    fi
    unresolved+=("$line")
  done <<< "$output"

  if ((${#unresolved[@]})); then
    printf 'Unresolved system dependency or symbol version for %s:\n' "$binary" >&2
    printf '  %s\n' "${unresolved[@]}" >&2
    return 1
  fi
}

while (($#)); do
  allow_game_runtime_libraries=0
  if [[ "$1" == "--allow-game-runtime-libraries" ]]; then
    allow_game_runtime_libraries=1
    shift
    if (($# == 0)) || [[ "$1" == -* ]]; then
      echo "--allow-game-runtime-libraries must precede an ELF shared library" >&2
      exit 2
    fi
  elif [[ "$1" == -* ]]; then
    echo "Unknown option: $1" >&2
    exit 2
  fi
  check_binary "$1" "$allow_game_runtime_libraries"
  shift
done
