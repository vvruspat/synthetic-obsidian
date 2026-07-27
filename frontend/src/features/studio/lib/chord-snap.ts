import type { VocalClip } from "@/domain/studio";

type ChordRegion = {
  label: string;
  width: number;
};

const ROOT_PITCH_CLASSES: Record<string, number> = {
  C: 0,
  D: 2,
  E: 4,
  F: 5,
  G: 7,
  A: 9,
  B: 11,
};

export function chordLabelAtPosition(chords: ChordRegion[], position: number) {
  let regionStart = 0;
  for (let index = 0; index < chords.length; index += 1) {
    const chord = chords[index];
    const regionEnd = regionStart + Math.max(0, chord.width);
    if (position < regionEnd || index === chords.length - 1) return chord.label;
    regionStart = regionEnd;
  }
  return null;
}

export function chordPitchClasses(label: string): number[] {
  const normalized = label.replace(/♭/g, "b").replace(/♯/g, "#").trim();
  const rootMatch = normalized.match(/^([A-Ga-g])([#b]?)/);
  if (!rootMatch) return [];

  let root = ROOT_PITCH_CLASSES[rootMatch[1].toUpperCase()];
  if (rootMatch[2] === "#") root += 1;
  if (rootMatch[2] === "b") root -= 1;
  root = (root + 12) % 12;

  const quality = normalized.slice(rootMatch[0].length).trim().toLowerCase();
  const halfDiminished = quality.includes("m7b5") || quality.includes("ø");
  const diminished = halfDiminished || quality.includes("dim") || quality.includes("°");
  const augmented = quality.includes("aug") || quality.startsWith("+");
  const power = quality === "5" || quality.startsWith("(5)");
  const suspended2 = quality.includes("sus2");
  const suspended4 = !suspended2 && quality.includes("sus");
  const minor =
    !diminished &&
    (/^m(?!aj)/.test(quality) || quality.startsWith("min") || quality.includes("minor"));

  const intervals = new Set<number>();
  if (power) {
    intervals.add(0);
    intervals.add(7);
  } else if (diminished) {
    intervals.add(0);
    intervals.add(3);
    intervals.add(6);
  } else if (augmented) {
    intervals.add(0);
    intervals.add(4);
    intervals.add(8);
  } else if (suspended2) {
    intervals.add(0);
    intervals.add(2);
    intervals.add(7);
  } else if (suspended4) {
    intervals.add(0);
    intervals.add(5);
    intervals.add(7);
  } else {
    intervals.add(0);
    intervals.add(minor ? 3 : 4);
    intervals.add(7);
  }

  if (quality.includes("b5")) {
    intervals.delete(7);
    intervals.add(6);
  } else if (quality.includes("#5")) {
    intervals.delete(7);
    intervals.add(8);
  }
  if (quality.includes("maj7") || quality.includes("major7") || quality.includes("△")) {
    intervals.add(11);
  } else if (halfDiminished || /(^|[^0-9])7/.test(quality)) {
    intervals.add(10);
  }
  if (/(^|[^0-9])6/.test(quality)) intervals.add(9);
  if (/(^|[^0-9])9/.test(quality)) intervals.add(2);
  if (/(^|[^0-9])11/.test(quality)) intervals.add(5);
  if (/(^|[^0-9])13/.test(quality)) intervals.add(9);

  return [...intervals]
    .map((interval) => (root + interval) % 12)
    .sort((left, right) => left - right);
}

export function snapPitchToChord(
  pitch: number,
  chordLabel: string | null,
  minimumPitch = 0,
  maximumPitch = 127,
) {
  if (!chordLabel) return pitch;
  const pitchClasses = new Set(chordPitchClasses(chordLabel));
  if (pitchClasses.size === 0) return pitch;

  let bestPitch = pitch;
  let bestDistance = Number.POSITIVE_INFINITY;
  for (
    let candidate = Math.ceil(minimumPitch);
    candidate <= Math.floor(maximumPitch);
    candidate += 1
  ) {
    if (!pitchClasses.has(((candidate % 12) + 12) % 12)) continue;
    const distance = Math.abs(candidate - pitch);
    if (distance < bestDistance) {
      bestDistance = distance;
      bestPitch = candidate;
    }
  }
  return bestPitch;
}

export function snapClipToChord(
  clip: VocalClip,
  chords: ChordRegion[],
  minimumPitch: number,
  maximumPitch: number,
): VocalClip {
  const snappedPitch = snapPitchToChord(
    clip.pitch,
    chordLabelAtPosition(chords, clip.x),
    minimumPitch,
    maximumPitch,
  );
  const pitchDelta = snappedPitch - clip.pitch;
  if (Math.abs(pitchDelta) < 0.0001) return clip;
  return {
    ...clip,
    pitch: snappedPitch,
    pitchCurve: clip.pitchCurve?.map((point) => ({
      ...point,
      pitch: point.pitch + pitchDelta,
    })),
  };
}

export function snapClipsToChords(
  clips: VocalClip[],
  clipIds: readonly string[],
  chords: ChordRegion[],
  minimumPitch: number,
  maximumPitch: number,
) {
  const selectedIds = new Set(clipIds);
  return clips.map((clip) =>
    selectedIds.has(clip.id) ? snapClipToChord(clip, chords, minimumPitch, maximumPitch) : clip,
  );
}
