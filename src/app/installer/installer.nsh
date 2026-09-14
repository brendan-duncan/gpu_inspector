; NSIS additions to the Windows installer (electron-builder includes installer.nsh from the
; buildResources directory, electron-builder.yml).
;
; The capture layer is registered as an implicit Vulkan layer for the installing user, the same
; registration the launch dialog's Register button makes (src/main/implicit_layer.ts). The layer's
; manifest names VKINSP_ENABLE as its enable_environment, so the loader loads it only into
; applications started with that variable set: registering it changes nothing for any other
; application. Uninstalling removes the registration; an update uninstalls and installs into the
; same directory, so the registration is made again.

!define VKINSP_IMPLICIT_LAYERS "SOFTWARE\Khronos\Vulkan\ImplicitLayers"
!define VKINSP_MANIFEST "$INSTDIR\resources\layer\VK_LAYER_INSPECTOR_capture.json"

!macro customInstall
  IfFileExists "${VKINSP_MANIFEST}" 0 +2
    WriteRegDWORD HKCU "${VKINSP_IMPLICIT_LAYERS}" "${VKINSP_MANIFEST}" 0
!macroend

!macro customUnInstall
  DeleteRegValue HKCU "${VKINSP_IMPLICIT_LAYERS}" "${VKINSP_MANIFEST}"
!macroend
