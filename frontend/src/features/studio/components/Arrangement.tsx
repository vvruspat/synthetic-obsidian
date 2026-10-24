import {
  ArrowPathIcon,
  ArrowsRightLeftIcon,
  ChevronDoubleDownIcon,
  ChevronDoubleUpIcon,
  ClockIcon,
  MusicalNoteIcon,
  PlusIcon,
  SpeakerWaveIcon,
  Squares2X2Icon,
} from "@heroicons/react/24/outline";
import type { CSSProperties } from "react";
import type { StudioProject } from "@/domain/studio";
import { ArrangementClip } from "@/features/studio/components/ArrangementClip";
import { BackingTrackSelect } from "@/features/studio/components/BackingTrackSelect";
import { PianoRoll, PianoToolbar } from "@/features/studio/components/PianoRoll";
import { TimelineRuler } from "@/features/studio/components/TimelineRuler";
import { TrackSidebar } from "@/features/studio/components/TrackSidebar";
import { WebGLSurface } from "@/features/studio/components/WebGLSurface";
import type { StudioController } from "@/features/studio/hooks/useStudioController";
import type { ArrangementRow } from "@/features/studio/lib/arrangement";
import { createTempoPath } from "@/features/studio/lib/musical-time";
import { getClipVisualWidthPercent, pitchToTop } from "@/features/studio/lib/pitch";
import { IconButton, RangeInput } from "@/ui";

type ArrangementProps = {
  controller: StudioController;
  project: StudioProject;
  status: string;
  vocalAnalysisRunning: boolean;
  instrumentalAnalysisRunning: boolean;
  backingGenerationRunning: boolean;
  backingAudioRenderRunning: boolean;
  backingGenerationTrack: string;
  backingAudioRenderTrack: string;
  onOpenVoiceTraining(): void;
};

function BackingTrackActionOverlay({
  track,
  generating,
  rendering,
  disabled,
  status,
  onRender,
  onRegenerate,
}: {
  track: string;
  generating: boolean;
  rendering: boolean;
  disabled: boolean;
  status: string;
  onRender(): void;
  onRegenerate(): void;
}) {
  const busy = generating || rendering;

  return (
    <div className={`backing-track-action-overlay ${busy ? "is-busy" : ""}`}>
      {busy ? <i className="track-analysis-spinner" aria-hidden="true" /> : null}
      <button type="button" disabled={busy || disabled} onClick={onRender}>
        <SpeakerWaveIcon aria-hidden="true" />
        Render
      </button>
      <button type="button" disabled={busy || disabled} onClick={onRegenerate}>
        <ArrowPathIcon aria-hidden="true" />
        Regenerate
      </button>
      {busy ? (
        <span role="status" aria-live="polite">
          {status || `${generating ? "Regenerating" : "Rendering"} ${track}...`}
        </span>
      ) : null}
    </div>
  );
}

export function Arrangement({
  controller,
  project,
  status,
  vocalAnalysisRunning,
  instrumentalAnalysisRunning,
  backingGenerationRunning,
  backingAudioRenderRunning,
  backingGenerationTrack,
  backingAudioRenderTrack,
  onOpenVoiceTraining,
}: ArrangementProps) {
  const {
    displayRows,
    backingGenerator,
    cycleRange,
    horizontalZoom,
    looping,
    musicalTimeline,
    playhead,
    selectedTracks,
    trackLayerState,
    timeLabel,
    refs,
    actions,
    serviceTracksCollapsed,
  } = controller;
  const tempoPath = createTempoPath(musicalTimeline);
  const busy =
    vocalAnalysisRunning ||
    instrumentalAnalysisRunning ||
    backingGenerationRunning ||
    backingAudioRenderRunning;
  const playheadLine = (
    <i className="playhead-line" onPointerDown={actions.startPlayheadDrag} aria-hidden="true" />
  );
  const renderLane = ({ kind, track, depth, key }: ArrangementRow) => {
    if (kind === "backing-group")
      return (
        <div className="backing-group-lane">
          {playheadLine}
          <div className="backing-group-actions">
            <span>{track.name}</span>
            <BackingTrackSelect
              label="Add Back"
              parentName={track.name}
              options={project.backingTrackOptions}
              generator={backingGenerator}
              disabled={!controller.trackLayerAvailability[track.id]?.notesAvailable || busy}
              disabledReason="Import or add notes to this track to generate backing vocals"
              onGeneratorChange={actions.setBackingGenerator}
              onSelect={(name) => actions.addBackingTrack(name, track.id)}
            />
          </div>
        </div>
      );
    const content = project.backingTrackContents.find((item) => item.id === track.id);
    const generating = backingGenerationRunning && backingGenerationTrack === track.id;
    const rendering = backingAudioRenderRunning && backingAudioRenderTrack === track.id;
    const layerState = trackLayerState[track.id];
    return (
      <div
        className={`audio-lane ${depth ? "harmony-lane" : "voice-lane"} ${selectedTracks.includes(track.id) ? "is-selected" : ""}`}
        data-track-id={track.id}
        key={key}
      >
        {playheadLine}
        <button
          type="button"
          className="track-lane-selection-target"
          aria-label={`Select ${track.name}`}
          aria-pressed={selectedTracks.includes(track.id)}
          onClick={(event) => actions.selectTrack(track.id, event.shiftKey)}
        />
        {track.clips.map((clip) => (
          <ArrangementClip
            key={clip.id}
            clip={clip}
            trackId={track.id}
            duration={project.timelineDurationSeconds}
            onSelect={() => actions.selectTrack(track.id, false)}
            onMove={(target, start) =>
              actions.moveArrangementClip(track.id, clip.id, target, start)
            }
            onDelete={() => actions.removeArrangementClip(track.id, clip.id)}
          />
        ))}
        {depth > 0 && !layerState?.notesMuted ? (
          <div className="harmony-notes">
            {(content?.clips ?? []).map((clip) => (
              <i
                key={clip.id}
                style={{
                  left: `${clip.x}%`,
                  top: `${Math.max(6, Math.min(76, pitchToTop(clip.pitch)))}%`,
                  width: `${getClipVisualWidthPercent(clip.width)}%`,
                }}
              />
            ))}
          </div>
        ) : null}
        {depth > 0 && (!content?.hasAudio || generating || rendering) ? (
          <BackingTrackActionOverlay
            track={track.name}
            generating={generating}
            rendering={rendering}
            disabled={
              (backingGenerationRunning || backingAudioRenderRunning) && !generating && !rendering
            }
            status={status}
            onRender={() => actions.renderBackingTrack(track.id)}
            onRegenerate={() => actions.regenerateBackingTrack(track.id)}
          />
        ) : null}
      </div>
    );
  };
  return (
    <>
      <section
        className="arrangement"
        aria-label="Track arrangement"
        ref={refs.arrangementScrollRef}
        onWheel={actions.handleTimelineWheel}
        onScroll={() => actions.syncHorizontalScroll("arrangement")}
      >
        <div
          className="timeline-content"
          style={
            {
              "--horizontal-zoom": horizontalZoom,
              "--playhead-position": `${playhead}%`,
            } as CSSProperties
          }
        >
          <div className="timeline-guide" ref={refs.timelineContentRef}>
            <WebGLSurface />
            <div className="metric-grid" aria-hidden="true">
              {musicalTimeline.grid.map((line) => (
                <i
                  className={`metric-${line.kind}`}
                  style={{ left: `${line.position}%` }}
                  key={`${line.time}:${line.kind}`}
                />
              ))}
            </div>
          </div>
          <div className="workspace-row timeline-header">
            <div className="sidebar-spacer track-row-header">
              <button
                type="button"
                className="service-tracks-toggle"
                aria-label={
                  serviceTracksCollapsed
                    ? "Show Tempo, Signature and Chord tracks"
                    : "Hide Tempo, Signature and Chord tracks"
                }
                aria-expanded={!serviceTracksCollapsed}
                onClick={() => actions.setServiceTracksCollapsed((current) => !current)}
              >
                {serviceTracksCollapsed ? (
                  <ChevronDoubleDownIcon aria-hidden="true" />
                ) : (
                  <ChevronDoubleUpIcon aria-hidden="true" />
                )}
              </button>
            </div>
            <TimelineRuler
              cycleRange={cycleRange}
              looping={looping}
              playhead={playhead}
              playheadLabel={timeLabel}
              timelineStartSeconds={project.timelineStartSeconds}
              timelineDurationSeconds={project.timelineDurationSeconds}
              horizontalZoom={horizontalZoom}
              grid={musicalTimeline.grid}
              onCycleRangeChange={actions.setCycleRange}
              onCycleRangeCommit={actions.commitCycleRange}
              onLoopingChange={actions.setLooping}
              onPlayheadChange={actions.setPlayheadPosition}
              onPlayheadPointerDown={actions.startPlayheadDrag}
            />
          </div>
          <div className="workspace-row service-row tempo-row">
            <div className="meta-header track-row-header">
              <ClockIcon className="meta-icon" aria-hidden="true" />
              Tempo
            </div>
            <div className="tempo-lane">
              {playheadLine}
              <svg viewBox="0 0 1000 54" preserveAspectRatio="none" aria-hidden="true">
                <path d={tempoPath} />
              </svg>
              {musicalTimeline.tempoChanges.map((change) => (
                <span
                  style={{ left: `${(change.start / musicalTimeline.duration) * 100}%` }}
                  key={`${change.start}:${change.bpm}`}
                >
                  {change.bpm}
                </span>
              ))}
            </div>
          </div>
          <div className="workspace-row service-row signature-row">
            <div className="meta-header track-row-header">
              <MusicalNoteIcon className="meta-icon" aria-hidden="true" />
              Signature
            </div>
            <div className="signature-lane">
              {playheadLine}
              {musicalTimeline.signatureRegions.map((signature) => (
                <div
                  style={{
                    left: `${(signature.start / musicalTimeline.duration) * 100}%`,
                    width: `${((signature.end - signature.start) / musicalTimeline.duration) * 100}%`,
                  }}
                  key={`${signature.start}:${signature.numerator}/${signature.denominator}`}
                >
                  {signature.numerator}/{signature.denominator}
                </div>
              ))}
            </div>
          </div>
          <div className="workspace-row service-row chord-row">
            <div className="meta-header track-row-header">
              <Squares2X2Icon className="meta-icon" aria-hidden="true" />
              Chord
            </div>
            <div className="chord-lane">
              {playheadLine}
              {project.chords.map((chord) => (
                <div style={{ width: `${chord.width}%` }} key={chord.label}>
                  {chord.label}
                </div>
              ))}
            </div>
          </div>
          <div className="workspace-row track-add-row">
            <div className="arrangement-add-toolbar track-row-header">
              <IconButton
                className="ui-compact-button"
                label="Add track"
                icon={PlusIcon}
                onClick={actions.addTrack}
              />
            </div>
            <div className="arrangement-add-toolbar">{playheadLine}</div>
          </div>
          {displayRows.map((row) => (
            <div
              className="workspace-row track-row"
              key={row.key}
              style={{ "--track-color": row.color } as CSSProperties}
            >
              <TrackSidebar row={row} controller={controller} backingRenderBusy={busy} />
              {renderLane(row)}
            </div>
          ))}
        </div>
      </section>
      <section className="piano-dock" aria-label="Pitch editor">
        <div className="piano-toolbar-row">
          <div className="piano-toolbar-side">
            <label
              className="piano-horizontal-zoom-control"
              htmlFor="horizontal-piano-zoom"
              title={`Horizontal zoom ${horizontalZoom.toFixed(2)}×`}
            >
              <ArrowsRightLeftIcon aria-hidden="true" />
              <span className="sr-only">Horizontal Piano Roll zoom</span>
              <RangeInput
                id="horizontal-piano-zoom"
                min={1}
                max={32}
                step={0.25}
                value={horizontalZoom}
                aria-label="Horizontal Piano Roll zoom"
                aria-valuetext={`${horizontalZoom.toFixed(2)} times`}
                onValueChange={actions.setHorizontalZoomFromInput}
              />
            </label>
          </div>
          <PianoToolbar
            controller={controller}
            backingRenderBusy={busy}
            backingRenderTrack={
              backingAudioRenderRunning ? backingAudioRenderTrack : backingGenerationTrack
            }
            onOpenVoiceTraining={onOpenVoiceTraining}
          />
        </div>
        <PianoRoll
          controller={controller}
          waveform={controller.editorWaveform}
          waveformDurationRatio={controller.editorWaveformDurationRatio}
        />
      </section>
    </>
  );
}
