import {
  type DragEvent,
  type PointerEvent as ReactPointerEvent,
  useEffect,
  useMemo,
  useRef,
  useState,
  type WheelEvent,
} from "react";
import type { PluginBridge } from "@/bridge/plugin-bridge";
import type {
  BackingTrackName,
  CorrectionToolId,
  CycleRange,
  StudioProject,
  TrackChannelState,
  TrackLayerAvailability,
  TrackLayerState,
  TrackName,
  VocalClip,
} from "@/domain/studio";
import {
  createMusicalTimeline,
  formatMusicalPosition,
  getMusicalContext,
} from "@/features/studio/lib/musical-time";
import { createPitchPath, PITCH_ROW_HEIGHT, PITCH_TOP } from "@/features/studio/lib/pitch";
import { getNextTrackSelection } from "@/features/studio/lib/track-selection";
import { formatTime, getCenteredTimelineScrollLeft } from "@/features/studio/lib/transport";

const HORIZONTAL_ZOOM_MIN = 1;
const HORIZONTAL_ZOOM_MAX = 32;
const VERTICAL_ZOOM_MIN = 1;
const VERTICAL_ZOOM_MAX = 4;

export function useStudioController(bridge: PluginBridge, project: StudioProject) {
  const [playing, setPlaying] = useState(false);
  const [playhead, setPlayhead] = useState(project.initialPlayhead);
  const [looping, setLoopingState] = useState(project.initialLoopActive);
  const [cycleRange, setCycleRangeState] = useState(project.initialCycleRange);
  const [volume, setVolume] = useState(project.initialVolume);
  const [outputLevels, setOutputLevels] = useState({ left: 0, right: 0 });
  const [clips, setClips] = useState(project.clips);
  const [horizontalZoom, setHorizontalZoom] = useState(1);
  const [verticalZoom, setVerticalZoom] = useState(1);
  const [leftCorrectionTool, setLeftCorrectionTool] = useState<CorrectionToolId>("pointer");
  const [rightCorrectionTool, setRightCorrectionTool] = useState<CorrectionToolId>("scissors");
  const [pianoCollapsed, setPianoCollapsed] = useState(false);
  const [serviceTracksCollapsed, setServiceTracksCollapsed] = useState(false);
  const [backingTracks, setBackingTracks] = useState<BackingTrackName[]>(
    project.initialBackingTracks,
  );
  const [selectedTracks, setSelectedTracks] = useState<TrackName[]>([
    project.hasVocal
      ? "Voice Main"
      : project.hasInstrumental
        ? "Instrumental"
        : (project.initialBackingTracks[0] ?? "Voice Main"),
  ]);
  const [trackState, setTrackState] = useState<Record<string, TrackChannelState>>(
    Object.fromEntries(project.initialTrackState.map(({ track, state }) => [track, state])),
  );
  const [trackLayerState, setTrackLayerState] = useState<Record<string, TrackLayerState>>(
    Object.fromEntries(project.initialTrackLayerState.map(({ track, state }) => [track, state])),
  );

  useEffect(() => {
    setClips(project.clips);
  }, [project.clips]);

  useEffect(() => {
    setBackingTracks(project.initialBackingTracks);
  }, [project.initialBackingTracks]);

  useEffect(() => {
    const availableTracks = new Set<TrackName>([
      ...(project.hasInstrumental ? (["Instrumental"] as const) : []),
      ...(project.hasVocal ? (["Voice Main"] as const) : []),
      ...backingTracks,
    ]);
    setSelectedTracks((current) => {
      const availableSelection = current.filter((track) => availableTracks.has(track));
      if (availableSelection.length > 0) return availableSelection;
      if (project.hasVocal) return ["Voice Main"];
      if (project.hasInstrumental) return ["Instrumental"];
      return backingTracks.length > 0 ? [backingTracks[0]] : ["Voice Main"];
    });
  }, [backingTracks, project.hasInstrumental, project.hasVocal]);

  useEffect(() => {
    setTrackState(
      Object.fromEntries(project.initialTrackState.map(({ track, state }) => [track, state])),
    );
  }, [project.initialTrackState]);

  useEffect(() => {
    setTrackLayerState(
      Object.fromEntries(project.initialTrackLayerState.map(({ track, state }) => [track, state])),
    );
  }, [project.initialTrackLayerState]);

  const editorRef = useRef<HTMLFieldSetElement>(null);
  const arrangementScrollRef = useRef<HTMLElement>(null);
  const timelineContentRef = useRef<HTMLDivElement>(null);
  const sidebarTrackScrollRef = useRef<HTMLDivElement>(null);
  const arrangementTrackScrollRef = useRef<HTMLDivElement>(null);
  const pianoRollScrollRef = useRef<HTMLDivElement>(null);
  const pianoKeysScrollRef = useRef<HTMLDivElement>(null);
  const horizontalZoomFrameRef = useRef(0);
  const verticalZoomAnchorRef = useRef<number | null>(null);
  const zoomWheelRef = useRef({ frame: 0, horizontal: 0, vertical: 0 });
  const clipDragRef = useRef<{ id: string; dy: number; pitch: number } | null>(null);
  const playheadDragRef = useRef(false);

  useEffect(
    () =>
      bridge.subscribe((event) => {
        switch (event.type) {
          case "project-state":
          case "status-state":
            break;
          case "transport-state":
            setPlaying(event.playing);
            setPlayhead(event.playhead);
            break;
          case "loop-state":
            setLoopingState(event.active);
            setCycleRangeState(event.range);
            break;
          case "volume-state":
            setVolume(event.value);
            break;
          case "output-meter-state":
            setOutputLevels({ left: event.left, right: event.right });
            break;
          case "export-complete":
            break;
          case "track-state":
            setTrackState((current) => ({
              ...current,
              [event.track]: event.state,
            }));
            break;
          case "track-layer-state":
            setTrackLayerState((current) => ({
              ...current,
              [event.track]: event.state,
            }));
            break;
          case "backing-track-added":
            setBackingTracks((current) =>
              current.includes(event.track) ? current : [...current, event.track],
            );
            break;
          case "clip-pitch-state":
            setClips((current) =>
              current.map((clip) =>
                clip.id === event.clipId ? { ...clip, pitch: event.pitch } : clip,
              ),
            );
            break;
        }
      }),
    [bridge],
  );

  useEffect(() => {
    if (!playing) return;
    let animation = 0;
    let previous = performance.now();
    const tick = (now: number) => {
      const delta = now - previous;
      if (delta < 32) {
        animation = requestAnimationFrame(tick);
        return;
      }
      previous = now;
      setPlayhead((current) => {
        const duration = Math.max(0.001, project.timelineDurationSeconds);
        const next = current + (delta / 1000 / duration) * 100;
        if (looping && next >= cycleRange.end) {
          const span = cycleRange.end - cycleRange.start;
          return cycleRange.start + ((next - cycleRange.start) % span);
        }
        if (next < 100) return next;
        setPlaying(false);
        return 100;
      });
      animation = requestAnimationFrame(tick);
    };
    animation = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(animation);
  }, [cycleRange.end, cycleRange.start, looping, playing, project.timelineDurationSeconds]);

  useEffect(() => {
    void backingTracks.length;
    const frame = requestAnimationFrame(() => {
      const sidebar = sidebarTrackScrollRef.current;
      const arrangement = arrangementTrackScrollRef.current;
      if (sidebar) sidebar.scrollTop = sidebar.scrollHeight;
      if (arrangement) arrangement.scrollTop = arrangement.scrollHeight;
    });
    return () => cancelAnimationFrame(frame);
  }, [backingTracks.length]);

  const timeLabel = useMemo(
    () => formatTime(playhead, project.timelineStartSeconds, project.timelineDurationSeconds),
    [playhead, project.timelineDurationSeconds, project.timelineStartSeconds],
  );
  const musicalTimeline = useMemo(
    () =>
      createMusicalTimeline(
        project.timelineDurationSeconds,
        project.tempoSegments,
        project.timeSignatures,
        project.tempo,
      ),
    [project.tempo, project.tempoSegments, project.timeSignatures, project.timelineDurationSeconds],
  );
  const playheadSeconds = (playhead / 100) * project.timelineDurationSeconds;
  const transportPosition = useMemo(
    () => formatMusicalPosition(musicalTimeline, playheadSeconds),
    [musicalTimeline, playheadSeconds],
  );
  const musicalContext = useMemo(
    () => getMusicalContext(musicalTimeline, playheadSeconds),
    [musicalTimeline, playheadSeconds],
  );
  const trackLayerAvailability = useMemo(() => {
    const availability: Record<string, TrackLayerAvailability> = {
      "Voice Main": {
        audioAvailable: project.hasVocal,
        notesAvailable: clips.length > 0,
      },
    };
    for (const track of backingTracks) {
      const content = project.backingTrackContents.find((item) => item.track === track);
      availability[track] = {
        audioAvailable: content?.hasAudio ?? false,
        notesAvailable: (content?.clips.length ?? 0) > 0,
      };
    }
    return availability;
  }, [backingTracks, clips.length, project.backingTrackContents, project.hasVocal]);
  const editorClips = useMemo(
    () =>
      selectedTracks.flatMap((track) => {
        if (track === "Instrumental") return [];
        if (track === "Voice Main") return clips;
        return project.backingTrackContents.find((item) => item.track === track)?.clips ?? [];
      }),
    [clips, project.backingTrackContents, selectedTracks],
  );
  const selectedTrackContent =
    selectedTracks.length === 1 && selectedTracks[0] !== "Instrumental"
      ? project.backingTrackContents.find((item) => item.track === selectedTracks[0])
      : undefined;
  const editorWaveform =
    selectedTracks.length !== 1
      ? []
      : selectedTracks[0] === "Instrumental"
        ? project.instrumentalWaveform
        : selectedTracks[0] === "Voice Main"
          ? project.vocalWaveform
          : (selectedTrackContent?.waveform ?? []);
  const editorWaveformDurationRatio =
    selectedTracks.length !== 1
      ? 0
      : selectedTracks[0] === "Instrumental"
        ? project.instrumentalDurationSeconds / project.timelineDurationSeconds
        : selectedTracks[0] === "Voice Main"
          ? project.vocalDurationSeconds / project.timelineDurationSeconds
          : (selectedTrackContent?.audioDurationSeconds ?? 0) / project.timelineDurationSeconds;
  const canEditSelectedClips = selectedTracks.length === 1 && selectedTracks[0] === "Voice Main";
  const sortedClips = useMemo(() => [...editorClips].sort((a, b) => a.x - b.x), [editorClips]);
  const pitchPath = useMemo(() => createPitchPath(sortedClips), [sortedClips]);

  const returnToStart = () => {
    setPlayhead(0);
    setPlaying(false);
    bridge.send({ type: "transport", action: "return-to-start" });
  };

  const togglePlayback = () => {
    setPlaying((current) => {
      bridge.send({
        type: "transport",
        action: current ? "pause" : "play",
      });
      return !current;
    });
  };

  const stopPlayback = () => {
    setPlaying(false);
    bridge.send({ type: "transport", action: "stop" });
  };

  const setLooping = (active: boolean) => {
    setLoopingState(active);
    bridge.send({ type: "set-loop", active, range: cycleRange });
  };

  const setCycleRange = (range: CycleRange) => {
    setCycleRangeState(range);
    bridge.send({ type: "set-loop", active: looping, range });
  };

  const setPlayheadPosition = (position: number) => {
    const next = Math.max(0, Math.min(100, position));
    setPlaying(false);
    setPlayhead(next);
    bridge.send({ type: "set-playhead", position: next });
  };

  const setVolumeValue = (value: number) => {
    setVolume(value);
    bridge.send({ type: "set-volume", value });
  };

  const toggleTrack = (name: TrackName, mode: keyof TrackChannelState) => {
    setTrackState((current) => {
      const previous = current[name] ?? { mute: false, solo: false };
      const next = { ...previous, [mode]: !previous[mode] };
      bridge.send({ type: "set-track-state", track: name, state: next });
      return { ...current, [name]: next };
    });
  };

  const toggleTrackLayer = (name: TrackName, layer: keyof TrackLayerState) => {
    setTrackLayerState((current) => {
      const previous = current[name] ?? { audioMuted: false, notesMuted: false };
      const next = { ...previous, [layer]: !previous[layer] };
      bridge.send({ type: "set-track-layer-state", track: name, state: next });
      return { ...current, [name]: next };
    });
  };

  const selectTrack = (name: TrackName, additive: boolean) => {
    const nextSelection = getNextTrackSelection(selectedTracks, name, additive);
    setSelectedTracks(nextSelection);
    bridge.send({ type: "select-tracks", tracks: nextSelection });
  };

  const addBackingTrack = (name: BackingTrackName) => {
    if (backingTracks.includes(name)) return;
    setBackingTracks((current) => [...current, name]);
    setTrackState((current) => ({
      ...current,
      [name]: { mute: false, solo: false },
    }));
    bridge.send({ type: "add-backing-track", track: name });
  };

  const regenerateBackingTrack = (name: BackingTrackName) => {
    bridge.send({ type: "regenerate-backing-track", track: name });
  };

  const renderBackingTrack = (name: BackingTrackName) => {
    bridge.send({ type: "render-backing-track", track: name });
  };

  const openProject = () => {
    bridge.send({ type: "project-action", action: "open-project" });
  };

  const saveProject = () => {
    bridge.send({ type: "project-action", action: "save-project" });
  };

  const exportAllTracks = () => {
    bridge.send({ type: "project-action", action: "export-all-tracks" });
  };

  const exportMidiFiles = () => {
    bridge.send({ type: "project-action", action: "export-midi" });
  };

  const syncVerticalScroll = (source: "roll" | "keys") => {
    const sourceElement =
      source === "roll" ? pianoRollScrollRef.current : pianoKeysScrollRef.current;
    const targetElement =
      source === "roll" ? pianoKeysScrollRef.current : pianoRollScrollRef.current;
    if (!sourceElement || !targetElement) return;
    const sourceMax = sourceElement.scrollHeight - sourceElement.clientHeight;
    const targetMax = targetElement.scrollHeight - targetElement.clientHeight;
    const progress = sourceMax > 0 ? sourceElement.scrollTop / sourceMax : 0;
    const next = progress * Math.max(0, targetMax);
    if (Math.abs(targetElement.scrollTop - next) > 0.5) {
      targetElement.scrollTop = next;
    }
  };

  const syncTrackScroll = (source: "sidebar" | "arrangement") => {
    const sourceElement =
      source === "sidebar" ? sidebarTrackScrollRef.current : arrangementTrackScrollRef.current;
    const targetElement =
      source === "sidebar" ? arrangementTrackScrollRef.current : sidebarTrackScrollRef.current;
    if (
      sourceElement &&
      targetElement &&
      Math.abs(targetElement.scrollTop - sourceElement.scrollTop) > 0.5
    ) {
      targetElement.scrollTop = sourceElement.scrollTop;
    }
  };

  const centerHorizontalZoomOnPlayhead = () => {
    const anchor = playhead / 100;
    cancelAnimationFrame(horizontalZoomFrameRef.current);
    horizontalZoomFrameRef.current = requestAnimationFrame(() => {
      horizontalZoomFrameRef.current = requestAnimationFrame(() => {
        const scroller = arrangementScrollRef.current;
        if (!scroller) return;
        scroller.scrollLeft = getCenteredTimelineScrollLeft(
          anchor,
          scroller.scrollWidth,
          scroller.clientWidth,
        );
      });
    });
  };

  const changeHorizontalZoom = (delta: number) => {
    centerHorizontalZoomOnPlayhead();
    setHorizontalZoom((current) =>
      Math.max(
        HORIZONTAL_ZOOM_MIN,
        Math.min(HORIZONTAL_ZOOM_MAX, Number((current + delta).toFixed(2))),
      ),
    );
  };

  const changeVerticalZoom = (delta: number) => {
    const scroller = pianoRollScrollRef.current;
    if (scroller) {
      verticalZoomAnchorRef.current =
        (scroller.scrollTop + scroller.clientHeight / 2) / Math.max(1, scroller.scrollHeight);
    }
    setVerticalZoom((current) =>
      Math.max(
        VERTICAL_ZOOM_MIN,
        Math.min(VERTICAL_ZOOM_MAX, Number((current + delta).toFixed(2))),
      ),
    );
  };

  const setHorizontalZoomFromInput = (value: number) => {
    centerHorizontalZoomOnPlayhead();
    setHorizontalZoom(Math.max(HORIZONTAL_ZOOM_MIN, Math.min(HORIZONTAL_ZOOM_MAX, value)));
  };

  const setVerticalZoomFromInput = (value: number) => {
    const scroller = pianoRollScrollRef.current;
    if (scroller) {
      verticalZoomAnchorRef.current =
        (scroller.scrollTop + scroller.clientHeight / 2) / Math.max(1, scroller.scrollHeight);
    }
    setVerticalZoom(value);
  };

  const queueWheelZoom = (axis: "horizontal" | "vertical", deltaY: number) => {
    const pending = zoomWheelRef.current;
    pending[axis] += deltaY;
    window.clearTimeout(pending.frame);
    pending.frame = window.setTimeout(() => {
      const horizontalDelta = pending.horizontal;
      const verticalDelta = pending.vertical;
      pending.frame = 0;
      pending.horizontal = 0;
      pending.vertical = 0;
      if (Math.abs(horizontalDelta) > 0.5) {
        const amount = Math.min(
          1.5,
          Math.max(0.25, Math.round(Math.abs(horizontalDelta) / 120) * 0.25),
        );
        changeHorizontalZoom(horizontalDelta < 0 ? amount : -amount);
      }
      if (Math.abs(verticalDelta) > 0.5) {
        const amount = Math.min(
          1.5,
          Math.max(0.25, Math.round(Math.abs(verticalDelta) / 120) * 0.25),
        );
        changeVerticalZoom(verticalDelta < 0 ? amount : -amount);
      }
    }, 70);
  };

  useEffect(() => {
    void verticalZoom;
    const frame = requestAnimationFrame(() => {
      const scroller = pianoRollScrollRef.current;
      const anchor = verticalZoomAnchorRef.current;
      if (scroller && anchor !== null) {
        scroller.scrollTop = anchor * scroller.scrollHeight - scroller.clientHeight / 2;
        const keyboard = pianoKeysScrollRef.current;
        if (keyboard) {
          const rollMax = scroller.scrollHeight - scroller.clientHeight;
          const keyboardMax = keyboard.scrollHeight - keyboard.clientHeight;
          const progress = rollMax > 0 ? scroller.scrollTop / rollMax : 0;
          keyboard.scrollTop = progress * Math.max(0, keyboardMax);
        }
      }
      verticalZoomAnchorRef.current = null;
    });
    return () => cancelAnimationFrame(frame);
  }, [verticalZoom]);

  const handleTimelineWheel = (event: WheelEvent<HTMLElement>) => {
    if (event.ctrlKey || event.metaKey) {
      event.preventDefault();
      queueWheelZoom("horizontal", event.deltaY);
      return;
    }
    if (event.shiftKey || Math.abs(event.deltaX) > Math.abs(event.deltaY)) {
      event.preventDefault();
      const scroller = arrangementScrollRef.current;
      if (scroller) {
        scroller.scrollLeft += event.shiftKey ? event.deltaY : event.deltaX;
      }
    }
  };

  const handlePianoWheel = (event: WheelEvent<HTMLDivElement>) => {
    if (event.altKey) {
      event.preventDefault();
      event.stopPropagation();
      queueWheelZoom("vertical", event.deltaY);
      return;
    }
    if (event.ctrlKey || event.metaKey) {
      event.preventDefault();
      event.stopPropagation();
      queueWheelZoom("horizontal", event.deltaY);
      return;
    }
    if (event.shiftKey || Math.abs(event.deltaX) > Math.abs(event.deltaY)) {
      event.preventDefault();
      event.stopPropagation();
      const scroller = arrangementScrollRef.current;
      if (scroller) {
        scroller.scrollLeft += event.shiftKey ? event.deltaY : event.deltaX;
      }
    }
  };

  const updatePlayheadFromClientX = (clientX: number) => {
    const timeline = timelineContentRef.current;
    if (!timeline) return;
    const rect = timeline.getBoundingClientRect();
    const next = ((clientX - rect.left) / Math.max(1, rect.width)) * 100;
    setPlayheadPosition(next);
  };

  const movePlayhead = (event: PointerEvent) => {
    if (playheadDragRef.current) {
      updatePlayheadFromClientX(event.clientX);
    }
  };

  const endPlayheadDrag = () => {
    playheadDragRef.current = false;
    window.removeEventListener("pointermove", movePlayhead);
    window.removeEventListener("pointerup", endPlayheadDrag);
    window.removeEventListener("pointercancel", endPlayheadDrag);
  };

  const startPlayheadDrag = (event: ReactPointerEvent<HTMLElement>) => {
    if (event.button !== 0) return;
    endPlayheadDrag();
    playheadDragRef.current = true;
    updatePlayheadFromClientX(event.clientX);
    window.addEventListener("pointermove", movePlayhead);
    window.addEventListener("pointerup", endPlayheadDrag);
    window.addEventListener("pointercancel", endPlayheadDrag);
    event.preventDefault();
    event.stopPropagation();
  };

  const moveClip = (event: PointerEvent) => {
    const drag = clipDragRef.current;
    const editor = editorRef.current;
    if (!drag || !editor) return;
    const rect = editor.getBoundingClientRect();
    const top = ((event.clientY - rect.top - drag.dy) / rect.height) * 100;
    const pitch = Math.max(48, Math.min(PITCH_TOP, PITCH_TOP - top / PITCH_ROW_HEIGHT));
    drag.pitch = pitch;
    setClips((current) => current.map((clip) => (clip.id === drag.id ? { ...clip, pitch } : clip)));
  };

  const endClipDrag = () => {
    const drag = clipDragRef.current;
    if (drag) {
      bridge.send({ type: "set-clip-pitch", clipId: drag.id, pitch: drag.pitch });
    }
    clipDragRef.current = null;
    window.removeEventListener("pointermove", moveClip);
    window.removeEventListener("pointerup", endClipDrag);
    window.removeEventListener("pointercancel", endClipDrag);
  };

  const startClipDrag = (event: ReactPointerEvent<HTMLButtonElement>, clip: VocalClip) => {
    if (!canEditSelectedClips) return;
    const rect = event.currentTarget.getBoundingClientRect();
    clipDragRef.current = {
      id: clip.id,
      dy: event.clientY - rect.top,
      pitch: clip.pitch,
    };
    event.currentTarget.setPointerCapture(event.pointerId);
    window.addEventListener("pointermove", moveClip);
    window.addEventListener("pointerup", endClipDrag);
    window.addEventListener("pointercancel", endClipDrag);
  };

  const dropSyllable = (event: DragEvent<HTMLFieldSetElement>) => {
    event.preventDefault();
    if (!canEditSelectedClips) return;
    const editor = editorRef.current;
    const label = event.dataTransfer.getData("text/syllable");
    if (!editor || !label) return;
    const rect = editor.getBoundingClientRect();
    const dropCenter = ((event.clientY - rect.top) / rect.height) * 100;
    const pitch = PITCH_TOP - (dropCenter - PITCH_ROW_HEIGHT / 2) / PITCH_ROW_HEIGHT;
    setClips((current) => [
      ...current,
      {
        id: String(Date.now()),
        label,
        x: Math.max(0, Math.min(92, ((event.clientX - rect.left) / rect.width) * 100)),
        pitch: Math.max(48, Math.min(PITCH_TOP, pitch)),
        width: 6,
        color: "cyan",
      },
    ]);
  };

  // biome-ignore lint/correctness/useExhaustiveDependencies: unmount cleanup must remove the listener functions captured by the active drag.
  useEffect(
    () => () => {
      window.clearTimeout(zoomWheelRef.current.frame);
      cancelAnimationFrame(horizontalZoomFrameRef.current);
      endPlayheadDrag();
      endClipDrag();
    },
    [],
  );

  return {
    playing,
    playhead,
    looping,
    cycleRange,
    volume,
    outputLevels,
    clips: editorClips,
    sortedClips,
    pitchPath,
    editorWaveform,
    editorWaveformDurationRatio,
    canEditSelectedClips,
    horizontalZoom,
    verticalZoom,
    leftCorrectionTool,
    rightCorrectionTool,
    pianoCollapsed,
    serviceTracksCollapsed,
    backingTracks,
    selectedTracks,
    trackState,
    trackLayerState,
    trackLayerAvailability,
    timeLabel,
    transportPosition,
    musicalTimeline,
    musicalContext,
    refs: {
      editorRef,
      arrangementScrollRef,
      timelineContentRef,
      sidebarTrackScrollRef,
      arrangementTrackScrollRef,
      pianoRollScrollRef,
      pianoKeysScrollRef,
    },
    actions: {
      returnToStart,
      togglePlayback,
      stopPlayback,
      setLooping,
      setCycleRange,
      setPlayheadPosition,
      setVolumeValue,
      toggleTrack,
      toggleTrackLayer,
      selectTrack,
      addBackingTrack,
      regenerateBackingTrack,
      renderBackingTrack,
      openProject,
      saveProject,
      exportAllTracks,
      exportMidiFiles,
      setLeftCorrectionTool,
      setRightCorrectionTool,
      setPianoCollapsed,
      setServiceTracksCollapsed,
      setHorizontalZoomFromInput,
      setVerticalZoomFromInput,
      syncVerticalScroll,
      syncTrackScroll,
      handleTimelineWheel,
      handlePianoWheel,
      startPlayheadDrag,
      startClipDrag,
      dropSyllable,
    },
  };
}

export type StudioController = ReturnType<typeof useStudioController>;
