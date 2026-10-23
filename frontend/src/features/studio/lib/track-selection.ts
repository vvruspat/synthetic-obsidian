import type { TrackName } from "@/domain/studio";

export function getNextTrackSelection(
  current: readonly TrackName[],
  track: TrackName,
  additive: boolean,
): TrackName[] {
  if (!additive) return [track];
  if (!current.includes(track)) return [...current, track];
  return current.filter((item) => item !== track);
}
