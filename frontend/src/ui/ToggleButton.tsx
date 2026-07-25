import type { ButtonHTMLAttributes, ReactNode } from "react";

type ToggleButtonProps = ButtonHTMLAttributes<HTMLButtonElement> & {
  active: boolean;
  children: ReactNode;
};

export function ToggleButton({
  active,
  className = "",
  children,
  type = "button",
  ...buttonProps
}: ToggleButtonProps) {
  return (
    <button
      {...buttonProps}
      type={type}
      className={`${className} ${active ? "selected" : ""}`.trim()}
      aria-pressed={active}
    >
      {children}
    </button>
  );
}
