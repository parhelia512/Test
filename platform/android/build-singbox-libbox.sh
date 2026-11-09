#!/usr/bin/env bash
set -euo pipefail

# Build libbox from the pinned sing-box-lx source, never from a third-party APK.
readonly repo='https://github.com/Leadaxe/sing-box-lx.git'
readonly version="${SINGBOX_VERSION:-1.14.4-lx.33}"
readonly revision="${SINGBOX_COMMIT:-2d9a0a52df7881f55484d48f9daecb21d135f3e9}" # v1.14.4-lx.33
readonly output="$(realpath -m "${1:?usage: build-singbox-libbox.sh OUTPUT_AAR}")"
readonly script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "$script_dir/../singbox-libbox-build.conf"

command -v go >/dev/null || { echo 'Go is required.' >&2; exit 1; }
test -n "${ANDROID_NDK_HOME:-}" || { echo 'ANDROID_NDK_HOME is required.' >&2; exit 1; }
test -f "$ANDROID_NDK_HOME/source.properties" || { echo 'ANDROID_NDK_HOME must contain an installed NDK.' >&2; exit 1; }
test -n "${ANDROID_HOME:-}" && test -f "$ANDROID_HOME/licenses/android-sdk-license" || {
    echo 'ANDROID_HOME must contain a licensed Android SDK.' >&2; exit 1;
}
export ANDROID_SDK_HOME="$ANDROID_HOME"
export NDK="$ANDROID_NDK_HOME"
test -n "${JAVA_HOME:-}" || { echo 'JAVA_HOME (JDK 17) is required.' >&2; exit 1; }
java_version="$("$JAVA_HOME/bin/java" --version)"
[[ "$java_version" == *"openjdk 17"* ]] || {
    echo 'Upstream libbox requires OpenJDK 17; choose a matching JAVA_HOME.' >&2
    exit 1
}

readonly work="$(mktemp -d "${TMPDIR:-/tmp}/clash-flux-singbox.XXXXXX")"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT
mkdir -p "$(dirname "$(realpath -m "$output")")"
# Fetch only the pinned commit: a full history clone can stall on large upstream
# repositories and is unnecessary for reproducible libbox builds.
git init -q "$work/source"
git -C "$work/source" remote add origin "$repo"
git -C "$work/source" -c http.lowSpeedLimit=1024 -c http.lowSpeedTime=60 \
    fetch --depth=1 origin "refs/tags/v${version}:refs/tags/v${version}"
git -C "$work/source" checkout --detach "v${version}"
test "$(git -C "$work/source" rev-parse HEAD)" = "$revision"
test "$(git -C "$work/source" describe --tags --exact-match HEAD)" = "v${version}"
git -C "$work/source" submodule update --init --recursive --depth=1 -- \
    submodules/wireguard-go submodules/sing-tun submodules/gvisor submodules/utls
pushd "$work/source" >/dev/null
make lib_install
export PATH="$(go env GOPATH)/bin:$PATH"
gomobile bind -v -o libbox.aar -target android/arm64 -androidapi 24 \
    -javapkg=io.nekohasekai -libname=box -trimpath -buildvcs=false \
    -ldflags "$libbox_linker_flags" -tags "$libbox_build_tags" ./experimental/libbox
test -s libbox.aar
mv libbox.aar "$output"
popd >/dev/null

# Gradle must identify cached AARs by source revision and content, not by their
# filename alone. This sidecar travels with CLASHFLUX_SINGBOX_AAR in CI/local builds.
readonly aar_sha256="$(sha256sum "$output" | cut -d ' ' -f 1)"
printf '{"version":"%s","revision":"%s","sha256":"%s","abi":"arm64-v8a"}\n' \
    "$version" "$revision" "$aar_sha256" > "$output.metadata.json.tmp"
mv "$output.metadata.json.tmp" "$output.metadata.json"
