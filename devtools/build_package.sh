#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

pkg_input="${1:-}"
out="${2:-}"

if [ -z "$pkg_input" ]; then
  echo "usage: $0 <package-path-or-source-file> [output]" >&2
  exit 1
fi

if [ -z "$out" ]; then
  out="build/$(basename "$pkg_input").uelf"
fi

if [ -d "$pkg_input" ]; then
  pkg_dir="$(cd "$pkg_input" && pwd)"
elif [ -f "$pkg_input" ]; then
  pkg_dir="$(cd "$(dirname "$pkg_input")" && pwd)"
else
  echo "error: package path does not exist: $pkg_input" >&2
  exit 1
fi

pkg_name="$(basename "$pkg_dir")"
if [ -n "${EYN_PKG_NAME:-}" ]; then
  pkg_name="$EYN_PKG_NAME"
fi

pkg_version_raw="${EYN_PKG_VERSION_INT:-${EYN_PKG_VERSION:-0}}"
if [[ "$pkg_version_raw" =~ ^[0-9]+$ ]]; then
  pkg_version_int="$pkg_version_raw"
else
  pkg_version_int=0
fi

ldscript="$repo_root/devtools/user_elf32.ld"
crt0="$repo_root/userland/crt0.S"
incdir="$repo_root/userland/include"
libc_dir="$repo_root/userland/libc"

resolve_cc() {
  if command -v i686-elf-gcc >/dev/null 2>&1; then
    echo i686-elf-gcc
  elif command -v gcc >/dev/null 2>&1; then
    echo gcc
  else
    echo ""
  fi
}

CC="$(resolve_cc)"
if [ -z "$CC" ]; then
  echo "No C compiler found (need i686-elf-gcc or gcc)." >&2
  exit 1
fi

if [[ "$CC" == "gcc" ]]; then
  if ! echo "int x;" | gcc -m32 -x c -c -o /dev/null - >/dev/null 2>&1; then
    echo "Host gcc does not support -m32. Install multilib (e.g., gcc-multilib) or use i686-elf-gcc." >&2
    exit 1
  fi
fi

CFLAGS_BASE=(
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

if [ -n "${EYN_EXTRA_CFLAGS:-}" ]; then
  read -r -a extra_cflags <<<"$EYN_EXTRA_CFLAGS"
  CFLAGS_BASE+=("${extra_cflags[@]}")
fi

build_runtime_archive() {
  local tmp_root="$1"

  local obj_libc_unistd="$tmp_root/user_libc_unistd.o"
  local obj_libc_string="$tmp_root/user_libc_string.o"
  local obj_libc_stdio="$tmp_root/user_libc_stdio.o"
  local obj_libc_fcntl="$tmp_root/user_libc_fcntl.o"
  local obj_libc_dirent="$tmp_root/user_libc_dirent.o"
  local obj_libc_gui="$tmp_root/user_libc_gui.o"
  local obj_libc_time="$tmp_root/user_libc_time.o"
  local obj_libc_stdlib="$tmp_root/user_libc_stdlib.o"
  local obj_libc_errno="$tmp_root/user_libc_errno.o"
  local obj_libc_exec="$tmp_root/user_libc_exec.o"
  local obj_libc_x11="$tmp_root/user_libc_x11.o"
  local obj_libc_setjmp="$tmp_root/user_libc_setjmp.o"
  local obj_libc_stat="$tmp_root/user_libc_stat.o"
  local obj_libc_ctype="$tmp_root/user_libc_ctype.o"
  local obj_libc_libgen="$tmp_root/user_libc_libgen.o"
  local obj_libc_notify="$tmp_root/user_libc_notify.o"
  local obj_libc_mbedtls_alloc="$tmp_root/user_libc_mbedtls_alloc.o"
  local obj_libc_signal="$tmp_root/user_libc_signal.o"
  local obj_libc_termios="$tmp_root/user_libc_termios.o"
  local obj_libc_posix_spawn="$tmp_root/user_libc_posix_spawn.o"
  local obj_libc_crypt="$tmp_root/user_libc_crypt.o"
  local obj_libc_net_byteorder="$tmp_root/user_libc_net_byteorder.o"

  "$CC" "${CFLAGS_BASE[@]}" -c "$libc_dir/unistd.c" -o "$obj_libc_unistd"
  "$CC" "${CFLAGS_BASE[@]}" -c "$libc_dir/string.c" -o "$obj_libc_string"
  "$CC" "${CFLAGS_BASE[@]}" -c "$libc_dir/stdio.c" -o "$obj_libc_stdio"
  "$CC" "${CFLAGS_BASE[@]}" -c "$libc_dir/fcntl.c" -o "$obj_libc_fcntl"
  "$CC" "${CFLAGS_BASE[@]}" -c "$libc_dir/dirent.c" -o "$obj_libc_dirent"
  "$CC" "${CFLAGS_BASE[@]}" -c "$libc_dir/gui.c" -o "$obj_libc_gui"
  "$CC" "${CFLAGS_BASE[@]}" -c "$libc_dir/time.c" -o "$obj_libc_time"
  "$CC" "${CFLAGS_BASE[@]}" -c "$libc_dir/stdlib.c" -o "$obj_libc_stdlib"
  "$CC" "${CFLAGS_BASE[@]}" -c "$libc_dir/errno.c" -o "$obj_libc_errno"
  "$CC" "${CFLAGS_BASE[@]}" -c "$libc_dir/exec.c" -o "$obj_libc_exec"
  "$CC" "${CFLAGS_BASE[@]}" -c "$libc_dir/x11.c" -o "$obj_libc_x11"
  "$CC" "${CFLAGS_BASE[@]}" -c "$libc_dir/setjmp.c" -o "$obj_libc_setjmp"
  "$CC" "${CFLAGS_BASE[@]}" -c "$libc_dir/stat.c" -o "$obj_libc_stat"
  "$CC" "${CFLAGS_BASE[@]}" -c "$libc_dir/ctype.c" -o "$obj_libc_ctype"
  "$CC" "${CFLAGS_BASE[@]}" -c "$libc_dir/libgen.c" -o "$obj_libc_libgen"
  "$CC" "${CFLAGS_BASE[@]}" -c "$libc_dir/notify.c" -o "$obj_libc_notify"
  "$CC" "${CFLAGS_BASE[@]}" -c "$libc_dir/mbedtls_alloc.c" -o "$obj_libc_mbedtls_alloc"
  "$CC" "${CFLAGS_BASE[@]}" -c "$libc_dir/signal.c" -o "$obj_libc_signal"
  "$CC" "${CFLAGS_BASE[@]}" -c "$libc_dir/termios.c" -o "$obj_libc_termios"
  "$CC" "${CFLAGS_BASE[@]}" -c "$libc_dir/posix_spawn.c" -o "$obj_libc_posix_spawn"
  "$CC" "${CFLAGS_BASE[@]}" -c "$libc_dir/crypt.c" -o "$obj_libc_crypt"
  "$CC" "${CFLAGS_BASE[@]}" -c "$libc_dir/net_byteorder.c" -o "$obj_libc_net_byteorder"

  local lib_archive="$tmp_root/libeync.a"
  rm -f "$lib_archive"
  ar rcs "$lib_archive" \
    "$obj_libc_x11" "$obj_libc_setjmp" "$obj_libc_stat" "$obj_libc_ctype" \
    "$obj_libc_libgen" "$obj_libc_notify" "$obj_libc_mbedtls_alloc" \
    "$obj_libc_exec" "$obj_libc_unistd" "$obj_libc_string" "$obj_libc_stdio" \
    "$obj_libc_fcntl" "$obj_libc_dirent" "$obj_libc_gui" "$obj_libc_time" \
    "$obj_libc_stdlib" "$obj_libc_errno" "$obj_libc_signal" \
    "$obj_libc_termios" "$obj_libc_posix_spawn" "$obj_libc_crypt" \
    "$obj_libc_net_byteorder"

  echo "$lib_archive"
}

build_pkgmeta_obj() {
  local tmp_root="$1"
  local pkgmeta_src="$tmp_root/user_pkgmeta.c"
  local obj_pkgmeta="$tmp_root/user_pkgmeta.o"

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

  "$CC" "${CFLAGS_BASE[@]}" \
    -DEYN_PKGMETA_NAME_LITERAL="\"$pkg_name\"" \
    -DEYN_PKGMETA_VERSION_INT="$pkg_version_int" \
    -c "$pkgmeta_src" -o "$obj_pkgmeta"

  echo "$obj_pkgmeta"
}

build_from_source_list() {
  local out_path="$1"
  shift
  local sources=("$@")

  if [ "${#sources[@]}" -eq 0 ]; then
    echo "error: no source files to compile for package $pkg_name" >&2
    return 1
  fi

  local tmp_root="$repo_root/tmp_user"
  mkdir -p "$tmp_root"

  local obj_crt="$tmp_root/user_crt0.o"
  "$CC" "${CFLAGS_BASE[@]}" -c "$crt0" -o "$obj_crt"

  local lib_archive
  lib_archive="$(build_runtime_archive "$tmp_root")"

  local obj_pkgmeta
  obj_pkgmeta="$(build_pkgmeta_obj "$tmp_root")"

  local pkg_cflags=("${CFLAGS_BASE[@]}" -I"$pkg_dir")
  if [ -d "$pkg_dir/include" ]; then
    pkg_cflags+=( -I"$pkg_dir/include" )
  fi
  if [ -d "$pkg_dir/src" ]; then
    pkg_cflags+=( -I"$pkg_dir/src" )
  fi
  if [ -d "$pkg_dir/generated" ]; then
    pkg_cflags+=( -I"$pkg_dir/generated" )
  fi
  if [ -d "$repo_root/shared" ]; then
    pkg_cflags+=( -I"$repo_root/shared" )
  fi

  local objs=()
  local idx=0
  local src
  for src in "${sources[@]}"; do
    if [ ! -f "$src" ]; then
      echo "error: listed source is missing: $src" >&2
      return 1
    fi
    local obj="$tmp_root/user_pkg_${pkg_name}_${idx}.o"
    "$CC" "${pkg_cflags[@]}" -c "$src" -o "$obj"
    objs+=("$obj")
    idx=$((idx + 1))
  done

  mkdir -p "$(dirname "$out_path")"

  "$CC" -m32 -nostdlib -nostartfiles \
    -Wl,-m,elf_i386 -Wl,-nostdlib -Wl,-e,_start -Wl,-T,"$ldscript" \
    -o "$out_path" "$obj_crt" "${objs[@]}" "$obj_pkgmeta" \
    -Wl,--start-group "$lib_archive" -lgcc -Wl,--end-group

  echo "Built $out_path"
}

extract_generated_build_files() {
  local build_sh="$1"
  awk '
    /^FILES="/ { in_files=1; sub(/^FILES="[[:space:]]*/, ""); }
    in_files {
      gsub(/"/, "");
      for (i=1; i<=NF; i++) if ($i ~ /\.c$/ && $i !~ /\*/) print $i;
      if ($0 ~ /"$/) in_files=0;
    }
  ' "$build_sh"
}

build_toybox_style_project() {
  local build_sh="$pkg_dir/generated/build.sh"

  if [ ! -f "$build_sh" ]; then
    if [ -f "$pkg_dir/Config.in" ] && [ ! -f "$pkg_dir/.config" ]; then
      make -C "$pkg_dir" defconfig >/dev/null
    fi

    if [ -f "$pkg_dir/scripts/make.sh" ]; then
      NOBUILD=1 make -C "$pkg_dir" toybox >/dev/null
    fi
  fi

  if [ ! -f "$build_sh" ]; then
    echo "error: expected generated build metadata at $build_sh" >&2
    return 1
  fi

  local sources=()
  local f

  for f in "$pkg_dir"/lib/*.c; do
    [ -f "$f" ] && sources+=("$f")
  done

  while IFS= read -r rel; do
    [ -n "$rel" ] || continue
    sources+=("$pkg_dir/$rel")
  done < <(extract_generated_build_files "$build_sh")

  build_from_source_list "$out" "${sources[@]}"
}

# Allow package-local override build script for custom build systems.
if [ -x "$pkg_dir/eyn-build.sh" ]; then
  export EYN_BUILD_CC="$CC"
  export EYN_BUILD_CFLAGS="${CFLAGS_BASE[*]}"
  export EYN_BUILD_PKG_NAME="$pkg_name"
  export EYN_BUILD_PKG_VERSION_INT="$pkg_version_int"
  export EYN_BUILD_OUT="$out"
  export EYN_BUILD_LDSCRIPT="$ldscript"
  export EYN_BUILD_CRT0="$crt0"
  export EYN_BUILD_USERLAND_INCLUDE="$incdir"
  exec "$pkg_dir/eyn-build.sh" "$pkg_dir" "$out"
fi

# Prefer project-style generated-source builds (for example toybox) before
# checking for legacy single-source fallback files.
if [ -f "$pkg_dir/generated/build.sh" ] || [ -f "$pkg_dir/scripts/make.sh" ] || [ -f "$pkg_dir/Config.in" ]; then
  build_toybox_style_project
  exit $?
fi

# If a package has a conventional single entry source, keep using the legacy builder.
if [ -f "$pkg_dir/${pkg_name}_uelf.c" ]; then
  exec env EYN_PKG_NAME="$pkg_name" EYN_PKG_VERSION_INT="$pkg_version_int" \
    bash "$repo_root/devtools/build_user_c.sh" "$pkg_dir/${pkg_name}_uelf.c" "$out"
fi

# If the caller passed a specific source file, compile it with the legacy single-source path.
if [ -f "$pkg_input" ] && [[ "$pkg_input" == *.c ]]; then
  exec env EYN_PKG_NAME="$pkg_name" EYN_PKG_VERSION_INT="$pkg_version_int" \
    bash "$repo_root/devtools/build_user_c.sh" "$pkg_input" "$out"
fi

echo "error: no recognized build model for package path: $pkg_dir" >&2
echo "hint: add executable eyn-build.sh to package root, or provide <name>_uelf.c" >&2
exit 1
