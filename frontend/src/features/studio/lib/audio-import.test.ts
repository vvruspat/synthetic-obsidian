import { File } from "node:buffer";
import { describe, expect, it } from "vitest";
import type { AudioTransferRequest, NativePluginHost } from "@/bridge/plugin-bridge";
import { transferAudioFiles } from "./audio-import";

describe("dropped audio transfer", () => {
  it("transfers binary data in bounded ordered chunks and preserves file order", async () => {
    const bytes = Uint8Array.from({ length: 600000 }, (_, i) => i % 251);
    const chunks: Uint8Array[] = [];
    const offsets: number[] = [];
    const names: string[] = [];
    const transfer: NonNullable<NativePluginHost["transferAudio"]> = async (request) => {
      if (request.action === "begin") {
        names.push(request.name);
        return { token: request.name };
      }
      if (request.action === "append") {
        chunks.push(Uint8Array.from(atob(request.data), (c) => c.charCodeAt(0)));
        offsets.push(request.offset);
        return { ok: true };
      }
      return { path: `/imports/${request.token}` };
    };
    const files = [new File([bytes], "lead.wav"), new File([new Uint8Array([1, 2])], "back.flac")];
    const paths = await transferAudioFiles(
      files as unknown as globalThis.File[],
      transfer,
      () => {},
    );
    expect(paths).toEqual(["/imports/lead.wav", "/imports/back.flac"]);
    expect(names).toEqual(["lead.wav", "back.flac"]);
    expect(offsets).toEqual([0, 262144, 524288, 0]);
    expect(chunks.every((chunk) => chunk.length <= 262144)).toBe(true);
    expect(Buffer.concat(chunks.slice(0, 3))).toEqual(Buffer.from(bytes));
  });
  it("aborts a failed transfer and never imports a partial file", async () => {
    const requests: AudioTransferRequest[] = [];
    const transfer: NonNullable<NativePluginHost["transferAudio"]> = async (request) => {
      requests.push(request);
      return request.action === "begin" ? { token: "x" } : { error: "Disk full" };
    };
    await expect(
      transferAudioFiles(
        [new File(["data"], "voice.wav")] as unknown as globalThis.File[],
        transfer,
        () => {},
      ),
    ).rejects.toThrow("Disk full");
    expect(requests.map((request) => request.action)).toEqual(["begin", "append", "abort"]);
  });
});
