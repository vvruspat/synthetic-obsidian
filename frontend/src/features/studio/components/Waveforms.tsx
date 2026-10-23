import { memo, useId, useMemo } from "react";
import type { VocalClip, WaveformData } from "@/domain/studio";
import { TONE_RGB } from "@/features/studio/lib/pitch";
import {
  decodeWaveform,
  getDecodedWaveformPeak,
  getDecodedWaveformRange,
} from "@/features/studio/lib/waveform";

export const PianoWaveform = memo(function PianoWaveform({
  clips,
  waveform,
  durationRatio,
}: {
  clips: VocalClip[];
  waveform: WaveformData;
  durationRatio: number;
}) {
  const gradientId = useId();
  const path = useMemo(() => {
    const decoded = decodeWaveform(waveform);
    const peak = getDecodedWaveformPeak(decoded);
    if (!decoded.pointCount || peak <= 0) return "";
    const upper: string[] = [],
      lower: string[] = [];
    for (let x = 0; x <= 2048; x++) {
      const [low, high] = getDecodedWaveformRange(
        decoded,
        Math.floor((x / 2049) * decoded.pointCount),
        Math.ceil(((x + 1) / 2049) * decoded.pointCount),
      );
      upper.push(`${x * durationRatio},${50 - (high / peak) * 30}`);
      lower.push(`${x * durationRatio},${50 - (low / peak) * 30}`);
    }
    return `M${upper.join("L")}L${lower.reverse().join("L")}Z`;
  }, [waveform, durationRatio]);
  return (
    <svg
      className="piano-waveform-canvas"
      viewBox="0 0 2048 100"
      preserveAspectRatio="none"
      aria-hidden="true"
    >
      <defs>
        <linearGradient id={gradientId}>
          <stop offset="0%" stopColor="rgb(0, 211, 235)" />
          {[...clips]
            .sort((a, b) => a.x - b.x)
            .flatMap((clip) => [
              <stop
                key={`${clip.id}-start`}
                offset={`${clip.x}%`}
                stopColor={`rgb(${TONE_RGB[clip.color]})`}
              />,
              <stop
                key={`${clip.id}-end`}
                offset={`${clip.x + clip.width}%`}
                stopColor={`rgb(${TONE_RGB[clip.color]})`}
              />,
            ])}
        </linearGradient>
      </defs>
      <path d={path} fill={`url(#${gradientId})`} />
    </svg>
  );
});
