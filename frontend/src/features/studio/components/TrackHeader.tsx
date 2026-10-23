import {
  ChevronDownIcon,
  ChevronRightIcon,
  MicrophoneIcon,
  MusicalNoteIcon,
  PlusIcon,
  SignalIcon,
  SparklesIcon,
  SpeakerWaveIcon,
  TrashIcon,
} from "@heroicons/react/24/outline";
import type { CSSProperties } from "react";
import type {
  TrackChannelState,
  TrackLayerAvailability,
  TrackLayerState,
  TrackName,
} from "@/domain/studio";
import { RangeInput, ToggleButton } from "@/ui";

type TrackHeaderProps = {
  depth?: number;
  childCount?: number;
  collapsed?: boolean;
  onToggleCollapsed?(): void;
  onImport?(): void;
  id: TrackName;
  name: TrackName;
  state: TrackChannelState;
  onToggle(mode: keyof TrackChannelState): void;
  volume?: number;
  onVolumeChange?(value: number): void;
  layerState?: TrackLayerState;
  layerAvailability?: TrackLayerAvailability;
  onToggleLayer?(layer: keyof TrackLayerState): void;
  selected: boolean;
  onSelect(additive: boolean): void;
  onDelete?(): void;
};

export function TrackHeader({
  depth = 0,
  childCount = 0,
  collapsed = false,
  onToggleCollapsed,
  onImport,
  id,
  name,
  state,
  onToggle,
  volume,
  onVolumeChange,
  layerState,
  layerAvailability,
  onToggleLayer,
  selected,
  onSelect,
  onDelete,
}: TrackHeaderProps) {
  const TrackIcon =
    name === "Voice Main" ? MicrophoneIcon : name === "Instrumental" ? SignalIcon : SparklesIcon;
  const volumeControlId = `track-volume-${id.toLowerCase().replace(/\s+/g, "-")}`;

  return (
    <div
      className={[
        "track-header",
        depth ? "is-child-track" : "",
        name === "Voice Main" ? "is-voice" : "",
        selected ? "is-selected" : "",
      ]
        .filter(Boolean)
        .join(" ")}
      style={{ paddingLeft: `${28 + depth * 14}px` }}
    >
      <button
        type="button"
        className="track-selection-target"
        aria-label={`Select ${name}`}
        aria-pressed={selected}
        onClick={(event) => onSelect(event.shiftKey)}
      />
      {childCount > 0 ? (
        <button
          type="button"
          className="track-disclosure"
          aria-label={`${collapsed ? "Expand" : "Collapse"} ${name} backing tracks`}
          aria-expanded={!collapsed}
          title={`${childCount} backing tracks`}
          onClick={onToggleCollapsed}
        >
          {collapsed ? (
            <ChevronRightIcon aria-hidden="true" />
          ) : (
            <ChevronDownIcon aria-hidden="true" />
          )}
          <span>{childCount}</span>
        </button>
      ) : (
        <span className="track-disclosure-placeholder" />
      )}
      {onImport ? (
        <button
          type="button"
          className="track-import-button"
          aria-label={`Add clips to ${name}`}
          title="Add audio clips"
          onClick={onImport}
        >
          <PlusIcon aria-hidden="true" />
        </button>
      ) : null}
      {onDelete ? (
        <button
          type="button"
          className="track-delete-button"
          aria-label={`Delete ${name}`}
          title={`Delete ${name}`}
          onClick={onDelete}
        >
          <TrashIcon aria-hidden="true" />
        </button>
      ) : null}
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
      <div className={`track-control-stack ${volume !== undefined ? "has-volume" : ""}`}>
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
        {volume !== undefined && onVolumeChange ? (
          <label
            className="track-volume-control"
            htmlFor={volumeControlId}
            title={`${name} volume ${Math.round(volume)}%`}
            style={{ "--track-volume": `${volume}%` } as CSSProperties}
          >
            <SpeakerWaveIcon aria-hidden="true" />
            <span className="sr-only">{name} volume</span>
            <RangeInput
              id={volumeControlId}
              min={0}
              max={100}
              step={1}
              value={volume}
              aria-label={`${name} volume`}
              aria-valuetext={`${Math.round(volume)} percent`}
              onValueChange={onVolumeChange}
            />
          </label>
        ) : null}
      </div>
    </div>
  );
}
