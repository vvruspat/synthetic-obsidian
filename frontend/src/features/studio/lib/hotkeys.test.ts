import { describe, expect, it } from "vitest";
import {
  assignShortcut,
  createDefaultShortcutBindings,
  formatShortcut,
  shortcutActionForEvent,
} from "@/features/studio/lib/hotkeys";

const keyboardEvent = (
  code: string,
  modifiers: Partial<Pick<KeyboardEvent, "altKey" | "ctrlKey" | "metaKey" | "shiftKey">> = {},
) =>
  ({
    code,
    altKey: false,
    ctrlKey: false,
    metaKey: false,
    shiftKey: false,
    ...modifiers,
  }) as KeyboardEvent;

describe("studio hotkeys", () => {
  it("maps the requested transport and history shortcuts", () => {
    const bindings = createDefaultShortcutBindings();

    expect(shortcutActionForEvent(keyboardEvent("Space"), bindings)).toBe("transport.play-pause");
    expect(shortcutActionForEvent(keyboardEvent("Enter"), bindings)).toBe(
      "transport.return-to-start",
    );
    expect(shortcutActionForEvent(keyboardEvent("KeyZ", { metaKey: true }), bindings)).toBe(
      "project.undo",
    );
    expect(
      shortcutActionForEvent(keyboardEvent("KeyZ", { metaKey: true, shiftKey: true }), bindings),
    ).toBe("project.redo");
  });

  it("uses number keys for left tools and command-number for right tools", () => {
    const bindings = createDefaultShortcutBindings();

    expect(shortcutActionForEvent(keyboardEvent("Digit4"), bindings)).toBe("tool.left.scissors");
    expect(shortcutActionForEvent(keyboardEvent("Digit4", { metaKey: true }), bindings)).toBe(
      "tool.right.scissors",
    );
  });

  it("moves duplicate shortcuts to the newly assigned command", () => {
    const bindings = createDefaultShortcutBindings();
    const next = assignShortcut(bindings, "project.open", bindings["project.save"]);

    expect(next["project.open"]).toBe("Meta+KeyS");
    expect(next["project.save"]).toBeNull();
    expect(formatShortcut(next["project.open"])).toBe("⌘S");
  });
});
