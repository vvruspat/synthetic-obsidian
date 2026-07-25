import {
  MicrophoneIcon,
  MusicalNoteIcon,
  SignalIcon,
  SparklesIcon,
} from "@heroicons/react/24/outline";
import type {
  TrackChannelState,
  TrackLayerAvailability,
  TrackLayerState,
  TrackName,
} from "@/domain/studio";
import { ToggleButton } from "@/ui";

type TrackHeaderProps = {
  name: TrackName;
  state: TrackChannelState;
  onToggle(mode: keyof TrackChannelState): void;
  layerState?: TrackLayerState;
  layerAvailability?: TrackLayerAvailability;
  onToggleLayer?(layer: keyof TrackLayerState): void;
  selected: boolean;
  onSelect(additive: boolean): void;
};

export function TrackHeader({
  name,
  state,
  onToggle,
  layerState,
  layerAvailability,
  onToggleLayer,
  selected,
  onSelect,
}: TrackHeaderProps) {
  const TrackIcon =
    name === "Voice Main" ? MicrophoneIcon : name === "Instrumental" ? SignalIcon : SparklesIcon;

  return (
    <div
      className={[
        "track-header",
        name === "Voice Main" ? "is-voice" : "",
        selected ? "is-selected" : "",
      ]
        .filter(Boolean)
        .join(" ")}
    >
      <button
        type="button"
        className="track-selection-target"
        aria-label={`Select ${name}`}
        aria-pressed={selected}
        onClick={(event) => onSelect(event.shiftKey)}
      />
      {layerState && onToggleLayer ? (
        <div className="track-layer-toggles">
          <button
            type="button"
            className={`track-layer-toggle ${
              layerState.audioMuted || !layerAvailability?.audioAvailable ? "is-off" : ""
            }`}
            aria-label={
              layerAvailability?.audioAvailable
                ? `${layerState.audioMuted ? "Unmute" : "Mute"} ${name} audio`
                : `${name} audio unavailable`
            }
            aria-pressed={layerState.audioMuted}
            title={
              layerAvailability?.audioAvailable
                ? `${layerState.audioMuted ? "Unmute" : "Mute"} audio`
                : "Audio not available"
            }
            disabled={!layerAvailability?.audioAvailable}
            onClick={() => onToggleLayer("audioMuted")}
          >
            <TrackIcon strokeWidth={1.8} aria-hidden="true" />
          </button>
          <button
            type="button"
            className={`track-layer-toggle ${
              layerState.notesMuted || !layerAvailability?.notesAvailable ? "is-off" : ""
            }`}
            aria-label={
              layerAvailability?.notesAvailable
                ? `${layerState.notesMuted ? "Unmute" : "Mute"} ${name} notes`
                : `${name} notes unavailable`
            }
            aria-pressed={layerState.notesMuted}
            title={
              layerAvailability?.notesAvailable
                ? `${layerState.notesMuted ? "Unmute" : "Mute"} notes`
                : "Notes not available"
            }
            disabled={!layerAvailability?.notesAvailable}
            onClick={() => onToggleLayer("notesMuted")}
          >
            <MusicalNoteIcon strokeWidth={1.8} aria-hidden="true" />
          </button>
        </div>
      ) : (
        <span className="track-icon" aria-hidden="true">
          <TrackIcon strokeWidth={1.8} />
        </span>
      )}
      <div className="track-control-stack">
        <span className="track-name">{name}</span>
        <div className="track-toggles">
          <ToggleButton
            active={state.mute}
            onClick={() => onToggle("mute")}
            aria-label={`Mute ${name}`}
          >
            M
          </ToggleButton>
          <ToggleButton
            active={state.solo}
            className={state.solo ? "solo" : ""}
            onClick={() => onToggle("solo")}
            aria-label={`Solo ${name}`}
          >
            S
          </ToggleButton>
        </div>
      </div>
    </div>
  );
}
