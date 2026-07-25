import type { ButtonHTMLAttributes } from "react";
import type { IconComponent } from "@/ui/types";

type IconButtonProps = Omit<ButtonHTMLAttributes<HTMLButtonElement>, "children"> & {
  label: string;
  icon: IconComponent;
  active?: boolean;
  iconClassName?: string;
};

export function IconButton({
  label,
  icon: Icon,
  active = false,
  className = "",
  iconClassName = "",
  type = "button",
  ...buttonProps
}: IconButtonProps) {
  return (
    <button
      {...buttonProps}
      type={type}
      className={`ui-icon-button ${className} ${active ? "is-active" : ""}`.trim()}
      aria-label={label}
      aria-pressed={active}
      title={buttonProps.title ?? label}
    >
      <Icon className={iconClassName} strokeWidth={1.8} aria-hidden="true" />
    </button>
  );
}
