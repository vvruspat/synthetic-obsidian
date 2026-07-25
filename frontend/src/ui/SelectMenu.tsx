import { ChevronDownIcon } from "@heroicons/react/24/outline";
import { useEffect, useRef } from "react";
import type { IconComponent } from "@/ui/types";

export type SelectMenuOption<TValue extends string> = {
  value: TValue;
  label: string;
  icon: IconComponent;
};

type SelectMenuProps<TValue extends string> = {
  label: string;
  value: TValue;
  options: SelectMenuOption<TValue>[];
  onChange(value: TValue): void;
  className?: string;
};

export function SelectMenu<TValue extends string>({
  label,
  value,
  options,
  onChange,
  className = "",
}: SelectMenuProps<TValue>) {
  const detailsRef = useRef<HTMLDetailsElement>(null);
  const selected = options.find((option) => option.value === value) ?? options[0];
  const SelectedIcon = selected.icon;

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
    <details className={`ui-select-menu ${className}`.trim()} ref={detailsRef}>
      <summary aria-label={`${label}: ${selected.label}`} title={`${label} · ${selected.label}`}>
        <span className="tool-glyph" aria-hidden="true">
          <SelectedIcon strokeWidth={1.8} />
        </span>
        <ChevronDownIcon className="select-chevron" strokeWidth={2} aria-hidden="true" />
      </summary>
      <div className="tool-menu" role="menu" aria-label={`${label} tool`}>
        {options.map((option) => {
          const OptionIcon = option.icon;
          const isSelected = option.value === value;
          return (
            <button
              type="button"
              role="menuitemradio"
              aria-checked={isSelected}
              className={isSelected ? "is-selected" : ""}
              key={option.value}
              onClick={(event) => {
                onChange(option.value);
                event.currentTarget.closest("details")?.removeAttribute("open");
              }}
            >
              <span className="tool-glyph" aria-hidden="true">
                <OptionIcon strokeWidth={1.8} />
              </span>
              <strong>{option.label}</strong>
            </button>
          );
        })}
      </div>
    </details>
  );
}
