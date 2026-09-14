import { ChevronDownIcon, PlusIcon, SparklesIcon } from "@heroicons/react/24/outline";
import { useCallback, useEffect, useLayoutEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";
import type { BackingGenerator, BackingTrackName } from "@/domain/studio";

type BackingTrackSelectProps = {
  options: readonly BackingTrackName[];
  generator: BackingGenerator;
  disabled: boolean;
  disabledReason?: string;
  onGeneratorChange(generator: BackingGenerator): void;
  onSelect(track: BackingTrackName): void;
};

export function BackingTrackSelect({
  options,
  generator,
  disabled,
  disabledReason,
  onGeneratorChange,
  onSelect,
}: BackingTrackSelectProps) {
  const detailsRef = useRef<HTMLDetailsElement>(null);
  const menuRef = useRef<HTMLDivElement>(null);
  const [open, setOpen] = useState(false);
  const [menuPosition, setMenuPosition] = useState({ left: 8, top: 8 });

  const updateMenuPosition = useCallback(() => {
    const anchor = detailsRef.current?.getBoundingClientRect();
    if (!anchor) return;
    const menuWidth = 252;
    const menuHeight = menuRef.current?.offsetHeight ?? Math.min(390, options.length * 31 + 10);
    const left = Math.max(
      8,
      Math.min(window.innerWidth - menuWidth - 8, anchor.left + anchor.width / 2 - menuWidth / 2),
    );
    const spaceAbove = anchor.top - 8;
    const spaceBelow = window.innerHeight - anchor.bottom - 8;
    const openAbove = spaceAbove > spaceBelow && spaceAbove >= Math.min(menuHeight, 220);
    const top = openAbove
      ? Math.max(8, anchor.top - menuHeight - 5)
      : Math.min(window.innerHeight - menuHeight - 8, anchor.bottom + 5);
    setMenuPosition({ left, top: Math.max(8, top) });
  }, [options.length]);

  useEffect(() => {
    const closeOnOutsideClick = (event: PointerEvent) => {
      const details = detailsRef.current;
      const menu = menuRef.current;
      if (
        details &&
        !details.contains(event.target as Node) &&
        !menu?.contains(event.target as Node)
      ) {
        details.removeAttribute("open");
        setOpen(false);
      }
    };
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key === "Escape") {
        detailsRef.current?.removeAttribute("open");
        setOpen(false);
      }
    };
    const reposition = () => updateMenuPosition();

    document.addEventListener("pointerdown", closeOnOutsideClick);
    document.addEventListener("keydown", closeOnEscape);
    window.addEventListener("resize", reposition);
    window.addEventListener("scroll", reposition, true);
    return () => {
      document.removeEventListener("pointerdown", closeOnOutsideClick);
      document.removeEventListener("keydown", closeOnEscape);
      window.removeEventListener("resize", reposition);
      window.removeEventListener("scroll", reposition, true);
    };
  }, [updateMenuPosition]);

  useLayoutEffect(() => {
    if (open) updateMenuPosition();
  }, [open, updateMenuPosition]);

  useEffect(() => {
    if (!disabled) return;
    detailsRef.current?.removeAttribute("open");
    setOpen(false);
  }, [disabled]);

  const closeMenu = () => {
    detailsRef.current?.removeAttribute("open");
    setOpen(false);
  };

  const menu = (
    <div
      className="tool-menu backing-track-menu is-portal"
      role="menu"
      aria-label="Backing track"
      ref={menuRef}
      style={menuPosition}
    >
      <fieldset className="backing-generator-picker" aria-label="Melody generator">
        <legend>Generator</legend>
        {(["local", "luna", "terra"] as const).map((value) => (
          <button
            type="button"
            className={generator === value ? "is-selected" : ""}
            aria-pressed={generator === value}
            key={value}
            onClick={() => onGeneratorChange(value)}
          >
            {value === "local" ? "Local" : value === "luna" ? "Luna" : "Terra"}
          </button>
        ))}
      </fieldset>
      {options.map((track) => (
        <button
          type="button"
          role="menuitem"
          key={track}
          onClick={() => {
            onSelect(track);
            closeMenu();
          }}
        >
          <span className="tool-glyph" aria-hidden="true">
            <SparklesIcon strokeWidth={1.8} />
          </span>
          <strong>{track}</strong>
        </button>
      ))}
    </div>
  );

  return (
    <details
      className={`backing-track-select ${disabled ? "is-disabled" : ""}`}
      ref={detailsRef}
      aria-disabled={disabled}
      inert={disabled}
      onToggle={(event) => {
        const nextOpen = event.currentTarget.open;
        setOpen(nextOpen);
        if (nextOpen) updateMenuPosition();
      }}
    >
      <summary
        aria-label={
          disabled ? (disabledReason ?? "Backing tracks unavailable") : "Add backing track"
        }
        title={disabled ? disabledReason : "Add backing track"}
      >
        <PlusIcon className="backing-track-select-icon" strokeWidth={1.8} aria-hidden="true" />
        <strong>Add backing track</strong>
        <ChevronDownIcon
          className="backing-track-select-chevron"
          strokeWidth={2}
          aria-hidden="true"
        />
      </summary>
      {open ? createPortal(menu, document.body) : null}
    </details>
  );
}
