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
