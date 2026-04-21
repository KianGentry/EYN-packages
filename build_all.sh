#!/bin/bash
# build_all.sh - Build all packages, publish to server, regenerate index/base artifacts
# Run from EYN-packages root

set -uo pipefail

BUILD_SCRIPT="./devtools/build_user_c.sh"
PACKAGES_DIR="./packages"
WWW_DIR="./www"
RELEASES_DIR="${WWW_DIR}/releases"
BASE_MANIFEST_FILE="${WWW_DIR}/base.manifest"
BASE_ARCHIVE_FILE="${WWW_DIR}/base.pkg"
SERVER_USER="server"
SERVER_WEB_ROOT="${EYN_PACKAGES_SERVER_WEB_ROOT:-/home/server/homeserver/www}"
SERVER_RELEASES="${SERVER_WEB_ROOT}/releases"
INDEX_FILE="./index.json"
SKIP_PUBLISH="${EYN_PACKAGES_SKIP_PUBLISH:-0}"
SERVER_HOST="${EYN_PACKAGES_SERVER_HOST:-eynos.duckdns.org}"
SERVER_SSH_HOST="${EYN_PACKAGES_SERVER_SSH_HOST:-192.168.1.200}"

normalize_system_flag() {
    local raw="${1:-}"
    raw="${raw//[[:space:]]/}"
    raw="${raw,,}"

    case "$raw" in
        ""|"1"|"true"|"yes"|"on"|"system")
            echo "true"
            ;;
        "0"|"false"|"no"|"off"|"user"|"non-system"|"nonsystem")
            echo "false"
            ;;
        *)
            echo "true"
            ;;
    esac
}

is_private_ipv4_literal() {
    local host="${1:-}"

    if [[ ! "$host" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
        return 1
    fi

    local o1=0 o2=0 o3=0 o4=0
    IFS='.' read -r o1 o2 o3 o4 <<< "$host"

    for octet in "$o1" "$o2" "$o3" "$o4"; do
        if ((octet < 0 || octet > 255)); then
            return 1
        fi
    done

    if ((o1 == 10)); then
        return 0
    fi
    if ((o1 == 172 && o2 >= 16 && o2 <= 31)); then
        return 0
    fi
    if ((o1 == 192 && o2 == 168)); then
        return 0
    fi
    if ((o1 == 127)); then
        return 0
    fi
    if ((o1 == 169 && o2 == 254)); then
        return 0
    fi

    return 1
}

parse_version_literal() {
    local raw="${1:-}"
    raw="${raw//[[:space:]]/}"

    if [[ "$raw" =~ ^([0-9]+) ]]; then
        echo "${BASH_REMATCH[1]}"
        return 0
    fi

    echo "0"
}

read_package_meta_flag() {
    local pkg_dir="$1"
    local flag="$2"
    local default="$3"
    local meta_file="${pkg_dir%/}/package.meta"

    if [ ! -f "$meta_file" ]; then
        echo "$default"
        return 0
    fi

    local line=""
    line="$(grep -Ei "^[[:space:]]*${flag}[[:space:]]*=" "$meta_file" | tail -n 1 || true)"
    if [ -z "$line" ]; then
        echo "$default"
        return 0
    fi

    local value="${line#*=}"
    value="${value//[[:space:]]/}"
    echo "$value"
}

read_package_system_flag() {
    local pkg_dir="$1"
    local raw
    raw="$(read_package_meta_flag "$pkg_dir" "SYSTEM" "")"
    normalize_system_flag "$raw"
}

read_package_prebuilt_flag() {
    local pkg_dir="$1"
    local raw
    raw="$(read_package_meta_flag "$pkg_dir" "PREBUILT" "false")"
    raw="${raw,,}"
    case "$raw" in
        "1"|"true"|"yes"|"on") echo "true" ;;
        *)                            echo "false" ;;
    esac
}

read_package_version_override() {
    local pkg_dir="$1"
    local raw
    raw="$(read_package_meta_flag "$pkg_dir" "VERSION" "")"
    raw="${raw,,}"

    if [ -z "$raw" ] || [ "$raw" = "auto" ]; then
        echo "0"
        return 0
    fi

    parse_version_literal "$raw"
}

tarball_has_payload() {
    local tar_path="$1"
    local pkg_name="$2"

    if [ ! -f "$tar_path" ]; then
        return 1
    fi

    if tar -tf "$tar_path" | grep -Eq "^(\./)?${pkg_name}$"; then
        return 0
    fi

    return 1
}

load_previous_versions() {
    local index_path="$1"

    if [ ! -f "$index_path" ]; then
        return 0
    fi

    python3 - "$index_path" <<'PY'
import json
import re
import sys

path = sys.argv[1]
try:
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
except Exception:
    sys.exit(0)

packages = data.get("packages", {})
if not isinstance(packages, dict):
    sys.exit(0)

def parse_version(v):
    if isinstance(v, int):
        return max(v, 0)
    if isinstance(v, str):
        m = re.match(r"\s*(\d+)", v)
        if m:
            return int(m.group(1))
    return 0

for name, pkg in packages.items():
    if not isinstance(pkg, dict):
        continue

    latest = parse_version(pkg.get("latest", 0))
    versions = pkg.get("versions", {})
    max_seen = latest

    if isinstance(versions, dict):
        for ver_key, ver_meta in versions.items():
            max_seen = max(max_seen, parse_version(ver_key))
            if isinstance(ver_meta, dict):
                max_seen = max(max_seen, parse_version(ver_meta.get("version", 0)))

    print(f"{name}\t{max_seen}")
PY
}

append_index_entry() {
    local pkgname="$1"
    local pkg_version="$2"
    local tarball="$3"
    local sha="$4"
    local system_flag="$5"

    pkg_version="$(parse_version_literal "$pkg_version")"
    if [ "$pkg_version" -lt 1 ]; then
        pkg_version=1
    fi

    if [ $first_pkg -eq 0 ]; then
        index+=','
    fi
    first_pkg=0

    index+="\"${pkgname}\": {"
    index+="\"description\": \"${pkgname}\"," 
    index+="\"system\": ${system_flag},"
    index+="\"versions\": {"
    index+="\"${pkg_version}\": {"
    index+="\"url\": \"https://${SERVER_HOST}/releases/${tarball}\"," 
    index+="\"sha256\": \"${sha}\"," 
    index+="\"deps\": []"
    index+="}},"
    index+="\"latest\": \"${pkg_version}\""
    index+="}"
}

if [ ! -f "$BUILD_SCRIPT" ]; then
    echo "error: could not find build script at ${BUILD_SCRIPT}"
    exit 1
fi

if is_private_ipv4_literal "$SERVER_HOST"; then
    if [ "${EYN_PACKAGES_ALLOW_PRIVATE_SERVER_HOST:-0}" != "1" ]; then
        echo "error: SERVER_HOST=${SERVER_HOST} is a private/local IPv4 literal"
        echo "       This would embed private URLs in index.json and break clients off-LAN."
        echo "       Use EYN_PACKAGES_SERVER_HOST=<public host> or set"
        echo "       EYN_PACKAGES_ALLOW_PRIVATE_SERVER_HOST=1 to override intentionally."
        exit 1
    fi
    echo "warning: embedding private SERVER_HOST=${SERVER_HOST} into package URLs (override enabled)"
fi

mkdir -p "$RELEASES_DIR"
mkdir -p "$WWW_DIR"

index='{"packages": {'
first_pkg=1
declare -a system_package_names
declare -a system_tarballs

declare -A previous_versions
while IFS=$'\t' read -r prev_name prev_ver; do
    if [ -z "$prev_name" ]; then
        continue
    fi
    previous_versions["$prev_name"]="$prev_ver"
done < <(load_previous_versions "$INDEX_FILE")

for pkg_dir in "$PACKAGES_DIR"/*/; do
    pkgname="$(basename "$pkg_dir")"
    src="${pkg_dir}${pkgname}_uelf.c"
    prebuilt_bin="${pkg_dir}${pkgname}"
    pkg_system="$(read_package_system_flag "$pkg_dir")"
    pkg_prebuilt="$(read_package_prebuilt_flag "$pkg_dir")"
    pkg_version_override="$(read_package_version_override "$pkg_dir")"

    prev_version="${previous_versions[$pkgname]:-0}"
    prev_version="$(parse_version_literal "$prev_version")"

    selected_version=0
    selected_tarball=""

    if [ "$pkg_prebuilt" = "true" ]; then
        if [ ! -f "$prebuilt_bin" ]; then
            echo "warning: PREBUILT=true for ${pkgname} but no binary found at ${prebuilt_bin}, skipping"
            continue
        fi

        if [ "$prev_version" -gt 0 ]; then
            selected_version="$prev_version"
            selected_tarball="${pkgname}-${selected_version}-i386.tar.gz"
            if [ -f "${RELEASES_DIR}/${selected_tarball}" ] \
                && [ "${RELEASES_DIR}/${selected_tarball}" -nt "$prebuilt_bin" ] \
                && tarball_has_payload "${RELEASES_DIR}/${selected_tarball}" "${pkgname}"; then
                echo "  skipping ${pkgname} (prebuilt, unchanged @ v${selected_version})"
                sha="$(sha256sum "${RELEASES_DIR}/${selected_tarball}" | awk '{print $1}')"
                append_index_entry "$pkgname" "$selected_version" "$selected_tarball" "$sha" "$pkg_system"
                if [ "$pkg_system" = "true" ]; then
                    system_package_names+=("$pkgname")
                    system_tarballs+=("$selected_tarball")
                fi
                continue
            fi
        fi

        selected_version=$((prev_version + 1))
        if [ "$pkg_version_override" -gt "$selected_version" ]; then
            selected_version="$pkg_version_override"
        fi
        selected_tarball="${pkgname}-${selected_version}-i386.tar.gz"

        echo "Packaging prebuilt ${pkgname} (v${selected_version})..."

        staging="/tmp/eynpkg_${pkgname}"
        rm -rf "$staging"
        mkdir -p "$staging"
        cp "$prebuilt_bin" "${staging}/${pkgname}"

        tar -czf "${RELEASES_DIR}/${selected_tarball}" -C "$staging" .
        rm -rf "$staging"

        if ! tarball_has_payload "${RELEASES_DIR}/${selected_tarball}" "${pkgname}"; then
            echo "error: tarball for prebuilt ${pkgname} has no installable payload, skipping"
            rm -f "${RELEASES_DIR}/${selected_tarball}"
            continue
        fi

        sha="$(sha256sum "${RELEASES_DIR}/${selected_tarball}" | awk '{print $1}')"
        append_index_entry "$pkgname" "$selected_version" "$selected_tarball" "$sha" "$pkg_system"

        if [ "$pkg_system" = "true" ]; then
            system_package_names+=("$pkgname")
            system_tarballs+=("$selected_tarball")
        fi

        echo "  packaged: ${selected_tarball} [${sha}]"
        continue
    fi

    if [ ! -f "$src" ]; then
        echo "warning: no source found for ${pkgname}, skipping"
        continue
    fi

    if [ "$prev_version" -gt 0 ]; then
        selected_version="$prev_version"
        selected_tarball="${pkgname}-${selected_version}-i386.tar.gz"
        if [ -f "${RELEASES_DIR}/${selected_tarball}" ] \
            && [ "${RELEASES_DIR}/${selected_tarball}" -nt "$src" ] \
            && tarball_has_payload "${RELEASES_DIR}/${selected_tarball}" "${pkgname}"; then
            echo "  skipping ${pkgname} (unchanged @ v${selected_version})"
            sha="$(sha256sum "${RELEASES_DIR}/${selected_tarball}" | awk '{print $1}')"
            append_index_entry "$pkgname" "$selected_version" "$selected_tarball" "$sha" "$pkg_system"

            if [ "$pkg_system" = "true" ]; then
                system_package_names+=("$pkgname")
                system_tarballs+=("$selected_tarball")
            fi
            continue
        fi
    fi

    selected_version=$((prev_version + 1))
    if [ "$pkg_version_override" -gt "$selected_version" ]; then
        selected_version="$pkg_version_override"
    fi
    selected_tarball="${pkgname}-${selected_version}-i386.tar.gz"

    echo "Building ${pkgname} (v${selected_version})..."

    staging="/tmp/eynpkg_${pkgname}"
    rm -rf "$staging"
    mkdir -p "$staging"

    abs_src="$(realpath "$src")"
    abs_out="$(realpath "$staging")/${pkgname}"

    if ! EYN_PKG_NAME="$pkgname" EYN_PKG_VERSION_INT="$selected_version" bash "$BUILD_SCRIPT" "$abs_src" "$abs_out"; then
        echo "error: build failed for ${pkgname}, skipping"
        rm -rf "$staging"
        continue
    fi

    if [ ! -f "$abs_out" ]; then
        echo "error: build output missing for ${pkgname}, skipping"
        rm -rf "$staging"
        continue
    fi

    tar -czf "${RELEASES_DIR}/${selected_tarball}" -C "$staging" .
    rm -rf "$staging"

    if ! tarball_has_payload "${RELEASES_DIR}/${selected_tarball}" "${pkgname}"; then
        echo "error: tarball for ${pkgname} has no installable payload, skipping"
        rm -f "${RELEASES_DIR}/${selected_tarball}"
        continue
    fi

    sha="$(sha256sum "${RELEASES_DIR}/${selected_tarball}" | awk '{print $1}')"
    append_index_entry "$pkgname" "$selected_version" "$selected_tarball" "$sha" "$pkg_system"

    if [ "$pkg_system" = "true" ]; then
        system_package_names+=("$pkgname")
        system_tarballs+=("$selected_tarball")
    fi

    echo "  built: ${selected_tarball} [${sha}]"
done

index+="}}"
echo "$index" > "$INDEX_FILE"
echo "index.json regenerated."

{
    echo "# EYN-OS base package manifest v1"
    for pkgname in "${system_package_names[@]}"; do
        echo "$pkgname"
    done
} > "$BASE_MANIFEST_FILE"

base_stage="$(mktemp -d /tmp/eyn_base_pkg_XXXXXX)"
trap 'rm -rf "$base_stage"' EXIT

for tarball in "${system_tarballs[@]}"; do
    cp "${RELEASES_DIR}/${tarball}" "$base_stage/"
done

tar -czf "$BASE_ARCHIVE_FILE" -C "$base_stage" .
rm -rf "$base_stage"
trap - EXIT

echo "base manifest regenerated: ${BASE_MANIFEST_FILE}"
echo "base archive regenerated: ${BASE_ARCHIVE_FILE}"

if [ "$SKIP_PUBLISH" = "1" ]; then
    echo "Skipping upload/push (EYN_PACKAGES_SKIP_PUBLISH=1)"
    exit 0
fi

echo "Uploading releases to server..."
scp "${RELEASES_DIR}"/*.tar.gz "${SERVER_USER}@${SERVER_SSH_HOST}:${SERVER_RELEASES}/"

echo "Uploading index/base artifacts to server..."
scp "$INDEX_FILE" "${SERVER_USER}@${SERVER_SSH_HOST}:${SERVER_WEB_ROOT}/index.json"
scp "$BASE_MANIFEST_FILE" "${SERVER_USER}@${SERVER_SSH_HOST}:${SERVER_WEB_ROOT}/base.manifest"
scp "$BASE_ARCHIVE_FILE" "${SERVER_USER}@${SERVER_SSH_HOST}:${SERVER_WEB_ROOT}/base.pkg"

echo "Pushing index..."
git add index.json
if git diff --cached --quiet; then
    echo "index.json unchanged; nothing to commit"
else
    git commit -m "rebuild package index"
    git push server main
fi

echo "All done."
