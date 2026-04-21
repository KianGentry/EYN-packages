#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

src="${1:-packages/hello/hello_uelf.c}"
out="${2:-build/hello.uelf}"

pkg_basename="$(basename "$src")"
pkg_name="${pkg_basename%_uelf.c}"
if [ "$pkg_name" = "$pkg_basename" ]; then
  pkg_name="${pkg_basename%.*}"
fi
if [ -n "${EYN_PKG_NAME:-}" ]; then
  pkg_name="$EYN_PKG_NAME"
fi

resolve_version_from_index() {
  local index_path="$repo_root/index.json"
  if [ ! -f "$index_path" ]; then
    return 1
  fi
  if ! command -v python3 >/dev/null 2>&1; then
    return 1
  fi

  python3 - "$index_path" "$pkg_name" <<'PY'
import json
import re
import sys

index_path = sys.argv[1]
pkg_name = sys.argv[2]

try:
    with open(index_path, "r", encoding="utf-8") as f:
        data = json.load(f)
except Exception:
    print("0")
    raise SystemExit(0)

packages = data.get("packages") if isinstance(data, dict) else None
entry = packages.get(pkg_name) if isinstance(packages, dict) else None
latest = entry.get("latest") if isinstance(entry, dict) else None

if isinstance(latest, int):
    print(latest if latest >= 0 else 0)
elif isinstance(latest, str):
    m = re.match(r"\s*(\d+)", latest)
    print(m.group(1) if m else "0")
else:
    print("0")
PY
}

pkg_version_raw="${EYN_PKG_VERSION_INT:-${EYN_PKG_VERSION:-}}"
if [ -z "$pkg_version_raw" ]; then
  pkg_version_raw="$(resolve_version_from_index || echo 0)"
fi
if [ -z "$pkg_version_raw" ]; then
  pkg_version_raw=0
fi

if [[ "$pkg_version_raw" =~ ^[0-9]+$ ]]; then
  pkg_version_int="$pkg_version_raw"
else
  pkg_version_int=0
fi

tmp_root="tmp_user"
mkdir -p "$tmp_root"

pkg_dir="$(cd "$(dirname "$src")" && pwd)"
pkg_include_dir="$pkg_dir/include"
pkg_src_dir="$pkg_dir/src"
shared_mbedtls_dir="$repo_root/shared/mbedtls"
shared_mbedtls_sources_file="$shared_mbedtls_dir/sources.list"
use_shared_mbedtls=0

ldscript="devtools/user_elf32.ld"
crt0="userland/crt0.S"

incdir="userland/include"
libc_dir="userland/libc"

obj_app="$tmp_root/user_app.o"
obj_crt="$tmp_root/user_crt0.o"
obj_libc_unistd="$tmp_root/user_libc_unistd.o"
obj_libc_string="$tmp_root/user_libc_string.o"
obj_libc_stdio="$tmp_root/user_libc_stdio.o"
obj_libc_fcntl="$tmp_root/user_libc_fcntl.o"
obj_libc_dirent="$tmp_root/user_libc_dirent.o"
obj_libc_gui="$tmp_root/user_libc_gui.o"
obj_libc_time="$tmp_root/user_libc_time.o"
obj_libc_stdlib="$tmp_root/user_libc_stdlib.o"
obj_libc_errno="$tmp_root/user_libc_errno.o"
obj_libc_x11="$tmp_root/user_libc_x11.o"
obj_libc_setjmp="$tmp_root/user_libc_setjmp.o"
obj_libc_stat="$tmp_root/user_libc_stat.o"
obj_libc_ctype="$tmp_root/user_libc_ctype.o"
obj_libc_libgen="$tmp_root/user_libc_libgen.o"
obj_libc_notify="$tmp_root/user_libc_notify.o"
obj_libc_mbedtls_alloc="$tmp_root/user_libc_mbedtls_alloc.o"
obj_pkgmeta="$tmp_root/user_pkgmeta.o"
lib_archive="$tmp_root/libeync.a"

# Prefer a cross-compiler if available.
if command -v i686-elf-gcc >/dev/null 2>&1; then
  CC=i686-elf-gcc
elif command -v gcc >/dev/null 2>&1; then
  CC=gcc
else
  echo "No C compiler found (need i686-elf-gcc or gcc)." >&2
  exit 1
fi

CFLAGS=(
  -m32
  -ffreestanding
  -fno-builtin
  -fno-pie
  -fno-pic
  -fno-plt
  -fno-stack-protector
  -fno-asynchronous-unwind-tables
  -fno-unwind-tables
  -fno-omit-frame-pointer
  -nostdlib
  -nostartfiles
  -I"$incdir"
  -Wall -Wextra
  -O2
)

if [ -d "$pkg_include_dir" ]; then
  CFLAGS+=( -I"$pkg_include_dir" )
fi
if [ -d "$pkg_include_dir/install" ]; then
  CFLAGS+=( -I"$pkg_include_dir/install" )
fi
if [ -d "$pkg_src_dir" ]; then
  CFLAGS+=( -I"$pkg_src_dir" )
fi

scan_sources=( "$src" )
if [ -d "$pkg_src_dir" ]; then
  while IFS= read -r scan_src; do
    scan_sources+=( "$scan_src" )
  done < <(find "$pkg_src_dir" -type f -name '*.c' | sort)
fi

if [ -d "$shared_mbedtls_dir" ]; then
  if grep -E -q '["<]mbedtls/' "${scan_sources[@]}"; then
    use_shared_mbedtls=1
    CFLAGS+=( -I"$repo_root/shared" )
  fi
fi

# Validate that gcc supports -m32 when not using a cross toolchain.
if [[ "$CC" == "gcc" ]]; then
  if ! echo "int x;" | gcc -m32 -x c -c -o /dev/null - >/dev/null 2>&1; then
    echo "Host gcc does not support -m32. Install multilib (e.g., gcc-multilib) or use i686-elf-gcc." >&2
    exit 1
  fi
fi

"$CC" "${CFLAGS[@]}" -c "$crt0" -o "$obj_crt"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/unistd.c" -o "$obj_libc_unistd"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/string.c" -o "$obj_libc_string"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/stdio.c" -o "$obj_libc_stdio"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/fcntl.c" -o "$obj_libc_fcntl"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/dirent.c" -o "$obj_libc_dirent"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/gui.c" -o "$obj_libc_gui"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/time.c" -o "$obj_libc_time"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/stdlib.c" -o "$obj_libc_stdlib"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/errno.c" -o "$obj_libc_errno"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/x11.c" -o "$obj_libc_x11"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/setjmp.c" -o "$obj_libc_setjmp"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/stat.c" -o "$obj_libc_stat"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/ctype.c" -o "$obj_libc_ctype"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/libgen.c" -o "$obj_libc_libgen"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/notify.c" -o "$obj_libc_notify"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/mbedtls_alloc.c" -o "$obj_libc_mbedtls_alloc"

pkgmeta_src="$tmp_root/user_pkgmeta.c"
cat > "$pkgmeta_src" <<'PKGMETA_EOF'
#include <eynos_pkgmeta.h>

#ifndef EYN_PKGMETA_NAME_LITERAL
#define EYN_PKGMETA_NAME_LITERAL "unknown"
#endif

#ifndef EYN_PKGMETA_VERSION_INT
#define EYN_PKGMETA_VERSION_INT 0
#endif

EYN_PKGMETA_V1(EYN_PKGMETA_NAME_LITERAL, EYN_PKGMETA_VERSION_INT);
PKGMETA_EOF

"$CC" "${CFLAGS[@]}" \
  -DEYN_PKGMETA_NAME_LITERAL="\"$pkg_name\"" \
  -DEYN_PKGMETA_VERSION_INT="$pkg_version_int" \
  -c "$pkgmeta_src" \
  -o "$obj_pkgmeta"

rm -f "$lib_archive"
ar rcs "$lib_archive" "$obj_libc_x11" "$obj_libc_setjmp" "$obj_libc_stat" "$obj_libc_ctype" "$obj_libc_libgen" "$obj_libc_notify" "$obj_libc_mbedtls_alloc" "$obj_libc_unistd" "$obj_libc_string" "$obj_libc_stdio" "$obj_libc_fcntl" "$obj_libc_dirent" "$obj_libc_gui" "$obj_libc_time" "$obj_libc_stdlib" "$obj_libc_errno"

"$CC" "${CFLAGS[@]}" -c "$src" -o "$obj_app"

extra_objs=()
if [ -d "$pkg_src_dir" ]; then
  extra_idx=0
  while IFS= read -r extra_src; do
    extra_obj="$tmp_root/user_pkg_${extra_idx}.o"
    "$CC" "${CFLAGS[@]}" -c "$extra_src" -o "$extra_obj"
    extra_objs+=( "$extra_obj" )
    extra_idx=$((extra_idx + 1))
  done < <(find "$pkg_src_dir" -type f -name '*.c' | sort)
fi

if [ "$use_shared_mbedtls" -eq 1 ]; then
  shared_idx=0
  if [ -f "$shared_mbedtls_sources_file" ]; then
    while IFS= read -r rel_src; do
      rel_src="${rel_src#./}"
      case "$rel_src" in
        ""|\#*) continue ;;
      esac
      shared_src="$shared_mbedtls_dir/$rel_src"
      if [ ! -f "$shared_src" ]; then
        echo "Missing shared mbedTLS source listed in sources.list: $rel_src" >&2
        exit 1
      fi
      shared_obj="$tmp_root/user_shared_${shared_idx}.o"
      "$CC" "${CFLAGS[@]}" -c "$shared_src" -o "$shared_obj"
      extra_objs+=( "$shared_obj" )
      shared_idx=$((shared_idx + 1))
    done < "$shared_mbedtls_sources_file"
  else
    while IFS= read -r shared_src; do
      shared_obj="$tmp_root/user_shared_${shared_idx}.o"
      "$CC" "${CFLAGS[@]}" -c "$shared_src" -o "$shared_obj"
      extra_objs+=( "$shared_obj" )
      shared_idx=$((shared_idx + 1))
    done < <(find "$shared_mbedtls_dir" -type f -name '*.c' | sort)
  fi
fi

# Link a simple ELF32 ET_EXEC at 0x00400000.
# --start-group/--end-group ensures cross-object references within the
# archive resolve regardless of insertion order (needed for x11.c → gui.c).
mkdir -p "$(dirname "$out")"
"$CC" -m32 -nostdlib -nostartfiles -Wl,-m,elf_i386 -Wl,-nostdlib -Wl,-e,_start -Wl,-T,"$ldscript" -o "$out" "$obj_crt" "$obj_app" "$obj_pkgmeta" "${extra_objs[@]}" -Wl,--start-group "$lib_archive" -lgcc -Wl,--end-group

echo "Built $out"
