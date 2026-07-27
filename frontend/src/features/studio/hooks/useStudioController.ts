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
  chordLabelAtPosition,
  snapClipsToChords,
  snapPitchToChord,
} from "@/features/studio/lib/chord-snap";
import {
  createMusicalTimeline,
  formatMusicalPosition,
  getMusicalContext,
} from "@/features/studio/lib/musical-time";
import { createPitchPath, PITCH_ROW_HEIGHT, PITCH_TOP } from "@/features/studio/lib/pitch";
import {
  createManualClip,
  getJoinCandidateIds,
  joinClips,
  MAX_GAIN_DB,
  MAX_VIBRATO_SCALE,
  MIN_CLIP_WIDTH_PERCENT,
  MIN_GAIN_DB,
  moveSelectedClips,
  nextManualClipId,
  PITCH_BOTTOM,
  scaleClipVibrato,
  setClipGain,
  splitClipAt,
  stretchClipStartTo,
  stretchClipTo,
} from "@/features/studio/lib/pitch-editing";
import { getNextTrackSelection } from "@/features/studio/lib/track-selection";
import { formatTime, getCenteredTimelineScrollLeft } from "@/features/studio/lib/transport";

const HORIZONTAL_ZOOM_MIN = 1;
const HORIZONTAL_ZOOM_MAX = 32;
const VERTICAL_ZOOM_MIN = 1;
const VERTICAL_ZOOM_MAX = 4;

type PitchEditorGesture =
  | {
      kind: "move";
      clipIds: string[];
      startClientY: number;
      source: VocalClip[];
      result: VocalClip[];
      changed: boolean;
      auditionClipId: string;
      lastAuditionPitch: number | null;
    }
  | {
      kind: "pencil";
      anchorX: number;
      requestedPitch: number;
      clip: VocalClip;
    }
  | {
      kind: "stretch";
      clipId: string;
      edge: "start" | "end";
      startClientX: number;
      originalStartX: number;
      originalEndX: number;
      source: VocalClip[];
      result: VocalClip[];
    }
  | {
      kind: "vibrato";
      clipId: string;
      startClientY: number;
      source: VocalClip[];
      result: VocalClip[];
      scale: number;
    }
  | {
      kind: "gain";
      clipId: string;
      startClientY: number;
      source: VocalClip[];
      result: VocalClip[];
      originalGainDb: number;
      gainDb: number;
    };

export function useStudioController(bridge: PluginBridge, project: StudioProject) {
  const [playing, setPlaying] = useState(false);
  const [playhead, setPlayhead] = useState(project.initialPlayhead);
  const [looping, setLoopingState] = useState(project.initialLoopActive);
  const [cycleRange, setCycleRangeState] = useState(project.initialCycleRange);
  const [volume, setVolume] = useState(project.initialVolume);
  const [outputLevels, setOutputLevels] = useState({ left: 0, right: 0 });
  const [clips, setClips] = useState(project.clips);
  const [backingClipsByTrack, setBackingClipsByTrack] = useState<
    Partial<Record<BackingTrackName, VocalClip[]>>
  >(
    Object.fromEntries(
      project.backingTrackContents.map(({ track, clips: trackClips }) => [track, trackClips]),
    ),
  );
  const [horizontalZoom, setHorizontalZoom] = useState(1);
  const [verticalZoom, setVerticalZoom] = useState(1);
  const [leftCorrectionTool, setLeftCorrectionTool] = useState<CorrectionToolId>("pointer");
  const [rightCorrectionTool, setRightCorrectionTool] = useState<CorrectionToolId>("scissors");
  const [chordSnapEnabled, setChordSnapEnabled] = useState(false);
  const [secondaryToolModifierActive, setSecondaryToolModifierActive] = useState(false);
  const [selectedClipIds, setSelectedClipIds] = useState<string[]>([]);
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
    const syncCommandModifier = (event: KeyboardEvent) => {
      setSecondaryToolModifierActive(event.metaKey);
    };
    const clearCommandModifier = () => setSecondaryToolModifierActive(false);

    window.addEventListener("keydown", syncCommandModifier);
    window.addEventListener("keyup", syncCommandModifier);
    window.addEventListener("blur", clearCommandModifier);
    return () => {
      window.removeEventListener("keydown", syncCommandModifier);
      window.removeEventListener("keyup", syncCommandModifier);
      window.removeEventListener("blur", clearCommandModifier);
    };
  }, []);

  useEffect(() => {
    setClips(project.clips);
  }, [project.clips]);

  useEffect(() => {
    setBackingClipsByTrack(
      Object.fromEntries(
        project.backingTrackContents.map(({ track, clips: trackClips }) => [track, trackClips]),
      ),
    );
  }, [project.backingTrackContents]);

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
  const editorGestureRef = useRef<PitchEditorGesture | null>(null);
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
            if (event.track === "Voice Main") {
              setClips((current) =>
                current.map((clip) =>
                  clip.id === event.clipId ? { ...clip, pitch: event.pitch } : clip,
                ),
              );
            } else {
              const backingTrack = event.track as BackingTrackName;
              setBackingClipsByTrack((current) => ({
                ...current,
                [backingTrack]: (current[backingTrack] ?? []).map((clip) =>
                  clip.id === event.clipId ? { ...clip, pitch: event.pitch } : clip,
                ),
              }));
            }
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
        notesAvailable: (backingClipsByTrack[track]?.length ?? 0) > 0,
      };
    }
    return availability;
  }, [
    backingClipsByTrack,
    backingTracks,
    clips.length,
    project.backingTrackContents,
    project.hasVocal,
  ]);
  const activePitchTrack: Exclude<TrackName, "Instrumental"> | null =
    selectedTracks.length === 1 && selectedTracks[0] !== "Instrumental" ? selectedTracks[0] : null;
  const chordSnapAvailable =
    activePitchTrack !== null && activePitchTrack !== "Voice Main" && project.chords.length > 0;
  const chordSnapActive = chordSnapEnabled && chordSnapAvailable;
  const activeEditorClips =
    activePitchTrack === null
      ? []
      : activePitchTrack === "Voice Main"
        ? clips
        : (backingClipsByTrack[activePitchTrack] ?? []);
  const editorClips = useMemo(
    () =>
      selectedTracks.flatMap((track) => {
        if (track === "Instrumental") return [];
        if (track === "Voice Main") return clips;
        return backingClipsByTrack[track] ?? [];
      }),
    [backingClipsByTrack, clips, selectedTracks],
  );
  useEffect(() => {
    const availableIds = new Set(editorClips.map((clip) => clip.id));
    setSelectedClipIds((current) => current.filter((clipId) => availableIds.has(clipId)));
  }, [editorClips]);
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
  const canEditSelectedClips = activePitchTrack !== null;
  const pitchHistory = project.pitchEditorHistory.find(
    ({ track }) => track === activePitchTrack,
  ) ?? { track: activePitchTrack ?? "Voice Main", canUndo: false, canRedo: false };
  const backingRenderAction =
    activePitchTrack !== null && activePitchTrack !== "Voice Main" && selectedTrackContent
      ? !selectedTrackContent.hasAudio
        ? {
            track: activePitchTrack,
            label: "Render" as const,
            voiceProfileId: selectedTrackContent.voiceProfileId,
          }
        : {
            track: activePitchTrack,
            label: "Re-render" as const,
            voiceProfileId: selectedTrackContent.voiceProfileId,
          }
      : null;
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
    setSelectedClipIds([]);
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

  const setBackingVoiceProfile = (name: BackingTrackName, profileId: string) => {
    bridge.send({
      type: "set-backing-voice-profile",
      track: name,
      profileId,
    });
  };

  const undoPitchEdit = () => {
    if (activePitchTrack !== null && pitchHistory.canUndo) {
      bridge.send({ type: "pitch-history", action: "undo", track: activePitchTrack });
    }
  };

  const redoPitchEdit = () => {
    if (activePitchTrack !== null && pitchHistory.canRedo) {
      bridge.send({ type: "pitch-history", action: "redo", track: activePitchTrack });
    }
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

  const editorPoint = (clientX: number, clientY: number) => {
    const editor = editorRef.current;
    if (!editor) return null;
    const rect = editor.getBoundingClientRect();
    const x = Math.max(0, Math.min(100, ((clientX - rect.left) / Math.max(1, rect.width)) * 100));
    const pitch = Math.max(
      PITCH_BOTTOM,
      Math.min(
        PITCH_TOP,
        PITCH_TOP - ((clientY - rect.top) / Math.max(1, rect.height)) * (100 / PITCH_ROW_HEIGHT),
      ),
    );
    return { rect, x, pitch };
  };

  const toolForPointerEvent = (event: { button: number; metaKey: boolean }) =>
    event.button === 2 || (event.button === 0 && event.metaKey)
      ? rightCorrectionTool
      : leftCorrectionTool;

  const setActiveEditorClips = (next: VocalClip[] | ((current: VocalClip[]) => VocalClip[])) => {
    if (activePitchTrack === null) return;
    if (activePitchTrack === "Voice Main") {
      setClips(next);
      return;
    }

    setBackingClipsByTrack((current) => {
      const currentClips = current[activePitchTrack] ?? [];
      const nextClips = typeof next === "function" ? next(currentClips) : next;
      return { ...current, [activePitchTrack]: nextClips };
    });
  };

  const moveEditorGesture = (event: PointerEvent) => {
    const gesture = editorGestureRef.current;
    const point = editorPoint(event.clientX, event.clientY);
    if (!gesture || !point) return;

    if (gesture.kind === "move") {
      const deltaPitch =
        (-(event.clientY - gesture.startClientY) / Math.max(1, point.rect.height)) *
        (100 / PITCH_ROW_HEIGHT);
      gesture.changed = gesture.changed || Math.abs(deltaPitch) > 0.01;
      const movedClips = moveSelectedClips(gesture.source, gesture.clipIds, deltaPitch);
      gesture.result = chordSnapActive
        ? snapClipsToChords(movedClips, gesture.clipIds, project.chords, PITCH_BOTTOM, PITCH_TOP)
        : movedClips;
      setActiveEditorClips(gesture.result);
      const auditionClip = gesture.result.find(
        (candidate) => candidate.id === gesture.auditionClipId,
      );
      const auditionPitch = auditionClip?.pitch ?? null;
      if (
        Math.abs(deltaPitch) > 0.01 &&
        auditionPitch !== null &&
        (gesture.lastAuditionPitch === null ||
          Math.abs(auditionPitch - gesture.lastAuditionPitch) >= 0.01)
      ) {
        gesture.lastAuditionPitch = auditionPitch;
        bridge.send({ type: "preview-pitch-tone", pitch: auditionPitch, restart: false });
      }
      return;
    }

    if (gesture.kind === "pencil") {
      if (Math.abs(point.x - gesture.anchorX) > MIN_CLIP_WIDTH_PERCENT) {
        const x = Math.min(point.x, gesture.anchorX);
        const width = Math.abs(point.x - gesture.anchorX);
        const pitch = chordSnapActive
          ? snapPitchToChord(
              gesture.requestedPitch,
              chordLabelAtPosition(project.chords, x),
              PITCH_BOTTOM,
              PITCH_TOP,
            )
          : gesture.requestedPitch;
        gesture.clip = createManualClip(gesture.clip.id, x, pitch, width, gesture.clip.color);
        setActiveEditorClips((current) =>
          current.map((clip) => (clip.id === gesture.clip.id ? gesture.clip : clip)),
        );
      }
      return;
    }

    if (gesture.kind === "stretch") {
      const deltaX = ((event.clientX - gesture.startClientX) / Math.max(1, point.rect.width)) * 100;
      gesture.result =
        gesture.edge === "start"
          ? stretchClipStartTo(gesture.source, gesture.clipId, gesture.originalStartX + deltaX)
          : stretchClipTo(gesture.source, gesture.clipId, gesture.originalEndX + deltaX);
      if (chordSnapActive) {
        gesture.result = snapClipsToChords(
          gesture.result,
          [gesture.clipId],
          project.chords,
          PITCH_BOTTOM,
          PITCH_TOP,
        );
      }
      setActiveEditorClips(gesture.result);
      return;
    }

    if (gesture.kind === "vibrato") {
      gesture.scale = Math.max(
        0,
        Math.min(MAX_VIBRATO_SCALE, 1 + (gesture.startClientY - event.clientY) / 64),
      );
      gesture.result = scaleClipVibrato(gesture.source, gesture.clipId, gesture.scale);
      setActiveEditorClips(gesture.result);
      return;
    }

    const gainDelta = ((gesture.startClientY - event.clientY) / 96) * 12;
    gesture.gainDb = Math.max(
      MIN_GAIN_DB,
      Math.min(MAX_GAIN_DB, gesture.originalGainDb + gainDelta),
    );
    gesture.result = setClipGain(gesture.source, gesture.clipId, gesture.gainDb);
    setActiveEditorClips(gesture.result);
  };

  const endEditorGesture = () => {
    const gesture = editorGestureRef.current;
    editorGestureRef.current = null;
    window.removeEventListener("pointermove", moveEditorGesture);
    window.removeEventListener("pointerup", endEditorGesture);
    window.removeEventListener("pointercancel", endEditorGesture);
    window.removeEventListener("blur", endEditorGesture);
    if (!gesture) return;

    if (gesture.kind === "move") {
      bridge.send({ type: "stop-pitch-tone" });
      if (gesture.changed) {
        const edits = gesture.clipIds.flatMap((clipId) => {
          const clip = gesture.result.find((candidate) => candidate.id === clipId);
          return clip ? [{ clipId, pitch: clip.pitch }] : [];
        });
        if (edits.length > 0 && activePitchTrack !== null) {
          bridge.send({
            type: "move-clips",
            track: activePitchTrack,
            clips: edits,
            snapToChord: chordSnapActive,
          });
        }
      }
      return;
    }

    if (gesture.kind === "pencil") {
      if (activePitchTrack !== null) {
        bridge.send({
          type: "add-clip",
          track: activePitchTrack,
          clip: {
            id: gesture.clip.id,
            x: gesture.clip.x,
            pitch: gesture.clip.pitch,
            width: gesture.clip.width,
          },
          snapToChord: chordSnapActive,
        });
      }
      return;
    }

    if (gesture.kind === "stretch") {
      const clip = gesture.result.find((candidate) => candidate.id === gesture.clipId);
      if (clip) {
        if (activePitchTrack !== null) {
          bridge.send({
            type: "set-clip-time",
            track: activePitchTrack,
            clipId: clip.id,
            x: clip.x,
            width: clip.width,
            snapToChord: chordSnapActive,
          });
        }
      }
      return;
    }

    if (gesture.kind === "vibrato") {
      if (activePitchTrack !== null) {
        bridge.send({
          type: "set-clip-vibrato",
          track: activePitchTrack,
          clipId: gesture.clipId,
          scale: gesture.scale,
        });
      }
      return;
    }

    if (activePitchTrack !== null) {
      bridge.send({
        type: "set-clip-gain",
        track: activePitchTrack,
        clipId: gesture.clipId,
        gainDb: gesture.gainDb,
      });
    }
  };

  const beginEditorGesture = (gesture: PitchEditorGesture) => {
    endEditorGesture();
    editorGestureRef.current = gesture;
    window.addEventListener("pointermove", moveEditorGesture);
    window.addEventListener("pointerup", endEditorGesture);
    window.addEventListener("pointercancel", endEditorGesture);
    window.addEventListener("blur", endEditorGesture);
  };

  const zoomAtEditorPosition = (x: number, zoomOut: boolean) => {
    const nextZoom = Math.max(
      HORIZONTAL_ZOOM_MIN,
      Math.min(HORIZONTAL_ZOOM_MAX, horizontalZoom * (zoomOut ? 0.5 : 2)),
    );
    cancelAnimationFrame(horizontalZoomFrameRef.current);
    setHorizontalZoom(nextZoom);
    horizontalZoomFrameRef.current = requestAnimationFrame(() => {
      horizontalZoomFrameRef.current = requestAnimationFrame(() => {
        const scroller = arrangementScrollRef.current;
        if (!scroller) return;
        scroller.scrollLeft = getCenteredTimelineScrollLeft(
          x / 100,
          scroller.scrollWidth,
          scroller.clientWidth,
        );
      });
    });
  };

  const beginPencilGesture = (event: ReactPointerEvent<HTMLElement>) => {
    const point = editorPoint(event.clientX, event.clientY);
    if (!point) return;
    const id = nextManualClipId(activeEditorClips);
    const defaultWidth = Math.max(
      MIN_CLIP_WIDTH_PERCENT,
      Math.min(100 - point.x, (0.5 / Math.max(0.001, project.timelineDurationSeconds)) * 100),
    );
    const colours: VocalClip["color"][] = ["cyan", "violet", "pink", "amber", "lime", "silver"];
    const pitch = chordSnapActive
      ? snapPitchToChord(
          point.pitch,
          chordLabelAtPosition(project.chords, point.x),
          PITCH_BOTTOM,
          PITCH_TOP,
        )
      : point.pitch;
    const clip = createManualClip(
      id,
      point.x,
      pitch,
      defaultWidth,
      colours[activeEditorClips.length % colours.length],
    );
    setActiveEditorClips((current) => [...current, clip]);
    setSelectedClipIds([id]);
    beginEditorGesture({
      kind: "pencil",
      anchorX: point.x,
      requestedPitch: point.pitch,
      clip,
    });
  };

  const startClipTool = (event: ReactPointerEvent<HTMLButtonElement>, clip: VocalClip) => {
    if (!canEditSelectedClips || (event.button !== 0 && event.button !== 2)) return;
    const tool = toolForPointerEvent(event);
    const point = editorPoint(event.clientX, event.clientY);
    if (!point) return;
    event.preventDefault();
    event.stopPropagation();

    if (tool === "pointer") {
      let nextSelection: string[];
      if (event.shiftKey) {
        nextSelection = selectedClipIds.includes(clip.id)
          ? selectedClipIds.filter((clipId) => clipId !== clip.id)
          : [...selectedClipIds, clip.id];
      } else {
        nextSelection = selectedClipIds.includes(clip.id) ? selectedClipIds : [clip.id];
      }
      setSelectedClipIds(nextSelection);
      if (nextSelection.includes(clip.id)) {
        event.currentTarget.setPointerCapture(event.pointerId);
        beginEditorGesture({
          kind: "move",
          clipIds: nextSelection,
          startClientY: event.clientY,
          source: activeEditorClips,
          result: activeEditorClips,
          changed: false,
          auditionClipId: clip.id,
          lastAuditionPitch: clip.pitch,
        });
        bridge.send({ type: "preview-pitch-tone", pitch: clip.pitch, restart: true });
      }
      return;
    }

    if (tool === "pencil") {
      beginPencilGesture(event);
      return;
    }

    if (tool === "eraser") {
      setActiveEditorClips((current) => current.filter((candidate) => candidate.id !== clip.id));
      setSelectedClipIds((current) => current.filter((clipId) => clipId !== clip.id));
      if (activePitchTrack !== null) {
        bridge.send({ type: "delete-clips", track: activePitchTrack, clipIds: [clip.id] });
      }
      return;
    }

    if (tool === "scissors") {
      const rightClipId = nextManualClipId(activeEditorClips);
      const splitResult = splitClipAt(activeEditorClips, clip.id, point.x, rightClipId);
      const result = chordSnapActive
        ? snapClipsToChords(
            splitResult,
            [clip.id, rightClipId],
            project.chords,
            PITCH_BOTTOM,
            PITCH_TOP,
          )
        : splitResult;
      if (result !== activeEditorClips) {
        setActiveEditorClips(result);
        setSelectedClipIds([clip.id, rightClipId]);
        if (activePitchTrack !== null) {
          bridge.send({
            type: "split-clip",
            track: activePitchTrack,
            clipId: clip.id,
            x: point.x,
            rightClipId,
            snapToChord: chordSnapActive,
          });
        }
      }
      return;
    }

    if (tool === "join") {
      const clipIds = getJoinCandidateIds(activeEditorClips, selectedClipIds, clip.id);
      const joinedResult = joinClips(activeEditorClips, clipIds);
      const joinedClipId = [...activeEditorClips]
        .filter((candidate) => clipIds.includes(candidate.id))
        .sort((left, right) => left.x - right.x)[0]?.id;
      const result =
        chordSnapActive && joinedClipId
          ? snapClipsToChords(joinedResult, [joinedClipId], project.chords, PITCH_BOTTOM, PITCH_TOP)
          : joinedResult;
      if (result !== activeEditorClips) {
        const target = [...activeEditorClips]
          .filter((candidate) => clipIds.includes(candidate.id))
          .sort((left, right) => left.x - right.x)[0];
        setActiveEditorClips(result);
        setSelectedClipIds(target ? [target.id] : []);
        if (activePitchTrack !== null) {
          bridge.send({
            type: "join-clips",
            track: activePitchTrack,
            clipIds,
            snapToChord: chordSnapActive,
          });
        }
      }
      return;
    }

    if (tool === "flex") {
      const clipBounds = event.currentTarget.getBoundingClientRect();
      const edge = event.clientX < clipBounds.left + clipBounds.width * 0.5 ? "start" : "end";
      setSelectedClipIds([clip.id]);
      beginEditorGesture({
        kind: "stretch",
        clipId: clip.id,
        edge,
        startClientX: event.clientX,
        originalStartX: clip.x,
        originalEndX: clip.x + clip.width,
        source: activeEditorClips,
        result: activeEditorClips,
      });
      return;
    }

    if (tool === "vibrato") {
      setSelectedClipIds([clip.id]);
      beginEditorGesture({
        kind: "vibrato",
        clipId: clip.id,
        startClientY: event.clientY,
        source: activeEditorClips,
        result: activeEditorClips,
        scale: 1,
      });
      return;
    }

    if (tool === "gain") {
      const originalGainDb = clip.gainDb ?? 0;
      setSelectedClipIds([clip.id]);
      beginEditorGesture({
        kind: "gain",
        clipId: clip.id,
        startClientY: event.clientY,
        source: activeEditorClips,
        result: activeEditorClips,
        originalGainDb,
        gainDb: originalGainDb,
      });
      return;
    }

    zoomAtEditorPosition(point.x, event.button === 2 || event.metaKey || event.altKey);
  };

  const startEditorTool = (event: ReactPointerEvent<HTMLFieldSetElement>) => {
    if (!canEditSelectedClips || (event.button !== 0 && event.button !== 2)) return;
    const tool = toolForPointerEvent(event);
    const point = editorPoint(event.clientX, event.clientY);
    if (!point) return;

    if (tool === "pointer") {
      setSelectedClipIds([]);
      return;
    }
    if (tool === "pencil") {
      event.preventDefault();
      beginPencilGesture(event);
      return;
    }
    if (tool === "zoom") {
      event.preventDefault();
      zoomAtEditorPosition(point.x, event.button === 2 || event.metaKey || event.altKey);
    }
  };

  const dropSyllable = (event: DragEvent<HTMLFieldSetElement>) => {
    event.preventDefault();
    if (!canEditSelectedClips || activePitchTrack !== "Voice Main") return;
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
      endEditorGesture();
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
    activePitchTrack,
    pitchHistory,
    backingRenderAction,
    horizontalZoom,
    verticalZoom,
    leftCorrectionTool,
    rightCorrectionTool,
    chordSnapEnabled,
    chordSnapAvailable,
    secondaryToolModifierActive,
    selectedClipIds,
    pianoCollapsed,
    serviceTracksCollapsed,
    backingTracks,
    voiceProfiles: project.voiceProfiles,
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
      setBackingVoiceProfile,
      undoPitchEdit,
      redoPitchEdit,
      openProject,
      saveProject,
      exportAllTracks,
      exportMidiFiles,
      setLeftCorrectionTool,
      setRightCorrectionTool,
      setChordSnapEnabled,
      setPianoCollapsed,
      setServiceTracksCollapsed,
      setHorizontalZoomFromInput,
      setVerticalZoomFromInput,
      syncVerticalScroll,
      syncTrackScroll,
      handleTimelineWheel,
      handlePianoWheel,
      startPlayheadDrag,
      startClipTool,
      startEditorTool,
      dropSyllable,
    },
  };
}

export type StudioController = ReturnType<typeof useStudioController>;
