import { ChevronDownIcon, PlusIcon, SparklesIcon } from "@heroicons/react/24/outline";
import { useEffect, useRef } from "react";
import type { BackingTrackName } from "@/domain/studio";

type BackingTrackSelectProps = {
  options: readonly BackingTrackName[];
  disabled: boolean;
  onSelect(track: BackingTrackName): void;
};

export function BackingTrackSelect({ options, disabled, onSelect }: BackingTrackSelectProps) {
  const detailsRef = useRef<HTMLDetailsElement>(null);

  useEffect(() => {
    const closeOnOutsideClick = (event: PointerEvent) => {
      const details = detailsRef.current;
      if (details && !details.contains(event.target as Node)) {
        details.removeAttribute("open");
      }
    };
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key === "Escape") {
        detailsRef.current?.removeAttribute("open");
      }
    };

    document.addEventListener("pointerdown", closeOnOutsideClick);
    document.addEventListener("keydown", closeOnEscape);
    return () => {
      document.removeEventListener("pointerdown", closeOnOutsideClick);
      document.removeEventListener("keydown", closeOnEscape);
    };
  }, []);

  return (
    <details
      className={`backing-track-select ${disabled ? "is-disabled" : ""}`}
      ref={detailsRef}
      aria-disabled={disabled}
      inert={disabled}
    >
      <summary aria-label={disabled ? "All backing tracks added" : "Add backing track"}>
        <PlusIcon className="backing-track-select-icon" strokeWidth={1.8} aria-hidden="true" />
        <strong>{disabled ? "All backing tracks added" : "Add backing track"}</strong>
        <ChevronDownIcon
          className="backing-track-select-chevron"
          strokeWidth={2}
          aria-hidden="true"
        />
      </summary>
      <div className="tool-menu backing-track-menu" role="menu" aria-label="Backing track">
        {options.map((track) => (
          <button
            type="button"
            role="menuitem"
            key={track}
            onClick={(event) => {
              onSelect(track);
              event.currentTarget.closest("details")?.removeAttribute("open");
            }}
          >
            <span className="tool-glyph" aria-hidden="true">
              <SparklesIcon strokeWidth={1.8} />
            </span>
            <strong>{track}</strong>
          </button>
        ))}
      </div>
    </details>
  );
}
