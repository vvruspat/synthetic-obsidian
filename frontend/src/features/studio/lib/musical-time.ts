import type { TempoSegment, TimeSignatureSegment } from "@/domain/studio";

const EPSILON = 1e-7;
const VIEWBOX_WIDTH = 1000;
const CENTER_Y = 27;
const TOP_Y = 11;
const BOTTOM_Y = 43;
const MAX_GRID_LINES = 10000;

export type MetricGridLine = {
  time: number;
  position: number;
  kind: "bar" | "beat";
  bar: number;
  beat: number;
  numerator: number;
  denominator: number;
};

export type MusicalTimeline = {
  duration: number;
  tempoChanges: Array<{ start: number; bpm: number }>;
  signatureRegions: TimeSignatureSegment[];
  grid: MetricGridLine[];
};

function compact(value: number): string {
  return String(Math.round(value * 100) / 100);
}

function clampTime(value: number, duration: number): number {
  return Math.max(0, Math.min(duration, value));
}

function normaliseTempoChanges(
  segments: TempoSegment[],
  duration: number,
  fallbackTempo: number,
): Array<{ start: number; bpm: number }> {
  const changes = segments
    .filter(
      (segment) =>
        Number.isFinite(segment.start) &&
        Number.isFinite(segment.bpm) &&
        segment.bpm >= 20 &&
        segment.bpm <= 300,
    )
    .map((segment) => ({
      start: clampTime(segment.start, duration),
      bpm: segment.bpm,
    }))
    .sort((left, right) => left.start - right.start);

  const deduplicated: Array<{ start: number; bpm: number }> = [];
  for (const change of changes) {
    const previous = deduplicated.at(-1);
    if (previous && Math.abs(previous.start - change.start) < EPSILON) {
      previous.bpm = change.bpm;
    } else {
      deduplicated.push(change);
    }
  }

  if (deduplicated.length === 0 || deduplicated[0].start > EPSILON) {
    deduplicated.unshift({
      start: 0,
      bpm: Math.max(20, Math.min(300, fallbackTempo)),
    });
  }
  return deduplicated;
}

function normaliseSignatureRegions(
  segments: TimeSignatureSegment[],
  duration: number,
): TimeSignatureSegment[] {
  const changes = segments
    .filter(
      (segment) =>
        Number.isFinite(segment.start) &&
        Number.isFinite(segment.numerator) &&
        Number.isFinite(segment.denominator),
    )
    .map((segment) => ({
      start: clampTime(segment.start, duration),
      end: clampTime(segment.end, duration),
      numerator: Math.max(1, Math.min(32, Math.round(segment.numerator))),
      denominator: Math.max(1, Math.min(32, Math.round(segment.denominator))),
    }))
    .sort((left, right) => left.start - right.start);

  const deduplicated: TimeSignatureSegment[] = [];
  for (const change of changes) {
    const previous = deduplicated.at(-1);
    if (previous && Math.abs(previous.start - change.start) < EPSILON) {
      deduplicated[deduplicated.length - 1] = change;
    } else {
      deduplicated.push(change);
    }
  }

  if (deduplicated.length === 0 || deduplicated[0].start > EPSILON) {
    deduplicated.unshift({
      start: 0,
      end: deduplicated[0]?.start ?? duration,
      numerator: 4,
      denominator: 4,
    });
  }

  return deduplicated.map((signature, index) => ({
    ...signature,
    end: Math.max(
      signature.start,
      Math.min(duration, deduplicated[index + 1]?.start ?? Math.max(signature.end, duration)),
    ),
  }));
}

function tempoAt(changes: MusicalTimeline["tempoChanges"], time: number): number {
  let bpm = changes[0]?.bpm ?? 120;
  for (const change of changes) {
    if (change.start > time + EPSILON) break;
    bpm = change.bpm;
  }
  return bpm;
}

function nextTempoChangeAfter(
  changes: MusicalTimeline["tempoChanges"],
  time: number,
  limit: number,
): number {
  return changes.find((change) => change.start > time + EPSILON)?.start ?? limit;
}

function advanceQuarterNotes(
  start: number,
  quarterNotes: number,
  tempoChanges: MusicalTimeline["tempoChanges"],
  limit: number,
): number {
  let time = start;
  let remaining = quarterNotes;

  while (remaining > EPSILON && time < limit - EPSILON) {
    const bpm = tempoAt(tempoChanges, time);
    const boundary = Math.min(limit, nextTempoChangeAfter(tempoChanges, time, limit));
    const secondsNeeded = (remaining * 60) / bpm;
    if (time + secondsNeeded <= boundary + EPSILON) {
      return Math.min(limit, time + secondsNeeded);
    }

    remaining -= ((boundary - time) * bpm) / 60;
    time = boundary;
  }

  return time;
}

function createMetricGrid(
  duration: number,
  tempoChanges: MusicalTimeline["tempoChanges"],
  signatureRegions: TimeSignatureSegment[],
): MetricGridLine[] {
  const grid: MetricGridLine[] = [];
  let nextBar = 1;

  for (const signature of signatureRegions) {
    let time = signature.start;
    let beatIndex = 0;
    let currentBar = nextBar;

    while (time < signature.end - EPSILON && grid.length < MAX_GRID_LINES) {
      const beat = (beatIndex % signature.numerator) + 1;
      const isBar = beat === 1;
      if (isBar) {
        currentBar = nextBar;
        nextBar += 1;
      }

      grid.push({
        time,
        position: (time / duration) * 100,
        kind: isBar ? "bar" : "beat",
        bar: currentBar,
        beat,
        numerator: signature.numerator,
        denominator: signature.denominator,
      });

      const nextTime = advanceQuarterNotes(
        time,
        4 / signature.denominator,
        tempoChanges,
        signature.end,
      );
      if (nextTime <= time + EPSILON) break;
      time = nextTime;
      beatIndex += 1;
    }
  }

  return grid;
}

export function createMusicalTimeline(
  durationSeconds: number,
  tempoSegments: TempoSegment[],
  timeSignatures: TimeSignatureSegment[],
  fallbackTempo: number,
): MusicalTimeline {
  const duration = Math.max(0.001, durationSeconds);
  const tempoChanges = normaliseTempoChanges(tempoSegments, duration, fallbackTempo);
  const signatureRegions = normaliseSignatureRegions(timeSignatures, duration);
  return {
    duration,
    tempoChanges,
    signatureRegions,
    grid: createMetricGrid(duration, tempoChanges, signatureRegions),
  };
}

export function getTimelineBarLabelStep(grid: MetricGridLine[], horizontalZoom: number): number {
  const barCount = grid.reduce((count, line) => count + (line.kind === "bar" ? 1 : 0), 0);
  const visibleBarCount = barCount / Math.max(1, horizontalZoom);
  const minimumStep = Math.max(1, visibleBarCount / 16);
  const steps = [1, 2, 4, 8, 16, 32, 64, 128];
  return steps.find((step) => step >= minimumStep) ?? 256;
}

export function createTempoPath(timeline: MusicalTimeline): string {
  const values = timeline.tempoChanges.map((change) => change.bpm);
  const minimum = Math.min(...values);
  const maximum = Math.max(...values);
  const range = maximum - minimum;
  const yFor = (value: number) =>
    range === 0 ? CENTER_Y : BOTTOM_Y - ((value - minimum) / range) * (BOTTOM_Y - TOP_Y);

  let path = `M0 ${compact(yFor(timeline.tempoChanges[0].bpm))}`;
  for (let index = 1; index < timeline.tempoChanges.length; index += 1) {
    const change = timeline.tempoChanges[index];
    path += `H${compact((change.start / timeline.duration) * VIEWBOX_WIDTH)}V${compact(
      yFor(change.bpm),
    )}`;
  }
  return `${path}H${VIEWBOX_WIDTH}`;
}

export function getMusicalContext(timeline: MusicalTimeline, timeSeconds: number) {
  const time = clampTime(timeSeconds, timeline.duration);
  let signature = timeline.signatureRegions[0];
  for (const candidate of timeline.signatureRegions) {
    if (candidate.start > time + EPSILON) break;
    signature = candidate;
  }

  return {
    tempo: tempoAt(timeline.tempoChanges, time),
    numerator: signature.numerator,
    denominator: signature.denominator,
  };
}

export function formatMusicalPosition(timeline: MusicalTimeline, timeSeconds: number): string {
  const time = clampTime(timeSeconds, timeline.duration);
  let current = timeline.grid[0];
  let next: MetricGridLine | undefined;
  for (let index = 0; index < timeline.grid.length; index += 1) {
    const candidate = timeline.grid[index];
    if (candidate.time > time + EPSILON) {
      next = candidate;
      break;
    }
    current = candidate;
  }

  const context = getMusicalContext(timeline, time);
  const nextTime =
    next?.time ?? current.time + (60 / context.tempo) * (4 / Math.max(1, current.denominator));
  const progress = Math.max(
    0,
    Math.min(1, (time - current.time) / Math.max(EPSILON, nextTime - current.time)),
  );
  const ticks = Math.min(959, Math.floor(progress * 960));
  return `${String(current.bar).padStart(3, "0")} | ${current.beat} | ${String(ticks).padStart(
    3,
    "0",
  )}`;
}
