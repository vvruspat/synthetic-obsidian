import type { CSSProperties } from "react";
import type { WaveformData } from "@/domain/studio";
import { CorrectionToolSelect } from "@/features/studio/components/CorrectionToolSelect";
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

export function PianoToolbar({ controller }: { controller: StudioController }) {
  const { pianoCollapsed, leftCorrectionTool, rightCorrectionTool, actions } = controller;

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
}: {
  controller: StudioController;
  waveform: WaveformData;
  waveformDurationRatio: number;
}) {
  const { clips, sortedClips, pitchPath, verticalZoom, musicalTimeline, refs, actions } =
    controller;

  return (
    <>
      <div className="piano-toolbar-slot">
        <PianoToolbar controller={controller} />
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
              className="clip-field"
              ref={refs.editorRef}
              onDragOver={(event) => event.preventDefault()}
              onDrop={actions.dropSyllable}
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
                return (
                  <button
                    type="button"
                    key={clip.id}
                    className={`vocal-clip clip-${clip.color}`}
                    style={
                      {
                        left: `${clip.x}%`,
                        top: `${pitchToTop(clip.pitch)}%`,
                        width: `${getClipVisualWidthPercent(clip.width)}%`,
                        height: `${PITCH_ROW_HEIGHT}%`,
                        background: fill.background,
                      } as CSSProperties
                    }
                    onPointerDown={(event) => actions.startClipDrag(event, clip)}
                    aria-label={`${clip.label}, pitch ${clip.pitch.toFixed(
                      2,
                    )}, ${fill.cents >= 0 ? "+" : ""}${fill.cents.toFixed(0)} cents`}
                  >
                    <span>{clip.label}</span>
                    <em>
                      {fill.cents >= 0 ? "+" : ""}
                      {fill.cents.toFixed(0)}¢
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
