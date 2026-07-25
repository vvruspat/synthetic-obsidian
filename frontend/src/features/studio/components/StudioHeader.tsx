import {
  ArrowPathIcon,
  ArrowUturnLeftIcon,
  ChevronDownIcon,
  FolderArrowDownIcon,
  FolderIcon,
  FolderOpenIcon,
  MusicalNoteIcon,
  PauseIcon,
  PlayIcon,
  Squares2X2Icon,
  StopIcon,
} from "@heroicons/react/24/outline";
import type { CSSProperties } from "react";
import type { StudioController } from "@/features/studio/hooks/useStudioController";
import { gainToDecibels, gainToMeterPercent } from "@/features/studio/lib/output-meter";
import { IconButton, RangeInput } from "@/ui";

type StudioHeaderProps = {
  controller: StudioController;
};

export function StudioHeader({ controller }: StudioHeaderProps) {
  const {
    playing,
    looping,
    transportPosition,
    timeLabel,
    volume,
    outputLevels,
    musicalContext,
    actions,
  } = controller;
  const leftOutputDb = gainToDecibels(outputLevels.left);
  const rightOutputDb = gainToDecibels(outputLevels.right);
  const outputPeakDb = Math.max(leftOutputDb, rightOutputDb);
  const outputPeakLabel = Number.isFinite(outputPeakDb) ? `${outputPeakDb.toFixed(1)} dB` : "−∞ dB";

  return (
    <header className="topbar">
      <div className="brand-lockup">
        <span className="brand-mark" aria-hidden="true" />
        <div className="brand-name">
          <strong>Synthetic</strong>
          <span>Obsidian</span>
        </div>
        <details className="header-project-menu">
          <summary>
            <FolderIcon className="header-project-icon" aria-hidden="true" />
            <span>Project</span>
            <ChevronDownIcon className="header-project-chevron" aria-hidden="true" />
          </summary>
          <div>
            <button
              type="button"
              onClick={(event) => {
                actions.openProject();
                event.currentTarget.closest("details")?.removeAttribute("open");
              }}
            >
              <FolderOpenIcon className="header-project-option-icon" aria-hidden="true" />
              <span>Open project</span>
            </button>
            <button
              type="button"
              onClick={(event) => {
                actions.saveProject();
                event.currentTarget.closest("details")?.removeAttribute("open");
              }}
            >
              <FolderArrowDownIcon className="header-project-option-icon" aria-hidden="true" />
              <span>Save project</span>
            </button>
            <i className="header-project-separator" aria-hidden="true" />
            <button
              type="button"
              onClick={(event) => {
                actions.exportAllTracks();
                event.currentTarget.closest("details")?.removeAttribute("open");
              }}
            >
              <Squares2X2Icon className="header-project-option-icon" aria-hidden="true" />
              <span>Export all tracks</span>
            </button>
            <button
              type="button"
              onClick={(event) => {
                actions.exportMidiFiles();
                event.currentTarget.closest("details")?.removeAttribute("open");
              }}
            >
              <MusicalNoteIcon className="header-project-option-icon" aria-hidden="true" />
              <span>Export MIDI files</span>
            </button>
          </div>
        </details>
      </div>
      <fieldset className="main-transport">
        <legend className="sr-only">Transport controls</legend>
        <div className="transport-buttons">
          <IconButton
            label="Return to start"
            icon={ArrowUturnLeftIcon}
            className="transport-button"
            iconClassName="transport-icon"
            onClick={actions.returnToStart}
          />
          <IconButton
            label={playing ? "Pause" : "Play"}
            icon={playing ? PauseIcon : PlayIcon}
            className="transport-button"
            iconClassName="transport-icon"
            active={playing}
            onClick={actions.togglePlayback}
          />
          <IconButton
            label="Stop"
            icon={StopIcon}
            className="transport-button"
            iconClassName="transport-icon"
            onClick={actions.stopPlayback}
          />
          <IconButton
            label="Loop"
            icon={ArrowPathIcon}
            className="transport-button"
            iconClassName="transport-icon"
            active={looping}
            onClick={() => actions.setLooping(!looping)}
          />
        </div>
        <div className="transport-display">
          <span className="position-display">
            <small>BAR · BEAT · TICK</small>
            <strong>{transportPosition}</strong>
          </span>
          <span>
            <small>TIME</small>
            <strong>{timeLabel}</strong>
          </span>
          <span className="tempo-display">
            <small>TEMPO</small>
            <strong>
              {musicalContext.tempo.toFixed(1)} <b>BPM</b>
            </strong>
          </span>
          <span className="meter-display">
            <small>METER</small>
            <strong>
              {musicalContext.numerator} / {musicalContext.denominator}
            </strong>
          </span>
        </div>
      </fieldset>
      <div className="master-output">
        <label className="output-card volume-control" htmlFor="master-volume">
          <span className="output-card-header">
            <small>MASTER VOLUME</small>
            <strong>{Math.round(volume)}%</strong>
          </span>
          <RangeInput
            id="master-volume"
            min={0}
            max={100}
            value={volume}
            aria-label="Master volume"
            style={{ "--volume": `${volume}%` } as CSSProperties}
            onValueChange={actions.setVolumeValue}
          />
        </label>
        <div className="output-card volume-meter">
          <span className="output-card-header">
            <small>OUT</small>
            <strong>{outputPeakLabel}</strong>
          </span>
          <div className="stereo-meter">
            <i
              style={
                {
                  "--level": `${gainToMeterPercent(outputLevels.left)}%`,
                } as CSSProperties
              }
            />
            <i
              style={
                {
                  "--level": `${gainToMeterPercent(outputLevels.right)}%`,
                } as CSSProperties
              }
            />
          </div>
        </div>
      </div>
    </header>
  );
}
