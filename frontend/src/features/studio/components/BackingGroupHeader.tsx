import { ChevronDownIcon, ChevronRightIcon, SparklesIcon } from "@heroicons/react/24/outline";
import type { ArrangementRow } from "@/features/studio/lib/arrangement";

export function BackingGroupHeader({ row, onToggle }: { row: ArrangementRow; onToggle(): void }) {
  return (
    <div className="backing-group-header" style={{ paddingLeft: 30 + row.depth * 16 }}>
      <span className="track-row-number">{row.number}</span>
      <button
        type="button"
        className="backing-group-disclosure"
        aria-label={`${row.track.collapsed ? "Expand" : "Collapse"} ${row.track.name} backing tracks`}
        aria-expanded={!row.track.collapsed}
        onClick={onToggle}
      >
        {row.track.collapsed ? (
          <ChevronRightIcon aria-hidden="true" />
        ) : (
          <ChevronDownIcon aria-hidden="true" />
        )}
        <SparklesIcon aria-hidden="true" />
      </button>
      <div>
        <strong>{row.track.name}</strong>
        <span>
          {row.childCount} backing {row.childCount === 1 ? "track" : "tracks"}
        </span>
      </div>
    </div>
  );
}
