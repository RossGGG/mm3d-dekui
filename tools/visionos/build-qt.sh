#!/bin/sh
set -eu

QT_VERSION=${QT_VERSION:-6.10.3}
TARGET=${TARGET:-simulator}
# full: Core, Gui, Widgets, ShaderTools and Multimedia for the Qt frontend.
# core: Qt Core only, which is all the native SwiftUI app needs.
PROFILE=${PROFILE:-full}
# macOS deletes /tmp files that have not been used for three days, which
# silently breaks a Qt installation there. Default to a persistent location.
QT_ROOT=${QT_ROOT:-${HOME}/Library/Developer/Dekui/qt}
case "${PROFILE}" in
    full|core) ;;
    *)
        echo "PROFILE must be 'full' or 'core'" >&2
        exit 2
        ;;
esac
XPLATFORM=macx-visionos-clang
DEPLOYMENT_TARGET=2.0
case "${TARGET}" in
    simulator)
        SDK_NAME=xrsimulator
        RELEASE_SUFFIX=Release-xrsimulator
        DEFAULT_TARGET_PREFIX=${QT_ROOT}/visionos-sim-${PROFILE}-${QT_VERSION}
        ;;
    device)
        SDK_NAME=xros
        RELEASE_SUFFIX=Release-xros
        DEFAULT_TARGET_PREFIX=${QT_ROOT}/visionos-device-${PROFILE}-${QT_VERSION}
        ;;
    # Qt 6 has no tvOS support; qtbase-6.10.3-tvos.patch adds enough of it for
    # Qt Core, which is all the Apple TV app uses.
    tvos-simulator)
        SDK_NAME=appletvsimulator
        RELEASE_SUFFIX=Release-appletvsimulator
        DEFAULT_TARGET_PREFIX=${QT_ROOT}/tvos-sim-${PROFILE}-${QT_VERSION}
        ;;
    tvos-device)
        SDK_NAME=appletvos
        RELEASE_SUFFIX=Release-appletvos
        DEFAULT_TARGET_PREFIX=${QT_ROOT}/tvos-device-${PROFILE}-${QT_VERSION}
        ;;
    *)
        echo "TARGET must be 'simulator', 'device', 'tvos-simulator' or 'tvos-device'" >&2
        exit 2
        ;;
esac
case "${TARGET}" in
    tvos-*)
        if [ "${PROFILE}" != core ]; then
            echo "tvOS builds support PROFILE=core only" >&2
            exit 2
        fi
        XPLATFORM=macx-tvos-clang
        DEPLOYMENT_TARGET=17.0
        # The permission plugins need Contacts and EventKit, and QtTest needs
        # fork(); tvOS has neither.
        PLATFORM_FEATURES="-no-feature-permissions -no-feature-testlib"
        ;;
esac

WORK_ROOT=${WORK_ROOT:-${QT_ROOT}/work-${TARGET}-${PROFILE}}
HOST_PREFIX=${HOST_PREFIX:-${QT_ROOT}/host-${PROFILE}-${QT_VERSION}}
TARGET_PREFIX=${TARGET_PREFIX:-${DEFAULT_TARGET_PREFIX}}
PARALLEL=${PARALLEL:-8}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SOURCE_ROOT=${WORK_ROOT}/src
HOST_BUILD=${WORK_ROOT}/host-qtbase-build
HOST_SHADER_BUILD=${WORK_ROOT}/host-qtshadertools-build
TARGET_BASE_BUILD=${WORK_ROOT}/visionos-qtbase-build
TARGET_SHADER_BUILD=${WORK_ROOT}/visionos-qtshadertools-build
TARGET_MULTIMEDIA_BUILD=${WORK_ROOT}/visionos-qtmultimedia-build
SDK_PATH=$(/usr/bin/xcrun --sdk "${SDK_NAME}" --show-sdk-path)

clone_module() {
    module=$1
    if [ ! -d "${SOURCE_ROOT}/${module}/.git" ]; then
        git clone --depth 1 --branch "v${QT_VERSION}" \
            "https://github.com/qt/${module}.git" "${SOURCE_ROOT}/${module}"
    fi
}

apply_once() {
    source_dir=$1
    patch_file=$2
    if git -C "${source_dir}" apply --check "${patch_file}" 2>/dev/null; then
        git -C "${source_dir}" apply "${patch_file}"
    elif ! git -C "${source_dir}" apply --reverse --check "${patch_file}" 2>/dev/null; then
        echo "Patch does not apply cleanly: ${patch_file}" >&2
        exit 1
    fi
}

normalize_xcode_release_dirs() {
    build_dir=$1
    if [ -d "${build_dir}/build" ]; then
        find "${build_dir}/build" -type d -name "${RELEASE_SUFFIX}" -print |
            while IFS= read -r dir; do
                parent=${dir%/*}
                [ -e "${parent}/Release" ] || ln -s "${RELEASE_SUFFIX}" "${parent}/Release"
            done
    fi
}

mkdir -p "${SOURCE_ROOT}" "${HOST_BUILD}" "${HOST_SHADER_BUILD}" \
    "${TARGET_BASE_BUILD}" "${TARGET_SHADER_BUILD}" "${TARGET_MULTIMEDIA_BUILD}"

clone_module qtbase
apply_once "${SOURCE_ROOT}/qtbase" "${SCRIPT_DIR}/qtbase-6.10.3-xcode26.patch"
if [ "${XPLATFORM}" = macx-tvos-clang ]; then
    apply_once "${SOURCE_ROOT}/qtbase" "${SCRIPT_DIR}/qtbase-6.10.3-tvos.patch"
fi
if [ "${PROFILE}" = full ]; then
    clone_module qtshadertools
    clone_module qtmultimedia
    apply_once "${SOURCE_ROOT}/qtmultimedia" "${SCRIPT_DIR}/qtmultimedia-6.10.3-xcode26.patch"
    GUI_FEATURES=
else
    GUI_FEATURES="-no-gui -no-widgets -no-feature-network ${PLATFORM_FEATURES:-}"
fi

if [ ! -f "${HOST_BUILD}/CMakeCache.txt" ]; then
    (cd "${HOST_BUILD}" && "${SOURCE_ROOT}/qtbase/configure" \
        -prefix "${HOST_PREFIX}" -release -nomake examples -nomake tests \
        -no-feature-dbus -no-feature-cups -no-feature-printsupport \
        -no-feature-sql -no-feature-opengl -no-feature-vulkan \
        -no-feature-icu ${GUI_FEATURES} -- -DCMAKE_BUILD_TYPE=Release)
fi
cmake --build "${HOST_BUILD}" --parallel "${PARALLEL}"
cmake --install "${HOST_BUILD}"

# Qt 6.10's configure probe expects Xcode's Release directory, while Xcode 26
# emits a platform-suffixed Release directory. The first configure pass creates
# the probe project;
# the symlink lets the direct CMake configure consume its result.
if [ ! -f "${TARGET_BASE_BUILD}/CMakeCache.txt" ]; then
    set +e
    (cd "${TARGET_BASE_BUILD}" && "${SOURCE_ROOT}/qtbase/configure" \
        -prefix "${TARGET_PREFIX}" -qt-host-path "${HOST_PREFIX}" \
        -platform macx-clang -xplatform "${XPLATFORM}" -sdk "${SDK_NAME}" \
        -release -nomake examples -nomake tests -no-feature-dbus \
        -no-feature-printsupport -no-feature-sql -no-feature-opengl \
        -no-feature-vulkan -no-feature-icu ${GUI_FEATURES} -cmake-generator Xcode -- \
        -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}")
    set -e
fi
if [ -d "${TARGET_BASE_BUILD}/config.tests/arch-arm64/${RELEASE_SUFFIX}" ] && \
        [ ! -e "${TARGET_BASE_BUILD}/config.tests/arch-arm64/Release" ]; then
    ln -s "${RELEASE_SUFFIX}" "${TARGET_BASE_BUILD}/config.tests/arch-arm64/Release"
fi
cmake -S "${SOURCE_ROOT}/qtbase" -B "${TARGET_BASE_BUILD}" -G Xcode \
    -DCMAKE_INSTALL_PREFIX="${TARGET_PREFIX}" \
    -DCMAKE_OSX_SYSROOT="${SDK_PATH}" -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}" -DQT_HOST_PATH="${HOST_PREFIX}" \
    -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF
cmake --build "${TARGET_BASE_BUILD}" --config Release --parallel "${PARALLEL}" -- \
    -quiet CODE_SIGNING_ALLOWED=NO
normalize_xcode_release_dirs "${TARGET_BASE_BUILD}"
cmake --install "${TARGET_BASE_BUILD}" --config Release
# A framework build installs no headers there, but Qt6::Core still lists it.
mkdir -p "${TARGET_PREFIX}/include"

if [ "${PROFILE}" = core ]; then
    echo "Host Qt:   ${HOST_PREFIX}"
    echo "Target Qt (${TARGET}, Core only): ${TARGET_PREFIX}"
    exit 0
fi

if [ ! -f "${HOST_SHADER_BUILD}/CMakeCache.txt" ]; then
    (cd "${HOST_SHADER_BUILD}" && "${HOST_PREFIX}/bin/qt-configure-module" \
        "${SOURCE_ROOT}/qtshadertools" -- -DCMAKE_BUILD_TYPE=Release \
        -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF)
fi
cmake --build "${HOST_SHADER_BUILD}" --parallel "${PARALLEL}"
cmake --install "${HOST_SHADER_BUILD}"

(cd "${TARGET_SHADER_BUILD}" && "${TARGET_PREFIX}/bin/qt-configure-module" \
    "${SOURCE_ROOT}/qtshadertools" -- -G Xcode \
    -DCMAKE_INSTALL_PREFIX="${TARGET_PREFIX}" -DCMAKE_OSX_SYSROOT="${SDK_PATH}" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}" \
    -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF -DQT_HOST_PATH="${HOST_PREFIX}")
cmake --build "${TARGET_SHADER_BUILD}" --config Release --parallel "${PARALLEL}" -- \
    -quiet CODE_SIGNING_ALLOWED=NO
normalize_xcode_release_dirs "${TARGET_SHADER_BUILD}"
cmake --install "${TARGET_SHADER_BUILD}" --config Release

(cd "${TARGET_MULTIMEDIA_BUILD}" && "${TARGET_PREFIX}/bin/qt-configure-module" \
    "${SOURCE_ROOT}/qtmultimedia" -no-feature-gstreamer -- -G Xcode \
    -DCMAKE_INSTALL_PREFIX="${TARGET_PREFIX}" -DCMAKE_OSX_SYSROOT="${SDK_PATH}" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}" \
    -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF -DQT_HOST_PATH="${HOST_PREFIX}")
cmake --build "${TARGET_MULTIMEDIA_BUILD}" --config Release --parallel "${PARALLEL}" -- \
    -quiet CODE_SIGNING_ALLOWED=NO
normalize_xcode_release_dirs "${TARGET_MULTIMEDIA_BUILD}"
cmake --install "${TARGET_MULTIMEDIA_BUILD}" --config Release

echo "Host Qt:   ${HOST_PREFIX}"
echo "Target Qt (${TARGET}): ${TARGET_PREFIX}"
