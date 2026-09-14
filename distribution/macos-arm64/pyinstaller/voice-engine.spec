from pathlib import Path
import os
import setuptools

root = Path(SPECPATH).parents[2]
tool = root / "tools" / "vocal_annotation_tool"
pitch = root / "research" / "svc_pitch"
soulx = Path.home() / ".synthetic_obsidian" / "runtime" / "soulx-singer" / "source"
identity = os.environ.get("SYNTHETIC_OBSIDIAN_CODESIGN_IDENTITY")
vendor_backports = Path(setuptools.__file__).parent / "_vendor" / "backports"

a = Analysis(
    [str(Path(SPECPATH) / "worker_dispatch.py")],
    pathex=[str(tool), str(pitch), str(soulx)],
    binaries=[],
    datas=[(str(vendor_backports), "backports")],
    hiddenimports=[
        "align_lyrics_torchaudio",
        "analyze_musical_context",
        "analyze_notes_pyin",
        "backing_audio_worker",
        "infer_gtsinger_boundaries",
        "render_backing_vocal_soulx",
        "render_backing_vocal_soulx_singer",
        "soulx_lora",
        "voice_lora_training_worker",
        "g2p_en",
        "nltk",
        "soulxsinger.models.soulxsinger",
        "soulxsinger.models.soulxsinger_svc",
        "soulxsinger.utils.data_processor",
        "soulxsinger.utils.file_utils",
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
    name="voice-engine-worker",
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
    name="voice-engine",
)
