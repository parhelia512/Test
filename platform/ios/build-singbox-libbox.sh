#!/usr/bin/env bash
set -euo pipefail

# Build the same sing-box-lx source as Android into an Apple Libbox.xcframework.
# The Xcode app and Packet Tunnel consume it in a later integration step.
readonly repo='https://github.com/Leadaxe/sing-box-lx.git'
readonly version="${SINGBOX_VERSION:-1.14.4-lx.33}"
readonly revision="${SINGBOX_COMMIT:-2d9a0a52df7881f55484d48f9daecb21d135f3e9}" # v1.14.4-lx.33
readonly output_arg="${1:?usage: build-singbox-libbox.sh OUTPUT_XCFRAMEWORK}"
readonly script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "$script_dir/../singbox-libbox-build.conf"

for command in go git make xcodebuild xcrun ditto; do
    command -v "$command" >/dev/null || {
        echo "$command is required to build the Apple Libbox framework." >&2
        exit 1
    }
done

xcrun --sdk iphoneos --show-sdk-path >/dev/null
xcrun --sdk iphonesimulator --show-sdk-path >/dev/null

output_parent="$(dirname "$output_arg")"
mkdir -p "$output_parent"
output_parent="$(cd "$output_parent" && pwd)"
readonly output="$output_parent/$(basename "$output_arg")"
if [[ -e "$output" ]]; then
    echo "Output already exists: $output" >&2
    exit 1
fi

readonly work="$(mktemp -d "${TMPDIR:-/tmp}/clash-flux-singbox-apple.XXXXXX")"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT

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
gomobile bind -v -target ios,iossimulator -libname=box \
    -tags-not-macos=with_low_memory -iosversion=15.0 -macosversion=13.0 -tvosversion=17.0 \
    -trimpath -buildvcs=false -ldflags "$libbox_linker_flags" \
    -tags "$libbox_build_tags,with_dhcp,grpcnotrace" ./experimental/libbox
popd >/dev/null

framework="$(find "$work/source" -maxdepth 1 -type d -name '*.xcframework' -print -quit)"
if [[ -z "$framework" ]]; then
    echo 'sing-box did not produce an Apple XCFramework.' >&2
    exit 1
fi
ditto "$framework" "$output"
test -s "$output/Info.plist"
echo "Built pinned sing-box Libbox XCFramework: $output"
