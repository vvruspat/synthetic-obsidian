import type {
  BackingTrackName,
  CycleRange,
  StudioProject,
  TrackChannelState,
  TrackLayerState,
  TrackName,
} from "@/domain/studio";

export type ProjectAction =
  | "open-audio"
  | "open-instrumental"
  | "open-project"
  | "save-project"
  | "load-json"
  | "load-lyrics"
  | "analyze"
  | "ai-parts"
  | "generate-backing"
  | "render-backing"
  | "save"
  | "undo"
  | "redo"
  | "export-all-tracks"
  | "export-midi"
  | "validate";

export type PluginCommand =
  | { type: "frontend-ready" }
  | { type: "project-action"; action: ProjectAction }
  | { type: "transport"; action: "return-to-start" | "play" | "pause" | "stop" }
  | { type: "set-loop"; active: boolean; range: CycleRange }
  | { type: "set-playhead"; position: number }
  | { type: "set-volume"; value: number }
  | {
      type: "set-track-state";
      track: string;
      state: TrackChannelState;
    }
  | {
      type: "set-track-layer-state";
      track: string;
      state: TrackLayerState;
    }
  | { type: "select-tracks"; tracks: TrackName[] }
  | { type: "add-backing-track"; track: BackingTrackName }
  | { type: "regenerate-backing-track"; track: BackingTrackName }
  | { type: "render-backing-track"; track: BackingTrackName }
  | {
      type: "pitch-history";
      action: "undo" | "redo";
      track: Exclude<TrackName, "Instrumental">;
    }
  | {
      type: "set-clip-pitch";
      track: Exclude<TrackName, "Instrumental">;
      clipId: string;
      pitch: number;
    }
  | {
      type: "add-clip";
      track: Exclude<TrackName, "Instrumental">;
      clip: { id: string; x: number; pitch: number; width: number };
    }
  | {
      type: "delete-clips";
      track: Exclude<TrackName, "Instrumental">;
      clipIds: string[];
    }
  | {
      type: "move-clips";
      track: Exclude<TrackName, "Instrumental">;
      clips: Array<{ clipId: string; pitch: number }>;
    }
  | {
      type: "preview-pitch-tone";
      pitch: number;
      restart: boolean;
    }
  | {
      type: "stop-pitch-tone";
    }
  | {
      type: "set-clip-time";
      track: Exclude<TrackName, "Instrumental">;
      clipId: string;
      x: number;
      width: number;
    }
  | {
      type: "split-clip";
      track: Exclude<TrackName, "Instrumental">;
      clipId: string;
      x: number;
      rightClipId: string;
    }
  | {
      type: "join-clips";
      track: Exclude<TrackName, "Instrumental">;
      clipIds: string[];
    }
  | {
      type: "set-clip-vibrato";
      track: Exclude<TrackName, "Instrumental">;
      clipId: string;
      scale: number;
    }
  | {
      type: "set-clip-gain";
      track: Exclude<TrackName, "Instrumental">;
      clipId: string;
      gainDb: number;
    };

export type PluginEvent =
  | { type: "project-state"; project: StudioProject }
  | {
      type: "status-state";
      message: string;
      vocalAnalysisRunning: boolean;
      instrumentalAnalysisRunning: boolean;
      backingGenerationRunning: boolean;
      backingAudioRenderRunning: boolean;
      backingGenerationTrack: string;
      backingAudioRenderTrack: string;
    }
  | {
      type: "export-complete";
      message: string;
      directory: string;
    }
  | { type: "transport-state"; playing: boolean; playhead: number }
  | { type: "loop-state"; active: boolean; range: CycleRange }
  | { type: "volume-state"; value: number }
  | { type: "output-meter-state"; left: number; right: number }
  | {
      type: "track-state";
      track: string;
      state: TrackChannelState;
    }
  | {
      type: "track-layer-state";
      track: string;
      state: TrackLayerState;
    }
  | { type: "backing-track-added"; track: BackingTrackName }
  | {
      type: "clip-pitch-state";
      track: Exclude<TrackName, "Instrumental">;
      clipId: string;
      pitch: number;
    };

export type PluginEventListener = (event: PluginEvent) => void;

export interface PluginBridge {
  send(command: PluginCommand): void;
  subscribe(listener: PluginEventListener): () => void;
}

export interface NativePluginHost {
  postMessage(message: string): void;
}
