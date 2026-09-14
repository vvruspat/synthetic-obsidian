from pathlib import Path
import os
from PyInstaller.utils.hooks import collect_data_files, collect_submodules

root = Path(SPECPATH).parents[2]
tool = root / "tools" / "vocal_annotation_tool"
model_code = root / "scripts" / "backing_transformer"
identity = os.environ.get("SYNTHETIC_OBSIDIAN_CODESIGN_IDENTITY")

a = Analysis(
    [str(Path(SPECPATH) / "worker_dispatch.py")],
    pathex=[str(tool), str(model_code)],
    binaries=[],
    datas=collect_data_files("mlx", includes=["lib/*.metallib"]),
    hiddenimports=[
        "backing_vocal_worker",
        "backing_vocal_music",
        "generate_backing_vocals_transformer",
        "model",
    ] + collect_submodules("mlx"),
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=["torch", "torchaudio", "matplotlib", "notebook", "IPython", "tkinter"],
    noarchive=False,
)
pyz = PYZ(a.pure)
exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name="backing-notes-worker",
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
    name="backing-notes",
)
