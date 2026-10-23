import {
  ArrowUturnLeftIcon,
  ArrowUturnRightIcon,
  MusicalNoteIcon,
  SpeakerWaveIcon,
} from "@heroicons/react/24/outline";
import { type CSSProperties, useState } from "react";
import type { WaveformData } from "@/domain/studio";
import { CorrectionToolSelect } from "@/features/studio/components/CorrectionToolSelect";
import { PianoKeyboard, PianoZoomControl } from "@/features/studio/components/PianoKeyboard";
import { VoiceProfileSelect } from "@/features/studio/components/VoiceProfileSelect";
import { PianoWaveform } from "@/features/studio/components/Waveforms";
import type { StudioController } from "@/features/studio/hooks/useStudioController";
import { formatShortcut } from "@/features/studio/lib/hotkeys";
import {
  getClipVisualWidthPercent,
  getPitchFill,
  PIANO_KEYS,
  PITCH_ROW_HEIGHT,
  PITCH_ROWS,
  pitchToTop,
} from "@/features/studio/lib/pitch";

export function PianoToolbar({
  controller,
  backingRenderBusy,
  backingRenderTrack,
  onOpenVoiceTraining,
}: {
  controller: StudioController;
  backingRenderBusy: boolean;
  backingRenderTrack: string;
  onOpenVoiceTraining(): void;
}) {
  const {
    pianoCollapsed,
    leftCorrectionTool,
    rightCorrectionTool,
    shortcutBindings,
    chordSnapEnabled,
    chordSnapAvailable,
    pitchHistory,
    backingRenderAction,
    voiceProfiles,
    actions,
  } = controller;
  const renderingSelectedTrack =
    backingRenderBusy && backingRenderTrack === backingRenderAction?.track;

  return (
    <div className="piano-toolbar">
      <div className="piano-tool-selectors">
        <CorrectionToolSelect
          label="Left mouse button"
          value={leftCorrectionTool}
          mouseButton="left"
          shortcuts={shortcutBindings}
          onChange={actions.setLeftCorrectionTool}
        />
        <CorrectionToolSelect
          label="Right mouse button"
          value={rightCorrectionTool}
          mouseButton="right"
          shortcuts={shortcutBindings}
          onChange={actions.setRightCorrectionTool}
        />
        <button
          type="button"
          className={`chord-snap-toggle ${chordSnapEnabled ? "is-active" : ""}`}
          aria-label="Snap notes to chord tones"
          aria-pressed={chordSnapEnabled}
          title={
            chordSnapAvailable
              ? `Snap notes to the current chord · ${formatShortcut(
                  shortcutBindings["editor.toggle-chord-snap"],
                )}`
              : "Select a vocal track with analyzed chords"
          }
          disabled={!chordSnapAvailable}
          onClick={() => actions.setChordSnapEnabled((current) => !current)}
        >
          <MusicalNoteIcon aria-hidden="true" />
          <span>Chord Snap</span>
        </button>
      </div>
      <div className="piano-history-actions">
        <button
          type="button"
          className="piano-toolbar-action"
          disabled={!pitchHistory.canUndo}
          aria-label="Undo pitch edit"
          title={`Undo pitch edit · ${formatShortcut(shortcutBindings["project.undo"])}`}
          onClick={actions.undoPitchEdit}
        >
          <ArrowUturnLeftIcon aria-hidden="true" />
        </button>
        <button
          type="button"
          className="piano-toolbar-action"
          disabled={!pitchHistory.canRedo}
          aria-label="Redo pitch edit"
          title={`Redo pitch edit · ${formatShortcut(shortcutBindings["project.redo"])}`}
          onClick={actions.redoPitchEdit}
        >
          <ArrowUturnRightIcon aria-hidden="true" />
        </button>
        {backingRenderAction ? (
          <>
            <VoiceProfileSelect
              track={backingRenderAction.track}
              profiles={voiceProfiles}
              value={backingRenderAction.voiceProfileId}
              disabled={backingRenderBusy}
              onChange={(profileId) =>
                actions.setBackingVoiceProfile(backingRenderAction.track, profileId)
              }
              onDelete={actions.deleteVoiceProfile}
              onCreateVoice={onOpenVoiceTraining}
            />
            <button
              type="button"
              className={`piano-toolbar-action piano-render-action ${
                backingRenderAction.label === "Re-render" ? "is-rerender" : ""
              } ${renderingSelectedTrack ? "is-rendering" : ""}`}
              disabled={backingRenderBusy}
              onClick={() => actions.renderBackingTrack(backingRenderAction.track)}
            >
              <SpeakerWaveIcon aria-hidden="true" />
              {renderingSelectedTrack
                ? backingRenderAction.label === "Re-render"
                  ? "Re-rendering…"
                  : "Rendering…"
                : backingRenderAction.label}
            </button>
          </>
        ) : null}
      </div>
      <button
        type="button"
        className={`piano-collapse-button ${pianoCollapsed ? "is-expand" : ""}`}
        aria-label={pianoCollapsed ? "Expand Piano Roll" : "Collapse Piano Roll"}
        aria-expanded={!pianoCollapsed}
        title={`${pianoCollapsed ? "Expand Piano Roll" : "Collapse Piano Roll"} · ${formatShortcut(
          shortcutBindings["editor.toggle-piano"],
        )}`}
        onClick={() => actions.setPianoCollapsed((current) => !current)}
      >
        <span aria-hidden="true" />
      </button>
    </div>
  );
}

export function PianoRoll({
  controller,
  waveform,
  waveformDurationRatio,
}: {
  controller: StudioController;
  waveform: WaveformData;
  waveformDurationRatio: number;
}) {
  const {
    clips,
    sortedClips,
    pitchPath,
    verticalZoom,
    musicalTimeline,
    leftCorrectionTool,
    rightCorrectionTool,
    secondaryToolModifierActive,
    dragChordPitchClasses,
    selectedClipIds,
    canEditSelectedClips,
    refs,
    actions,
  } = controller;
  const visibleCorrectionTool = secondaryToolModifierActive
    ? rightCorrectionTool
    : leftCorrectionTool;
  const [scissorsGuideX, setScissorsGuideX] = useState<number | null>(null);

  return (
    <div
      className="piano-roll"
      style={
        {
          "--horizontal-zoom": controller.horizontalZoom,
          "--vertical-zoom": verticalZoom,
          "--pitch-rows": PITCH_ROWS,
        } as CSSProperties
      }
    >
      <div
        className="piano-vertical-scroll"
        ref={refs.pianoRollScrollRef}
        onScroll={() => actions.syncHorizontalScroll("piano")}
        onWheel={actions.handlePianoWheel}
      >
        <div className="piano-scroll-content">
          <PianoKeyboard controller={controller} />
          <fieldset
            className={`clip-field tool-${visibleCorrectionTool} ${
              canEditSelectedClips ? "is-editable" : "is-read-only"
            }`}
            ref={refs.editorRef}
            onDragOver={(event) => event.preventDefault()}
            onDrop={actions.dropSyllable}
            onPointerDown={actions.startEditorTool}
            onPointerMove={(event) => {
              if (visibleCorrectionTool !== "scissors") {
                if (scissorsGuideX !== null) setScissorsGuideX(null);
                return;
              }
              const bounds = event.currentTarget.getBoundingClientRect();
              setScissorsGuideX(
                Math.max(0, Math.min(100, ((event.clientX - bounds.left) / bounds.width) * 100)),
              );
            }}
            onPointerLeave={() => setScissorsGuideX(null)}
            onContextMenu={(event) => event.preventDefault()}
          >
            <legend className="sr-only">Pitch correction clip editor</legend>
            <PianoWaveform
              clips={sortedClips}
              waveform={waveform}
              durationRatio={waveformDurationRatio}
            />
            <div
              className="pitch-row-grid"
              style={{
                gridTemplateRows: `repeat(${PITCH_ROWS}, minmax(0, 1fr))`,
              }}
              aria-hidden="true"
            >
              {PIANO_KEYS.map((key) => (
                <i
                  className={`${key.black ? "is-black" : ""} ${
                    dragChordPitchClasses.includes(key.pitch % 12) ? "is-chord-tone" : ""
                  }`}
                  key={`row-${key.label}`}
                />
              ))}
            </div>
            <div className="piano-metric-grid" aria-hidden="true">
              {musicalTimeline.grid.map((line) => (
                <i
                  className={`metric-${line.kind}`}
                  style={{ left: `${line.position}%` }}
                  key={`${line.time}:${line.kind}`}
                />
              ))}
            </div>
            <svg
              className="pitch-curve"
              viewBox="0 0 100 100"
              preserveAspectRatio="none"
              aria-hidden="true"
            >
              <path className="pitch-curve-glow" d={pitchPath} />
              <path d={pitchPath} />
            </svg>
            {visibleCorrectionTool === "scissors" && scissorsGuideX !== null ? (
              <i
                className="scissors-cut-guide"
                style={{ left: `${scissorsGuideX}%` }}
                aria-hidden="true"
              />
            ) : null}
            {clips.map((clip) => {
              const fill = getPitchFill(clip);
              const gainDb = clip.gainDb ?? 0;
              const gainBrightness = Math.max(0.38, Math.min(1.7, 10 ** (gainDb / 40)));
              const isSelected = selectedClipIds.includes(clip.id);
              return (
                <button
                  type="button"
                  key={clip.id}
                  className={`vocal-clip clip-${clip.color} ${isSelected ? "is-selected" : ""}`}
                  style={
                    {
                      left: `${clip.x}%`,
                      top: `${pitchToTop(clip.pitch)}%`,
                      width: `${getClipVisualWidthPercent(clip.width)}%`,
                      height: `${PITCH_ROW_HEIGHT}%`,
                      background: fill.background,
                      "--clip-gain-brightness": gainBrightness,
                    } as CSSProperties
                  }
                  draggable={false}
                  onPointerDown={(event) => actions.startClipTool(event, clip)}
                  onDragStart={(event) => event.preventDefault()}
                  onContextMenu={(event) => event.preventDefault()}
                  aria-pressed={isSelected}
                  aria-label={`${clip.label}, pitch ${clip.pitch.toFixed(
                    2,
                  )}, ${fill.cents >= 0 ? "+" : ""}${fill.cents.toFixed(
                    0,
                  )} cents, ${gainDb >= 0 ? "+" : ""}${gainDb.toFixed(1)} decibels`}
                />
              );
            })}
          </fieldset>
        </div>
      </div>
      <PianoZoomControl controller={controller} />
    </div>
  );
}
