import type { WaveformData } from "@/domain/studio";

export type DecodedWaveform = {
  pointCount: number;
  values: Float32Array;
};

export function decodeWaveform(waveform: WaveformData): DecodedWaveform {
  if (Array.isArray(waveform)) {
    const values = new Float32Array(waveform.length * 2);
    for (let index = 0; index < waveform.length; index += 1) {
      values[index * 2] = waveform[index][0];
      values[index * 2 + 1] = waveform[index][1];
    }
    return { pointCount: waveform.length, values };
  }

  if (waveform.encoding !== "i16le-base64" || waveform.pointCount <= 0 || !waveform.data) {
    return { pointCount: 0, values: new Float32Array() };
  }

  try {
    const bytes = Uint8Array.from(atob(waveform.data), (character) => character.charCodeAt(0));
    const pointCount = Math.min(waveform.pointCount, Math.floor(bytes.byteLength / 4));
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const values = new Float32Array(pointCount * 2);
    for (let index = 0; index < pointCount; index += 1) {
      values[index * 2] = view.getInt16(index * 4, true) / 32767;
      values[index * 2 + 1] = view.getInt16(index * 4 + 2, true) / 32767;
    }
    return { pointCount, values };
  } catch {
    return { pointCount: 0, values: new Float32Array() };
  }
}

export function getDecodedWaveformPeak(waveform: DecodedWaveform): number {
  let peak = 0;
  for (let index = 0; index < waveform.values.length; index += 1) {
    peak = Math.max(peak, Math.abs(waveform.values[index]));
  }
  return peak;
}

export function getDecodedWaveformRange(
  waveform: DecodedWaveform,
  firstIndex: number,
  lastIndex: number,
): readonly [minimum: number, maximum: number] {
  const first = Math.max(0, Math.min(waveform.pointCount - 1, firstIndex));
  const last = Math.max(first + 1, Math.min(waveform.pointCount, lastIndex));
  let minimum = 0;
  let maximum = 0;
  for (let index = first; index < last; index += 1) {
    minimum = Math.min(minimum, waveform.values[index * 2]);
    maximum = Math.max(maximum, waveform.values[index * 2 + 1]);
  }
  return [minimum, maximum];
}
