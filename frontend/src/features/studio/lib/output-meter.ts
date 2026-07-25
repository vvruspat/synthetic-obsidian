const METER_FLOOR_DB = -60;

export function gainToDecibels(gain: number): number {
  return gain > 0 ? 20 * Math.log10(gain) : Number.NEGATIVE_INFINITY;
}

export function gainToMeterPercent(gain: number): number {
  const decibels = gainToDecibels(gain);
  if (!Number.isFinite(decibels)) return 0;
  return Math.max(0, Math.min(100, ((decibels - METER_FLOOR_DB) / -METER_FLOOR_DB) * 100));
}
