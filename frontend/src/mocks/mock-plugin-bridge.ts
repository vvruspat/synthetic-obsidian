import type {
  PluginBridge,
  PluginCommand,
  PluginEvent,
  PluginEventListener,
} from "@/bridge/plugin-bridge";

export class MockPluginBridge implements PluginBridge {
  readonly commands: PluginCommand[] = [];
  private readonly listeners = new Set<PluginEventListener>();

  send(command: PluginCommand) {
    this.commands.push(command);
  }

  subscribe(listener: PluginEventListener) {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  emit(event: PluginEvent) {
    for (const listener of this.listeners) {
      listener(event);
    }
  }
}
