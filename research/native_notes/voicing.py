"""Relative pitch/periodicity inputs, using the editor's existing native YIN."""
import numpy as np
import torch

from .multiscale import MultiScaleNoteBoundaryNet, FEATURES as BASE_FEATURES

SCHEMA = "synthetic-obsidian-note-boundary-tcn-v3"
CONTRACT = "strongest-linear-multiscale-yin-relative-v3"
YIN_CONTRACT = "native-yin-8k-40ms-hop10ms-v1"
EXTRA_FEATURES = ["yin_confidence", "yin_voiced", "voiced_context_210ms",
                  "relative_pitch_810ms", "pitch_delta_100ms", "pitch_delta_support",
                  "pitch_spread_210ms"]
FEATURES = BASE_FEATURES + len(EXTRA_FEATURES)


def voicing_features(analysis_frames, count):
    """Frames are [MIDI estimate, confidence, voiced], at a 10-ms hop.

    No absolute pitch is passed to the network. Gaps stay gaps; unsupported pitch
    differences are zero, with a separate support channel distinguishing them.
    """
    frames = np.asarray(analysis_frames, dtype=np.float64).reshape(-1, 3)
    if not 0 <= count - len(frames) <= 1 or not np.isfinite(frames).all():
        raise ValueError("Native YIN frames differ from the feature time grid")
    if np.any((frames[:, 1:] < 0) | (frames[:, 1:] > 1)) or np.any((frames[:, 2] != 0) & (frames[:, 2] != 1)):
        raise ValueError("Invalid native YIN confidence/voicing")
    padded = np.zeros((count, 3), np.float64)
    padded[:len(frames)] = frames
    pitch, confidence, voiced = padded.T
    index = np.arange(count)

    def window(values, left, right):
        prefix = np.r_[0.0, np.cumsum(values, dtype=np.float64)]
        return prefix[np.clip(index + right, 0, count)] - prefix[np.clip(index + left, 0, count)]

    def stats(left, right):
        support = window(voiced, left, right)
        mean = window(pitch * voiced, left, right) / np.maximum(support, 1)
        square = window(pitch * pitch * voiced, left, right) / np.maximum(support, 1)
        return support, mean, np.sqrt(np.maximum(0, square - mean * mean))

    local_count, _, local_spread = stats(-10, 11)
    _, local_pitch, _ = stats(-40, 41)
    past_count, past_pitch, _ = stats(-13, -3)
    future_count, future_pitch, _ = stats(3, 13)
    supported = (past_count >= 5) & (future_count >= 5)
    relative = np.where(voiced > 0, np.clip((pitch - local_pitch) / 6.0, -1, 1), 0)
    delta = np.where(supported, np.clip((future_pitch - past_pitch) / 6.0, -1, 1), 0)
    delta_support = np.where(supported, np.sqrt(past_count * future_count) / 10.0, 0)
    spread = np.where(local_count >= 2, np.clip(local_spread / 3.0, 0, 1), 0)
    return np.stack([confidence, voiced, local_count / 21.0, relative, delta, delta_support, spread]).astype(np.float32)


class VoicingNoteBoundaryNet(MultiScaleNoteBoundaryNet):
    def __init__(self, input_features=FEATURES):
        super().__init__(input_features)

    def initialize_multiscale(self, state):
        # New cues initially contribute zero: predictions exactly preserve v5.
        extended = dict(state)
        weight = torch.zeros_like(self.input_projection[0].weight)
        weight[:, :BASE_FEATURES] = state["input_projection.0.weight"]
        extended["input_projection.0.weight"] = weight
        self.load_state_dict(extended)


def feature_metadata():
    from .multiscale import feature_metadata as base_metadata
    metadata = base_metadata()
    metadata.update(schema=SCHEMA, feature_contract=CONTRACT, input_features=FEATURES,
                    yin_contract=YIN_CONTRACT, extra_features=metadata["extra_features"] + EXTRA_FEATURES)
    return metadata
