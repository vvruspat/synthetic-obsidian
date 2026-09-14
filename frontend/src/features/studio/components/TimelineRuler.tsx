import { memo, type PointerEvent as ReactPointerEvent, useRef } from "react";
import type { CycleRange } from "@/domain/studio";
import { getTimelineBarLabelStep, type MetricGridLine } from "@/features/studio/lib/musical-time";
import { getMinimumCycleWidthPercent } from "@/features/studio/lib/transport";

type CycleDrag = {
  mode: "create" | "move" | "resize-start" | "resize-end";
  pointerId: number;
  anchor: number;
  initial: CycleRange;
  current: CycleRange;
  initialActive: boolean;
  active: boolean;
  moved: boolean;
};

type TimelineRulerProps = {
  cycleRange: CycleRange;
  looping: boolean;
  playhead: number;
  playheadLabel: string;
  timelineStartSeconds: number;
  timelineDurationSeconds: number;
  horizontalZoom: number;
  grid: MetricGridLine[];
  onCycleRangeChange(range: CycleRange, active: boolean): void;
  onCycleRangeCommit(range: CycleRange, active: boolean): void;
  onLoopingChange(active: boolean): void;
  onPlayheadChange(position: number): void;
  onPlayheadPointerDown(event: ReactPointerEvent<HTMLButtonElement>): void;
};

export const TimelineRuler = memo(function TimelineRuler({
  cycleRange,
  looping,
  playhead,
  playheadLabel,
  timelineStartSeconds,
  timelineDurationSeconds,
  horizontalZoom,
  grid,
  onCycleRangeChange,
  onCycleRangeCommit,
  onLoopingChange,
  onPlayheadChange,
  onPlayheadPointerDown,
}: TimelineRulerProps) {
  const minimumCycleWidth = getMinimumCycleWidthPercent(timelineDurationSeconds);
  const barLabelStep = getTimelineBarLabelStep(grid, horizontalZoom);
  const barMarks = grid.filter(
    (line) => line.kind === "bar" && (line.bar - 1) % barLabelStep === 0,
  );
  const snapPoints = [0, ...grid.map((line) => line.position), 100];
  const dragRef = useRef<CycleDrag | null>(null);
  const snap = (value: number) => {
    const clamped = Math.max(0, Math.min(100, value));
    return snapPoints.reduce((closest, candidate) =>
      Math.abs(candidate - clamped) < Math.abs(closest - clamped) ? candidate : closest,
    );
  };
  const pointerPercent = (event: ReactPointerEvent<HTMLFieldSetElement>) => {
    const rect = event.currentTarget.getBoundingClientRect();
    return snap(((event.clientX - rect.left) / rect.width) * 100);
  };

  const handlePointerDown = (event: ReactPointerEvent<HTMLFieldSetElement>) => {
    if (event.button !== 0) return;
    const target = event.target as HTMLElement;
    const handle = target.closest<HTMLElement>("[data-cycle-handle]")?.dataset.cycleHandle;
    const insideCycle = Boolean(target.closest(".cycle-area"));
    const anchor = pointerPercent(event);
    const mode: CycleDrag["mode"] =
      handle === "start"
        ? "resize-start"
        : handle === "end"
          ? "resize-end"
          : insideCycle
            ? "move"
            : "create";

    dragRef.current = {
      mode,
      pointerId: event.pointerId,
      anchor,
      initial: cycleRange,
      current: cycleRange,
      initialActive: looping,
      active: mode === "create" || mode.startsWith("resize") ? true : looping,
      moved: false,
    };

    if (mode === "create") {
      const end = Math.min(100, anchor + minimumCycleWidth);
      const range = { start: end - minimumCycleWidth, end };
      dragRef.current.current = range;
      onCycleRangeChange(range, true);
    }

    event.currentTarget.setPointerCapture(event.pointerId);
    event.preventDefault();
  };

  const handlePointerMove = (event: ReactPointerEvent<HTMLFieldSetElement>) => {
    const drag = dragRef.current;
    if (!drag || drag.pointerId !== event.pointerId) return;
    const pointer = pointerPercent(event);
    if (Math.abs(pointer - drag.anchor) >= 0.1) {
      drag.moved = true;
    }

    if (drag.mode === "create") {
      let start = Math.min(drag.anchor, pointer);
      let end = Math.max(drag.anchor, pointer);
      if (end - start < minimumCycleWidth) {
        if (pointer < drag.anchor) {
          start = Math.max(0, drag.anchor - minimumCycleWidth);
          end = drag.anchor;
        } else {
          start = drag.anchor;
          end = Math.min(100, drag.anchor + minimumCycleWidth);
        }
      }
      drag.current = { start, end };
      onCycleRangeChange(drag.current, drag.active);
      return;
    }

    if (drag.mode === "move") {
      const width = drag.initial.end - drag.initial.start;
      const delta = pointer - drag.anchor;
      const start = Math.max(0, Math.min(100 - width, snap(drag.initial.start + delta)));
      drag.current = { start, end: start + width };
      onCycleRangeChange(drag.current, drag.active);
      return;
    }

    if (drag.mode === "resize-start") {
      drag.current = {
        start: Math.max(0, Math.min(pointer, drag.initial.end - minimumCycleWidth)),
        end: drag.initial.end,
      };
      onCycleRangeChange(drag.current, drag.active);
      return;
    }

    drag.current = {
      start: drag.initial.start,
      end: Math.min(100, Math.max(pointer, drag.initial.start + minimumCycleWidth)),
    };
    onCycleRangeChange(drag.current, drag.active);
  };

  const handlePointerUp = (event: ReactPointerEvent<HTMLFieldSetElement>) => {
    const drag = dragRef.current;
    if (!drag || drag.pointerId !== event.pointerId) return;
    if (drag.mode === "move" && !drag.moved) {
      onLoopingChange(!looping);
    } else {
      onCycleRangeCommit(drag.current, drag.active);
    }
    dragRef.current = null;
    if (event.currentTarget.hasPointerCapture(event.pointerId)) {
      event.currentTarget.releasePointerCapture(event.pointerId);
    }
  };

  const handlePointerCancel = (event: ReactPointerEvent<HTMLFieldSetElement>) => {
    const drag = dragRef.current;
    if (drag?.pointerId === event.pointerId) {
      onCycleRangeChange(drag.initial, drag.initialActive);
      dragRef.current = null;
    }
  };

  return (
    <fieldset
      className="timeline-ruler"
      onPointerDown={handlePointerDown}
      onPointerMove={handlePointerMove}
      onPointerUp={handlePointerUp}
      onPointerCancel={handlePointerCancel}
    >
      <legend className="sr-only">
        Timeline from {timelineStartSeconds} to {timelineStartSeconds + timelineDurationSeconds}{" "}
        seconds
      </legend>
      <button
        type="button"
        className={`cycle-area ${looping ? "is-active" : ""}`}
        style={{
          left: `${cycleRange.start}%`,
          minWidth: `${minimumCycleWidth}%`,
          width: `${cycleRange.end - cycleRange.start}%`,
        }}
        aria-label={`Cycle area from ${(
          timelineStartSeconds + (cycleRange.start / 100) * timelineDurationSeconds
        ).toFixed(2)} to ${(
          timelineStartSeconds + (cycleRange.end / 100) * timelineDurationSeconds
        ).toFixed(2)} seconds`}
        aria-pressed={looping}
        title="Drag to move · drag edges to resize · click to toggle cycle"
        onKeyDown={(event) => {
          if (event.key === "Enter" || event.key === " ") {
            event.preventDefault();
            onLoopingChange(!looping);
          }
        }}
      >
        <i className="cycle-locator is-left" data-cycle-handle="start" aria-hidden="true" />
        <i className="cycle-locator is-right" data-cycle-handle="end" aria-hidden="true" />
      </button>
      <div className="ruler-metric-grid" aria-hidden="true">
        {grid.map((line) => (
          <i
            className={`metric-${line.kind}`}
            style={{ left: `${line.position}%` }}
            key={`${line.time}:${line.kind}`}
          />
        ))}
      </div>
      <button
        type="button"
        className="ruler-seek-area"
        aria-label="Move playhead"
        title="Click or drag to move playhead"
        onPointerDown={onPlayheadPointerDown}
      />
      {barMarks.map((line) => (
        <div className="ruler-bar-label" style={{ left: `${line.position}%` }} key={line.bar}>
          <span>{line.bar}</span>
        </div>
      ))}
      <button
        type="button"
        className="playhead-handle"
        style={{ left: `${playhead}%` }}
        aria-label={`Playhead at ${playheadLabel}`}
        title="Drag playhead"
        onPointerDown={onPlayheadPointerDown}
        onKeyDown={(event) => {
          if (event.key === "ArrowLeft" || event.key === "ArrowRight") {
            event.preventDefault();
            onPlayheadChange(
              Math.max(0, Math.min(100, playhead + (event.key === "ArrowLeft" ? -0.25 : 0.25))),
            );
          } else if (event.key === "Home" || event.key === "End") {
            event.preventDefault();
            onPlayheadChange(event.key === "Home" ? 0 : 100);
          }
        }}
      />
    </fieldset>
  );
});
