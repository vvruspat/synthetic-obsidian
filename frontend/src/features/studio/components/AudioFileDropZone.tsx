import { type ReactNode, useRef, useState } from "react";
import type { StudioController } from "@/features/studio/hooks/useStudioController";
import { transferAudioFiles } from "@/features/studio/lib/audio-import";

export function AudioFileDropZone({
  controller,
  busy,
  children,
}: {
  controller: StudioController;
  busy: boolean;
  children: ReactNode;
}) {
  const [dragging, setDragging] = useState(false);
  const [progress, setProgress] = useState("");
  const [error, setError] = useState("");
  const depth = useRef(0);
  const transferring = useRef(false);
  const selected =
    controller.selectedTracks.length === 1
      ? controller.arrangementTracks.find(
          (track) => track.id === controller.selectedTracks[0] && !track.parentId,
        )
      : undefined;
  const start = (controller.musicalTimeline.duration * controller.playhead) / 100;

  return (
    <section
      className="audio-drop-zone"
      aria-label="Audio workspace"
      onDragEnter={(event) => {
        if (!event.dataTransfer.types.includes("Files")) return;
        event.preventDefault();
        depth.current += 1;
        setDragging(true);
      }}
      onDragOver={(event) => {
        if (!event.dataTransfer.types.includes("Files")) return;
        event.preventDefault();
        event.dataTransfer.dropEffect = busy || transferring.current ? "none" : "copy";
      }}
      onDragLeave={(event) => {
        event.preventDefault();
        depth.current = Math.max(0, depth.current - 1);
        if (!depth.current) setDragging(false);
      }}
      onDrop={async (event) => {
        event.preventDefault();
        depth.current = 0;
        setDragging(false);
        if (busy || transferring.current) return;
        const transfer = window.syntheticObsidianHost?.transferAudio;
        if (!transfer) {
          setError("Audio file import is available in the desktop app.");
          return;
        }
        const files = Array.from(event.dataTransfer.files);
        const target = selected?.id ?? "";
        const position = start;
        transferring.current = true;
        setError("");
        setProgress("Importing audio files…");
        try {
          const paths = await transferAudioFiles(files, transfer, (name, percent) =>
            setProgress(`${name} · ${percent}%`),
          );
          controller.actions.importDroppedAudio(paths, target, position);
        } catch (failure) {
          setError(failure instanceof Error ? failure.message : "Audio import failed.");
        } finally {
          transferring.current = false;
          setProgress("");
        }
      }}
    >
      {children}
      {dragging || progress ? (
        <div className="audio-drop-overlay" role="status">
          <strong>
            {progress || (busy ? "Wait for the current operation" : "Import audio clips")}
          </strong>
          {!progress && !busy ? (
            <span>
              {selected ? `To ${selected.name}` : "Create a new track"} at {start.toFixed(2)} s
            </span>
          ) : null}
        </div>
      ) : null}
      {error ? (
        <div className="audio-import-error" role="alert">
          <span>{error}</span>
          <button type="button" onClick={() => setError("")}>
            Dismiss
          </button>
        </div>
      ) : null}
    </section>
  );
}
