#!/usr/bin/env python3
"""
world_bridge.py — WORLD + Vocos synthesis bridge for Synthetic Obsidian.

Pipeline:
  1. WORLD analysis  — harvest F0, cheaptrick spectral envelope, d4c aperiodicity
  2. F0 replacement  — contour convention: >0 = correct to Hz, ==0 = keep original
  3. WORLD synthesis — intermediate audio with corrected pitch
  4. Vocos vocoder   — neural synthesis from mel, eliminates WORLD phase artifacts
                       (falls back to plain WORLD output if Vocos unavailable)

Startup sequence:
  1. Script binds a TCP server socket on 127.0.0.1:0 (OS assigns free port).
  2. Prints "PORT:<n>\n" to stdout so C++ can read the port number.
  3. Accepts exactly one connection from the C++ parent.
  4. Processes frames in a loop until the connection is closed.

Binary protocol (little-endian, same for both directions):
──────────────────────────────────────────────────────────────────────────
C++ → socket  (one REQUEST frame):
  [8]  double   sample_rate
  [4]  int32    num_samples
  [4]  int32    num_f0_frames
  [4*num_samples]  float32  audio[]      (mono, normalised -1..1)
  [8*num_f0_frames] float64 f0_target[]  (Hz; 0 = keep original pitch)

Python → socket  (one RESPONSE frame):
  [4]  int32    status           (0=ok, 1=pyworld unavailable, 2=error)
  [4]  int32    out_samples      (0 if status != 0)
  [4*out_samples]  float32  audio[]

Failure modes:
  • pyworld not installed → status=1, out_samples=0, then close socket.
  • synthesis exception   → status=2, out_samples=0 (loop continues).
  • segment < 50ms        → status=2, out_samples=0.
──────────────────────────────────────────────────────────────────────────
"""

import sys
import struct
import os
import socket as _socket

# ── Redirect stdout AFTER printing the port so imports don't corrupt it ───────
_real_stderr = sys.stderr


def _eprint(*args):
    print(*args, file=_real_stderr)


import numpy as np
import librosa

try:
    import pyworld as pw
    PYWORLD_AVAILABLE = True
except ImportError:
    PYWORLD_AVAILABLE = False
    _eprint("[world_bridge] pyworld not available")

# ── Vocos neural vocoder (24 kHz) ─────────────────────────────────────────────
# Vocos re-synthesises audio from a mel spectrogram produced by WORLD.
# This eliminates WORLD's phase incoherence and harmonic distortion.
VOCOS_SR = 24_000
VOCOS_AVAILABLE = False
_vocos_model = None

try:
    import torch
    import vocos as _vocos_lib
    _vocos_model = _vocos_lib.Vocos.from_pretrained("charactr/vocos-mel-24khz")
    _vocos_model.eval()
    VOCOS_AVAILABLE = True
    _eprint("[world_bridge] Vocos loaded (24 kHz)")
except Exception as _e:
    _eprint(f"[world_bridge] Vocos unavailable: {_e}")


# ── Socket I/O helpers ────────────────────────────────────────────────────────

def _recv_exact(conn: _socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("socket closed")
        buf += chunk
    return bytes(buf)


def _send_error(conn: _socket.socket, status: int) -> None:
    conn.sendall(struct.pack("<ii", status, 0))


def _send_audio(conn: _socket.socket, audio: np.ndarray) -> None:
    out = audio.astype(np.float32)
    conn.sendall(struct.pack("<ii", 0, len(out)))
    conn.sendall(out.tobytes())


# ── WORLD analysis helpers ────────────────────────────────────────────────────

# WORLD frame period in ms — 5 ms gives a good trade-off between resolution
# and processing time for typical vocal material.
FRAME_PERIOD_MS = 5.0


def _interp_f0(f0_target_in: np.ndarray, num_frames_world: int) -> np.ndarray:
    """Linearly interpolate the incoming F0 contour to WORLD's frame count."""
    n_in = len(f0_target_in)
    if n_in == num_frames_world:
        return f0_target_in.copy()
    x_in  = np.linspace(0.0, 1.0, n_in)
    x_out = np.linspace(0.0, 1.0, num_frames_world)
    return np.interp(x_out, x_in, f0_target_in)


# ── Vocos enhancement ─────────────────────────────────────────────────────────

def _vocos_enhance(audio_f32: np.ndarray, sr: float) -> np.ndarray:
    """
    Improve audio quality using Vocos neural vocoder.

    Steps:
      1. Resample to 24 kHz using torchaudio (near-instant, high quality).
      2. Extract mel spectrogram via Vocos feature extractor.
      3. Vocos inference → clean audio.
      4. Resample back to original sample rate.
    """
    import torch
    import torchaudio

    orig_sr = int(sr)
    audio_t = torch.from_numpy(audio_f32).unsqueeze(0)  # [1, T]

    # Resample to Vocos native rate
    if orig_sr != VOCOS_SR:
        audio_t = torchaudio.functional.resample(audio_t, orig_sr, VOCOS_SR)

    with torch.no_grad():
        features = _vocos_model.feature_extractor(audio_t)
        y_vocos  = _vocos_model.decode(features)  # [1, T']

    # Resample back to original rate
    if orig_sr != VOCOS_SR:
        y_vocos = torchaudio.functional.resample(y_vocos, VOCOS_SR, orig_sr)

    y_out = y_vocos.squeeze(0).numpy()

    # Match length to input
    n = len(audio_f32)
    if len(y_out) >= n:
        return y_out[:n].astype(np.float32)
    return np.pad(y_out, (0, n - len(y_out))).astype(np.float32)


# ── Main synthesis pipeline ───────────────────────────────────────────────────

def _resynthesize(audio: np.ndarray, sr: float,
                  f0_target: np.ndarray) -> np.ndarray:
    """
    WORLD analysis → F0 replacement → WORLD synthesis → Vocos enhancement.

    F0 contour convention (from C++):
      > 0   correct this frame to the given Hz
      == 0  keep the original harvest F0 (gap / don't-correct region)

    With this convention, linear interpolation near note boundaries naturally
    blends toward the original pitch — no explicit crossfade needed in C++.
    """
    x = audio.astype(np.float64)

    # ── WORLD analysis ────────────────────────────────────────────────────────
    f0, t = pw.harvest(x, sr, frame_period=FRAME_PERIOD_MS)
    sp    = pw.cheaptrick(x, f0, t, sr)
    ap    = pw.d4c(x, f0, t, sr)

    # ── F0 replacement ────────────────────────────────────────────────────────
    f0_new = _interp_f0(f0_target, len(f0))

    # Where contour == 0, keep original detected F0 (gap / unvoiced region).
    voiced = f0 > 0
    f0_new = np.where(f0_new > 0.0, f0_new, f0)
    f0_new = np.where(voiced, f0_new, 0.0)   # restore silence/breath
    f0_new = np.clip(f0_new, 0.0, 1760.0)
    f0_new[f0_new > 0] = np.clip(f0_new[f0_new > 0], 55.0, 1760.0)

    # ── WORLD synthesis ───────────────────────────────────────────────────────
    y = pw.synthesize(f0_new, sp, ap, sr, frame_period=FRAME_PERIOD_MS)

    # Match length to input
    n = len(x)
    if len(y) >= n:
        y = y[:n]
    else:
        y = np.pad(y, (0, n - len(y)))

    y = y.astype(np.float32)

    # ── Vocos enhancement — remove WORLD phase artifacts ─────────────────────
    if VOCOS_AVAILABLE:
        try:
            y = _vocos_enhance(y, sr)
        except Exception as exc:
            _eprint(f"[world_bridge] Vocos enhancement failed, using WORLD output: {exc}")

    return y


# ── Main loop ─────────────────────────────────────────────────────────────────

def main() -> None:
    # ── Start TCP server, announce port to parent via stdout ──────────────────
    srv = _socket.socket(_socket.AF_INET, _socket.SOCK_STREAM)
    srv.setsockopt(_socket.SOL_SOCKET, _socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    port = srv.getsockname()[1]
    srv.listen(1)

    # Print port BEFORE redirecting stdout so C++ can read it cleanly
    sys.stdout.write(f"PORT:{port}\n")
    sys.stdout.flush()
    # Now silence stdout so library noise doesn't reach C++
    sys.stdout = open(os.devnull, "w")

    if not PYWORLD_AVAILABLE:
        try:
            conn, _ = srv.accept()
            _send_error(conn, 1)
            conn.close()
        except Exception:
            pass
        srv.close()
        sys.exit(0)

    # ── Accept the single C++ connection ─────────────────────────────────────
    conn, _ = srv.accept()
    srv.close()

    while True:
        try:
            hdr = _recv_exact(conn, 8 + 4 + 4)
        except ConnectionError:
            break

        sr, num_samples, num_f0_frames = struct.unpack_from("<dii", hdr)

        try:
            audio_bytes = _recv_exact(conn, num_samples * 4)
            f0_bytes    = _recv_exact(conn, num_f0_frames * 8)
        except ConnectionError:
            break

        audio    = np.frombuffer(audio_bytes, dtype=np.float32).copy()
        f0_input = np.frombuffer(f0_bytes,    dtype=np.float64).copy()

        if num_samples < int(sr * 0.05):  # 50 ms minimum for WORLD
            _send_error(conn, 2)
            continue

        try:
            result = _resynthesize(audio, sr, f0_input)
            _send_audio(conn, result)
        except Exception as exc:
            _eprint(f"[world_bridge] synthesis error: {exc}")
            _send_error(conn, 2)

    conn.close()


if __name__ == "__main__":
    main()
