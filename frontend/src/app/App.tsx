import { createPluginBridge } from "@/bridge/create-plugin-bridge";
import { StudioApp } from "@/features/studio/StudioApp";

const pluginBridge = createPluginBridge();

export function App() {
  return <StudioApp bridge={pluginBridge} />;
}
