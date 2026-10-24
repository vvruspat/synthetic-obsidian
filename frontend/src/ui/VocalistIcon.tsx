import type { SVGProps } from "react";

export function VocalistIcon(props: SVGProps<SVGSVGElement>) {
  return (
    <svg
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth={1.8}
      aria-hidden="true"
      {...props}
    >
      <circle cx="8" cy="6" r="3" />
      <path d="M2 21v-3a6 6 0 0 1 12 0v3M19 15v6m-3 0h6" strokeLinecap="round" />
      <rect x="17" y="5" width="4" height="7" rx="2" />
      <path d="M15 10v1a4 4 0 0 0 8 0v-1" strokeLinecap="round" />
    </svg>
  );
}
