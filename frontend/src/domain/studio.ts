export const BACKING_TRACK_OPTIONS = [
  "Unison Double",
  "Octave Above",
  "Octave Below",
  "Third Above",
  "Third Below",
  "Sixth Above",
  "Sixth Below",
  "Fifth Above",
  "Fifth Below",
  "Fourth Above",
  "Fourth Below",
  "Drone Root",
  "Drone Fifth",
  "Drone Third",
  "Contrary Motion Harmony",
  "Oblique Motion Harmony",
  "Parallel Thirds",
  "Parallel Sixths",
  "Choir Soprano",
  "Choir Alto",
  "Choir Tenor",
  "Choir Bass",
  "Suspension Harmony",
  "Passing Tone Harmony",
  "Tension-Resolution Harmony",
  "Dynamic Counterpoint",
] as const;

export type BackingTrackName = (typeof BACKING_TRACK_OPTIONS)[number];
export type BackingGenerator = "local" | "luna" | "terra";
export type TrackName = string;
export type BackingTrackInstance = {
  id: string;
  name: string;
  style: BackingTrackName;
};
export type ClipColor = "cyan" | "violet" | "pink" | "amber" | "lime" | "silver";

export type TrackChannelState = {
  mute: boolean;
  solo: boolean;
};

export type TrackLayerState = {
  audioMuted: boolean;
  notesMuted: boolean;
};

export type TrackLayerAvailability = {
  audioAvailable: boolean;
  notesAvailable: boolean;
};

export type VocalClip = {
  id: string;
  label: string;
  x: number;
  pitch: number;
  width: number;
  color: ClipColor;
  gainDb?: number;
  pitchCurve?: Array<{ x: number; pitch: number }>;
  legatoFromPrevious?: boolean;
  legatoToNext?: boolean;
};

export type CorrectionToolId =
  | "pointer"
  | "pencil"
  | "eraser"
  | "scissors"
  | "join"
  | "flex"
  | "vibrato"
  | "gain"
  | "zoom";

export type CycleRange = {
  start: number;
  end: number;
};

export type WaveformPoint = readonly [minimum: number, maximum: number];

export type PackedWaveform = {
  encoding: "i16le-base64";
  pointCount: number;
  data: string;
};

export type WaveformData = WaveformPoint[] | PackedWaveform;

export type TempoSegment = {
  start: number;
  end: number;
  bpm: number;
};

export type TimeSignatureSegment = {
  start: number;
  end: number;
  numerator: number;
  denominator: number;
};

export type BackingTrackContent = {
  id: string;
  track: BackingTrackName;
  name: string;
  voiceProfileId: string;
  hasAudio: boolean;
  audioDurationSeconds: number;
  clips: VocalClip[];
  waveform: WaveformData;
};

export type VoiceProfileOption = {
  id: string;
  name: string;
  quality: "low" | "balanced" | "high";
  active: boolean;
};

export type PitchEditorHistoryState = {
  track: Exclude<TrackName, "Instrumental">;
  canUndo: boolean;
  canRedo: boolean;
};

export type ArrangementClip = {
  id: string;
  name: string;
  start: number;
  offset: number;
  duration: number;
  waveform: WaveformData;
};

export type ArrangementTrack = {
  id: string;
  name: string;
  parentId: string;
  collapsed: boolean;
  clips: ArrangementClip[];
};

export type StudioProject = {
  tracks?: ArrangementTrack[];
  tempo: number;
  meter: string;
  hasInstrumental: boolean;
  hasVocal: boolean;
  hasBackingAudio: boolean;
  instrumentalDurationSeconds: number;
  vocalDurationSeconds: number;
  backingAudioDurationSeconds: number;
  timelineStartSeconds: number;
  timelineDurationSeconds: number;
  initialPlayhead: number;
  initialVolume: number;
  initialInstrumentalVolume: number;
  initialLoopActive: boolean;
  initialCycleRange: CycleRange;
  backingTrackOptions: readonly BackingTrackName[];
  voiceProfiles: VoiceProfileOption[];
  initialBackingTracks: BackingTrackInstance[];
  initialTrackState: Array<{ track: string; state: TrackChannelState }>;
  initialTrackLayerState: Array<{ track: string; state: TrackLayerState }>;
  clips: VocalClip[];
  backingClips: VocalClip[];
  backingTrackContents: BackingTrackContent[];
  pitchEditorHistory: PitchEditorHistoryState[];
  vocalWaveform: WaveformData;
  instrumentalWaveform: WaveformData;
  backingWaveform: WaveformData;
  chords: Array<{ label: string; width: number }>;
  timeSignatures: TimeSignatureSegment[];
  tempoSegments: TempoSegment[];
};
