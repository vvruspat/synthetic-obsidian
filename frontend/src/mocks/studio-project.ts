import {
  BACKING_TRACK_OPTIONS,
  type ClipColor,
  type StudioProject,
  type VocalClip,
} from "@/domain/studio";

const segmentWidths = [6, 5, 4, 7, 6, 6, 5, 7, 3, 4, 4, 6, 6, 7, 5, 4, 7, 4, 4];
const segmentLabels = [
  "sylla—",
  "syl—",
  "s—",
  "sylla—",
  "syll—",
  "syllable",
  "syl—",
  "syllable",
  "s—",
  "s—",
  "s—",
  "sylla—",
  "syllable",
  "syllable",
  "sylla—",
  "…",
  "syllable",
  "……",
  "syll—",
];
const segmentPitches = [
  67.92, 68.08, 67.84, 68.18, 67.76, 64.72, 64.93, 70.46, 62.31, 60.88, 55.94, 58.27, 64.82, 57.61,
  62.84, 55.92, 70.54, 57.79, 55.68,
];
const segmentColors: ClipColor[] = [
  "violet",
  "violet",
  "cyan",
  "cyan",
  "silver",
  "pink",
  "violet",
  "violet",
  "violet",
  "cyan",
  "cyan",
  "lime",
  "amber",
  "pink",
  "violet",
  "cyan",
  "cyan",
  "lime",
  "amber",
];

function createClips(): VocalClip[] {
  let offset = 0;
  return segmentWidths.map((width, index) => {
    const clip: VocalClip = {
      id: String(index + 1),
      label: segmentLabels[index],
      x: offset,
      pitch: segmentPitches[index],
      width,
      color: segmentColors[index],
    };
    offset += width;
    return clip;
  });
}

function createWaveform(seed: number): Array<readonly [number, number]> {
  return Array.from({ length: 512 }, (_, index) => {
    const phase = index / 511;
    const envelope = 0.16 + 0.68 * Math.abs(Math.sin(phase * seed + Math.sin(phase * 29)));
    const detail = 0.55 + 0.45 * Math.abs(Math.sin(phase * 701));
    const amplitude = envelope * detail;
    return [-amplitude, amplitude] as const;
  });
}

const mockClips = createClips();

export const mockStudioProject: StudioProject = {
  tempo: 94,
  meter: "4 / 4",
  hasInstrumental: true,
  hasVocal: true,
  hasBackingAudio: false,
  instrumentalDurationSeconds: 8,
  vocalDurationSeconds: 7.4,
  backingAudioDurationSeconds: 0,
  timelineStartSeconds: 28,
  timelineDurationSeconds: 8,
  initialPlayhead: 12,
  initialVolume: 76,
  initialLoopActive: true,
  initialCycleRange: { start: 25, end: 50 },
  backingTrackOptions: BACKING_TRACK_OPTIONS,
  voiceProfiles: [
    {
      id: "lena-lora-demo",
      name: "Lena",
      quality: "high",
      active: true,
    },
  ],
  initialBackingTracks: ["Third Above"],
  initialTrackState: [
    { track: "Instrumental", state: { mute: false, solo: false } },
    { track: "Voice Main", state: { mute: false, solo: false } },
    { track: "Third Above", state: { mute: false, solo: false } },
  ],
  initialTrackLayerState: [
    { track: "Voice Main", state: { audioMuted: false, notesMuted: false } },
    { track: "Third Above", state: { audioMuted: false, notesMuted: false } },
  ],
  clips: mockClips,
  backingClips: mockClips
    .filter((_, index) => index % 2 === 0)
    .map((clip) => ({ ...clip, id: `backing-${clip.id}`, pitch: clip.pitch + 4 })),
  backingTrackContents: [
    {
      track: "Third Above",
      voiceProfileId: "lena-lora-demo",
      hasAudio: false,
      audioDurationSeconds: 0,
      clips: mockClips
        .filter((_, index) => index % 2 === 0)
        .map((clip) => ({ ...clip, id: `third-above-${clip.id}`, pitch: clip.pitch + 4 })),
      waveform: [],
    },
  ],
  pitchEditorHistory: [
    { track: "Voice Main", canUndo: false, canRedo: false },
    { track: "Third Above", canUndo: false, canRedo: false },
  ],
  vocalWaveform: createWaveform(37),
  instrumentalWaveform: createWaveform(23),
  backingWaveform: [],
  chords: [
    { label: "Gm", width: 29 },
    { label: "E♭", width: 23 },
    { label: "B♭m", width: 31 },
    { label: "G♭", width: 17 },
  ],
  timeSignatures: [
    { start: 0, end: 4.16, numerator: 4, denominator: 4 },
    { start: 4.16, end: 6.64, numerator: 3, denominator: 4 },
    { start: 6.64, end: 8, numerator: 6, denominator: 8 },
  ],
  tempoSegments: [
    { start: 0, end: 1.04, bpm: 92 },
    { start: 1.04, end: 2.88, bpm: 94 },
    { start: 2.88, end: 4.64, bpm: 96 },
    { start: 4.64, end: 6.4, bpm: 93 },
    { start: 6.4, end: 7.52, bpm: 95 },
    { start: 7.52, end: 8, bpm: 94 },
  ],
};
