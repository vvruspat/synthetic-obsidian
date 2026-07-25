import { useEffect, useRef, useState } from "react";
import type { PluginBridge, ProjectAction } from "@/bridge/plugin-bridge";
import { Arrangement } from "@/features/studio/components/Arrangement";
import { ExportToast, type ExportToastState } from "@/features/studio/components/ExportToast";
import { ProjectStatusBar } from "@/features/studio/components/ProjectStatusBar";
import { StudioHeader } from "@/features/studio/components/StudioHeader";
import { TrackSidebar } from "@/features/studio/components/TrackSidebar";
import { useStudioController } from "@/features/studio/hooks/useStudioController";
import { mockStudioProject } from "@/mocks/studio-project";

export function StudioApp({ bridge }: { bridge: PluginBridge }) {
  const [exportToast, setExportToast] = useState<ExportToastState | null>(null);
  const exportToastTimeoutRef = useRef(0);
  const [nativeState, setNativeState] = useState({
    project: mockStudioProject,
    status: "",
    vocalAnalysisRunning: false,
    instrumentalAnalysisRunning: false,
    backingGenerationRunning: false,
    backingAudioRenderRunning: false,
    backingGenerationTrack: "",
    backingAudioRenderTrack: "",
  });

  useEffect(() => {
    const unsubscribe = bridge.subscribe((event) => {
      if (event.type === "project-state") {
        setNativeState((current) => ({
          ...current,
          project: event.project,
        }));
      } else if (event.type === "status-state") {
        setNativeState((current) => ({
          ...current,
          status: event.message,
          vocalAnalysisRunning: event.vocalAnalysisRunning,
          instrumentalAnalysisRunning: event.instrumentalAnalysisRunning,
          backingGenerationRunning: event.backingGenerationRunning,
          backingAudioRenderRunning: event.backingAudioRenderRunning,
          backingGenerationTrack: event.backingGenerationTrack,
          backingAudioRenderTrack: event.backingAudioRenderTrack,
        }));
      } else if (event.type === "export-complete") {
        window.clearTimeout(exportToastTimeoutRef.current);
        setExportToast({
          id: Date.now(),
          message: event.message,
          directory: event.directory,
        });
        exportToastTimeoutRef.current = window.setTimeout(() => {
          setExportToast(null);
        }, 6000);
      }
    });
    bridge.send({ type: "frontend-ready" });
    return () => {
      unsubscribe();
      window.clearTimeout(exportToastTimeoutRef.current);
    };
  }, [bridge]);

  return (
    <StudioWorkspace
      bridge={bridge}
      project={nativeState.project}
      status={nativeState.status}
      vocalAnalysisRunning={nativeState.vocalAnalysisRunning}
      instrumentalAnalysisRunning={nativeState.instrumentalAnalysisRunning}
      backingGenerationRunning={nativeState.backingGenerationRunning}
      backingAudioRenderRunning={nativeState.backingAudioRenderRunning}
      backingGenerationTrack={nativeState.backingGenerationTrack}
      backingAudioRenderTrack={nativeState.backingAudioRenderTrack}
      exportToast={exportToast}
      onDismissExportToast={() => {
        window.clearTimeout(exportToastTimeoutRef.current);
        setExportToast(null);
      }}
    />
  );
}

function StudioWorkspace({
  bridge,
  project,
  status,
  vocalAnalysisRunning,
  instrumentalAnalysisRunning,
  backingGenerationRunning,
  backingAudioRenderRunning,
  backingGenerationTrack,
  backingAudioRenderTrack,
  exportToast,
  onDismissExportToast,
}: {
  bridge: PluginBridge;
  project: typeof mockStudioProject;
  status: string;
  vocalAnalysisRunning: boolean;
  instrumentalAnalysisRunning: boolean;
  backingGenerationRunning: boolean;
  backingAudioRenderRunning: boolean;
  backingGenerationTrack: string;
  backingAudioRenderTrack: string;
  exportToast: ExportToastState | null;
  onDismissExportToast(): void;
}) {
  const controller = useStudioController(bridge, project);
  const { pianoCollapsed, serviceTracksCollapsed } = controller;
  const sendProjectAction = (action: ProjectAction) => {
    bridge.send({ type: "project-action", action });
  };

  return (
    <main className="studio-shell">
      <StudioHeader controller={controller} />
      <section
        className={[
          "workspace",
          pianoCollapsed ? "is-piano-collapsed" : "",
          serviceTracksCollapsed ? "is-service-tracks-collapsed" : "",
        ]
          .filter(Boolean)
          .join(" ")}
      >
        <TrackSidebar controller={controller} />
        <Arrangement
          controller={controller}
          project={project}
          status={status}
          vocalAnalysisRunning={vocalAnalysisRunning}
          instrumentalAnalysisRunning={instrumentalAnalysisRunning}
          backingGenerationRunning={backingGenerationRunning}
          backingAudioRenderRunning={backingAudioRenderRunning}
          backingGenerationTrack={backingGenerationTrack}
          backingAudioRenderTrack={backingAudioRenderTrack}
          onProjectAction={sendProjectAction}
        />
      </section>
      <ProjectStatusBar status={status} />
      {exportToast ? <ExportToast toast={exportToast} onDismiss={onDismissExportToast} /> : null}
    </main>
  );
}
