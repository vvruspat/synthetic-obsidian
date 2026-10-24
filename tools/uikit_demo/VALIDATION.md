# Validation — 24 October 2026

## Local macOS

- Apple Clang Debug build: both applications and the native test executable pass.
- CTest runs 5 suites / 32 sections: demo integration, gallery lifecycle, base controls, editor surfaces and editor chrome. Additional queued notification lifetime checks execute through the real message loop. Zero failures; no JUCE assertions in the final log.
- Tests cover silent/deduplicated updates, scalar clamping/quantisation, balanced keyboard/mouse/accessibility gestures, cancellation and destruction, menu navigation/dismissal/overflow, callback deletion, common time transforms, cached geometry, signatures, note insertion/splitting/removal, collapsed groups, and minimum/large layouts.
- Native UI automation verified Play/Stop, mute, gain through accessibility, global zoom, backing group collapse, Escape returning menu focus, keyboard menu navigation into the gallery, a silent model update, Tab navigation and slider arrow-key increments.
- Actual JUCE renders inspected at 1586×992 and 1000×700; 2× render exported. Native macOS window also inspected at Retina backing scale. No other-platform DPI claim is made.
- GPT-6 Astra reviewed the implementation and actual native screenshots. Lifetime, accessibility-gesture and keyboard-synchronisation findings were fixed. Final visual review approved the composition and hierarchy; the result is not pixel-perfect. Synthetic waveforms are smoother and pitch traces more disconnected than the supplied reference.

## Cross-platform and memory checks

The dedicated `Native UI kit` CI workflow builds and tests Windows, macOS and Linux, and exports a native screenshot on each platform. Its actual status is reported on the pull request; workflow configuration alone is not a passing result.

The separate AddressSanitizer build passed all native tests on macOS (leak detection disabled). Snapshot overwrite was also verified by exporting twice to the same path and checking for a single PNG stream.

## Scope

No audio engine/AI integration, parameter IDs, serialization format, Python API or ONNX schema changes. These are message-thread UI components plus a demonstration model. No release packages or production migration were performed.
