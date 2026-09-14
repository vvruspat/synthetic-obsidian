# Synthetic Obsidian runtime layout

Synthetic Obsidian is distributed for macOS Apple Silicon (`arm64`) only.
Users do not install Python, Conda, Homebrew, or `pip`.

The native application reads:

```text
~/Library/Application Support/Synthetic Obsidian/
  Runtime/
    runtime-manifest.json
    Packs/
      core/<version>/
      backing-notes/<version>/
      voice-engine/<version>/
  Models/
    Analysis/
    Backing Vocals/
    SoulX-Singer/
  Training/Jobs/
  Voice Profiles/
```

Each runtime pack may provide frozen workers in `workers/<worker-name>`.
During development it may instead provide an embedded interpreter under
`python/bin/python3` and Python entry points under `scripts/`.

Runtime archives containing executable code must be signed and notarized
before publication. The Runtime Manager must verify both the archive checksum
and the signing Team ID before activating a new version. Installation should
extract into a staging directory and atomically replace
`runtime-manifest.json` only after verification succeeds.

Legacy data under `~/.synthetic_obsidian/` remains readable so existing voice
profiles and active training jobs are not lost. New clean installations use
Application Support.

## First-test DMG

`build_first_test_dmg.sh` assembles the native app, the three frozen worker
packs, and the required local model files into one signed Apple Silicon DMG.
The user only drags Synthetic Obsidian into Applications; Python, Conda,
Homebrew, and network model downloads are not required.

The script notarizes and staples the disk image with the existing
`SMC-notarize` Keychain profile. Set `SYNTHETIC_OBSIDIAN_NOTARY_PROFILE` to
override the profile name, or explicitly set
`SYNTHETIC_OBSIDIAN_SKIP_NOTARIZATION=1` only for a local unnotarized test.
