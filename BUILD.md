# Build Instructions — Synthetic Obsidian

## Prerequisites

### macOS
```bash
# Xcode Command Line Tools
xcode-select --install

# CMake 3.22+
brew install cmake

# Ninja (faster builds, optional)
brew install ninja
```

### Windows
- Visual Studio 2022 (Community or higher) with "Desktop development with C++" workload
- CMake 3.22+ (bundled with VS or via https://cmake.org)

---

## First Build (downloads JUCE automatically via FetchContent)

### macOS — Debug
```bash
cd /path/to/synthetic-obsidian

cmake -B build/debug \
      -G "Xcode" \
      -DCMAKE_BUILD_TYPE=Debug

cmake --build build/debug --config Debug -- -jobs $(sysctl -n hw.logicalcpu)
```

### macOS — Release (Universal Binary)
```bash
cmake -B build/release \
      -G "Xcode" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"

cmake --build build/release --config Release -- -jobs $(sysctl -n hw.logicalcpu)
```

### macOS — Ninja (faster iteration)
```bash
cmake -B build/ninja \
      -G Ninja \
      -DCMAKE_BUILD_TYPE=Debug

cmake --build build/ninja -j$(sysctl -n hw.logicalcpu)
```

### Windows — Visual Studio
```cmd
cmake -B build\vs2022 -G "Visual Studio 17 2022" -A x64
cmake --build build\vs2022 --config Debug
```

---

## Output locations (after build)

### macOS
```
build/debug/SyntheticObsidian_artefacts/Debug/
├── Standalone/
│   └── Synthetic Obsidian.app          ← run this for testing
└── VST3/
    └── Synthetic Obsidian.vst3         ← copy to ~/Library/Audio/Plug-Ins/VST3/
```

### Windows
```
build\vs2022\SyntheticObsidian_artefacts\Debug\
├── Standalone\
│   └── Synthetic Obsidian.exe
└── VST3\
    └── Synthetic Obsidian.vst3         ← copy to C:\Program Files\Common Files\VST3\
```

---

## Running the Standalone app (fastest way to test)

```bash
# macOS
open "build/debug/SyntheticObsidian_artefacts/Debug/Standalone/Synthetic Obsidian.app"

# or with lldb for debugging
lldb "build/debug/SyntheticObsidian_artefacts/Debug/Standalone/Synthetic Obsidian.app"
```

---

## Install VST3 for DAW testing

```bash
# macOS — install to user VST3 folder
cp -r "build/debug/SyntheticObsidian_artefacts/Debug/VST3/Synthetic Obsidian.vst3" \
      ~/Library/Audio/Plug-Ins/VST3/
```

Then rescan plugins in your DAW (Ableton → Preferences → Plugins, Reaper → Options → Preferences → Plug-ins).

---

## Incremental builds

After changing source files, just re-run:
```bash
cmake --build build/debug --config Debug -- -jobs $(sysctl -n hw.logicalcpu)
```

CMake will only recompile changed translation units.

---

## Clean build

```bash
rm -rf build/
# Then run the cmake configure + build commands again
```

Note: first build downloads JUCE (~800MB) and compiles it. This takes 10-20 minutes.
Subsequent builds are much faster (only your source files recompile).

---

## Troubleshooting

**"JUCE not found"** → CMake downloads JUCE automatically via FetchContent. Ensure internet access during first build.

**Xcode signing errors** → Add to cmake configure:
```bash
-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED=NO \
-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY=""
```

**"Plugin not found in DAW"** → Rescan plugins. On macOS, check that the `.vst3` bundle is in `~/Library/Audio/Plug-Ins/VST3/` or `/Library/Audio/Plug-Ins/VST3/`.

**Windows: missing DLLs** → Build with `/MT` (static CRT) or install Visual C++ Redistributable.
