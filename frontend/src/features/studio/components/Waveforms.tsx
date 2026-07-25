import { memo, type RefObject, useCallback, useEffect, useMemo, useRef } from "react";
import type { VocalClip, WaveformData } from "@/domain/studio";
import { TONE_RGB } from "@/features/studio/lib/pitch";
import {
  type DecodedWaveform,
  decodeWaveform,
  getDecodedWaveformPeak,
  getDecodedWaveformRange,
} from "@/features/studio/lib/waveform";

type WaveformViewport = {
  width: number;
  height: number;
  offset: number;
  timelineWidth: number;
};

function useViewportCanvas(
  draw: (context: CanvasRenderingContext2D, viewport: WaveformViewport) => void,
  scrollContainerRef: RefObject<HTMLElement | null>,
) {
  const canvasRef = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    const canvas = canvasRef.current;
    const context = canvas?.getContext("2d");
    const scroller = scrollContainerRef.current;
    if (!canvas || !context || !scroller) return;

    let frame = 0;
    let previousRenderKey = "";
    const render = () => {
      const viewportWidth = Math.max(1, scroller.clientWidth);
      const timelineWidth = Math.max(1, scroller.scrollWidth);
      const width = Math.min(timelineWidth, viewportWidth * 2);
      const tileStep = Math.max(1, viewportWidth / 2);
      const maximumOffset = Math.max(0, timelineWidth - width);
      const offset = Math.max(
        0,
        Math.min(maximumOffset, Math.floor(scroller.scrollLeft / tileStep) * tileStep - tileStep),
      );
      canvas.style.left = `${offset}px`;
      canvas.style.width = `${width}px`;
      const height = Math.max(1, Math.round(canvas.getBoundingClientRect().height));
      const renderKey = `${offset}:${width}:${height}:${timelineWidth}`;
      if (renderKey === previousRenderKey) return;
      previousRenderKey = renderKey;

      const dpr = Math.max(
        0.75,
        Math.min(window.devicePixelRatio || 1, 2, 8192 / width, 512 / height),
      );
      canvas.width = Math.max(1, Math.round(width * dpr));
      canvas.height = Math.max(1, Math.round(height * dpr));
      context.setTransform(dpr, 0, 0, dpr, 0, 0);
      context.clearRect(0, 0, width, height);
      draw(context, {
        width,
        height,
        offset,
        timelineWidth,
      });
    };
    const scheduleDraw = () => {
      cancelAnimationFrame(frame);
      frame = requestAnimationFrame(render);
    };

    const observer = new ResizeObserver(scheduleDraw);
    observer.observe(scroller);
    if (canvas.parentElement) observer.observe(canvas.parentElement);
    scroller.addEventListener("scroll", scheduleDraw, { passive: true });
    scheduleDraw();
    return () => {
      observer.disconnect();
      scroller.removeEventListener("scroll", scheduleDraw);
      cancelAnimationFrame(frame);
    };
  }, [draw, scrollContainerRef]);

  return canvasRef;
}

function getRangeAtTimelineX(
  waveform: DecodedWaveform,
  timelineX: number,
  waveformWidth: number,
): readonly [minimum: number, maximum: number] {
  const first = Math.floor((timelineX / Math.max(1, waveformWidth)) * waveform.pointCount);
  const last = Math.ceil(((timelineX + 1) / Math.max(1, waveformWidth)) * waveform.pointCount);
  return getDecodedWaveformRange(waveform, first, Math.max(first + 1, last));
}

function traceWaveform(
  context: CanvasRenderingContext2D,
  viewport: WaveformViewport,
  waveformWidth: number,
  center: number,
  waveform: DecodedWaveform,
  amplitudeScale: number,
  start = 0,
  end = viewport.width,
) {
  const firstX = Math.max(0, Math.floor(start), Math.floor(-viewport.offset));
  const lastX = Math.min(
    viewport.width,
    Math.ceil(end),
    Math.ceil(waveformWidth - viewport.offset),
  );
  if (lastX <= firstX || waveform.pointCount === 0) return false;

  context.beginPath();
  const firstRange = getRangeAtTimelineX(waveform, viewport.offset + firstX, waveformWidth);
  context.moveTo(firstX, center - firstRange[1] * amplitudeScale);
  for (let x = firstX + 1; x <= lastX; x += 1) {
    const range = getRangeAtTimelineX(waveform, viewport.offset + x, waveformWidth);
    context.lineTo(x, center - range[1] * amplitudeScale);
  }
  for (let x = lastX; x >= firstX; x -= 1) {
    const range = getRangeAtTimelineX(waveform, viewport.offset + x, waveformWidth);
    context.lineTo(x, center - range[0] * amplitudeScale);
  }
  context.closePath();
  return true;
}

export const TrackWaveform = memo(function TrackWaveform({
  variant,
  waveform,
  durationRatio,
  scrollContainerRef,
}: {
  variant: "instrumental" | "voice" | "backing";
  waveform: WaveformData;
  durationRatio: number;
  scrollContainerRef: RefObject<HTMLElement | null>;
}) {
  const voice = variant === "voice";
  const backing = variant === "backing";
  const decoded = useMemo(() => decodeWaveform(waveform), [waveform]);
  const peak = useMemo(() => getDecodedWaveformPeak(decoded), [decoded]);
  const draw = useCallback(
    (context: CanvasRenderingContext2D, viewport: WaveformViewport) => {
      if (peak <= 0) return;

      const waveformWidth = viewport.timelineWidth * Math.max(0, Math.min(1, durationRatio));
      const center = viewport.height / 2;
      const maxAmplitude = viewport.height * (voice ? 0.34 : 0.43);
      const gradient = context.createLinearGradient(0, 0, 0, viewport.height);
      const color = voice ? "0, 100, 255" : backing ? "132, 42, 232" : "76, 78, 225";
      gradient.addColorStop(0, `rgb(${color})`);
      gradient.addColorStop(
        0.5,
        voice ? "rgb(26, 164, 255)" : backing ? "rgb(54, 238, 224)" : "rgb(133, 148, 255)",
      );
      gradient.addColorStop(1, `rgb(${color})`);
      if (!traceWaveform(context, viewport, waveformWidth, center, decoded, maxAmplitude / peak))
        return;
      context.fillStyle = gradient;
      context.shadowBlur = voice ? 5 : 4;
      context.shadowColor = `rgba(${color}, .68)`;
      context.fill();
    },
    [backing, decoded, durationRatio, peak, voice],
  );
  const canvasRef = useViewportCanvas(draw, scrollContainerRef);

  return <canvas ref={canvasRef} className={`track-waveform waveform-${variant}`} />;
});

export const PianoWaveform = memo(function PianoWaveform({
  clips,
  waveform,
  durationRatio,
  scrollContainerRef,
}: {
  clips: VocalClip[];
  waveform: WaveformData;
  durationRatio: number;
  scrollContainerRef: RefObject<HTMLElement | null>;
}) {
  const decoded = useMemo(() => decodeWaveform(waveform), [waveform]);
  const peak = useMemo(() => getDecodedWaveformPeak(decoded), [decoded]);
  const draw = useCallback(
    (context: CanvasRenderingContext2D, viewport: WaveformViewport) => {
      if (peak <= 0) return;

      const waveformWidth = viewport.timelineWidth * Math.max(0, Math.min(1, durationRatio));
      const center = viewport.height / 2;
      const amplitudeScale = (viewport.height * 0.3) / peak;
      const drawSegment = (start: number, end: number, rgb: string, alpha: number) => {
        const gradient = context.createLinearGradient(0, 0, 0, viewport.height);
        gradient.addColorStop(0, `rgba(${rgb}, ${alpha})`);
        gradient.addColorStop(0.5, `rgba(${rgb}, ${Math.min(1, alpha + 0.2)})`);
        gradient.addColorStop(1, `rgba(${rgb}, ${alpha})`);
        if (
          !traceWaveform(
            context,
            viewport,
            waveformWidth,
            center,
            decoded,
            amplitudeScale,
            start,
            end,
          )
        )
          return;
        context.fillStyle = gradient;
        context.shadowBlur = 4;
        context.shadowColor = `rgba(${rgb}, .56)`;
        context.fill();
      };

      drawSegment(0, viewport.width, TONE_RGB.cyan, 0.24);
      for (const clip of clips) {
        drawSegment(
          (clip.x / 100) * viewport.timelineWidth - viewport.offset,
          ((clip.x + clip.width) / 100) * viewport.timelineWidth - viewport.offset,
          TONE_RGB[clip.color],
          1,
        );
      }
    },
    [clips, decoded, durationRatio, peak],
  );
  const canvasRef = useViewportCanvas(draw, scrollContainerRef);

  return <canvas ref={canvasRef} className="piano-waveform-canvas" />;
});
