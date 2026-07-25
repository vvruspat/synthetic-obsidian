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
  "Pedal Tone Root",
  "Pedal Tone Fifth",
  "Contrary Motion Harmony",
  "Oblique Motion Harmony",
  "Parallel Thirds",
  "Parallel Sixths",
  "Choir Soprano",
  "Choir Alto",
  "Choir Tenor",
  "Choir Bass",
  "Two-Part Harmony",
  "Three-Part Harmony",
  "Four-Part Harmony",
  "Pop Harmony",
  "Folk Harmony",
  "Gospel Harmony",
  "Classical Choral Harmony",
  "Barbershop Harmony",
  "Suspension Harmony",
  "Passing Tone Harmony",
  "Tension-Resolution Harmony",
  "Dynamic Counterpoint",
] as const;

export type BackingTrackName = (typeof BACKING_TRACK_OPTIONS)[number];
export type TrackName = "Instrumental" | "Voice Main" | BackingTrackName;
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
  track: BackingTrackName;
  hasAudio: boolean;
  audioDurationSeconds: number;
  clips: VocalClip[];
  waveform: WaveformData;
};

export type StudioProject = {
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
  initialLoopActive: boolean;
  initialCycleRange: CycleRange;
  backingTrackOptions: readonly BackingTrackName[];
  initialBackingTracks: BackingTrackName[];
  initialTrackState: Array<{ track: string; state: TrackChannelState }>;
  initialTrackLayerState: Array<{ track: string; state: TrackLayerState }>;
  clips: VocalClip[];
  backingClips: VocalClip[];
  backingTrackContents: BackingTrackContent[];
  vocalWaveform: WaveformData;
  instrumentalWaveform: WaveformData;
  backingWaveform: WaveformData;
  chords: Array<{ label: string; width: number }>;
  timeSignatures: TimeSignatureSegment[];
  tempoSegments: TempoSegment[];
};
