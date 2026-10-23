import {
  type DragEvent,
  type PointerEvent as ReactPointerEvent,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
  type WheelEvent,
} from "react";
import type { PluginBridge } from "@/bridge/plugin-bridge";
import type {
  BackingGenerator,
  BackingTrackInstance,
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
  arrangementRows,
  arrangementWaveform,
  projectTracks,
  visibleTrackRows,
} from "@/features/studio/lib/arrangement";
import {
  chordLabelAtPosition,
  chordPitchClasses,
  snapClipsToChords,
  snapPitchToChord,
} from "@/features/studio/lib/chord-snap";
import type { ShortcutBindings } from "@/features/studio/lib/hotkeys";
import {
  createMusicalTimeline,
  formatMusicalPosition,
  getMusicalContext,
} from "@/features/studio/lib/musical-time";
import {
  createPitchPath,
  PITCH_ROW_HEIGHT,
  PITCH_ROWS,
  PITCH_TOP,
} from "@/features/studio/lib/pitch";
import {
  createManualClip,
  getJoinCandidateIds,
  getMinimumClipWidthPercent,
  joinClips,
  MAX_GAIN_DB,
  MAX_VIBRATO_SCALE,
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
import {
  formatTime,
  getCenteredTimelineScrollLeft,
  getGestureZoom,
} from "@/features/studio/lib/transport";

const HORIZONTAL_ZOOM_MIN = 1;
const HORIZONTAL_ZOOM_MAX = 32;
const VERTICAL_ZOOM_MIN = 1;
const VERTICAL_ZOOM_MAX = 4;
const BACKING_GENERATOR_STORAGE_KEY = "synthetic-obsidian.backing-generator";
const initialBackingGenerator = (): BackingGenerator => {
  const stored = window.localStorage.getItem(BACKING_GENERATOR_STORAGE_KEY);
  return stored === "local" || stored === "terra" || stored === "luna" ? stored : "luna";
};
const normaliseWheelDelta = (deltaY: number, deltaMode: number) =>
  deltaY * (deltaMode === 1 ? 16 : deltaMode === 2 ? window.innerHeight : 1);

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

export function useStudioController(
  bridge: PluginBridge,
  project: StudioProject,
  shortcutBindings: ShortcutBindings,
) {
  const arrangementTracks = useMemo(() => projectTracks(project), [project]);
  const trackRows = useMemo(() => visibleTrackRows(arrangementTracks), [arrangementTracks]);
  const displayRows = useMemo(() => arrangementRows(arrangementTracks), [arrangementTracks]);
  const minimumClipWidth = getMinimumClipWidthPercent(project.timelineDurationSeconds);
  const [playing, setPlaying] = useState(false);
  const [playhead, setPlayhead] = useState(project.initialPlayhead);
  const [looping, setLoopingState] = useState(project.initialLoopActive);
  const [cycleRange, setCycleRangeState] = useState(project.initialCycleRange);
  const loopingRef = useRef(project.initialLoopActive);
  const cycleRangeRef = useRef(project.initialCycleRange);
  const [volume, setVolume] = useState(project.initialVolume);
  const [instrumentalVolume, setInstrumentalVolume] = useState(
    project.initialInstrumentalVolume ?? 70,
  );
  const [backingGenerator, setBackingGeneratorState] =
    useState<BackingGenerator>(initialBackingGenerator);
  const [outputLevels, setOutputLevels] = useState({ left: 0, right: 0 });
  const [clips, setClips] = useState(project.clips);
  const [backingClipsByTrack, setBackingClipsByTrack] = useState<
    Partial<Record<string, VocalClip[]>>
  >(
    Object.fromEntries(
      project.backingTrackContents.map(({ id, clips: trackClips }) => [id, trackClips]),
    ),
  );
  const [horizontalZoom, setHorizontalZoom] = useState(1);
  const [verticalZoom, setVerticalZoom] = useState(1);
  const [leftCorrectionTool, setLeftCorrectionTool] = useState<CorrectionToolId>("pointer");
  const [rightCorrectionTool, setRightCorrectionTool] = useState<CorrectionToolId>("scissors");
  const [chordSnapEnabled, setChordSnapEnabled] = useState(false);
  const [dragChordPitchClasses, setDragChordPitchClasses] = useState<number[]>([]);
  const [secondaryToolModifierActive, setSecondaryToolModifierActive] = useState(false);
  const [selectedClipIds, setSelectedClipIds] = useState<string[]>([]);
  const [pianoCollapsed, setPianoCollapsed] = useState(false);
  const [serviceTracksCollapsed, setServiceTracksCollapsed] = useState(false);
  const [backingTracks, setBackingTracks] = useState<BackingTrackInstance[]>(
    project.initialBackingTracks,
  );
  const [selectedTracks, setSelectedTracks] = useState<TrackName[]>([
    project.hasVocal
      ? "Voice Main"
      : project.hasInstrumental
        ? "Instrumental"
        : (project.initialBackingTracks[0]?.id ?? "Voice Main"),
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
        project.backingTrackContents.map(({ id, clips: trackClips }) => [id, trackClips]),
      ),
    );
  }, [project.backingTrackContents]);

  useEffect(() => {
    setBackingTracks(project.initialBackingTracks);
  }, [project.initialBackingTracks]);

  useEffect(() => {
    const availableTracks = new Set(trackRows.map(({ track }) => track.id));
    setSelectedTracks((current) => {
      if (current.length === 0) return current;
      const availableSelection = current.filter((track) => availableTracks.has(track));
      if (availableSelection.length > 0) return availableSelection;
      const hidden = arrangementTracks.find((track) => current.includes(track.id));
      if (hidden?.parentId && availableTracks.has(hidden.parentId)) return [hidden.parentId];
      return trackRows[0] ? [trackRows[0].track.id] : [];
    });
  }, [arrangementTracks, trackRows]);

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

  useEffect(() => {
    setInstrumentalVolume(project.initialInstrumentalVolume ?? 70);
  }, [project.initialInstrumentalVolume]);

  const editorRef = useRef<HTMLFieldSetElement>(null);
  const arrangementScrollRef = useRef<HTMLElement>(null);
  const timelineContentRef = useRef<HTMLDivElement>(null);
  const pianoRollScrollRef = useRef<HTMLDivElement>(null);
  const horizontalZoomAnchorRef = useRef<number | null>(null);
  const verticalZoomAnchorRef = useRef<number | null>(null);
  const pianoViewportInitialised = useRef(false);
  const zoomWheelRef = useRef({ frame: 0, horizontal: 0, vertical: 0 });
  const editorGestureRef = useRef<PitchEditorGesture | null>(null);
  const playheadDragRef = useRef(false);

  useEffect(
    () =>
      bridge.subscribe((event) => {
        switch (event.type) {
          case "project-state":
          case "status-state":
          case "audio-settings-state":
            break;
          case "transport-state":
            setPlaying(event.playing);
            setPlayhead(event.playhead);
            break;
          case "loop-state":
            loopingRef.current = event.active;
            cycleRangeRef.current = event.range;
            setLoopingState(event.active);
            setCycleRangeState(event.range);
            break;
          case "volume-state":
            setVolume(event.value);
            break;
          case "instrumental-volume-state":
            setInstrumentalVolume(event.value);
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
          case "track-selected":
            setSelectedTracks([event.track]);
            setSelectedClipIds([]);
            bridge.send({ type: "select-tracks", tracks: [event.track] });
            break;
          case "backing-track-created":
            setBackingTracks((current) =>
              current.some((track) => track.id === event.track.id)
                ? current
                : [...current, event.track],
            );
            setSelectedTracks([event.track.id]);
            setSelectedClipIds([]);
            bridge.send({ type: "select-tracks", tracks: [event.track.id] });
            break;
          case "clip-pitch-state":
            if (event.track === "Voice Main") {
              setClips((current) =>
                current.map((clip) =>
                  clip.id === event.clipId ? { ...clip, pitch: event.pitch } : clip,
                ),
              );
            } else {
              const backingTrack = event.track;
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
    if (!playing || window.syntheticObsidianHost) return;
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

  useLayoutEffect(() => {
    const selected = selectedTracks[0];
    if (!selected) return;
    arrangementScrollRef.current
      ?.querySelector<HTMLElement>(`[data-track-header="${CSS.escape(selected)}"]`)
      ?.scrollIntoView({ block: "nearest", inline: "nearest" });
  }, [selectedTracks]);

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
      const content = project.backingTrackContents.find((item) => item.id === track.id);
      availability[track.id] = {
        audioAvailable: content?.hasAudio ?? false,
        notesAvailable: (backingClipsByTrack[track.id]?.length ?? 0) > 0,
      };
    }
    for (const track of arrangementTracks) {
      availability[track.id] = {
        audioAvailable: track.clips.some((clip) =>
          Array.isArray(clip.waveform) ? clip.waveform.length > 0 : clip.waveform.pointCount > 0,
        ),
        notesAvailable:
          track.id === "Voice Main"
            ? clips.length > 0
            : (backingClipsByTrack[track.id]?.length ?? 0) > 0,
      };
    }
    return availability;
  }, [
    arrangementTracks,
    backingClipsByTrack,
    backingTracks,
    clips.length,
    project.backingTrackContents,
    project.hasVocal,
  ]);
  const activePitchTrack: Exclude<TrackName, "Instrumental"> | null =
    selectedTracks.length === 1 && selectedTracks[0] !== "Instrumental" ? selectedTracks[0] : null;
  const chordSnapAvailable = activePitchTrack !== null && project.chords.length > 0;
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
      ? project.backingTrackContents.find((item) => item.id === selectedTracks[0])
      : undefined;
  const arrangedEditorWaveform = useMemo(() => {
    if (!project.tracks || selectedTracks.length !== 1) return null;
    const track = arrangementTracks.find((item) => item.id === selectedTracks[0]);
    return track ? arrangementWaveform(track, project.timelineDurationSeconds) : null;
  }, [project.tracks, selectedTracks, arrangementTracks, project.timelineDurationSeconds]);
  const editorWaveform =
    arrangedEditorWaveform ??
    (selectedTracks.length !== 1
      ? []
      : selectedTracks[0] === "Instrumental"
        ? project.instrumentalWaveform
        : selectedTracks[0] === "Voice Main"
          ? project.vocalWaveform
          : (selectedTrackContent?.waveform ?? []));
  const editorWaveformDurationRatio = arrangedEditorWaveform
    ? 1
    : selectedTracks.length !== 1
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
    activePitchTrack !== null &&
    arrangementTracks.find((track) => track.id === activePitchTrack)?.parentId &&
    selectedTrackContent
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
    const next = !playing;
    setPlaying(next);
    bridge.send({
      type: "transport",
      action: next ? "play" : "pause",
    });
  };

  const stopPlayback = () => {
    setPlaying(false);
    bridge.send({ type: "transport", action: "stop" });
  };

  const setLooping = (active: boolean) => {
    loopingRef.current = active;
    setLoopingState(active);
    bridge.send({ type: "set-loop", active, range: cycleRangeRef.current });
  };

  const setCycleRange = (range: CycleRange, active: boolean) => {
    cycleRangeRef.current = range;
    loopingRef.current = active;
    setCycleRangeState(range);
    setLoopingState(active);
  };

  const commitCycleRange = (range: CycleRange, active: boolean) => {
    setCycleRange(range, active);
    bridge.send({ type: "set-loop", active, range });
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

  const setInstrumentalVolumeValue = (value: number) => {
    setInstrumentalVolume(value);
    bridge.send({ type: "set-instrumental-volume", value });
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

  const addBackingTrack = (name: BackingTrackName, parentId = selectedTracks[0]) => {
    if (!parentId) return;
    bridge.send({ type: "add-backing-track", track: name, generator: backingGenerator, parentId });
  };

  const regenerateBackingTrack = (trackId: string) => {
    bridge.send({ type: "regenerate-backing-track", track: trackId, generator: backingGenerator });
  };

  const setBackingGenerator = (generator: BackingGenerator) => {
    setBackingGeneratorState(generator);
    window.localStorage.setItem(BACKING_GENERATOR_STORAGE_KEY, generator);
  };

  const renderBackingTrack = (trackId: string) => {
    bridge.send({ type: "render-backing-track", track: trackId });
  };

  const removeBackingTrack = (trackId: string) => {
    bridge.send({ type: "remove-backing-track", track: trackId });
  };

  const setBackingVoiceProfile = (trackId: string, profileId: string) => {
    bridge.send({
      type: "set-backing-voice-profile",
      track: trackId,
      profileId,
    });
  };

  const deleteVoiceProfile = (profileId: string) => {
    bridge.send({ type: "delete-voice-profile", profileId });
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

  const newProject = () => {
    bridge.send({ type: "project-action", action: "new-project" });
  };

  const saveProject = () => {
    bridge.send({ type: "project-action", action: "save-project" });
  };

  const undo = () => {
    bridge.send({ type: "project-action", action: "undo" });
  };

  const redo = () => {
    bridge.send({ type: "project-action", action: "redo" });
  };

  const exportAllTracks = () => {
    bridge.send({ type: "project-action", action: "export-all-tracks" });
  };

  const exportMidiFiles = () => {
    bridge.send({ type: "project-action", action: "export-midi" });
  };

  const syncHorizontalScroll = (source: "arrangement" | "piano") => {
    const from =
      source === "arrangement" ? arrangementScrollRef.current : pianoRollScrollRef.current;
    const to = source === "arrangement" ? pianoRollScrollRef.current : arrangementScrollRef.current;
    if (from && to && Math.abs(to.scrollLeft - from.scrollLeft) > 0.5)
      to.scrollLeft = from.scrollLeft;
  };

  useLayoutEffect(() => {
    const scroller = pianoRollScrollRef.current;
    if (
      !pianoCollapsed &&
      (project.hasVocal || project.initialBackingTracks.length > 0) &&
      scroller &&
      scroller.clientHeight > 0 &&
      !pianoViewportInitialised.current
    ) {
      scroller.scrollTop =
        ((PITCH_TOP - 60) / PITCH_ROWS) * scroller.scrollHeight - scroller.clientHeight / 2;
      pianoViewportInitialised.current = true;
    }
  }, [pianoCollapsed, project.hasVocal, project.initialBackingTracks.length]);

  const changeHorizontalZoom = (wheelDelta: number) => {
    horizontalZoomAnchorRef.current = playhead / 100;
    setHorizontalZoom((current) =>
      getGestureZoom(current, wheelDelta, HORIZONTAL_ZOOM_MIN, HORIZONTAL_ZOOM_MAX),
    );
  };

  const changeVerticalZoom = (wheelDelta: number) => {
    const scroller = pianoRollScrollRef.current;
    if (scroller) {
      verticalZoomAnchorRef.current =
        (scroller.scrollTop + scroller.clientHeight / 2) / Math.max(1, scroller.scrollHeight);
    }
    setVerticalZoom((current) =>
      getGestureZoom(current, wheelDelta, VERTICAL_ZOOM_MIN, VERTICAL_ZOOM_MAX),
    );
  };

  const setHorizontalZoomFromInput = (value: number) => {
    horizontalZoomAnchorRef.current = playhead / 100;
    setHorizontalZoom(Math.max(HORIZONTAL_ZOOM_MIN, Math.min(HORIZONTAL_ZOOM_MAX, value)));
  };

  const setVerticalZoomFromInput = (value: number) => {
    const scroller = pianoRollScrollRef.current;
    if (scroller) {
      verticalZoomAnchorRef.current =
        (scroller.scrollTop + scroller.clientHeight / 2) / Math.max(1, scroller.scrollHeight);
    }
    setVerticalZoom(Math.max(VERTICAL_ZOOM_MIN, Math.min(VERTICAL_ZOOM_MAX, value)));
  };

  const queueWheelZoom = (axis: "horizontal" | "vertical", deltaY: number) => {
    const pending = zoomWheelRef.current;
    pending[axis] += deltaY;
    if (pending.frame !== 0) return;
    pending.frame = requestAnimationFrame(() => {
      const horizontalDelta = pending.horizontal;
      const verticalDelta = pending.vertical;
      pending.frame = 0;
      pending.horizontal = 0;
      pending.vertical = 0;
      if (Math.abs(horizontalDelta) > 0.01) changeHorizontalZoom(horizontalDelta);
      if (Math.abs(verticalDelta) > 0.01) changeVerticalZoom(verticalDelta);
    });
  };

  useLayoutEffect(() => {
    void horizontalZoom;
    const scroller = arrangementScrollRef.current;
    const anchor = horizontalZoomAnchorRef.current;
    if (scroller && anchor !== null) {
      scroller.scrollLeft = getCenteredTimelineScrollLeft(
        anchor,
        timelineContentRef.current?.clientWidth ?? scroller.scrollWidth,
        scroller.clientWidth -
          (scroller.scrollWidth -
            (timelineContentRef.current?.clientWidth ?? scroller.scrollWidth)),
      );
    }
    horizontalZoomAnchorRef.current = null;
  }, [horizontalZoom]);

  useEffect(() => {
    void verticalZoom;
    const frame = requestAnimationFrame(() => {
      const scroller = pianoRollScrollRef.current;
      const anchor = verticalZoomAnchorRef.current;
      if (scroller && anchor !== null) {
        scroller.scrollTop = anchor * scroller.scrollHeight - scroller.clientHeight / 2;
      }
      verticalZoomAnchorRef.current = null;
    });
    return () => cancelAnimationFrame(frame);
  }, [verticalZoom]);

  const handleTimelineWheel = (event: WheelEvent<HTMLElement>) => {
    if (event.ctrlKey || event.metaKey) {
      event.preventDefault();
      queueWheelZoom("horizontal", normaliseWheelDelta(event.deltaY, event.deltaMode));
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
      queueWheelZoom("vertical", normaliseWheelDelta(event.deltaY, event.deltaMode));
      return;
    }
    if (event.ctrlKey || event.metaKey) {
      event.preventDefault();
      event.stopPropagation();
      queueWheelZoom("horizontal", normaliseWheelDelta(event.deltaY, event.deltaMode));
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
      if (Math.abs(point.x - gesture.anchorX) > minimumClipWidth) {
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
        gesture.clip = createManualClip(
          gesture.clip.id,
          x,
          pitch,
          width,
          gesture.clip.color,
          minimumClipWidth,
        );
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
          ? stretchClipStartTo(
              gesture.source,
              gesture.clipId,
              gesture.originalStartX + deltaX,
              minimumClipWidth,
            )
          : stretchClipTo(
              gesture.source,
              gesture.clipId,
              gesture.originalEndX + deltaX,
              minimumClipWidth,
            );
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
    setDragChordPitchClasses([]);
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
    horizontalZoomAnchorRef.current = x / 100;
    setHorizontalZoom(nextZoom);
  };

  const beginPencilGesture = (event: ReactPointerEvent<HTMLElement>) => {
    const point = editorPoint(event.clientX, event.clientY);
    if (!point) return;
    const id = nextManualClipId(activeEditorClips);
    const defaultWidth = Math.max(
      minimumClipWidth,
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
      minimumClipWidth,
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
        const chordLabel = chordLabelAtPosition(project.chords, clip.x);
        setDragChordPitchClasses(chordLabel ? chordPitchClasses(chordLabel) : []);
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
      const splitResult = splitClipAt(
        activeEditorClips,
        clip.id,
        point.x,
        rightClipId,
        minimumClipWidth,
      );
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
        pitch: Math.max(PITCH_BOTTOM, Math.min(PITCH_TOP, pitch)),
        width: 6,
        color: "cyan",
      },
    ]);
  };

  // biome-ignore lint/correctness/useExhaustiveDependencies: unmount cleanup must remove the listener functions captured by the active drag.
  useEffect(
    () => () => {
      cancelAnimationFrame(zoomWheelRef.current.frame);
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
    instrumentalVolume,
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
    shortcutBindings,
    chordSnapEnabled,
    chordSnapAvailable,
    dragChordPitchClasses,
    secondaryToolModifierActive,
    selectedClipIds,
    pianoCollapsed,
    serviceTracksCollapsed,
    backingTracks,
    arrangementTracks,
    trackRows,
    displayRows,
    backingGenerator,
    voiceProfiles: project.voiceProfiles,
    backingTrackContents: project.backingTrackContents,
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
      pianoRollScrollRef,
    },
    actions: {
      returnToStart,
      togglePlayback,
      stopPlayback,
      setLooping,
      setCycleRange,
      commitCycleRange,
      setPlayheadPosition,
      setVolumeValue,
      setInstrumentalVolumeValue,
      toggleTrack,
      toggleTrackLayer,
      selectTrack,
      addBackingTrack,
      addTrack: () => bridge.send({ type: "add-track" }),
      removeTrack: (track: string) => bridge.send({ type: "remove-track", track }),
      importAudio: () =>
        bridge.send({
          type: "import-audio",
          track:
            selectedTracks.length === 1 &&
            arrangementTracks.some((track) => track.id === selectedTracks[0] && !track.parentId)
              ? selectedTracks[0]
              : "",
        }),
      importTrackAudio: (track: string) => bridge.send({ type: "import-track-audio", track }),
      importDroppedAudio: (files: string[], track: string, start: number) =>
        bridge.send({ type: "import-dropped-audio", track, start, files }),
      toggleTrackCollapsed: (track: string, collapsed: boolean) =>
        bridge.send({ type: "set-track-collapsed", track, collapsed }),
      moveArrangementClip: (track: string, clipId: string, targetTrack: string, start: number) =>
        bridge.send({ type: "move-arrangement-clip", track, clipId, targetTrack, start }),
      removeArrangementClip: (track: string, clipId: string) =>
        bridge.send({ type: "remove-arrangement-clip", track, clipId }),
      setBackingGenerator,
      regenerateBackingTrack,
      renderBackingTrack,
      removeBackingTrack,
      setBackingVoiceProfile,
      deleteVoiceProfile,
      undoPitchEdit,
      redoPitchEdit,
      undo,
      redo,
      newProject,
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
      handleTimelineWheel,
      handlePianoWheel,
      syncHorizontalScroll,
      startPlayheadDrag,
      startClipTool,
      startEditorTool,
      dropSyllable,
    },
  };
}

export type StudioController = ReturnType<typeof useStudioController>;
