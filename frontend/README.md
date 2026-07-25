# Synthetic Obsidian Frontend

Client-only React interface for Synthetic Obsidian. The production output is a
static Vite SPA with relative asset paths. The JUCE Vocal Annotation Tool embeds
that output directly into its application bundle and serves it through
`WebBrowserComponent::ResourceProvider`.

The initial source was imported from
`vvruspat/synthetic-obsidian-fe` at commit
`2a5cc050fc30dd246af76030a779269528baf208`; `frontend/` is now the canonical
copy maintained with the native project.

## Development

Requires Node.js 22.13 or newer.

```bash
npm install
npm run dev
```

Quality checks:

```bash
npm run check
```

The static production bundle is written to `dist/`. It is intentionally checked
in so C++-only builds and release packaging do not depend on Node.js:

```bash
npm run build
```

## Project structure

```text
src/
  app/                  Application composition
  bridge/               Typed boundary to the C++/web-view host
  domain/               Studio domain types
  features/studio/      Studio UI, state, interactions and pure helpers
  mocks/                Browser-development data and mock native bridge
  styles/               Global and studio styles
  ui/                   Reusable UI component library
```

## Native bridge

When the frontend runs inside the standalone application, the host should
provide this object before the SPA starts:

```ts
window.syntheticObsidianHost = {
  postMessage(message: string) {
    // Forward the JSON string to the C++ application.
  },
};
```

Messages use the discriminated `PluginCommand` union from
`src/bridge/plugin-bridge.ts`. In an ordinary browser the app automatically
uses `MockPluginBridge`, so UI work does not depend on the plugin process.

The JUCE host installs this object with an early WebView user script and forwards
commands to `tools/vocal_annotation_tool/Main.cpp` on the message thread.

To push state back into the UI, the native host calls the function installed by
the frontend:

```ts
window.syntheticObsidianDispatch?.(
  JSON.stringify({
    type: "transport-state",
    playing: true,
    playhead: 42,
  }),
);
```

Inbound messages use the `PluginEvent` union from the same bridge module.

The first native handshake is `{ type: "frontend-ready" }`. The backend then
sends the current project, transport, loop, track, volume, and status state.
Project state contains real annotation notes, analyzed tempo segments, and
decimated min/max waveform samples for the loaded vocal and instrumental. Audio
decoding happens on a native background thread; the mock project is only used by
`npm run dev`.

Tempo and time-signature segments retain their native start/end times. The
frontend derives tempo automation, signature regions, bar/beat grid lines,
`BAR · BEAT · TICK`, and the current transport tempo/meter from that shared
musical timeline.

The native API stays behind this adapter: the UI does not import a particular
web-view SDK and can be embedded with JUCE, WebView2, WKWebView, CEF, or another
host without rewriting feature components.
