import {
  ArrowPathIcon,
  CommandLineIcon,
  CpuChipIcon,
  SpeakerWaveIcon,
  XMarkIcon,
} from "@heroicons/react/24/outline";
import { type KeyboardEvent as ReactKeyboardEvent, useEffect, useMemo, useState } from "react";
import type { AudioSettingsState, PluginBridge } from "@/bridge/plugin-bridge";
import {
  assignShortcut,
  createDefaultShortcutBindings,
  formatShortcut,
  SHORTCUT_DEFINITIONS,
  type ShortcutActionId,
  type ShortcutBindings,
  shortcutFromKeyboardEvent,
} from "@/features/studio/lib/hotkeys";

export const EMPTY_AUDIO_SETTINGS_STATE: AudioSettingsState = {
  deviceType: "",
  inputDevices: [],
  outputDevices: [],
  inputDevice: "",
  outputDevice: "",
  sampleRate: 0,
  bufferSize: 0,
  error: "",
};

export function SettingsDialog({
  bridge,
  audio,
  shortcuts,
  open,
  onClose,
  onShortcutsChange,
}: {
  bridge: PluginBridge;
  audio: AudioSettingsState;
  shortcuts: ShortcutBindings;
  open: boolean;
  onClose(): void;
  onShortcutsChange(shortcuts: ShortcutBindings): void;
}) {
  const [tab, setTab] = useState<"audio" | "shortcuts">("audio");
  const [recordingAction, setRecordingAction] = useState<ShortcutActionId | null>(null);

  useEffect(() => {
    if (!open) return;
    bridge.send({ type: "audio-settings", action: "request" });
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key === "Escape" && recordingAction === null) onClose();
    };
    window.addEventListener("keydown", closeOnEscape);
    return () => window.removeEventListener("keydown", closeOnEscape);
  }, [bridge, onClose, open, recordingAction]);

  const shortcutGroups = useMemo(() => {
    const groups = new Map<string, typeof SHORTCUT_DEFINITIONS>();
    for (const definition of SHORTCUT_DEFINITIONS) {
      const definitions = groups.get(definition.category) ?? [];
      groups.set(definition.category, [...definitions, definition]);
    }
    return groups;
  }, []);

  if (!open) return null;

  const setAudioDevices = (inputDevice: string, outputDevice: string) => {
    bridge.send({
      type: "audio-settings",
      action: "set-devices",
      inputDevice,
      outputDevice,
    });
  };

  const captureShortcut = (action: ShortcutActionId, event: ReactKeyboardEvent) => {
    event.preventDefault();
    event.stopPropagation();
    if (event.key === "Escape") {
      setRecordingAction(null);
      return;
    }
    if (event.key === "Backspace" || event.key === "Delete") {
      onShortcutsChange(assignShortcut(shortcuts, action, null));
      setRecordingAction(null);
      return;
    }

    const shortcut = shortcutFromKeyboardEvent(event.nativeEvent);
    if (!shortcut) return;
    onShortcutsChange(assignShortcut(shortcuts, action, shortcut));
    setRecordingAction(null);
  };

  return (
    <div className="settings-backdrop">
      <section
        className="settings-dialog"
        role="dialog"
        aria-modal="true"
        aria-labelledby="settings-title"
      >
        <header className="settings-header">
          <span className="settings-mark" aria-hidden="true">
            <CpuChipIcon />
          </span>
          <div>
            <small>APPLICATION</small>
            <h2 id="settings-title">Settings</h2>
          </div>
          <button
            className="settings-close"
            type="button"
            aria-label="Close settings"
            onClick={onClose}
          >
            <XMarkIcon aria-hidden="true" />
          </button>
        </header>

        <div className="settings-layout">
          <nav className="settings-tabs" aria-label="Settings sections">
            <button
              type="button"
              className={tab === "audio" ? "is-active" : ""}
              onClick={() => setTab("audio")}
            >
              <SpeakerWaveIcon aria-hidden="true" />
              Audio
            </button>
            <button
              type="button"
              className={tab === "shortcuts" ? "is-active" : ""}
              onClick={() => setTab("shortcuts")}
            >
              <CommandLineIcon aria-hidden="true" />
              Hotkeys
            </button>
          </nav>

          <div className="settings-content">
            {tab === "audio" ? (
              <>
                <div className="settings-section-heading">
                  <div>
                    <small>AUDIO DEVICE</small>
                    <h3>Input &amp; output</h3>
                  </div>
                  <button
                    type="button"
                    title="Refresh audio device list"
                    onClick={() => bridge.send({ type: "audio-settings", action: "request" })}
                  >
                    <ArrowPathIcon aria-hidden="true" />
                    Refresh
                  </button>
                </div>

                <div className="audio-settings-grid">
                  <label>
                    <span>OUTPUT DEVICE</span>
                    <select
                      value={audio.outputDevice}
                      disabled={audio.outputDevices.length === 0}
                      onChange={(event) => setAudioDevices(audio.inputDevice, event.target.value)}
                    >
                      {audio.outputDevices.map((device) => (
                        <option key={device} value={device}>
                          {device}
                        </option>
                      ))}
                    </select>
                  </label>
                  <label>
                    <span>INPUT DEVICE</span>
                    <select
                      value={audio.inputDevice}
                      onChange={(event) => setAudioDevices(event.target.value, audio.outputDevice)}
                    >
                      <option value="">No input</option>
                      {audio.inputDevices.map((device) => (
                        <option key={device} value={device}>
                          {device}
                        </option>
                      ))}
                    </select>
                  </label>
                </div>

                <div className="audio-settings-status">
                  <span>
                    <small>DRIVER</small>
                    <strong>{audio.deviceType || "Unavailable"}</strong>
                  </span>
                  <span>
                    <small>SAMPLE RATE</small>
                    <strong>
                      {audio.sampleRate > 0 ? `${(audio.sampleRate / 1000).toFixed(1)} kHz` : "—"}
                    </strong>
                  </span>
                  <span>
                    <small>BUFFER</small>
                    <strong>{audio.bufferSize > 0 ? `${audio.bufferSize} samples` : "—"}</strong>
                  </span>
                </div>

                {audio.error ? (
                  <p className="audio-settings-error" role="alert">
                    {audio.error}
                  </p>
                ) : null}

                <button
                  className="audio-test-button"
                  type="button"
                  disabled={!audio.outputDevice}
                  onClick={() => bridge.send({ type: "audio-settings", action: "test-output" })}
                >
                  <SpeakerWaveIcon aria-hidden="true" />
                  Test output
                </button>
                <p className="settings-note">
                  Selecting an input may cause macOS to request microphone access once. The selected
                  devices are stored for the next launch.
                </p>
              </>
            ) : (
              <>
                <div className="settings-section-heading">
                  <div>
                    <small>KEY COMMANDS</small>
                    <h3>Hotkeys</h3>
                  </div>
                  <button
                    type="button"
                    onClick={() => {
                      onShortcutsChange(createDefaultShortcutBindings());
                      setRecordingAction(null);
                    }}
                  >
                    <ArrowPathIcon aria-hidden="true" />
                    Restore defaults
                  </button>
                </div>
                <p className="settings-note settings-note-top">
                  Click a shortcut and press a new key combination. Backspace clears it. A duplicate
                  combination is moved to the new command.
                </p>
                <div className="shortcut-settings-list">
                  {[...shortcutGroups.entries()].map(([category, definitions]) => (
                    <section key={category}>
                      <h4>{category}</h4>
                      {definitions.map((definition) => (
                        <div className="shortcut-settings-row" key={definition.id}>
                          <span>{definition.label}</span>
                          <button
                            type="button"
                            className={recordingAction === definition.id ? "is-recording" : ""}
                            aria-label={`Shortcut for ${definition.label}`}
                            onClick={() => setRecordingAction(definition.id)}
                            onBlur={() =>
                              setRecordingAction((current) =>
                                current === definition.id ? null : current,
                              )
                            }
                            onKeyDown={(event) => captureShortcut(definition.id, event)}
                          >
                            {recordingAction === definition.id
                              ? "Press keys…"
                              : formatShortcut(shortcuts[definition.id])}
                          </button>
                        </div>
                      ))}
                    </section>
                  ))}
                </div>
              </>
            )}
          </div>
        </div>
      </section>
    </div>
  );
}
