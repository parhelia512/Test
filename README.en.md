# Clash-Flux

<img src="resources/images/clash_flux_logo.png" width="112" alt="Clash-Flux Logo">

[简体中文](README.md) | English

Clash-Flux is a mobile-only proxy client built with C++23 and HuxerUI.
Android runs sing-box libbox in a dedicated background process, with TUN provided by
`VpnService`; the UI process owns subscription storage, configuration compilation and
presentation. The iOS App and Packet Tunnel remain experimental Simulator integrations,
without a promise of device support or an IPA release.
The core is pinned to [Leadaxe/sing-box-lx](https://github.com/Leadaxe/sing-box-lx)
`v1.14.4-lx.33`, commit `2d9a0a52df7881f55484d48f9daecb21d135f3e9`;
Android and experimental iOS libbox builds use this same source.

Windows, Linux and macOS desktop sources, entry points, installers, services and demos
have been removed. Desktop CLI, tray, system-proxy configuration, SCM/systemd services,
external core processes and `pppd`/RAS implementations are no longer included. Existing
configuration data remains readable where it is shared with mobile models, but those
desktop capabilities are unavailable.

## Features

- Remote and local Clash YAML or sing-box JSON profiles, with refresh, activation,
  editing, deletion, QR sharing, camera and gallery import, plus fidelity reporting.
- Traffic, outbound mode, selected profile and LAN address cards. A mobile floating
  button controls the system VPN, with Binder-backed state, foreground-service recovery
  and battery-optimization settings on Android.
- Proxy groups and nodes, latency tests, connections and logs, rules and resource
  management, and runtime diagnostics.
- OpenVPN `.ovpn` files are imported as ordinary profiles and compiled into sing-box
  endpoints; desktop-specific standalone sessions are not created.
- Android arm64 update downloads with digest, release-signature and version checks,
  followed by system-confirmed installation.
- Light, dark and system themes, custom colors, mobile navigation, secondary pages
  and safe-area-aware layouts.

## Requirements

- CMake 4.4 (baseline 4.4.2) and Ninja.
- The pinned HuxerUI source revision or a matching mobile SDK.
- Android: JDK 17, Android SDK/NDK r28, Go 1.26.10 and an arm64 libbox AAR built by the script.
- iOS: macOS, Xcode, LLVM 19, Go and iPhoneOS/Simulator SDKs.
- Reproducible dependencies built through CPS; see [dependency management](docs/cps-dependencies.md).

`third_party/huxerui` is a fetched dependency, not application source. Fetch its pinned
revision with:

```bash
curl -fL https://codeload.github.com/FarnaHerry/HuxerUI/tar.gz/69f24079bafa4f19d31e387e66a6b9bac8f3776f \
  -o /tmp/huxerui.tar.gz
tar -xzf /tmp/huxerui.tar.gz -C third_party
mv third_party/HuxerUI-69f24079bafa4f19d31e387e66a6b9bac8f3776f third_party/huxerui
```

## Android

Build libbox and its version/revision/ABI/digest metadata:

```bash
platform/android/build-singbox-libbox.sh /absolute/path/libbox-arm64.aar
export CLASHFLUX_SINGBOX_AAR=/absolute/path/libbox-arm64.aar
export HUXERUI_HOME=/path/to/huxerui-sdk
huxerui build android --source third_party/huxerui --profile debug
```

Gradle can also be invoked from `platform/android`. `stageSingboxAar` requires the AAR
and its matching `.metadata.json`; `stageBundledRuleSets` is mandatory. Production APKs
require the long-term release key and both v1/v2 signatures. See [Android build](docs/android-build.md).

For a standalone native check:

```bash
cmake -S . -B build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/ndk/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
  -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Debug \
  -DHUXERUI_HOME="$PWD/third_party/huxerui"
cmake --build build-android --target clash-flux
```

The top-level CMake project accepts only Android or iOS configurations. iOS scripts and
limitations are documented in [iOS build](docs/ios-build.md). CI builds unsigned Simulator
binaries only and publishes no iOS release assets.

## Layout

- `platform/android`: app, background VPN service, AIDL/JNI, Gradle and libbox scripts.
- `platform/ios`: experimental App, Packet Tunnel and Xcode integration.
- `src`: mobile UI, subscription compiler, models and shared business logic.
- `tests/android`: Android host-JVM policy regressions.
- `cmake`, `third_party`: native build and pinned dependencies.
- `docs`: design, build and historical documents. Desktop-specific documents are
  historical references, not a statement of current platform support.

See the [v0.3.100 release notes](docs/releases/v0.3.100.md). Development continues.
