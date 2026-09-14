import type {
  BackingGenerator,
  BackingTrackInstance,
  BackingTrackName,
  CycleRange,
  StudioProject,
  TrackChannelState,
  TrackLayerState,
  TrackName,
} from "@/domain/studio";

export type ProjectAction =
  | "new-project"
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

export type AudioSettingsState = {
  deviceType: string;
  inputDevices: string[];
  outputDevices: string[];
  inputDevice: string;
  outputDevice: string;
  sampleRate: number;
  bufferSize: number;
  error: string;
};

export type VoiceTrainingSource = {
  id: string;
  name: string;
  durationSeconds: number;
  sampleRate: number;
  channels: number;
  status: "ready" | "warning" | "rejected";
  message: string;
};

export type VoiceTrainingJob = {
  jobId: string;
  presetName: string;
  quality: "balanced" | "high";
  status:
    | "queued"
    | "preparing"
    | "loading"
    | "preprocessing"
    | "training"
    | "saving"
    | "cancelling"
    | "complete"
    | "cancelled"
    | "error";
  stage: string;
  message: string;
  progress: number;
  currentStep: number;
  totalSteps: number;
  createdAt: string;
  updatedAt: string;
  profileDirectory: string;
  adapterFile: string;
  canCancel: boolean;
  loss?: number;
};

export type VoiceTrainingState = {
  phase: "idle" | "ready" | "preparing" | "cancelling" | "complete" | "cancelled" | "error";
  message: string;
  progress: number;
  device: string;
  isAppleSilicon: boolean;
  sources: VoiceTrainingSource[];
  totalDurationSeconds: number;
  minimumDurationSeconds: number;
  recommendedDurationSeconds: number;
  canCreateProfile: boolean;
  activeProfileName: string;
  outputDirectory: string;
  fineTuningAvailable: boolean;
  runtimeStatus: "ready" | "missing" | "unsupported";
  runtimeMessage: string;
  runtimeInstallDirectory: string;
  canStartTraining: boolean;
  jobs: VoiceTrainingJob[];
};

export type PluginCommand =
  | { type: "frontend-ready" }
  | { type: "project-action"; action: ProjectAction }
  | {
      type: "audio-settings";
      action: "request" | "set-devices" | "test-output";
      inputDevice?: string;
      outputDevice?: string;
    }
  | {
      type: "voice-training";
      action:
        | "request-state"
        | "add-files"
        | "add-current-track"
        | "remove-file"
        | "clear-files"
        | "create-profile"
        | "start-training"
        | "cancel-training"
        | "reveal-training"
        | "cancel"
        | "reveal-profile";
      sourceId?: string;
      jobId?: string;
      presetName?: string;
      quality?: "balanced" | "high";
    }
  | { type: "transport"; action: "return-to-start" | "play" | "pause" | "stop" }
  | { type: "set-loop"; active: boolean; range: CycleRange }
  | { type: "set-playhead"; position: number }
  | { type: "set-volume"; value: number }
  | { type: "set-instrumental-volume"; value: number }
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
  | { type: "add-backing-track"; track: BackingTrackName; generator?: BackingGenerator }
  | { type: "regenerate-backing-track"; track: string; generator?: BackingGenerator }
  | { type: "render-backing-track"; track: string }
  | { type: "remove-backing-track"; track: string }
  | { type: "delete-voice-profile"; profileId: string }
  | {
      type: "set-backing-voice-profile";
      track: string;
      profileId: string;
    }
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
      snapToChord?: boolean;
    }
  | {
      type: "add-clip";
      track: Exclude<TrackName, "Instrumental">;
      clip: { id: string; x: number; pitch: number; width: number };
      snapToChord?: boolean;
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
      snapToChord?: boolean;
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
      snapToChord?: boolean;
    }
  | {
      type: "split-clip";
      track: Exclude<TrackName, "Instrumental">;
      clipId: string;
      x: number;
      rightClipId: string;
      snapToChord?: boolean;
    }
  | {
      type: "join-clips";
      track: Exclude<TrackName, "Instrumental">;
      clipIds: string[];
      snapToChord?: boolean;
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
  | { type: "audio-settings-state"; state: AudioSettingsState }
  | { type: "voice-training-state"; state: VoiceTrainingState }
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
  | { type: "instrumental-volume-state"; value: number }
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
  | { type: "backing-track-created"; track: BackingTrackInstance }
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
