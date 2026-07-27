import { ArrowPathIcon, SpeakerWaveIcon } from "@heroicons/react/24/outline";
import type { ProjectAction } from "@/bridge/plugin-bridge";
import type { BackingTrackName, StudioProject, VoiceProfileOption } from "@/domain/studio";
import { BackingTrackSelect } from "@/features/studio/components/BackingTrackSelect";
import { PianoRoll } from "@/features/studio/components/PianoRoll";
import { TimelineRuler } from "@/features/studio/components/TimelineRuler";
import { VoiceProfileSelect } from "@/features/studio/components/VoiceProfileSelect";
import { TrackWaveform } from "@/features/studio/components/Waveforms";
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
  onProjectAction(action: ProjectAction): void;
};

function TrackLaneOverlay({
  track,
  empty,
  analyzing,
  disabled = false,
  disabledReason,
  status,
  onSelect,
}: {
  track: "instrumental" | "vocal";
  empty: boolean;
  analyzing: boolean;
  disabled?: boolean;
  disabledReason?: string;
  status: string;
  onSelect(): void;
}) {
  if (analyzing) {
    return (
      <div
        className={`track-lane-overlay track-lane-${track} is-analyzing`}
        role="status"
        aria-live="polite"
      >
        <i className="track-analysis-spinner" aria-hidden="true" />
        <strong>{track === "instrumental" ? "Analyzing instrumental" : "Analyzing vocal"}</strong>
        <span>{status}</span>
      </div>
    );
  }

  if (!empty) return null;

  return (
    <div className={`track-lane-overlay track-lane-${track} is-empty`}>
      <button
        type="button"
        disabled={disabled}
        title={disabled ? disabledReason : undefined}
        onClick={onSelect}
      >
        {track === "instrumental" ? "Select Instrumental" : "Select Vocal"}
      </button>
    </div>
  );
}

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
}: {
  track: BackingTrackName;
  generating: boolean;
  rendering: boolean;
  disabled: boolean;
  status: string;
  voiceProfiles: VoiceProfileOption[];
  voiceProfileId: string;
  onRender(): void;
  onRegenerate(): void;
  onVoiceProfileChange(profileId: string): void;
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
  vocalAnalysisRunning,
  instrumentalAnalysisRunning,
  backingGenerationRunning,
  backingAudioRenderRunning,
  backingGenerationTrack,
  backingAudioRenderTrack,
  onProjectAction,
}: ArrangementProps) {
  const {
    backingTracks,
    cycleRange,
    horizontalZoom,
    looping,
    musicalTimeline,
    playhead,
    selectedTracks,
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
    >
      <div className="arrangement-viewport-toolbars">
        <TrackLaneOverlay
          track="instrumental"
          empty={!project.hasInstrumental}
          analyzing={instrumentalAnalysisRunning}
          status={status}
          onSelect={() => onProjectAction("open-instrumental")}
        />
        <TrackLaneOverlay
          track="vocal"
          empty={!project.hasVocal}
          analyzing={vocalAnalysisRunning}
          disabled={!project.hasInstrumental}
          disabledReason="Select instrumental first"
          status={status}
          onSelect={() => onProjectAction("open-audio")}
        />
        <div className="backing-track-toolbar">
          <BackingTrackSelect
            options={project.backingTrackOptions.filter((name) => !backingTracks.includes(name))}
            disabled={backingTracks.length === project.backingTrackOptions.length}
            onSelect={actions.addBackingTrack}
          />
        </div>
      </div>
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
            <div
              className={`audio-lane instrumental-lane ${
                selectedTracks.includes("Instrumental") ? "is-selected" : ""
              }`}
            >
              <button
                type="button"
                className="track-lane-selection-target"
                aria-label="Select Instrumental"
                aria-pressed={selectedTracks.includes("Instrumental")}
                onClick={(event) => actions.selectTrack("Instrumental", event.shiftKey)}
              />
              {project.hasInstrumental ? (
                <TrackWaveform
                  variant="instrumental"
                  waveform={project.instrumentalWaveform}
                  scrollContainerRef={refs.arrangementScrollRef}
                  durationRatio={
                    project.instrumentalDurationSeconds / project.timelineDurationSeconds
                  }
                />
              ) : null}
              {project.hasInstrumental ? <span>Instrumental</span> : null}
            </div>
            <div
              className={`audio-lane voice-lane ${
                selectedTracks.includes("Voice Main") ? "is-selected" : ""
              }`}
            >
              <button
                type="button"
                className="track-lane-selection-target"
                aria-label="Select Voice Main"
                aria-pressed={selectedTracks.includes("Voice Main")}
                onClick={(event) => actions.selectTrack("Voice Main", event.shiftKey)}
              />
              {project.hasVocal ? (
                <TrackWaveform
                  variant="voice"
                  waveform={project.vocalWaveform}
                  scrollContainerRef={refs.arrangementScrollRef}
                  durationRatio={project.vocalDurationSeconds / project.timelineDurationSeconds}
                />
              ) : null}
              {project.hasVocal ? <span>Voice Main</span> : null}
            </div>
            <div className="backing-track-toolbar-slot" aria-hidden="true" />
            <div
              className="arrangement-track-stack"
              ref={refs.arrangementTrackScrollRef}
              onScroll={() => actions.syncTrackScroll("arrangement")}
            >
              {backingTracks.map((name) => {
                const content = project.backingTrackContents.find((item) => item.track === name);
                const generating = backingGenerationRunning && backingGenerationTrack === name;
                const rendering = backingAudioRenderRunning && backingAudioRenderTrack === name;
                const anotherTrackBusy =
                  (backingGenerationRunning || backingAudioRenderRunning) &&
                  !generating &&
                  !rendering;
                return (
                  <div
                    className={`audio-lane harmony-lane ${
                      selectedTracks.includes(name) ? "is-selected" : ""
                    }`}
                    key={name}
                  >
                    <button
                      type="button"
                      className="track-lane-selection-target"
                      aria-label={`Select ${name}`}
                      aria-pressed={selectedTracks.includes(name)}
                      onClick={(event) => actions.selectTrack(name, event.shiftKey)}
                    />
                    {content?.hasAudio ? (
                      <TrackWaveform
                        variant="backing"
                        waveform={content.waveform}
                        scrollContainerRef={refs.arrangementScrollRef}
                        durationRatio={
                          content.audioDurationSeconds / project.timelineDurationSeconds
                        }
                      />
                    ) : null}
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
                    {!content?.hasAudio ? (
                      <BackingTrackActionOverlay
                        track={name}
                        generating={generating}
                        rendering={rendering}
                        disabled={anotherTrackBusy}
                        status={status}
                        voiceProfiles={project.voiceProfiles}
                        voiceProfileId={content?.voiceProfileId ?? ""}
                        onRender={() => actions.renderBackingTrack(name)}
                        onRegenerate={() => actions.regenerateBackingTrack(name)}
                        onVoiceProfileChange={(profileId) =>
                          actions.setBackingVoiceProfile(name, profileId)
                        }
                      />
                    ) : null}
                    <span>{name}</span>
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
          />
        </div>
      </div>
    </section>
  );
}
