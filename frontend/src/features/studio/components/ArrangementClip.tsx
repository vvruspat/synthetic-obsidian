import { TrashIcon } from "@heroicons/react/24/outline";
import { useMemo, useRef, useState } from "react";
import type { ArrangementClip as Clip } from "@/domain/studio";
import { clipStartAfterDrag } from "@/features/studio/lib/arrangement";
import { decodeWaveform, getDecodedWaveformRange } from "@/features/studio/lib/waveform";

type Props = {
  clip: Clip;
  trackId: string;
  duration: number;
  onMove(targetTrack: string, start: number): void;
  onDelete(): void;
  onSelect(): void;
};

export function ArrangementClip({ clip, trackId, duration, onMove, onDelete, onSelect }: Props) {
  const [preview, setPreview] = useState<number | null>(null);
  const gesture = useRef<{ x: number; width: number; start: number } | null>(null);
  const path = useMemo(() => {
    const waveform = decodeWaveform(clip.waveform);
    if (!waveform.pointCount) return "";
    const points: string[] = [];
    for (let x = 0; x < 256; x++) {
      const [low, high] = getDecodedWaveformRange(
        waveform,
        Math.floor((x / 256) * waveform.pointCount),
        Math.ceil(((x + 1) / 256) * waveform.pointCount),
      );
      points.push(`M${x},${24 - high * 22}L${x},${24 - low * 22}`);
    }
    return points.join(" ");
  }, [clip.waveform]);
  return (
    <div
      className={`arrangement-clip ${preview !== null ? "is-dragging" : ""}`}
      style={{
        left: `${((preview ?? clip.start) / duration) * 100}%`,
        width: `${(clip.duration / duration) * 100}%`,
      }}
    >
      <button
        type="button"
        className="arrangement-clip-drag"
        aria-label={`${clip.name}, ${clip.start.toFixed(2)} seconds. Drag to move; Delete to remove.`}
        title={`${clip.name} · ${clip.start.toFixed(2)}s · ${clip.duration.toFixed(2)}s`}
        onClick={(event) => {
          event.currentTarget.focus();
          onSelect();
        }}
        onPointerDown={(event) => {
          if (event.button !== 0) return;
          event.stopPropagation();
          event.currentTarget.focus();
          const lane = event.currentTarget.closest<HTMLElement>("[data-track-id]");
          if (!lane) return;
          onSelect();
          gesture.current = {
            x: event.clientX,
            width: lane.getBoundingClientRect().width,
            start: clip.start,
          };
          event.currentTarget.setPointerCapture(event.pointerId);
        }}
        onPointerMove={(event) => {
          const drag = gesture.current;
          if (!drag) return;
          setPreview(clipStartAfterDrag(drag.start, event.clientX - drag.x, drag.width, duration));
        }}
        onPointerUp={(event) => {
          const drag = gesture.current;
          gesture.current = null;
          setPreview(null);
          if (!drag) return;
          event.currentTarget.releasePointerCapture(event.pointerId);
          const target = document
            .elementFromPoint(event.clientX, event.clientY)
            ?.closest<HTMLElement>("[data-track-id]")?.dataset.trackId;
          if (!target) return;
          if (Math.abs(event.clientX - drag.x) > 2 || target !== trackId) {
            onMove(
              target,
              clipStartAfterDrag(drag.start, event.clientX - drag.x, drag.width, duration),
            );
          }
        }}
        onPointerCancel={() => {
          gesture.current = null;
          setPreview(null);
        }}
        onLostPointerCapture={() => {
          gesture.current = null;
          setPreview(null);
        }}
        onKeyDown={(event) => {
          event.stopPropagation();
          if (event.key === "Delete" || event.key === "Backspace") {
            event.preventDefault();
            onDelete();
          }
          if (event.key === "ArrowLeft" || event.key === "ArrowRight") {
            event.preventDefault();
            onMove(
              trackId,
              Math.max(
                0,
                clip.start + (event.key === "ArrowLeft" ? -1 : 1) * (event.shiftKey ? 1 : 0.1),
              ),
            );
          }
        }}
      >
        <span>{clip.name}</span>
        <svg viewBox="0 0 256 48" preserveAspectRatio="none" aria-hidden="true">
          <path d={path} />
        </svg>
      </button>
      <button
        type="button"
        className="arrangement-clip-delete"
        aria-label={`Delete clip ${clip.name}`}
        title="Delete clip"
        onClick={onDelete}
      >
        <TrashIcon aria-hidden="true" />
      </button>
    </div>
  );
}
