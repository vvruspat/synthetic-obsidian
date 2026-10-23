import type { ArrangementTrack, StudioProject } from "@/domain/studio";
import { decodeWaveform, getDecodedWaveformRange } from "./waveform";

export function projectTracks(project: StudioProject): ArrangementTrack[] {
  if (project.tracks) return project.tracks;
  const roots: ArrangementTrack[] = [
    {
      id: "Instrumental",
      name: "Instrumental",
      parentId: "",
      collapsed: false,
      clips: project.hasInstrumental
        ? [
            {
              id: "instrumental",
              name: "Instrumental",
              start: 0,
              offset: 0,
              duration: project.instrumentalDurationSeconds,
              waveform: project.instrumentalWaveform,
            },
          ]
        : [],
    },
    {
      id: "Voice Main",
      name: "Voice Main",
      parentId: "",
      collapsed: false,
      clips: project.hasVocal
        ? [
            {
              id: "vocal",
              name: "Voice Main",
              start: 0,
              offset: 0,
              duration: project.vocalDurationSeconds,
              waveform: project.vocalWaveform,
            },
          ]
        : [],
    },
  ];
  return [
    ...roots,
    ...project.initialBackingTracks.map((track) => {
      const content = project.backingTrackContents.find((item) => item.id === track.id);
      return {
        id: track.id,
        name: track.name,
        parentId: "Voice Main",
        collapsed: false,
        clips: [
          {
            id: `clip-${track.id}`,
            name: track.name,
            start: 0,
            offset: 0,
            duration: content?.audioDurationSeconds || project.timelineDurationSeconds,
            waveform: content?.waveform ?? [],
          },
        ],
      };
    }),
  ];
}

export function visibleTrackRows(tracks: ArrangementTrack[]) {
  const rows: Array<{ track: ArrangementTrack; depth: number; childCount: number }> = [];
  const visited = new Set<string>();
  const visit = (track: ArrangementTrack, depth: number) => {
    if (visited.has(track.id)) return;
    visited.add(track.id);
    const children = tracks.filter((candidate) => candidate.parentId === track.id);
    rows.push({ track, depth, childCount: children.length });
    if (!track.collapsed) for (const child of children) visit(child, depth + 1);
  };
  for (const track of tracks) if (!track.parentId) visit(track, 0);
  return rows;
}

export function clipStartAfterDrag(
  start: number,
  deltaPixels: number,
  timelineWidth: number,
  duration: number,
) {
  if (!Number.isFinite(deltaPixels) || timelineWidth <= 0 || duration <= 0) return start;
  return Math.max(0, start + (deltaPixels / timelineWidth) * duration);
}

export function arrangementWaveform(track: ArrangementTrack, duration: number) {
  const result: Array<readonly [number, number]> = Array.from({ length: 2048 }, () => [0, 0]);
  for (const clip of track.clips) {
    const source = decodeWaveform(clip.waveform);
    if (!source.pointCount || clip.duration <= 0) continue;
    const first = Math.max(0, Math.floor((clip.start / duration) * result.length));
    const last = Math.min(
      result.length,
      Math.ceil(((clip.start + clip.duration) / duration) * result.length),
    );
    for (let i = first; i < last; i++) {
      const relative = Math.max(0, ((i / result.length) * duration - clip.start) / clip.duration);
      const [low, high] = getDecodedWaveformRange(
        source,
        Math.floor(relative * source.pointCount),
        Math.ceil((relative + duration / result.length / clip.duration) * source.pointCount),
      );
      result[i] = [result[i][0] + low, result[i][1] + high];
    }
  }
  return result;
}
