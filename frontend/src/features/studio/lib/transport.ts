export function formatTime(
  playhead: number,
  timelineStartSeconds: number,
  timelineDurationSeconds: number,
) {
  const seconds = timelineStartSeconds + (playhead / 100) * timelineDurationSeconds;
  return `${seconds.toFixed(2)}s`;
}

export function getCenteredTimelineScrollLeft(
  position: number,
  timelineWidth: number,
  viewportWidth: number,
): number {
  const maximum = Math.max(0, timelineWidth - viewportWidth);
  return Math.max(
    0,
    Math.min(maximum, Math.max(0, Math.min(1, position)) * timelineWidth - viewportWidth / 2),
  );
}

export function getGestureZoom(
  currentZoom: number,
  wheelDelta: number,
  minimumZoom: number,
  maximumZoom: number,
) {
  const boundedDelta = Math.max(-48, Math.min(48, wheelDelta));
  const nextZoom = currentZoom * Math.exp(-boundedDelta * 0.006);
  return Math.max(minimumZoom, Math.min(maximumZoom, Number(nextZoom.toFixed(3))));
}

export function getMinimumCycleWidthPercent(timelineDurationSeconds: number): number {
  if (!Number.isFinite(timelineDurationSeconds) || timelineDurationSeconds <= 0) {
    return 100;
  }
  return Math.min(100, 100 / timelineDurationSeconds);
}
