import type { CorrectionToolId } from "@/domain/studio";

export type ShortcutActionId =
  | "app.settings"
  | "project.new"
  | "project.open"
  | "project.save"
  | "project.undo"
  | "project.redo"
  | "project.export"
  | "transport.play-pause"
  | "transport.return-to-start"
  | "transport.toggle-loop"
  | "editor.toggle-chord-snap"
  | "editor.toggle-piano"
  | "view.horizontal-zoom-out"
  | "view.horizontal-zoom-in"
  | "view.vertical-zoom-out"
  | "view.vertical-zoom-in"
  | `tool.left.${CorrectionToolId}`
  | `tool.right.${CorrectionToolId}`;

export type ShortcutBindings = Record<ShortcutActionId, string | null>;

export type ShortcutDefinition = {
  id: ShortcutActionId;
  category: "Project" | "Transport" | "Editor" | "Left mouse tool" | "Right mouse tool";
  label: string;
  defaultShortcut: string;
};

export const CORRECTION_TOOL_ORDER: CorrectionToolId[] = [
  "pointer",
  "pencil",
  "eraser",
  "scissors",
  "join",
  "flex",
  "vibrato",
  "gain",
  "zoom",
];

const TOOL_LABELS: Record<CorrectionToolId, string> = {
  pointer: "Pointer Tool",
  pencil: "Pencil Tool",
  eraser: "Eraser Tool",
  scissors: "Scissors Tool",
  join: "Join Tool",
  flex: "Flex Tool",
  vibrato: "Flex Pitch Vibrato",
  gain: "Gain Tool",
  zoom: "Zoom Tool",
};

const TOOL_CODES = [
  "Digit1",
  "Digit2",
  "Digit3",
  "Digit4",
  "Digit5",
  "Digit6",
  "Digit7",
  "Digit8",
  "Digit9",
];

export const SHORTCUT_DEFINITIONS: ShortcutDefinition[] = [
  {
    id: "app.settings",
    category: "Project",
    label: "Settings",
    defaultShortcut: "Meta+Comma",
  },
  {
    id: "project.new",
    category: "Project",
    label: "New project",
    defaultShortcut: "Meta+KeyN",
  },
  {
    id: "project.open",
    category: "Project",
    label: "Open project",
    defaultShortcut: "Meta+KeyO",
  },
  {
    id: "project.save",
    category: "Project",
    label: "Save project",
    defaultShortcut: "Meta+KeyS",
  },
  {
    id: "project.undo",
    category: "Project",
    label: "Undo",
    defaultShortcut: "Meta+KeyZ",
  },
  {
    id: "project.redo",
    category: "Project",
    label: "Redo",
    defaultShortcut: "Shift+Meta+KeyZ",
  },
  {
    id: "project.export",
    category: "Project",
    label: "Export all tracks",
    defaultShortcut: "Meta+KeyE",
  },
  {
    id: "transport.play-pause",
    category: "Transport",
    label: "Play / Pause",
    defaultShortcut: "Space",
  },
  {
    id: "transport.return-to-start",
    category: "Transport",
    label: "Return playhead to start",
    defaultShortcut: "Enter",
  },
  {
    id: "transport.toggle-loop",
    category: "Transport",
    label: "Toggle cycle",
    defaultShortcut: "KeyL",
  },
  {
    id: "editor.toggle-chord-snap",
    category: "Editor",
    label: "Toggle Chord Snap",
    defaultShortcut: "KeyS",
  },
  {
    id: "editor.toggle-piano",
    category: "Editor",
    label: "Expand / collapse Pitch Editor",
    defaultShortcut: "KeyP",
  },
  {
    id: "view.horizontal-zoom-out",
    category: "Editor",
    label: "Horizontal zoom out",
    defaultShortcut: "Minus",
  },
  {
    id: "view.horizontal-zoom-in",
    category: "Editor",
    label: "Horizontal zoom in",
    defaultShortcut: "Equal",
  },
  {
    id: "view.vertical-zoom-out",
    category: "Editor",
    label: "Vertical zoom out",
    defaultShortcut: "Alt+Minus",
  },
  {
    id: "view.vertical-zoom-in",
    category: "Editor",
    label: "Vertical zoom in",
    defaultShortcut: "Alt+Equal",
  },
  ...CORRECTION_TOOL_ORDER.map<ShortcutDefinition>((tool, index) => ({
    id: `tool.left.${tool}`,
    category: "Left mouse tool",
    label: TOOL_LABELS[tool],
    defaultShortcut: TOOL_CODES[index],
  })),
  ...CORRECTION_TOOL_ORDER.map<ShortcutDefinition>((tool, index) => ({
    id: `tool.right.${tool}`,
    category: "Right mouse tool",
    label: TOOL_LABELS[tool],
    defaultShortcut: `Meta+${TOOL_CODES[index]}`,
  })),
];

const STORAGE_KEY = "synthetic-obsidian.shortcuts.v1";

export function createDefaultShortcutBindings(): ShortcutBindings {
  return Object.fromEntries(
    SHORTCUT_DEFINITIONS.map(({ id, defaultShortcut }) => [id, defaultShortcut]),
  ) as ShortcutBindings;
}

export function loadShortcutBindings(): ShortcutBindings {
  const defaults = createDefaultShortcutBindings();
  try {
    const saved = JSON.parse(window.localStorage.getItem(STORAGE_KEY) ?? "{}") as Record<
      string,
      unknown
    >;
    for (const definition of SHORTCUT_DEFINITIONS) {
      const value = saved[definition.id];
      if (typeof value === "string" || value === null) {
        defaults[definition.id] = value;
      }
    }
  } catch {
    // Invalid or unavailable local storage should never prevent the editor from starting.
  }
  return defaults;
}

export function saveShortcutBindings(bindings: ShortcutBindings) {
  try {
    window.localStorage.setItem(STORAGE_KEY, JSON.stringify(bindings));
  } catch {
    // Keep the in-memory mapping when persistent browser storage is unavailable.
  }
}

export function shortcutFromKeyboardEvent(event: KeyboardEvent): string | null {
  if (
    event.code === "MetaLeft" ||
    event.code === "MetaRight" ||
    event.code === "ShiftLeft" ||
    event.code === "ShiftRight" ||
    event.code === "AltLeft" ||
    event.code === "AltRight" ||
    event.code === "ControlLeft" ||
    event.code === "ControlRight"
  ) {
    return null;
  }

  const code = event.code === "NumpadEnter" ? "Enter" : event.code;
  if (!code) return null;

  const parts: string[] = [];
  if (event.ctrlKey) parts.push("Control");
  if (event.altKey) parts.push("Alt");
  if (event.shiftKey) parts.push("Shift");
  if (event.metaKey) parts.push("Meta");
  parts.push(code);
  return parts.join("+");
}

const CODE_LABELS: Record<string, string> = {
  Space: "Space",
  Enter: "↩",
  Escape: "Esc",
  Backspace: "⌫",
  Delete: "⌦",
  Minus: "−",
  Equal: "=",
  Comma: ",",
  BracketLeft: "[",
  BracketRight: "]",
  ArrowLeft: "←",
  ArrowRight: "→",
  ArrowUp: "↑",
  ArrowDown: "↓",
};

export function formatShortcut(shortcut: string | null): string {
  if (!shortcut) return "—";
  return shortcut
    .split("+")
    .map((part) => {
      if (part === "Meta") return "⌘";
      if (part === "Shift") return "⇧";
      if (part === "Alt") return "⌥";
      if (part === "Control") return "⌃";
      if (part.startsWith("Key")) return part.slice(3);
      if (part.startsWith("Digit")) return part.slice(5);
      return CODE_LABELS[part] ?? part;
    })
    .join("");
}

export function assignShortcut(
  bindings: ShortcutBindings,
  action: ShortcutActionId,
  shortcut: string | null,
): ShortcutBindings {
  const next = { ...bindings };
  if (shortcut) {
    for (const definition of SHORTCUT_DEFINITIONS) {
      if (definition.id !== action && next[definition.id] === shortcut) {
        next[definition.id] = null;
      }
    }
  }
  next[action] = shortcut;
  return next;
}

export function shortcutActionForEvent(
  event: KeyboardEvent,
  bindings: ShortcutBindings,
): ShortcutActionId | null {
  const shortcut = shortcutFromKeyboardEvent(event);
  if (!shortcut) return null;
  return SHORTCUT_DEFINITIONS.find(({ id }) => bindings[id] === shortcut)?.id ?? null;
}
