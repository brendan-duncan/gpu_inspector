#!/usr/bin/env bash
# Sets up a clean checkout of GPU Inspector on Linux: checks the prerequisites, initializes the
# Vulkan-Headers submodule, builds the layer (and the test application), and installs the app's
# node modules.
#
#   tools/setup.sh                  check prerequisites, then build everything
#   tools/setup.sh --install-deps   install the missing system packages first (uses sudo)
#   tools/setup.sh --check          only report what is missing, build nothing
#   tools/setup.sh --debug          build the native side as Debug instead of Release
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_TYPE=Release
INSTALL_DEPS=0
CHECK_ONLY=0

for arg in "$@"; do
    case "$arg" in
        --install-deps) INSTALL_DEPS=1 ;;
        --check) CHECK_ONLY=1 ;;
        --debug) BUILD_TYPE=Debug ;;
        -h|--help) sed -n '2,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $arg (try --help)" >&2; exit 2 ;;
    esac
done

# This script builds the layer, which has no Apple target, so on macOS it has nothing to do that
# npm cannot do; say so rather than fail later looking for a Linux package manager.
if [ "$(uname -s)" = "Darwin" ]; then
    cat >&2 <<'MSG'
tools/setup.sh is for Linux. The macOS build is the user interface alone (there is no Apple
build of the capture layer), and needs only:

    cd app && npm install && npm start

See the macOS section of README.md.
MSG
    exit 2
fi

red() { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[33m%s\033[0m\n' "$*"; }
step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

# ----------------------------------------------------------------------------------------------
# Package manager

PM=""
for pm in apt-get dnf pacman zypper; do
    if command -v "$pm" >/dev/null 2>&1; then PM="$pm"; break; fi
done

# package_for <generic-name>: the package that provides it on this distribution.
package_for() {
    local name="$1"
    case "$PM:$name" in
        apt-get:compiler) echo build-essential ;;
        dnf:compiler) echo gcc-c++ ;;
        pacman:compiler) echo base-devel ;;
        zypper:compiler) echo gcc-c++ ;;

        apt-get:ninja) echo ninja-build ;;
        dnf:ninja) echo ninja-build ;;
        pacman:ninja) echo ninja ;;
        zypper:ninja) echo ninja ;;

        apt-get:vulkan-dev) echo libvulkan-dev ;;
        dnf:vulkan-dev) echo vulkan-loader-devel ;;
        pacman:vulkan-dev) echo vulkan-headers ;;
        zypper:vulkan-dev) echo vulkan-devel ;;

        apt-get:xcb-dev) echo libxcb1-dev ;;
        dnf:xcb-dev) echo libxcb-devel ;;
        pacman:xcb-dev) echo libxcb ;;
        zypper:xcb-dev) echo libxcb-devel ;;

        apt-get:x11-dev) echo libx11-dev ;;
        dnf:x11-dev) echo libX11-devel ;;
        pacman:x11-dev) echo libx11 ;;
        zypper:x11-dev) echo libX11-devel ;;

        apt-get:wayland-dev) echo libwayland-dev ;;
        dnf:wayland-dev) echo wayland-devel ;;
        pacman:wayland-dev) echo wayland ;;
        zypper:wayland-dev) echo wayland-devel ;;

        apt-get:glslc) echo glslc ;;
        dnf:glslc) echo glslc ;;
        pacman:glslc) echo shaderc ;;
        zypper:glslc) echo shaderc ;;

        apt-get:spirv-tools) echo spirv-tools ;;
        dnf:spirv-tools) echo spirv-tools ;;
        pacman:spirv-tools) echo spirv-tools ;;
        zypper:spirv-tools) echo spirv-tools ;;

        *:spirv-cross) echo spirv-cross ;;
        *:cmake) echo cmake ;;
        *:python3) echo python3 ;;
        *:git) echo git ;;
        *:nodejs) echo nodejs ;;
        apt-get:npm) echo npm ;;
        *:npm) echo "" ;;   # bundled with nodejs elsewhere
        *) echo "$name" ;;
    esac
}

MISSING_REQUIRED=()
MISSING_OPTIONAL=()

# need <generic-name> <required|optional> <what it is for> -- <test command...>
need() {
    local name="$1" kind="$2" purpose="$3"; shift 4
    if "$@" >/dev/null 2>&1; then
        green "  ok       $name"
        return 0
    fi
    local pkg; pkg="$(package_for "$name")"
    if [[ "$kind" == required ]]; then
        red "  MISSING  $name — $purpose"
        if [[ -n "$pkg" ]]; then MISSING_REQUIRED+=("$pkg"); fi
    else
        yellow "  optional $name — $purpose"
        if [[ -n "$pkg" ]]; then MISSING_OPTIONAL+=("$pkg"); fi
    fi
    return 0
}

step "Checking prerequisites"
need git required "cloning and submodules" -- command -v git
need cmake required "builds the Vulkan layer" -- command -v cmake
need compiler required "C++20 compiler" -- command -v c++
need python3 required "generates the layer from vk.xml" -- command -v python3
need ninja optional "faster builds than make" -- command -v ninja
need nodejs required "builds and runs the Electron app" -- command -v node
need npm required "installs the app's dependencies" -- command -v npm
need vulkan-dev required "Vulkan loader, and headers for the test app" -- pkg-config --exists vulkan
need xcb-dev required "XCB surfaces, and window creation in the test app" -- pkg-config --exists xcb
need x11-dev optional "Xlib surface arguments in captures" -- pkg-config --exists x11
need wayland-dev optional "Wayland surface arguments in captures" -- pkg-config --exists wayland-client
need glslc required "compiles the test app's shaders" -- command -v glslc
need vulkan-tools optional "vulkaninfo, to check the driver is working" -- command -v vulkaninfo
need spirv-tools optional "SPIR-V disassembly in the Inspect panel" -- command -v spirv-dis
need spirv-cross optional "GLSL/HLSL shader text in the Inspect panel" -- command -v spirv-cross

if [[ ${#MISSING_REQUIRED[@]} -gt 0 || ${#MISSING_OPTIONAL[@]} -gt 0 ]]; then
    ALL=("${MISSING_REQUIRED[@]}" "${MISSING_OPTIONAL[@]}")
    case "$PM" in
        apt-get) INSTALL_CMD=(sudo apt-get install -y "${ALL[@]}") ;;
        dnf) INSTALL_CMD=(sudo dnf install -y "${ALL[@]}") ;;
        pacman) INSTALL_CMD=(sudo pacman -S --needed "${ALL[@]}") ;;
        zypper) INSTALL_CMD=(sudo zypper install -y "${ALL[@]}") ;;
        *) INSTALL_CMD=() ;;
    esac
    echo
    if [[ ${#INSTALL_CMD[@]} -eq 0 ]]; then
        red "No supported package manager found; install the packages above by hand."
        if [[ ${#MISSING_REQUIRED[@]} -gt 0 ]]; then exit 1; fi
    elif [[ $INSTALL_DEPS -eq 1 ]]; then
        step "Installing packages"
        echo "  ${INSTALL_CMD[*]}"
        if [[ "$PM" == apt-get ]]; then sudo apt-get update; fi
        "${INSTALL_CMD[@]}"
    else
        echo "Install the missing packages with:"
        echo
        echo "  ${INSTALL_CMD[*]}"
        echo
        echo "or re-run this script as: tools/setup.sh --install-deps"
        if [[ ${#MISSING_REQUIRED[@]} -gt 0 ]]; then exit 1; fi
    fi
fi

# A Vulkan driver (ICD) is needed to run anything, but it is hardware specific, so only warn.
if ! compgen -G "/usr/share/vulkan/icd.d/*.json" >/dev/null && \
   ! compgen -G "/etc/vulkan/icd.d/*.json" >/dev/null; then
    yellow "No Vulkan driver (ICD) found in /usr/share/vulkan/icd.d. Install your GPU's Vulkan"
    yellow "driver — mesa-vulkan-drivers for AMD/Intel, the proprietary driver for NVIDIA."
fi

if [[ $CHECK_ONLY -eq 1 ]]; then exit 0; fi

# ----------------------------------------------------------------------------------------------
# Build

step "Updating submodules"
git -C "$ROOT" submodule update --init --recursive

step "Building the layer and test app ($BUILD_TYPE)"
GENERATOR=()
if command -v ninja >/dev/null 2>&1; then GENERATOR=(-G Ninja); fi
cmake -S "$ROOT" -B "$ROOT/build" "${GENERATOR[@]}" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$ROOT/build" --config "$BUILD_TYPE" --parallel

step "Installing app dependencies"
npm --prefix "$ROOT/app" install

step "Done"
cat <<EOF
Start the inspector with:

  cd app && npm start

then point the launcher at a Vulkan executable — for example the bundled test application,
$ROOT/build/bin/vkinsp_triangle — and press Launch, then Capture in the Capture tab.

For an entry in the desktop's application list and dock, with the right icon:

  tools/install_desktop_entry.sh
EOF
