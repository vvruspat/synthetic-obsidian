import {
  CheckIcon,
  ChevronDownIcon,
  PlusCircleIcon,
  TrashIcon,
  UserCircleIcon,
} from "@heroicons/react/24/outline";
import { useCallback, useEffect, useLayoutEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";
import type { VoiceProfileOption } from "@/domain/studio";
import { VocalistIcon } from "@/ui";

const qualityLabel = (profile: VoiceProfileOption) => {
  if (profile.quality === "low") return "Low quality";
  if (profile.quality === "high") return "High quality";
  return "Balanced";
};

export function VoiceProfileSelect({
  track,
  profiles,
  value,
  disabled,
  compact = false,
  onChange,
  onDelete,
  onCreateVoice,
}: {
  track: string;
  profiles: VoiceProfileOption[];
  value: string;
  disabled: boolean;
  compact?: boolean;
  onChange(profileId: string): void;
  onDelete?(profileId: string): void;
  onCreateVoice?(): void;
}) {
  const detailsRef = useRef<HTMLDetailsElement>(null);
  const menuRef = useRef<HTMLDivElement>(null);
  const [open, setOpen] = useState(false);
  const [menuPosition, setMenuPosition] = useState({ left: 8, top: 8 });
  const selected =
    profiles.find((profile) => profile.id === value) ?? profiles.find((profile) => profile.active);

  const updateMenuPosition = useCallback(() => {
    const anchor = detailsRef.current?.getBoundingClientRect();
    if (!anchor) return;
    const menuWidth = 260;
    const menuHeight = menuRef.current?.offsetHeight ?? Math.min(360, profiles.length * 43 + 62);
    const left = Math.max(
      8,
      Math.min(window.innerWidth - menuWidth - 8, anchor.left + anchor.width / 2 - menuWidth / 2),
    );
    const spaceAbove = anchor.top - 8;
    const spaceBelow = window.innerHeight - anchor.bottom - 8;
    const openAbove = spaceAbove > spaceBelow && spaceAbove >= Math.min(menuHeight, 240);
    const top = openAbove
      ? Math.max(8, anchor.top - menuHeight - 5)
      : Math.min(window.innerHeight - menuHeight - 8, anchor.bottom + 5);
    setMenuPosition({ left, top: Math.max(8, top) });
  }, [profiles.length]);

  useEffect(() => {
    if (!open) return;
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
  }, [open, updateMenuPosition]);

  useLayoutEffect(() => {
    if (open) updateMenuPosition();
  }, [open, updateMenuPosition]);

  useEffect(() => {
    if (disabled) {
      detailsRef.current?.removeAttribute("open");
      setOpen(false);
    }
  }, [disabled]);

  const closeMenu = () => {
    detailsRef.current?.removeAttribute("open");
    setOpen(false);
  };

  const menu = (
    <div
      className="voice-profile-menu is-portal"
      role="menu"
      aria-label={`Voice model for ${track}`}
      ref={menuRef}
      style={menuPosition}
    >
      {profiles.map((profile) => (
        <div className="voice-profile-option-row" key={profile.id}>
          <button
            type="button"
            role="menuitemradio"
            aria-checked={profile.id === selected?.id}
            className={profile.id === selected?.id ? "is-selected" : ""}
            onClick={() => {
              if (profile.id !== selected?.id) onChange(profile.id);
              closeMenu();
            }}
          >
            <UserCircleIcon className="voice-profile-option-icon" aria-hidden="true" />
            <span>
              <strong>{profile.name}</strong>
              <small>
                {qualityLabel(profile)}
                {profile.active ? " · Active LoRA" : ""}
              </small>
            </span>
            {profile.id === selected?.id ? <CheckIcon aria-hidden="true" /> : null}
          </button>
          {onDelete && profile.id !== "source-timbre" ? (
            <button
              type="button"
              className="voice-profile-delete-option"
              aria-label={`Delete ${profile.name}`}
              title={`Delete ${profile.name}`}
              onClick={() => {
                if (window.confirm(`Delete voice “${profile.name}”? It will be moved to Trash.`)) {
                  onDelete(profile.id);
                  closeMenu();
                }
              }}
            >
              <TrashIcon aria-hidden="true" />
            </button>
          ) : null}
        </div>
      ))}
      {onCreateVoice ? (
        <>
          <i className="voice-profile-menu-separator" aria-hidden="true" />
          <button
            type="button"
            role="menuitem"
            className="voice-profile-create-option"
            onClick={() => {
              onCreateVoice();
              closeMenu();
            }}
          >
            <PlusCircleIcon className="voice-profile-option-icon" aria-hidden="true" />
            <span>
              <strong>Create new voice</strong>
              <small>Train a new LoRA profile</small>
            </span>
          </button>
        </>
      ) : null}
    </div>
  );

  return (
    <details
      className={`voice-profile-select ${compact ? "is-compact" : ""} ${disabled ? "is-disabled" : ""}`}
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
        className={compact ? `ui-compact-button ${open ? "selected" : ""}` : undefined}
        aria-label={`Voice model for ${track}`}
        title={`Voice for ${track}: ${selected?.name ?? "Choose voice"} · Change to rerender`}
      >
        {compact ? (
          <VocalistIcon aria-hidden="true" />
        ) : (
          <>
            <UserCircleIcon className="voice-profile-summary-icon" aria-hidden="true" />
            <span className="voice-profile-summary-copy">
              <strong>{selected?.name ?? "Choose voice"}</strong>
              <small>{selected ? qualityLabel(selected) : "No voice selected"}</small>
            </span>
            <ChevronDownIcon className="voice-profile-chevron" aria-hidden="true" />
          </>
        )}
      </summary>
      {open ? createPortal(menu, document.body) : null}
    </details>
  );
}
