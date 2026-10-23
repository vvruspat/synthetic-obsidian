import { ArrowPathIcon, SpeakerWaveIcon } from "@heroicons/react/24/outline";
import type { CSSProperties } from "react";
import type { StudioProject, VoiceProfileOption } from "@/domain/studio";
import { ArrangementClip } from "@/features/studio/components/ArrangementClip";
import { BackingTrackSelect } from "@/features/studio/components/BackingTrackSelect";
import { PianoRoll } from "@/features/studio/components/PianoRoll";
import { TimelineRuler } from "@/features/studio/components/TimelineRuler";
import { VoiceProfileSelect } from "@/features/studio/components/VoiceProfileSelect";
import { WebGLSurface } from "@/features/studio/components/WebGLSurface";
import type { StudioController } from "@/features/studio/hooks/useStudioController";
import { createTempoPath } from "@/features/studio/lib/musical-time";
import { getClipVisualWidthPercent, pitchToTop } from "@/features/studio/lib/pitch";

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
  voiceProfiles,
  voiceProfileId,
  onRender,
  onRegenerate,
  onVoiceProfileChange,
  onDeleteVoiceProfile,
  onOpenVoiceTraining,
}: {
  track: string;
  generating: boolean;
  rendering: boolean;
  disabled: boolean;
  status: string;
  voiceProfiles: VoiceProfileOption[];
  voiceProfileId: string;
  onRender(): void;
  onRegenerate(): void;
  onVoiceProfileChange(profileId: string): void;
  onDeleteVoiceProfile(profileId: string): void;
  onOpenVoiceTraining(): void;
}) {
  const busy = generating || rendering;

  return (
    <div className={`backing-track-action-overlay ${busy ? "is-busy" : ""}`}>
      {busy ? <i className="track-analysis-spinner" aria-hidden="true" /> : null}
      <VoiceProfileSelect
        track={track}
        profiles={voiceProfiles}
        value={voiceProfileId}
        disabled={busy || disabled}
        onChange={onVoiceProfileChange}
        onDelete={onDeleteVoiceProfile}
        onCreateVoice={onOpenVoiceTraining}
      />
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
  } = controller;
  const tempoPath = createTempoPath(musicalTimeline);

  return (
    <section
      className="arrangement"
      ref={refs.arrangementScrollRef}
      onWheel={actions.handleTimelineWheel}
      onScroll={actions.handleTimelineScroll}
    >
      <div
        className="timeline-content"
        ref={refs.timelineContentRef}
        style={{ width: `${horizontalZoom * 100}%` }}
      >
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
        <div className="arrangement-canvas">
          <WebGLSurface />
          <div
            className="playhead-line"
            style={{ left: `${playhead}%` }}
            onPointerDown={actions.startPlayheadDrag}
            aria-hidden="true"
          />
          <div className="metric-grid" aria-hidden="true">
            {musicalTimeline.grid.map((line) => (
              <i
                className={`metric-${line.kind}`}
                style={{ left: `${line.position}%` }}
                key={`${line.time}:${line.kind}`}
              />
            ))}
          </div>
          <div className="tempo-lane">
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
          <div className="signature-lane">
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
          <div className="chord-lane">
            {project.chords.map((chord) => (
              <div style={{ width: `${chord.width}%` }} key={chord.label}>
                {chord.label}
              </div>
            ))}
          </div>
          <div className="arrangement-track-section">
            <div className="arrangement-add-toolbar" />
            <div
              className="arrangement-track-stack"
              ref={refs.arrangementTrackScrollRef}
              onScroll={() => actions.syncTrackScroll("arrangement")}
            >
              {displayRows.map(({ kind, key, track, depth, color }) => {
                const rowStyle = { "--track-color": color } as CSSProperties;
                if (kind === "backing-group")
                  return (
                    <div className="backing-group-lane" key={key} style={rowStyle}>
                      <div className="backing-group-actions">
                        <span>{track.name}</span>
                        <BackingTrackSelect
                          label="Add Back"
                          parentName={track.name}
                          options={project.backingTrackOptions}
                          generator={backingGenerator}
                          disabled={
                            !controller.trackLayerAvailability[track.id]?.notesAvailable ||
                            backingGenerationRunning ||
                            backingAudioRenderRunning
                          }
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
                    style={rowStyle}
                  >
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
                          (backingGenerationRunning || backingAudioRenderRunning) &&
                          !generating &&
                          !rendering
                        }
                        status={status}
                        voiceProfiles={project.voiceProfiles}
                        voiceProfileId={content?.voiceProfileId ?? ""}
                        onRender={() => actions.renderBackingTrack(track.id)}
                        onRegenerate={() => actions.regenerateBackingTrack(track.id)}
                        onVoiceProfileChange={(profileId) =>
                          actions.setBackingVoiceProfile(track.id, profileId)
                        }
                        onDeleteVoiceProfile={actions.deleteVoiceProfile}
                        onOpenVoiceTraining={onOpenVoiceTraining}
                      />
                    ) : null}
                  </div>
                );
              })}
            </div>
          </div>
          <PianoRoll
            controller={controller}
            waveform={controller.editorWaveform}
            waveformDurationRatio={controller.editorWaveformDurationRatio}
            backingRenderBusy={backingGenerationRunning || backingAudioRenderRunning}
            backingRenderTrack={
              backingAudioRenderRunning ? backingAudioRenderTrack : backingGenerationTrack
            }
            onOpenVoiceTraining={onOpenVoiceTraining}
          />
        </div>
      </div>
    </section>
  );
}
