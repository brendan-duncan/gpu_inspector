#!/bin/bash
# Removes what deb-postinst.sh registered. Only the manifest this package wrote is removed: a
# layer the user registered for their own account (~/.local/share/vulkan/implicit_layer.d) is
# theirs, and the inspector's Register button is what turns that one off.
set -e

TARGET="/usr/share/vulkan/implicit_layer.d/VK_LAYER_INSPECTOR_capture.json"
if [ -f "$TARGET" ]; then
    rm -f "$TARGET"
    # Left behind if anything else put a layer there, which rmdir declining is exactly the check.
    rmdir --ignore-fail-on-non-empty /usr/share/vulkan/implicit_layer.d 2>/dev/null || true
fi

exit 0
