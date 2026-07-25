import type { NativePluginHost } from "@/bridge/plugin-bridge";

declare global {
  interface Window {
    syntheticObsidianHost?: NativePluginHost;
    syntheticObsidianDispatch?: (message: string) => void;
  }
}
