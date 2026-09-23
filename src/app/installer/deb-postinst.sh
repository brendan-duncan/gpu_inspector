#!/bin/bash
# The .deb's postinst. Two jobs, in order:
#
#   1. Everything electron-builder's own postinst template would have done. Naming this file as
#      `afterInstall` in electron-builder.yml *replaces* that template rather than adding to it
#      (app-builder-lib/templates/linux/after-install.tpl), so it has to be repeated here. Leaving
#      it out installs a package that cannot start: on Ubuntu 24+, where
#      kernel.apparmor_restrict_unprivileged_userns=1, Chromium's sandbox needs either the shipped
#      AppArmor profile loaded or a setuid chrome-sandbox, and without one of them the app aborts
#      before its first window ("userns_create ... DENIED ... capname=sys_admin" in the kernel log).
#   2. Registering the Vulkan capture layer for every user on the machine, the counterpart of what
#      installer.nsh does in the Windows registry (docs/LAUNCH.md, "An application started
#      elsewhere").
#
# Keep step 1 in step with the template when electron-builder is upgraded.
set -e

PRODUCT_DIR="/opt/GPUInspector"
EXECUTABLE="gpu-inspector"

# --- electron-builder's after-install.tpl -------------------------------------------------------

if type update-alternatives >/dev/null 2>&1; then
    # Remove previous link if it doesn't use update-alternatives
    if [ -L "/usr/bin/$EXECUTABLE" ] && [ -e "/usr/bin/$EXECUTABLE" ] &&
       [ "$(readlink "/usr/bin/$EXECUTABLE")" != "/etc/alternatives/$EXECUTABLE" ]; then
        rm -f "/usr/bin/$EXECUTABLE"
    fi
    update-alternatives --install "/usr/bin/$EXECUTABLE" "$EXECUTABLE" "$PRODUCT_DIR/$EXECUTABLE" 100 ||
        ln -sf "$PRODUCT_DIR/$EXECUTABLE" "/usr/bin/$EXECUTABLE"
else
    ln -sf "$PRODUCT_DIR/$EXECUTABLE" "/usr/bin/$EXECUTABLE"
fi

# Check if user namespaces are supported by the kernel and working with a quick test:
if ! { [ -L /proc/self/ns/user ] && unshare --user true; }; then
    # Use SUID chrome-sandbox only on systems without user namespaces:
    chmod 4755 "$PRODUCT_DIR/chrome-sandbox" || true
else
    chmod 0755 "$PRODUCT_DIR/chrome-sandbox" || true
fi

if hash update-mime-database 2>/dev/null; then
    update-mime-database /usr/share/mime || true
fi

if hash update-desktop-database 2>/dev/null; then
    update-desktop-database /usr/share/applications || true
fi

# Install the AppArmor profile (Ubuntu 24+). The dry run first, because a profile using abi/4.0 is
# not understood by the AppArmor in 22.04, where the app runs fine without it.
if apparmor_status --enabled >/dev/null 2>&1; then
    APPARMOR_PROFILE_SOURCE="$PRODUCT_DIR/resources/apparmor-profile"
    APPARMOR_PROFILE_TARGET="/etc/apparmor.d/$EXECUTABLE"
    if apparmor_parser --skip-kernel-load --debug "$APPARMOR_PROFILE_SOURCE" >/dev/null 2>&1; then
        cp -f "$APPARMOR_PROFILE_SOURCE" "$APPARMOR_PROFILE_TARGET"
        # Live AppArmor changes are meaningless in a chroot (image building, for instance).
        if ! { [ -x /usr/bin/ischroot ] && /usr/bin/ischroot; } && hash apparmor_parser 2>/dev/null; then
            # -W -T, as dh_apparmor does, so abstraction updates are pulled in too.
            apparmor_parser --replace --write-cache --skip-read-cache "$APPARMOR_PROFILE_TARGET"
        fi
    else
        echo "gpu-inspector: skipping the AppArmor profile; this AppArmor does not support it." >&2
    fi
fi

# --- the Vulkan capture layer -------------------------------------------------------------------

# Registering it does not turn it on. The manifest carries enable_environment VKINSP_ENABLE=1, so
# the loader ignores the layer in every process that does not have that variable set — which is
# what the inspector sets, for the session it started, when you press Register and then Wait.
#
# The shipped manifest names its library relatively ("./libVkLayer_inspector_capture.so"), which
# only resolves beside the manifest, so the copy written here is given the installed absolute path.
LAYER_DIR="$PRODUCT_DIR/resources/layer"
MANIFEST="$LAYER_DIR/VK_LAYER_INSPECTOR_capture.json"
LIBRARY="$LAYER_DIR/libVkLayer_inspector_capture.so"
TARGET_DIR="/usr/share/vulkan/implicit_layer.d"
TARGET="$TARGET_DIR/VK_LAYER_INSPECTOR_capture.json"

# A build without the layer (the app alone) has nothing to register.
if [ ! -f "$MANIFEST" ] || [ ! -f "$LIBRARY" ]; then
    exit 0
fi

mkdir -p "$TARGET_DIR"
sed "s|\"library_path\": \"\./libVkLayer_inspector_capture\.so\"|\"library_path\": \"$LIBRARY\"|" \
    "$MANIFEST" > "$TARGET.tmp"
# Only replace the installed manifest once the rewrite is known to have worked.
if grep -q "\"library_path\": \"$LIBRARY\"" "$TARGET.tmp"; then
    mv "$TARGET.tmp" "$TARGET"
    chmod 644 "$TARGET"
else
    rm -f "$TARGET.tmp"
    echo "gpu-inspector: could not rewrite the layer manifest; the layer is not registered system-wide." >&2
    echo "gpu-inspector: the inspector can still register it for your account (Launch, then Register)." >&2
fi

exit 0
