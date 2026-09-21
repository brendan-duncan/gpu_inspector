// GPU Inspector plugin SDK: the types a backend module is written against (docs/PLUGINS.md).
//
// Types only. A backend module gets everything it runs with from the host its `activate` is given
// (PluginHost), so importing this file adds nothing to the module's bundle: esbuild and tsc drop
// type-only imports. The types are the app's own, re-exported from where they live, so a plugin
// built in this repository is checked against exactly what the app reads.
export type {
  Backend, BackendAdvice, BackendLiveMeasurements, BackendReplay, DetailContext, DetailSection, DetailValue,
} from "../../app/src/renderer/backend.js";
export type { PluginHost } from "../../app/src/renderer/plugin_host.js";
export type {
  BoundIndexBuffer, BoundStageBuffer, BoundStageSampler, BoundStageTexture, BoundVertexBuffer, CommandSets,
} from "../../app/src/renderer/command_sets.js";
export type { DrawState } from "../../app/src/renderer/draw_state.js";
export type { ResourceSource } from "../../app/src/renderer/frame_graph.js";
export type { NodeKind, RawAccess, RawResource } from "../../app/src/renderer/render_graph.js";
export type { CaptureData, CapturedBuffer, CapturedTexture } from "../../app/src/renderer/capture_data.js";
export type { FrameFinding } from "../../app/src/renderer/vulkan/frame_analysis.js";
export type { ObjectLookup, VulkanObject as InspectorObject } from "../../app/src/renderer/vulkan/vulkan_object.js";
export type {
  AddObjectMessage, ArgObject, ArgValue, CaptureBufferInfo, CaptureCommand, CaptureTextureInfo, HandleRef, LayerMessage, UiRequest,
} from "../../app/src/shared/protocol.js";

/** The contract version this SDK describes; plugin.json's `sdk`. */
export const SDK_VERSION = 1;
