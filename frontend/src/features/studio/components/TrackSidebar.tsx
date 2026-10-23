import {
  ArrowsRightLeftIcon,
  ChevronDoubleDownIcon,
  ChevronDoubleUpIcon,
  ClockIcon,
  MusicalNoteIcon,
  Squares2X2Icon,
} from "@heroicons/react/24/outline";
import { TrackHeader } from "@/features/studio/components/TrackHeader";
import type { StudioController } from "@/features/studio/hooks/useStudioController";
import {
  BLACK_KEY_PITCHES,
  getWhiteKeyBounds,
  PITCH_ROW_HEIGHT,
  pitchToTop,
  WHITE_KEY_PITCHES,
} from "@/features/studio/lib/pitch";
import { RangeInput } from "@/ui";

export function TrackSidebar({ controller }: { controller: StudioController }) {
  const {
    trackRows,
    selectedTracks,
    trackState,
    trackLayerState,
    trackLayerAvailability,
    horizontalZoom,
    verticalZoom,
    instrumentalVolume,
    dragChordPitchClasses,
    serviceTracksCollapsed,
    refs,
    actions,
  } = controller;
  const renderTrackHeader = (
    id: Parameters<typeof actions.toggleTrack>[0],
    name = id,
    onDelete?: () => void,
    depth = 0,
    childCount = 0,
    collapsed = false,
  ) => (
    <TrackHeader
      depth={depth}
      childCount={childCount}
      collapsed={collapsed}
      onToggleCollapsed={() => actions.toggleTrackCollapsed(id, !collapsed)}
      onImport={() => actions.importTrackAudio(id)}
      id={id}
      name={name}
      state={trackState[id] ?? { mute: false, solo: false }}
      onToggle={(mode) => actions.toggleTrack(id, mode)}
      volume={id === "Instrumental" ? instrumentalVolume : undefined}
      onVolumeChange={id === "Instrumental" ? actions.setInstrumentalVolumeValue : undefined}
      layerState={
        id === "Instrumental"
          ? undefined
          : (trackLayerState[id] ?? { audioMuted: false, notesMuted: false })
      }
      layerAvailability={id === "Instrumental" ? undefined : trackLayerAvailability[id]}
      onToggleLayer={
        id === "Instrumental" ? undefined : (layer) => actions.toggleTrackLayer(id, layer)
      }
      selected={selectedTracks.includes(id)}
      onSelect={(additive) => actions.selectTrack(id, additive)}
      onDelete={onDelete}
    />
  );

  return (
    <aside className="lane-sidebar">
      <div className="sidebar-spacer">
        <button
          type="button"
          className="service-tracks-toggle"
          aria-label={
            serviceTracksCollapsed
              ? "Show Tempo, Signature and Chord tracks"
              : "Hide Tempo, Signature and Chord tracks"
          }
          aria-expanded={!serviceTracksCollapsed}
          title={serviceTracksCollapsed ? "Show service tracks" : "Hide service tracks"}
          onClick={() => actions.setServiceTracksCollapsed((current) => !current)}
        >
          {serviceTracksCollapsed ? (
            <ChevronDoubleDownIcon aria-hidden="true" />
          ) : (
            <ChevronDoubleUpIcon aria-hidden="true" />
          )}
        </button>
      </div>
      <div className="meta-header tempo-label">
        <span className="meta-icon" aria-hidden="true">
          <ClockIcon strokeWidth={1.8} />
        </span>
        <span>Tempo</span>
      </div>
      <div className="meta-header">
        <span className="meta-icon" aria-hidden="true">
          <MusicalNoteIcon strokeWidth={1.8} />
        </span>
        <span>Signature</span>
      </div>
      <div className="meta-header">
        <span className="meta-icon" aria-hidden="true">
          <Squares2X2Icon strokeWidth={1.8} />
        </span>
        <span>Chord</span>
      </div>
      <div className="sidebar-track-section">
        <div className="arrangement-add-toolbar sidebar-add-toolbar">
          <button type="button" onClick={actions.addTrack}>
            + Add track
          </button>
        </div>
        <div
          className="sidebar-track-stack"
          ref={refs.sidebarTrackScrollRef}
          onScroll={() => actions.syncTrackScroll("sidebar")}
        >
          {trackRows.map(({ track, depth, childCount }) => (
            <div key={track.id}>
              {renderTrackHeader(
                track.id,
                track.name,
                () => {
                  if (
                    window.confirm(
                      `Delete track “${track.name}”${childCount ? " and its backing tracks" : ""}?`,
                    )
                  )
                    actions.removeTrack(track.id);
                },
                depth,
                childCount,
                track.collapsed,
              )}
            </div>
          ))}
        </div>
      </div>
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
      <div className="piano-labels">
        <label
          className="piano-zoom-control"
          htmlFor="vertical-piano-zoom"
          title={`Vertical zoom ${verticalZoom.toFixed(2)}×`}
        >
          <span className="sr-only">Vertical Piano Roll zoom</span>
          <RangeInput
            id="vertical-piano-zoom"
            min={1}
            max={4}
            step={0.05}
            value={verticalZoom}
            aria-label="Vertical Piano Roll zoom"
            aria-valuetext={`${verticalZoom.toFixed(2)} times`}
            onValueChange={actions.setVerticalZoomFromInput}
          />
        </label>
        <div
          className="piano-keyboard-viewport"
          ref={refs.pianoKeysScrollRef}
          onScroll={() => actions.syncVerticalScroll("keys")}
          onWheel={actions.handlePianoWheel}
          aria-hidden="true"
        >
          <div
            className="piano-keyboard-scroll-content"
            style={{ height: `${verticalZoom * 100}%` }}
          >
            <div className="sidebar-keyboard">
              <div className="piano-white-keys">
                {WHITE_KEY_PITCHES.map((pitch, index) => {
                  const bounds = getWhiteKeyBounds(index);
                  return (
                    <i
                      className={`piano-white-key ${
                        dragChordPitchClasses.includes(pitch % 12) ? "is-chord-tone" : ""
                      }`}
                      key={`white-${pitch}`}
                      style={{
                        top: `${bounds.top}%`,
                        height: `${bounds.height}%`,
                      }}
                    >
                      {pitch % 12 === 0 ? <span>{`C${Math.floor(pitch / 12) - 1}`}</span> : null}
                    </i>
                  );
                })}
              </div>
              {BLACK_KEY_PITCHES.map((pitch) => (
                <i
                  className={`piano-black-key ${
                    dragChordPitchClasses.includes(pitch % 12) ? "is-chord-tone" : ""
                  }`}
                  key={`black-${pitch}`}
                  style={{
                    top: `${pitchToTop(pitch)}%`,
                    height: `${PITCH_ROW_HEIGHT}%`,
                  }}
                />
              ))}
            </div>
          </div>
        </div>
      </div>
    </aside>
  );
}
