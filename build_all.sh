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
SERVER_IP="eynos.duckdns.org"
SERVER_RELEASES="/home/server/homeserver/www/releases"
INDEX_FILE="./index.json"
VERSION="1.0.0"   # bump this manually or extend the script to read per-package versioning later
SKIP_PUBLISH="${EYN_PACKAGES_SKIP_PUBLISH:-0}"

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

read_package_system_flag() {
    local pkg_dir="$1"
    local meta_file="${pkg_dir%/}/package.meta"

    if [ ! -f "$meta_file" ]; then
        echo "true"
        return 0
    fi

    local line=""
    line="$(grep -Ei '^[[:space:]]*SYSTEM[[:space:]]*=' "$meta_file" | tail -n 1 || true)"
    if [ -z "$line" ]; then
        echo "true"
        return 0
    fi

    local value="${line#*=}"
    normalize_system_flag "$value"
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

append_index_entry() {
    local pkgname="$1"
    local tarball="$2"
    local sha="$3"
    local system_flag="$4"

    if [ $first_pkg -eq 0 ]; then
        index+=','
    fi
    first_pkg=0

    index+="\"${pkgname}\": {"
    index+="\"description\": \"${pkgname}\","
    index+="\"system\": ${system_flag},"
    index+="\"versions\": {"
    index+="\"${VERSION}\": {"
    index+="\"url\": \"http://${SERVER_IP}/releases/${tarball}\","
    index+="\"sha256\": \"${sha}\","
    index+="\"deps\": []"
    index+="}},"
    index+="\"latest\": \"${VERSION}\""
    index+="}"
}

if [ ! -f "$BUILD_SCRIPT" ]; then
    echo "error: could not find build script at ${BUILD_SCRIPT}"
    exit 1
fi

mkdir -p "$RELEASES_DIR"
mkdir -p "$WWW_DIR"

index='{"packages": {'
first_pkg=1
declare -a system_package_names
declare -a system_tarballs

for pkg_dir in "$PACKAGES_DIR"/*/; do
    pkgname=$(basename "$pkg_dir")
    src="${pkg_dir}${pkgname}_uelf.c"

    if [ ! -f "$src" ]; then
        echo "warning: no source found for ${pkgname}, skipping"
        continue
    fi

    pkg_system="$(read_package_system_flag "$pkg_dir")"
    tarball="${pkgname}-${VERSION}-i386.tar.gz"

    if [ -f "${RELEASES_DIR}/${tarball}" ] \
        && [ "${RELEASES_DIR}/${tarball}" -nt "$src" ] \
        && tarball_has_payload "${RELEASES_DIR}/${tarball}" "${pkgname}"; then
        echo "  skipping ${pkgname} (unchanged)"
        sha=$(sha256sum "${RELEASES_DIR}/${tarball}" | awk '{print $1}')

        append_index_entry "$pkgname" "$tarball" "$sha" "$pkg_system"

        if [ "$pkg_system" = "true" ]; then
            system_package_names+=("$pkgname")
            system_tarballs+=("$tarball")
        fi
        continue
    fi

    if [ -f "${RELEASES_DIR}/${tarball}" ] \
        && [ "${RELEASES_DIR}/${tarball}" -nt "$src" ]; then
        echo "  rebuilding ${pkgname} (existing tarball missing expected payload)"
    fi

    echo "Building ${pkgname}..."

    staging="/tmp/eynpkg_${pkgname}"
    rm -rf "$staging"
    mkdir -p "$staging"

    abs_src="$(realpath "$src")"
    abs_out="$(realpath "$staging")/${pkgname}"

    if ! bash "$BUILD_SCRIPT" "$abs_src" "$abs_out"; then
        echo "error: build failed for ${pkgname}, skipping"
        rm -rf "$staging"
        continue
    fi

    if [ ! -f "$abs_out" ]; then
        echo "error: build output missing for ${pkgname}, skipping"
        rm -rf "$staging"
        continue
    fi

    tar -czf "${RELEASES_DIR}/${tarball}" -C "$staging" .
    rm -rf "$staging"

    if ! tarball_has_payload "${RELEASES_DIR}/${tarball}" "${pkgname}"; then
        echo "error: tarball for ${pkgname} has no installable payload, skipping"
        rm -f "${RELEASES_DIR}/${tarball}"
        continue
    fi

    sha=$(sha256sum "${RELEASES_DIR}/${tarball}" | awk '{print $1}')
    append_index_entry "$pkgname" "$tarball" "$sha" "$pkg_system"

    if [ "$pkg_system" = "true" ]; then
        system_package_names+=("$pkgname")
        system_tarballs+=("$tarball")
    fi

    echo "  built: ${tarball} [${sha}]"
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
scp ${RELEASES_DIR}/*.tar.gz ${SERVER_USER}@${SERVER_IP}:${SERVER_RELEASES}/

echo "Pushing index..."
git add index.json
git commit -m "rebuild all packages ${VERSION}"
git push server main

echo "All done."
