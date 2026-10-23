import { describe, expect, it } from "vitest";
import type { ArrangementTrack } from "@/domain/studio";
import {
  arrangementRows,
  arrangementWaveform,
  clipStartAfterDrag,
  visibleTrackRows,
} from "./arrangement";

const track = (id: string, parentId = "", collapsed = false): ArrangementTrack => ({
  id,
  name: id,
  parentId,
  collapsed,
  clips: [],
});
describe("arrangement hierarchy", () => {
  it("keeps backing tracks below their parent, independent of insertion order", () => {
    const tracks = [
      track("a"),
      track("b"),
      track("b1", "b"),
      track("a1", "a"),
      track("a2", "a"),
      track("a11", "a1"),
    ];
    expect(visibleTrackRows(tracks).map(({ track, depth }) => [track.id, depth])).toEqual([
      ["a", 0],
      ["a1", 1],
      ["a11", 2],
      ["a2", 1],
      ["b", 0],
      ["b1", 1],
    ]);
  });
  it("collapses descendants without changing or deleting them", () => {
    const tracks = [track("a", "", true), track("a1", "a"), track("a11", "a1"), track("b")];
    expect(visibleTrackRows(tracks).map(({ track }) => track.id)).toEqual(["a", "b"]);
    expect(tracks).toHaveLength(4);
    tracks[0].collapsed = false;
    expect(visibleTrackRows(tracks)).toHaveLength(4);
  });
  it("does not impose a track or backing style limit", () => {
    const tracks = [
      track("lead"),
      ...Array.from({ length: 100 }, (_, i) => track(String(i), "lead")),
    ];
    expect(visibleTrackRows(tracks)).toHaveLength(101);
    expect(visibleTrackRows(tracks)[0].childCount).toBe(100);
  });
  it("does not recurse on malformed parent cycles", () => {
    expect(visibleTrackRows([track("a", "b"), track("b", "a")])).toEqual([]);
  });
});
describe("audio clip placement", () => {
  it("converts drag pixels using the zoomed timeline width", () => {
    expect(clipStartAfterDrag(4, 100, 1000, 60)).toBe(10);
    expect(clipStartAfterDrag(4, 200, 2000, 60)).toBe(10);
  });
  it("allows extending the arrangement and clamps the left edge", () => {
    expect(clipStartAfterDrag(58, 100, 1000, 60)).toBe(64);
    expect(clipStartAfterDrag(4, -100, 1000, 60)).toBe(0);
  });
  it("ignores invalid pointer geometry", () => {
    expect(clipStartAfterDrag(4, Number.NaN, 1000, 60)).toBe(4);
    expect(clipStartAfterDrag(4, 100, 0, 60)).toBe(4);
  });
});

describe("arranged waveforms", () => {
  it("keeps gaps and shifted clips aligned with the piano roll", () => {
    const source = track("voice");
    source.clips = [
      { id: "c", name: "c", start: 2, offset: 0, duration: 2, waveform: [[-0.5, 0.5]] },
    ];
    const waveform = arrangementWaveform(source, 8);
    expect(waveform[0]).toEqual([0, 0]);
    expect(waveform[700]).toEqual([-0.5, 0.5]);
    expect(waveform[1500]).toEqual([0, 0]);
  });
});

describe("backing group rows", () => {
  it("places a distinct Add Back group between each source and its children", () => {
    const rows = arrangementRows([
      track("Instrumental"),
      track("Verse"),
      track("Chorus"),
      track("Double", "Verse"),
    ]);
    expect(rows.map((row) => row.key)).toEqual([
      "Instrumental",
      "Verse",
      "group:Verse",
      "Double",
      "Chorus",
      "group:Chorus",
    ]);
    expect(rows.map((row) => row.number)).toEqual([1, 2, 3, 4, 5, 6]);
    expect(rows[3].depth).toBe(1);
    expect(rows[1].color).toBe(rows[2].color);
    expect(rows[2].color).toBe(rows[3].color);
    expect(rows[4].color).not.toBe(rows[1].color);
  });
  it("keeps the group and Add Back visible when its children are collapsed", () => {
    const rows = arrangementRows([
      track("Verse", "", true),
      track("Double", "Verse"),
      track("Chorus"),
    ]);
    expect(rows.map((row) => row.key)).toEqual(["Verse", "group:Verse", "Chorus", "group:Chorus"]);
    expect(rows[1].childCount).toBe(1);
  });
});
