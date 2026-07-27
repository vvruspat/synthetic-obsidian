import {
  ArrowUturnLeftIcon,
  ArrowUturnRightIcon,
  MusicalNoteIcon,
  SpeakerWaveIcon,
} from "@heroicons/react/24/outline";
import type { CSSProperties } from "react";
import type { WaveformData } from "@/domain/studio";
import { CorrectionToolSelect } from "@/features/studio/components/CorrectionToolSelect";
import { VoiceProfileSelect } from "@/features/studio/components/VoiceProfileSelect";
import { PianoWaveform } from "@/features/studio/components/Waveforms";
import type { StudioController } from "@/features/studio/hooks/useStudioController";
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
}: {
  controller: StudioController;
  backingRenderBusy: boolean;
  backingRenderTrack: string;
}) {
  const {
    pianoCollapsed,
    leftCorrectionTool,
    rightCorrectionTool,
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
          onChange={actions.setLeftCorrectionTool}
        />
        <CorrectionToolSelect
          label="Right mouse button"
          value={rightCorrectionTool}
          onChange={actions.setRightCorrectionTool}
        />
        <button
          type="button"
          className={`chord-snap-toggle ${chordSnapEnabled ? "is-active" : ""}`}
          aria-label="Snap backing notes to chord tones"
          aria-pressed={chordSnapEnabled}
          title={
            chordSnapAvailable
              ? "Snap backing notes to the current chord"
              : "Select a backing track with analyzed chords"
          }
          disabled={!chordSnapAvailable}
          onClick={() => actions.setChordSnapEnabled((current) => !current)}
        >
          <MusicalNoteIcon aria-hidden="true" />
          <span>Chord</span>
        </button>
      </div>
      <div className="piano-history-actions">
        <button
          type="button"
          className="piano-toolbar-action"
          disabled={!pitchHistory.canUndo}
          aria-label="Undo pitch edit"
          title="Undo pitch edit"
          onClick={actions.undoPitchEdit}
        >
          <ArrowUturnLeftIcon aria-hidden="true" />
        </button>
        <button
          type="button"
          className="piano-toolbar-action"
          disabled={!pitchHistory.canRedo}
          aria-label="Redo pitch edit"
          title="Redo pitch edit"
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
        title={pianoCollapsed ? "Expand Piano Roll" : "Collapse Piano Roll"}
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
  backingRenderBusy,
  backingRenderTrack,
}: {
  controller: StudioController;
  waveform: WaveformData;
  waveformDurationRatio: number;
  backingRenderBusy: boolean;
  backingRenderTrack: string;
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
    selectedClipIds,
    canEditSelectedClips,
    refs,
    actions,
  } = controller;
  const visibleCorrectionTool = secondaryToolModifierActive
    ? rightCorrectionTool
    : leftCorrectionTool;

  return (
    <>
      <div className="piano-toolbar-slot">
        <PianoToolbar
          controller={controller}
          backingRenderBusy={backingRenderBusy}
          backingRenderTrack={backingRenderTrack}
        />
      </div>
      <div className="piano-roll">
        <div
          className="piano-vertical-scroll"
          ref={refs.pianoRollScrollRef}
          onScroll={() => actions.syncVerticalScroll("roll")}
          onWheel={actions.handlePianoWheel}
        >
          <div className="piano-scroll-content" style={{ height: `${verticalZoom * 100}%` }}>
            <fieldset
              className={`clip-field tool-${visibleCorrectionTool} ${
                canEditSelectedClips ? "is-editable" : "is-read-only"
              }`}
              ref={refs.editorRef}
              onDragOver={(event) => event.preventDefault()}
              onDrop={actions.dropSyllable}
              onPointerDown={actions.startEditorTool}
              onContextMenu={(event) => event.preventDefault()}
            >
              <legend className="sr-only">Pitch correction clip editor</legend>
              <div
                className="pitch-row-grid"
                style={{
                  gridTemplateRows: `repeat(${PITCH_ROWS}, minmax(0, 1fr))`,
                }}
                aria-hidden="true"
              >
                {PIANO_KEYS.map((key) => (
                  <i className={key.black ? "is-black" : ""} key={`row-${key.label}`} />
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
                  >
                    <span>{clip.label}</span>
                    <em>
                      {Math.abs(gainDb) >= 0.05
                        ? `${gainDb >= 0 ? "+" : ""}${gainDb.toFixed(1)}dB`
                        : `${fill.cents >= 0 ? "+" : ""}${fill.cents.toFixed(0)}¢`}
                    </em>
                    <i />
                  </button>
                );
              })}
            </fieldset>
          </div>
        </div>
        <div className="segmented-waveform" aria-hidden="true">
          {sortedClips.map((clip) => (
            <div
              key={`wave-${clip.id}`}
              className={`wave-segment wave-${clip.color}`}
              style={{
                left: `${clip.x}%`,
                width: `${getClipVisualWidthPercent(clip.width)}%`,
              }}
            />
          ))}
        </div>
        <PianoWaveform
          clips={sortedClips}
          waveform={waveform}
          durationRatio={waveformDurationRatio}
          scrollContainerRef={refs.arrangementScrollRef}
        />
      </div>
    </>
  );
}
