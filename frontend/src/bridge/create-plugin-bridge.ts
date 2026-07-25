import type { PluginBridge, PluginEvent, PluginEventListener } from "@/bridge/plugin-bridge";
import { MockPluginBridge } from "@/mocks/mock-plugin-bridge";

export function createPluginBridge(): PluginBridge {
  const nativeHost = window.syntheticObsidianHost;

  if (!nativeHost) {
    return new MockPluginBridge();
  }

  const listeners = new Set<PluginEventListener>();
  window.syntheticObsidianDispatch = (message) => {
    try {
      const event = JSON.parse(message) as PluginEvent;
      if (!event || typeof event !== "object" || typeof event.type !== "string") {
        return;
      }
      for (const listener of listeners) {
        listener(event);
      }
    } catch {
      // Ignore malformed messages at the native boundary.
    }
  };

  return {
    send(command) {
      nativeHost.postMessage(JSON.stringify(command));
    },
    subscribe(listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
  };
}
