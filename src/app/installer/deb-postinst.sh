#!/bin/bash
# Registers the Vulkan capture layer for every user on the machine, the counterpart of what
# installer.nsh does in the Windows registry (docs/LAUNCH.md, "An application started elsewhere").
#
# Registering it does not turn it on. The manifest carries enable_environment VKINSP_ENABLE=1, so
# the loader ignores the layer in every process that does not have that variable set — which is
# what the inspector sets, for the session it started, when you press Register and then Wait.
#
# The shipped manifest names its library relatively ("./libVkLayer_inspector_capture.so"), which
# only resolves beside the manifest, so the copy written here is given the installed absolute path.
set -e

LAYER_DIR="/opt/GPU Inspector/resources/layer"
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
