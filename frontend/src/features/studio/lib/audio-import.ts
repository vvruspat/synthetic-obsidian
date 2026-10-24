import type { NativePluginHost } from "@/bridge/plugin-bridge";

const CHUNK_BYTES = 256 * 1024;

export async function transferAudioFiles(
  files: readonly File[],
  transfer: NonNullable<NativePluginHost["transferAudio"]>,
  onProgress: (name: string, percent: number) => void,
): Promise<string[]> {
  if (
    !files.length ||
    files.some((file) => !/\.(wav|aiff?|flac|mp3|ogg)$/i.test(file.name) || !file.size)
  ) {
    throw new Error("Choose WAV, AIFF, FLAC, MP3 or OGG audio files.");
  }
  const paths: string[] = [];
  for (const file of files) {
    const begin = await transfer({ action: "begin", name: file.name, size: file.size });
    if (begin.error || !begin.token)
      throw new Error(begin.error || "Could not start audio import.");
    const token = begin.token;
    try {
      for (let offset = 0; offset < file.size; offset += CHUNK_BYTES) {
        const bytes = new Uint8Array(await file.slice(offset, offset + CHUNK_BYTES).arrayBuffer());
        const parts: string[] = [];
        for (let i = 0; i < bytes.length; i += 8192) {
          parts.push(String.fromCharCode(...bytes.subarray(i, i + 8192)));
        }
        const reply = await transfer({
          action: "append",
          token,
          offset,
          data: btoa(parts.join("")),
        });
        if (reply.error || !reply.ok) throw new Error(reply.error || "Audio transfer failed.");
        onProgress(file.name, Math.round(((offset + bytes.length) / file.size) * 100));
      }
      const reply = await transfer({ action: "finish", token });
      if (reply.error || !reply.path)
        throw new Error(reply.error || "Could not finish audio import.");
      paths.push(reply.path);
    } catch (error) {
      await transfer({ action: "abort", token }).catch(() => {});
      throw error;
    }
  }
  return paths;
}
