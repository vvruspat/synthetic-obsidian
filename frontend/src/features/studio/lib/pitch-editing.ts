import type { VocalClip } from "@/domain/studio";
import { PITCH_ROWS, PITCH_TOP } from "@/features/studio/lib/pitch";

export const PITCH_BOTTOM = PITCH_TOP - PITCH_ROWS + 1;
export const MIN_CLIP_WIDTH_PERCENT = 0.25;
export const MIN_GAIN_DB = -24;
export const MAX_GAIN_DB = 12;
export const MAX_VIBRATO_SCALE = 4;

const clamp = (value: number, minimum: number, maximum: number) =>
  Math.max(minimum, Math.min(maximum, value));

function selectedClipSet(clipIds: readonly string[]) {
  return new Set(clipIds);
}

function representativePitch(clip: VocalClip) {
  const pitches = (clip.pitchCurve ?? [])
    .map((point) => point.pitch)
    .filter(Number.isFinite)
    .sort((a, b) => a - b);
  return pitches.length > 0 ? pitches[Math.floor(pitches.length / 2)] : clip.pitch;
}

function pitchAtTimelinePosition(clip: VocalClip, x: number) {
  const curve = [...(clip.pitchCurve ?? [])]
    .filter((point) => Number.isFinite(point.x) && Number.isFinite(point.pitch))
    .sort((a, b) => a.x - b.x);
  if (curve.length === 0) return clip.pitch;
  if (x <= curve[0].x) return curve[0].pitch;
  if (x >= curve[curve.length - 1].x) return curve[curve.length - 1].pitch;

  for (let index = 1; index < curve.length; index += 1) {
    const previous = curve[index - 1];
    const next = curve[index];
    if (x <= next.x) {
      const progress = (x - previous.x) / Math.max(0.0001, next.x - previous.x);
      return previous.pitch + (next.pitch - previous.pitch) * progress;
    }
  }

  return curve[curve.length - 1].pitch;
}

export function moveSelectedClips(
  clips: VocalClip[],
  clipIds: readonly string[],
  requestedDeltaPitch: number,
) {
  const selectedIds = selectedClipSet(clipIds);
  const selected = clips.filter((clip) => selectedIds.has(clip.id));
  if (selected.length === 0) return clips;

  const minimumPitch = Math.min(...selected.map((clip) => clip.pitch));
  const maximumPitch = Math.max(...selected.map((clip) => clip.pitch));
  const deltaPitch = clamp(
    requestedDeltaPitch,
    PITCH_BOTTOM - minimumPitch,
    PITCH_TOP - maximumPitch,
  );

  return clips.map((clip) =>
    selectedIds.has(clip.id)
      ? {
          ...clip,
          pitch: clip.pitch + deltaPitch,
          pitchCurve: clip.pitchCurve?.map((point) => ({
            ...point,
            pitch: point.pitch + deltaPitch,
          })),
        }
      : clip,
  );
}

export function stretchClipTo(clips: VocalClip[], clipId: string, requestedEndX: number) {
  return clips.map((clip) => {
    if (clip.id !== clipId) return clip;
    const nextWidth = clamp(
      requestedEndX - clip.x,
      MIN_CLIP_WIDTH_PERCENT,
      Math.max(MIN_CLIP_WIDTH_PERCENT, 100 - clip.x),
    );
    const scale = nextWidth / Math.max(MIN_CLIP_WIDTH_PERCENT, clip.width);

    return {
      ...clip,
      width: nextWidth,
      pitchCurve: clip.pitchCurve?.map((point) => ({
        ...point,
        x: clip.x + (point.x - clip.x) * scale,
      })),
    };
  });
}

export function stretchClipStartTo(clips: VocalClip[], clipId: string, requestedStartX: number) {
  return clips.map((clip) => {
    if (clip.id !== clipId) return clip;
    const endX = clip.x + clip.width;
    const nextStartX = clamp(requestedStartX, 0, endX - MIN_CLIP_WIDTH_PERCENT);
    const nextWidth = endX - nextStartX;
    const scale = nextWidth / Math.max(MIN_CLIP_WIDTH_PERCENT, clip.width);

    return {
      ...clip,
      x: nextStartX,
      width: nextWidth,
      pitchCurve: clip.pitchCurve?.map((point) => ({
        ...point,
        x: endX - (endX - point.x) * scale,
      })),
    };
  });
}

export function scaleClipVibrato(clips: VocalClip[], clipId: string, scale: number) {
  const safeScale = clamp(scale, 0, MAX_VIBRATO_SCALE);
  return clips.map((clip) =>
    clip.id === clipId
      ? {
          ...clip,
          pitchCurve: clip.pitchCurve?.map((point) => ({
            ...point,
            pitch: clip.pitch + (point.pitch - clip.pitch) * safeScale,
          })),
        }
      : clip,
  );
}

export function setClipGain(clips: VocalClip[], clipId: string, gainDb: number) {
  const safeGain = clamp(gainDb, MIN_GAIN_DB, MAX_GAIN_DB);
  return clips.map((clip) => (clip.id === clipId ? { ...clip, gainDb: safeGain } : clip));
}

export function createManualClip(
  id: string,
  x: number,
  pitch: number,
  width: number,
  color: VocalClip["color"],
): VocalClip {
  const safeX = clamp(x, 0, 100 - MIN_CLIP_WIDTH_PERCENT);
  const safeWidth = clamp(width, MIN_CLIP_WIDTH_PERCENT, 100 - safeX);
  const safePitch = clamp(pitch, PITCH_BOTTOM, PITCH_TOP);
  return {
    id,
    label: id,
    x: safeX,
    pitch: safePitch,
    width: safeWidth,
    color,
    gainDb: 0,
    pitchCurve: [
      { x: safeX, pitch: safePitch },
      { x: safeX + safeWidth, pitch: safePitch },
    ],
  };
}

export function splitClipAt(clips: VocalClip[], clipId: string, splitX: number, rightId: string) {
  const source = clips.find((clip) => clip.id === clipId);
  if (
    !source ||
    splitX <= source.x + MIN_CLIP_WIDTH_PERCENT ||
    splitX >= source.x + source.width - MIN_CLIP_WIDTH_PERCENT
  ) {
    return clips;
  }

  const splitPitch = pitchAtTimelinePosition(source, splitX);
  const leftCurve = (source.pitchCurve ?? []).filter((point) => point.x < splitX);
  const rightCurve = (source.pitchCurve ?? []).filter((point) => point.x > splitX);
  const left: VocalClip = {
    ...source,
    width: splitX - source.x,
    legatoToNext: true,
    pitchCurve: [...leftCurve, { x: splitX, pitch: splitPitch }],
  };
  const right: VocalClip = {
    ...source,
    id: rightId,
    label: source.label === source.id ? rightId : source.label,
    x: splitX,
    width: source.x + source.width - splitX,
    legatoFromPrevious: true,
    pitchCurve: [{ x: splitX, pitch: splitPitch }, ...rightCurve],
  };

  return clips.flatMap((clip) => (clip.id === clipId ? [left, right] : [clip]));
}

export function joinClips(clips: VocalClip[], clipIds: readonly string[]) {
  const selectedIds = selectedClipSet(clipIds);
  const selected = clips
    .filter((clip) => selectedIds.has(clip.id))
    .sort((left, right) => left.x - right.x);
  if (selected.length < 2) return clips;

  const first = selected[0];
  const last = selected[selected.length - 1];
  const end = Math.max(...selected.map((clip) => clip.x + clip.width));
  const labels = selected.map((clip) => clip.label.trim()).filter(Boolean);
  const uniqueLabels = labels.filter((label, index) => labels.indexOf(label) === index);
  const duration = selected.reduce((total, clip) => total + clip.width, 0);
  const gainDb =
    selected.reduce((total, clip) => total + (clip.gainDb ?? 0) * clip.width, 0) /
    Math.max(MIN_CLIP_WIDTH_PERCENT, duration);
  const pitchCurve = selected
    .flatMap((clip) => clip.pitchCurve ?? [])
    .sort((left, right) => left.x - right.x);
  const joined: VocalClip = {
    ...first,
    label: uniqueLabels.join(" "),
    width: end - first.x,
    pitch: representativePitch({ ...first, pitchCurve }),
    gainDb,
    pitchCurve,
    legatoFromPrevious: first.legatoFromPrevious,
    legatoToNext: last.legatoToNext,
  };

  return clips
    .filter((clip) => !selectedIds.has(clip.id) || clip.id === first.id)
    .map((clip) => (clip.id === first.id ? joined : clip));
}

export function getJoinCandidateIds(
  clips: VocalClip[],
  selectedIds: readonly string[],
  clickedId: string,
): string[] {
  if (selectedIds.includes(clickedId) && selectedIds.length > 1) {
    return [...selectedIds];
  }

  const sorted = [...clips].sort((left, right) => left.x - right.x);
  const clickedIndex = sorted.findIndex((clip) => clip.id === clickedId);
  if (clickedIndex < 0 || sorted.length < 2) return [clickedId];
  if (clickedIndex === sorted.length - 1) return [sorted[clickedIndex - 1].id, clickedId];
  return [clickedId, sorted[clickedIndex + 1].id];
}

export function nextManualClipId(clips: VocalClip[]) {
  const nextNumber =
    clips.reduce((highest, clip) => {
      const match = /^n(\d+)$/.exec(clip.id);
      return match ? Math.max(highest, Number(match[1])) : highest;
    }, 0) + 1;
  return `n${String(nextNumber).padStart(3, "0")}`;
}
