import type { StudioController } from "@/features/studio/hooks/useStudioController";
import { PIANO_KEYS } from "@/features/studio/lib/pitch";
import { RangeInput } from "@/ui";

export function PianoKeyboard({ controller }: { controller: StudioController }) {
  return (
    <div className="piano-labels" aria-hidden="true">
      <div className="sidebar-keyboard">
        {PIANO_KEYS.map((key) => (
          <i
            key={key.pitch}
            className={`piano-key ${key.black ? "is-black" : ""} ${controller.dragChordPitchClasses.includes(key.pitch % 12) ? "is-chord-tone" : ""}`}
          >
            {key.pitch % 12 === 0 ? <span>{key.label}</span> : null}
          </i>
        ))}
      </div>
    </div>
  );
}

export function PianoZoomControl({ controller }: { controller: StudioController }) {
  return (
    <label
      className="piano-zoom-control"
      htmlFor="vertical-piano-zoom"
      title={`Vertical zoom ${controller.verticalZoom.toFixed(2)}×`}
    >
      <span className="sr-only">Vertical Piano Roll zoom</span>
      <RangeInput
        id="vertical-piano-zoom"
        min={1}
        max={4}
        step={0.05}
        value={controller.verticalZoom}
        aria-label="Vertical Piano Roll zoom"
        aria-valuetext={`${controller.verticalZoom.toFixed(2)} times`}
        onValueChange={controller.actions.setVerticalZoomFromInput}
      />
    </label>
  );
}
