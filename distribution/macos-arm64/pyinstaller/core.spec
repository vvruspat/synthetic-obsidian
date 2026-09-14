from pathlib import Path
import os

root = Path(SPECPATH).parents[2]
tool = root / "tools" / "vocal_annotation_tool"
pitch = root / "research" / "svc_pitch"
identity = os.environ.get("SYNTHETIC_OBSIDIAN_CODESIGN_IDENTITY")

a = Analysis(
    [str(Path(SPECPATH) / "worker_dispatch.py")],
    pathex=[str(root), str(tool), str(pitch)],
    binaries=[],
    datas=[],
    hiddenimports=[
        "align_lyrics_torchaudio",
        "analyze_musical_context",
        "analyze_notes_pyin",
        "infer_gtsinger_boundaries",
        "research.gtsinger_alignment.train_baseline",
    ],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=["matplotlib", "notebook", "IPython", "tkinter"],
    noarchive=False,
)
pyz = PYZ(a.pure)
exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name="core-worker",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=True,
    target_arch="arm64",
    codesign_identity=identity,
    entitlements_file=str(root / "distribution" / "macos-arm64" / "worker.entitlements"),
)
coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=False,
    name="core",
)
