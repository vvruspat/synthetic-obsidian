# Native studio control parity

The visible `VocalAnnotationTool` workspace uses the custom JUCE UI kit. This
inventory maps the former React controls to the native workspace and its real
backend operations.

| Workflow | Native location |
| --- | --- |
| New/open/save, selected-track File Import, exports | Project menu; existing project backend |
| Play/pause, stop, rewind, cycle toggle, master gain/meters | Transport |
| Create/move/resize/toggle cycle | Dedicated cycle band above the seek ruler; Escape cancels |
| Global lane visibility | Global Tracks disclosure |
| Add/import/delete audio track, Mute/Solo | Track menus; direct Mute/Solo buttons |
| Independent Audio/Notes layers | Compact waveform/notes icon toggles |
| Shift-select tracks | Combined read-only pitch overlay; single selection enables editing |
| Add backing type + generator | Direct Add backing popup on source/group; Local/Luna/Terra followed by harmony style |
| Voice profile, render/re-render, regenerate | Backing row profile selector and track menu |
| Backing hierarchy collapse | Source group disclosure, available before the first child |
| Voice lab and active training progress | Project menu; native training/settings panel |
| Device input/output, refresh/test/apply | Settings, Audio / Voice / Backing tab |
| Voice sources, profiles, training/cancel/reveal/delete | Settings and Voice Lab |
| Editable keys, clear/duplicate transfer/reset | Settings, Key commands tab; persisted in native Settings/shortcuts.json |
| Export result directory / dismiss | Completion notification |

## Piano roll

The single-row toolbar has vertical zoom on the left, Undo/Redo, compact tool
icons, Chord Snap, and horizontal zoom/collapse on the right. Tool names stay
inside the dropdown menus. Time panning uses a standard JUCE scrollbar.

Both Click and Command-click (Ctrl-click on Windows/Linux) menus expose Pointer,
Pencil, Eraser, Scissors, Join, Flex, Vibrato, Gain and Zoom. Right-click also uses
the secondary tool. Pointer edits continuous pitch and auditions it without
moving time. Flex stretches the selected boundary. Pencil supports either drag
direction and a half-second default. Gain/vibrato preview locally and commit once.
Chord Snap transforms preview, audition and commit consistently.

The keyboard and pitch editor scroll vertically together. Zoom spans 1–4×;
Pinch changes horizontal zoom; Option/Alt + pinch changes vertical zoom around
the pointer. Alt+wheel changes vertical zoom, Command/Ctrl+wheel horizontal zoom
and Shift+wheel pans time. These gestures also work over the ruler and keyboard. The collapse button retains the toolbar, selection, zoom and scroll.
Tool defaults and shortcuts follow the previous editor.

A subtle audio waveform sits behind the selected track's notes. It uses the
actual arrangement clip positions, source offsets and durations, converted through
the tempo map. Silent gaps stay empty; multiple selected tracks hide the waveform
to avoid implying that one track's audio belongs to another. The waveform stays
centered in the visible piano viewport while pitches scroll, with geometry cached
outside paint. Backing-group headers share the track-header column width.

Vocal-fragment edits retain source note bounds/regions and exact pitch; a group
edit transforms each source once and creates one history action. Incoming data,
Escape, focus loss, hide/disable and capture loss cancel drafts and stop audition.

## Verification

`NativeStudioTests` checks real command/event payloads, both tool assignments,
gain and chord-snap editing, direct backing creation, cycle creation/movement/both
edges/toggle/cancellation, multiple tracks, focus, all 34 shortcut defaults and
custom bindings, and toolbar bounds at 1000/1280/1586 widths. `ObsidianUIKitTests`
covers individual gestures, cancellation, callback deletion and painting caches.
`ArrangementTests` covers playback, document round trips and source-note edits.

Fixture renders can be exported without opening or changing a user project:
`NativeStudioTests --snapshot-dir /absolute/output/directory`.
