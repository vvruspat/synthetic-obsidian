import { describe, expect, it } from "vitest";
import type { VocalClip } from "@/domain/studio";
import {
  chordPitchClasses,
  snapClipToChord,
  snapPitchToChord,
} from "@/features/studio/lib/chord-snap";
import {
  createMusicalTimeline,
  createTempoPath,
  formatMusicalPosition,
  getMusicalContext,
  getTimelineBarLabelStep,
} from "@/features/studio/lib/musical-time";
import { gainToDecibels, gainToMeterPercent } from "@/features/studio/lib/output-meter";
import {
  createPitchPath,
  getClipVisualWidthPercent,
  getPitchFill,
  pitchToTop,
} from "@/features/studio/lib/pitch";
import {
  createManualClip,
  getJoinCandidateIds,
  joinClips,
  moveSelectedClips,
  nextManualClipId,
  scaleClipVibrato,
  setClipGain,
  splitClipAt,
  stretchClipStartTo,
  stretchClipTo,
} from "@/features/studio/lib/pitch-editing";
import { getNextTrackSelection } from "@/features/studio/lib/track-selection";
import { formatTime, getCenteredTimelineScrollLeft } from "@/features/studio/lib/transport";
import {
  decodeWaveform,
  getDecodedWaveformPeak,
  getDecodedWaveformRange,
} from "@/features/studio/lib/waveform";
import { MockPluginBridge } from "@/mocks/mock-plugin-bridge";

describe("studio value helpers", () => {
  it("maps real output gain to decibels and meter width", () => {
    expect(gainToDecibels(1)).toBe(0);
    expect(gainToDecibels(0.5)).toBeCloseTo(-6.02, 2);
    expect(gainToDecibels(0)).toBe(Number.NEGATIVE_INFINITY);
    expect(gainToMeterPercent(1)).toBe(100);
    expect(gainToMeterPercent(0.001)).toBe(0);
    expect(gainToMeterPercent(0.1)).toBeCloseTo(66.67, 2);
  });

  it("formats time", () => {
    expect(formatTime(50, 28, 8)).toBe("32.00s");
  });

  it("centers horizontal zoom on the playhead", () => {
    expect(getCenteredTimelineScrollLeft(0.25, 3200, 1000)).toBe(300);
    expect(getCenteredTimelineScrollLeft(0, 3200, 1000)).toBe(0);
    expect(getCenteredTimelineScrollLeft(1, 3200, 1000)).toBe(2200);
  });

  it("adapts timeline bar labels to horizontal zoom", () => {
    const timeline = createMusicalTimeline(
      160,
      [{ start: 0, end: 160, bpm: 94 }],
      [{ start: 0, end: 160, numerator: 4, denominator: 4 }],
      94,
    );
    expect(getTimelineBarLabelStep(timeline.grid, 1)).toBe(4);
    expect(getTimelineBarLabelStep(timeline.grid, 4)).toBe(1);
    expect(getTimelineBarLabelStep(timeline.grid, 32)).toBe(1);
  });

  it("supports single and Shift track selection", () => {
    expect(getNextTrackSelection(["Voice Main"], "Third Below", false)).toEqual(["Third Below"]);
    expect(getNextTrackSelection(["Voice Main"], "Third Below", true)).toEqual([
      "Voice Main",
      "Third Below",
    ]);
    expect(getNextTrackSelection(["Voice Main", "Third Below"], "Voice Main", true)).toEqual([
      "Third Below",
    ]);
  });

  it("decodes compact high-resolution waveform envelopes", () => {
    const data = btoa(String.fromCharCode(0x01, 0x80, 0xff, 0x7f, 0x00, 0xc0, 0x00, 0x40));
    const waveform = decodeWaveform({
      encoding: "i16le-base64",
      pointCount: 2,
      data,
    });

    expect(waveform.pointCount).toBe(2);
    expect(getDecodedWaveformPeak(waveform)).toBe(1);
    const [minimum, maximum] = getDecodedWaveformRange(waveform, 1, 2);
    expect(minimum).toBeCloseTo(-16384 / 32767);
    expect(maximum).toBeCloseTo(16384 / 32767);
  });

  it("maps pitch into the visible piano range", () => {
    expect(pitchToTop(72)).toBe(0);
    expect(pitchToTop(48)).toBe(96);
    expect(pitchToTop(100)).toBe(0);
  });

  it("builds clip color and curve data", () => {
    const clips: VocalClip[] = [
      {
        id: "1",
        label: "a",
        x: 0,
        pitch: 60.25,
        width: 8,
        color: "cyan",
        pitchCurve: [
          { x: 0, pitch: 60.1 },
          { x: 4, pitch: 60.4 },
          { x: 8, pitch: 60.2 },
        ],
      },
      { id: "2", label: "b", x: 8, pitch: 61, width: 8, color: "violet" },
    ];

    expect(getPitchFill(clips[0]).cents).toBe(25);
    expect(createPitchPath(clips)).toContain("L");
    expect(createPitchPath(clips)).not.toContain("C");
    expect(
      createPitchPath([
        { ...clips[0], legatoToNext: true },
        { ...clips[1], legatoFromPrevious: true },
      ]),
    ).toContain("C");
  });

  it("preserves the analyzed duration of short clips", () => {
    expect(getClipVisualWidthPercent(0.08)).toBe(0.08);
    expect(getClipVisualWidthPercent(-0.08)).toBe(0);
  });

  it("moves selected pitch clips vertically as a clamped group", () => {
    const clips: VocalClip[] = [
      {
        id: "n001",
        label: "one",
        x: 2,
        pitch: 60,
        width: 10,
        color: "cyan",
        pitchCurve: [{ x: 4, pitch: 60.25 }],
      },
      { id: "n002", label: "two", x: 20, pitch: 70, width: 8, color: "violet" },
    ];
    const moved = moveSelectedClips(clips, ["n001", "n002"], 8);

    expect(moved[0].x).toBe(2);
    expect(moved[1].x).toBe(20);
    expect(moved[0].pitch).toBe(62);
    expect(moved[1].pitch).toBe(72);
    expect(moved[0].pitchCurve?.[0]).toEqual({ x: 4, pitch: 62.25 });
  });

  it("snaps backing pitches to the active chord tones", () => {
    expect(chordPitchClasses("Gm7")).toEqual([2, 5, 7, 10]);
    expect(chordPitchClasses("E♭maj7")).toEqual([2, 3, 7, 10]);
    expect(chordPitchClasses("Bm7b5")).toEqual([2, 5, 9, 11]);
    expect(chordPitchClasses("C(5)")).toEqual([0, 7]);
    expect(snapPitchToChord(61.8, "C", 48, 72)).toBe(60);
    expect(snapPitchToChord(62.8, "C", 48, 72)).toBe(64);

    const clip = createManualClip("n010", 55, 66, 8, "cyan");
    const snapped = snapClipToChord(
      clip,
      [
        { label: "C", width: 50 },
        { label: "Dm", width: 50 },
      ],
      48,
      72,
    );
    expect(snapped.pitch).toBe(65);
    expect(snapped.pitchCurve?.map(({ pitch }) => pitch)).toEqual([65, 65]);
  });

  it("creates, stretches, splits, and joins manual clips", () => {
    const clip = createManualClip("n001", 10, 64, 20, "cyan");
    const stretched = stretchClipTo([clip], clip.id, 40);
    expect(stretched[0].width).toBe(30);
    expect(stretched[0].pitchCurve?.at(-1)?.x).toBe(40);

    const stretchedFromStart = stretchClipStartTo([clip], clip.id, 5);
    expect(stretchedFromStart[0].x).toBe(5);
    expect(stretchedFromStart[0].width).toBe(25);
    expect(stretchedFromStart[0].pitchCurve?.[0].x).toBe(5);
    expect(stretchedFromStart[0].pitchCurve?.at(-1)?.x).toBe(30);

    const split = splitClipAt(stretched, clip.id, 25, "n002");
    expect(split.map(({ id, x, width }) => ({ id, x, width }))).toEqual([
      { id: "n001", x: 10, width: 15 },
      { id: "n002", x: 25, width: 15 },
    ]);
    expect(split[0].legatoToNext).toBe(true);
    expect(split[1].legatoFromPrevious).toBe(true);

    const joined = joinClips(split, ["n001", "n002"]);
    expect(joined).toHaveLength(1);
    expect(joined[0].width).toBe(30);
    expect(getJoinCandidateIds(split, [], "n001")).toEqual(["n001", "n002"]);
  });

  it("edits vibrato and gain without changing the note anchor", () => {
    const clip: VocalClip = {
      id: "n004",
      label: "vibe",
      x: 0,
      pitch: 60,
      width: 10,
      color: "pink",
      pitchCurve: [
        { x: 0, pitch: 59.8 },
        { x: 5, pitch: 60.3 },
      ],
    };

    const vibrato = scaleClipVibrato([clip], clip.id, 2)[0];
    expect(vibrato.pitch).toBe(60);
    expect(vibrato.pitchCurve?.[0].pitch).toBeCloseTo(59.6);
    expect(vibrato.pitchCurve?.[1].pitch).toBeCloseTo(60.6);
    expect(setClipGain([clip], clip.id, 30)[0].gainDb).toBe(12);
    expect(nextManualClipId([clip, { ...clip, id: "n009" }])).toBe("n010");
  });

  it("builds a constant tempo line from a single analyzed segment", () => {
    const timeline = createMusicalTimeline(
      4,
      [{ start: 0, end: 4, bpm: 94 }],
      [{ start: 0, end: 4, numerator: 4, denominator: 4 }],
      120,
    );
    expect(createTempoPath(timeline)).toBe("M0 27H1000");
  });

  it("builds tempo, signature, grid, and transport from one musical timeline", () => {
    const timeline = createMusicalTimeline(
      4,
      [
        { start: 0, end: 1, bpm: 120 },
        { start: 1, end: 4, bpm: 60 },
      ],
      [
        { start: 0, end: 2, numerator: 4, denominator: 4 },
        { start: 2, end: 4, numerator: 3, denominator: 4 },
      ],
      120,
    );

    expect(createTempoPath(timeline)).toBe("M0 11H250V43H1000");
    expect(timeline.signatureRegions).toEqual([
      { start: 0, end: 2, numerator: 4, denominator: 4 },
      { start: 2, end: 4, numerator: 3, denominator: 4 },
    ]);
    expect(timeline.grid.map(({ time, kind }) => [time, kind])).toEqual([
      [0, "bar"],
      [0.5, "beat"],
      [1, "beat"],
      [2, "bar"],
      [3, "beat"],
    ]);
    expect(formatMusicalPosition(timeline, 3.5)).toBe("002 | 2 | 480");
    expect(getMusicalContext(timeline, 0.75)).toEqual({
      tempo: 120,
      numerator: 4,
      denominator: 4,
    });
    expect(getMusicalContext(timeline, 2.5)).toEqual({
      tempo: 60,
      numerator: 3,
      denominator: 4,
    });
  });

  it("uses the signature denominator as the grid beat unit", () => {
    const timeline = createMusicalTimeline(
      4,
      [{ start: 0, end: 4, bpm: 60 }],
      [{ start: 0, end: 4, numerator: 6, denominator: 8 }],
      60,
    );

    expect(timeline.grid.map(({ time, kind }) => [time, kind])).toEqual([
      [0, "bar"],
      [0.5, "beat"],
      [1, "beat"],
      [1.5, "beat"],
      [2, "beat"],
      [2.5, "beat"],
      [3, "bar"],
      [3.5, "beat"],
    ]);
  });

  it("supports the bidirectional mock plugin bridge", () => {
    const bridge = new MockPluginBridge();
    const received: number[] = [];
    const unsubscribe = bridge.subscribe((event) => {
      if (event.type === "volume-state") {
        received.push(event.value);
      }
    });

    bridge.send({ type: "set-volume", value: 72 });
    bridge.emit({ type: "volume-state", value: 68 });
    unsubscribe();
    bridge.emit({ type: "volume-state", value: 64 });

    expect(bridge.commands).toEqual([{ type: "set-volume", value: 72 }]);
    expect(received).toEqual([68]);
  });
});
