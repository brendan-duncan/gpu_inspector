#!/bin/bash
# The .deb's postrm, undoing deb-postinst.sh. As with the postinst, naming this file as
# electron-builder's `afterRemove` *replaces* its own template
# (app-builder-lib/templates/linux/after-remove.tpl), so that template's work is repeated here.
#
# dpkg runs this on an upgrade too (old postrm, then new postinst), so everything it takes away is
# something the postinst puts back — which is the order dpkg guarantees.
set -e

PRODUCT_DIR="/opt/GPU Inspector"
EXECUTABLE="gpu-inspector"

# --- electron-builder's after-remove.tpl --------------------------------------------------------

# update-alternatives --remove <name> <path>: 'path' is the registered alternative, not the
# generic symlink. https://man7.org/linux/man-pages/man1/update-alternatives.1.html
if type update-alternatives >/dev/null 2>&1; then
    update-alternatives --remove "$EXECUTABLE" "$PRODUCT_DIR/$EXECUTABLE"
else
    rm -f "/usr/bin/$EXECUTABLE"
fi

APPARMOR_PROFILE_DEST="/etc/apparmor.d/$EXECUTABLE"
if [ -f "$APPARMOR_PROFILE_DEST" ]; then
    # Unload before deleting, so the policy is not left enforced until the next reboot. Live
    # AppArmor changes are meaningless in a chroot, the same guard the postinst uses.
    if apparmor_status --enabled >/dev/null 2>&1; then
        if ! { [ -x /usr/bin/ischroot ] && /usr/bin/ischroot; } && hash apparmor_parser 2>/dev/null; then
            apparmor_parser --remove "$APPARMOR_PROFILE_DEST" || true
        fi
    fi
    rm -f "$APPARMOR_PROFILE_DEST"
fi

# --- the Vulkan capture layer -------------------------------------------------------------------

# Only the manifest this package wrote is removed: a layer the user registered for their own
# account (~/.local/share/vulkan/implicit_layer.d) is theirs, and the inspector's Register button
# is what turns that one off.
TARGET="/usr/share/vulkan/implicit_layer.d/VK_LAYER_INSPECTOR_capture.json"
if [ -f "$TARGET" ]; then
    rm -f "$TARGET"
    # Left behind if anything else put a layer there, which rmdir declining is exactly the check.
    rmdir --ignore-fail-on-non-empty /usr/share/vulkan/implicit_layer.d 2>/dev/null || true
fi

exit 0
