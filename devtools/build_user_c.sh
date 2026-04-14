#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

src="${1:-packages/hello/hello_uelf.c}"
out="${2:-build/hello.uelf}"

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

rm -f "$lib_archive"
ar rcs "$lib_archive" "$obj_libc_x11" "$obj_libc_setjmp" "$obj_libc_stat" "$obj_libc_ctype" "$obj_libc_libgen" "$obj_libc_unistd" "$obj_libc_string" "$obj_libc_stdio" "$obj_libc_fcntl" "$obj_libc_dirent" "$obj_libc_gui" "$obj_libc_time" "$obj_libc_stdlib" "$obj_libc_errno"

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
"$CC" -m32 -nostdlib -nostartfiles -Wl,-m,elf_i386 -Wl,-nostdlib -Wl,-e,_start -Wl,-T,"$ldscript" -o "$out" "$obj_crt" "$obj_app" "${extra_objs[@]}" -Wl,--start-group "$lib_archive" -lgcc -Wl,--end-group

echo "Built $out"
