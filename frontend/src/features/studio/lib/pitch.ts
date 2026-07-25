import type { VocalClip } from "@/domain/studio";

export const PITCH_ROWS = 25;
export const PITCH_TOP = 72;
export const PITCH_ROW_HEIGHT = 100 / PITCH_ROWS;

const NOTE_NAMES = ["C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"];
const BLACK_NOTES = new Set([1, 3, 6, 8, 10]);

export const WHITE_KEY_PITCHES = [72, 71, 69, 67, 65, 64, 62, 60, 59, 57, 55, 53, 52, 50, 48];
export const BLACK_KEY_PITCHES = [70, 68, 66, 63, 61, 58, 56, 54, 51, 49];

export const PIANO_KEYS = Array.from({ length: PITCH_ROWS }, (_, index) => {
  const midi = PITCH_TOP - index;
  const note = ((midi % 12) + 12) % 12;
  return {
    label: `${NOTE_NAMES[note]}${Math.floor(midi / 12) - 1}`,
    black: BLACK_NOTES.has(note),
  };
});

export const TONE_RGB: Record<VocalClip["color"], string> = {
  cyan: "0, 211, 235",
  violet: "125, 28, 246",
  pink: "248, 0, 158",
  amber: "255, 174, 0",
  lime: "41, 222, 83",
  silver: "191, 211, 232",
};

export function pitchToTop(pitch: number) {
  return Math.max(0, Math.min(100 - PITCH_ROW_HEIGHT, (PITCH_TOP - pitch) * PITCH_ROW_HEIGHT));
}

export function getWhiteKeyBounds(index: number) {
  const pitch = WHITE_KEY_PITCHES[index];
  const center = pitchToTop(pitch) + PITCH_ROW_HEIGHT / 2;
  const higherCenter =
    index > 0 ? pitchToTop(WHITE_KEY_PITCHES[index - 1]) + PITCH_ROW_HEIGHT / 2 : null;
  const lowerCenter =
    index < WHITE_KEY_PITCHES.length - 1
      ? pitchToTop(WHITE_KEY_PITCHES[index + 1]) + PITCH_ROW_HEIGHT / 2
      : null;
  const top = higherCenter === null ? 0 : (higherCenter + center) / 2;
  const bottom = lowerCenter === null ? 100 : (center + lowerCenter) / 2;

  return { top, height: bottom - top };
}

export function getPitchFill(clip: VocalClip) {
  const cents = (clip.pitch - Math.round(clip.pitch)) * 100;
  const spill = Math.min(50, Math.abs(cents));
  const rgb = TONE_RGB[clip.color];
  const pale = `rgba(${rgb}, .27)`;
  const solid = `rgba(${rgb}, .88)`;

  return {
    cents,
    background:
      cents >= 0
        ? `linear-gradient(180deg, ${pale} 0 ${spill}%, ${solid} ${spill}% 100%)`
        : `linear-gradient(180deg, ${solid} 0 ${100 - spill}%, ${pale} ${100 - spill}% 100%)`,
  };
}

export function getClipVisualWidthPercent(width: number) {
  return Math.max(0, width);
}

export function createPitchPath(clips: VocalClip[]) {
  const sorted = [...clips].sort((a, b) => a.x - b.x);
  let path = "";
  let previous:
    | {
        clip: VocalClip;
        point: { x: number; y: number };
      }
    | undefined;

  for (const clip of sorted) {
    const curve =
      clip.pitchCurve && clip.pitchCurve.length > 1
        ? clip.pitchCurve
            .filter((point) => Number.isFinite(point.x) && Number.isFinite(point.pitch))
            .sort((a, b) => a.x - b.x)
            .map((point) => ({
              x: point.x,
              y: pitchToTop(point.pitch) + PITCH_ROW_HEIGHT / 2,
            }))
        : [
            {
              x: clip.x,
              y: pitchToTop(clip.pitch) + PITCH_ROW_HEIGHT / 2,
            },
            {
              x: clip.x + clip.width,
              y: pitchToTop(clip.pitch) + PITCH_ROW_HEIGHT / 2,
            },
          ];
    if (curve.length < 2) continue;

    const first = curve[0];
    if (previous?.clip.legatoToNext && clip.legatoFromPrevious) {
      const distance = first.x - previous.point.x;
      path += ` M ${previous.point.x} ${previous.point.y} C ${previous.point.x + distance * 0.48} ${previous.point.y}, ${first.x - distance * 0.48} ${first.y}, ${first.x} ${first.y}`;
    }

    path += ` M ${first.x} ${first.y}`;
    for (const point of curve.slice(1)) path += ` L ${point.x} ${point.y}`;
    previous = { clip, point: curve[curve.length - 1] };
  }

  return path.trim();
}
