#!/bin/bash
# build_all.sh - Build all packages, publish to server, regenerate index.json
# Run from EYN-packages root

OS_ROOT="../"
BUILD_SCRIPT="${OS_ROOT}devtools/build_user_c.sh"
PACKAGES_DIR="./packages"
RELEASES_DIR="./www/releases"
SERVER_USER="server"
SERVER_IP="eynos.duckdns.org"
SERVER_RELEASES="/home/server/homeserver/www/releases"
INDEX_FILE="./index.json"
VERSION="1.0.0"   # bump this manually or extend the script to read per-package versioning later

if [ ! -f "$BUILD_SCRIPT" ]; then
    echo "error: could not find build script at ${BUILD_SCRIPT}"
    exit 1
fi

mkdir -p "$RELEASES_DIR"

# Start building index.json
index='{"packages": {'
first_pkg=1

for pkg_dir in "$PACKAGES_DIR"/*/; do
    pkgname=$(basename "$pkg_dir")
    src="${pkg_dir}${pkgname}_uelf.c"

    if [ ! -f "$src" ]; then
        echo "warning: no source found for ${pkgname}, skipping"
        continue
    fi

    tarball="${pkgname}-${VERSION}-i386.tar.gz"

    if [ -f "${RELEASES_DIR}/${tarball}" ] && [ "${RELEASES_DIR}/${tarball}" -nt "$src" ]; then
        echo "  skipping ${pkgname} (unchanged)"
        # Still need to include it in the index, so grab existing sha
        sha=$(sha256sum "${RELEASES_DIR}/${tarball}" | awk '{print $1}')

        if [ $first_pkg -eq 0 ]; then
            index+=','
        fi
        first_pkg=0

        index+="\"${pkgname}\": {"
        index+="\"description\": \"${pkgname}\","
        index+="\"versions\": {"
        index+="\"${VERSION}\": {"
        index+="\"url\": \"http://${SERVER_IP}/releases/${tarball}\","
        index+="\"sha256\": \"${sha}\","
        index+="\"deps\": []"
        index+="}},"
        index+="\"latest\": \"${VERSION}\""
        index+="}"
        continue
    fi

    echo "Building ${pkgname}..."

    # Build binary into a temp staging dir
    staging="/tmp/eynpkg_${pkgname}"
    mkdir -p "$staging"

    abs_src="$(realpath "$src")"
    abs_out="$(realpath "$staging")/${pkgname}"

    bash "$BUILD_SCRIPT" "$abs_src" -o "$abs_out"
    if [ $? -ne 0 ]; then
        echo "error: build failed for ${pkgname}, skipping"
        rm -rf "$staging"
        continue
    fi

    # Package into tarball
    # tarball="${pkgname}-${VERSION}-i386.tar.gz"
    tar -czf "${RELEASES_DIR}/${tarball}" -C "$staging" .
    rm -rf "$staging"

    # SHA256
    sha=$(sha256sum "${RELEASES_DIR}/${tarball}" | awk '{print $1}')

    # Append to index
    if [ $first_pkg -eq 0 ]; then
        index+=','
    fi
    first_pkg=0

    index+="\"${pkgname}\": {"
    index+="\"description\": \"${pkgname}\","
    index+="\"versions\": {"
    index+="\"${VERSION}\": {"
    index+="\"url\": \"http://${SERVER_IP}/releases/${tarball}\","
    index+="\"sha256\": \"${sha}\","
    index+="\"deps\": []"
    index+="}},"
    index+="\"latest\": \"${VERSION}\""
    index+="}"

    echo "  built: ${tarball} [${sha}]"
done

index+="}}"

# Write index
echo "$index" > "$INDEX_FILE"
echo "index.json regenerated."

# SCP releases to server
echo "Uploading releases to server..."
scp ${RELEASES_DIR}/*.tar.gz ${SERVER_USER}@${SERVER_IP}:${SERVER_RELEASES}/
if [ $? -ne 0 ]; then
    echo "error: SCP failed, check server connectivity"
    exit 1
fi

# Push index via git
echo "Pushing index..."
git add index.json
git commit -m "rebuild all packages ${VERSION}"
git push server master

echo "All done."
