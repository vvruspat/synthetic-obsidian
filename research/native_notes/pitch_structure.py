"""Relative F0 centres, edge trends and modulation; native-compatible, offline only."""
import numpy as np
import torch

from . import voicing

SCHEMA = "synthetic-obsidian-note-boundary-tcn-v4"
CONTRACT = "strongest-linear-multiscale-yin-structure-v4"
STRUCTURE_CONTRACT = "native-yin-centres-difference-acf-810ms-v1"
EXTRA_FEATURES = [
    "pitch_center_delta_200ms", "pitch_center_delta_400ms",
    "pitch_center_support_200ms", "pitch_center_support_400ms",
    "pitch_trend_past_200ms", "pitch_trend_future_200ms",
    "pitch_stability_past_200ms", "pitch_stability_future_200ms",
    "pitch_persistence_past_200ms", "pitch_persistence_future_200ms",
    "pitch_modulation_regularity_810ms", "pitch_modulation_frequency_810ms",
    "pitch_modulation_extent_810ms", "pitch_modulation_support_810ms",
]
FEATURES = voicing.FEATURES + len(EXTRA_FEATURES)


def structure_features(analysis_frames, count):
    frames = np.asarray(analysis_frames, dtype=np.float64).reshape(-1, 3)
    if count <= 0 or not 0 <= count - len(frames) <= 1 or not np.isfinite(frames).all():
        raise ValueError("Pitch structure requires the native YIN time grid")
    if np.any((frames[:, 1:] < 0) | (frames[:, 1:] > 1)) or np.any((frames[:, 2] != 0) & (frames[:, 2] != 1)):
        raise ValueError("Invalid native voicing")
    values = np.zeros((count, 3), np.float64)
    values[:len(frames)] = frames
    pitch, _, voiced = values.T
    index = np.arange(count, dtype=np.float64)

    def prefix(x):
        return np.r_[0., np.cumsum(x, dtype=np.float64)]

    def sums(p, left, right):
        return p[np.clip(index.astype(int) + right, 0, count)] - p[np.clip(index.astype(int) + left, 0, count)]

    moments = [prefix(x) for x in (voiced, pitch * voiced, pitch * pitch * voiced,
                                   index * voiced, index * index * voiced, index * pitch * voiced)]

    def stats(left, right):
        n, total, square, time, time2, cross = [sums(p, left, right) for p in moments]
        divisor = np.maximum(n, 1)
        mean = total / divisor
        variance = np.maximum(0, square / divisor - mean * mean)
        time_variance = np.maximum(0, time2 / divisor - (time / divisor) ** 2)
        covariance = cross / divisor - time / divisor * mean
        slope = np.where(time_variance > 1e-6, covariance / np.maximum(time_variance, 1e-6), 0)
        residual = np.sqrt(np.maximum(0, variance - slope * covariance))
        return n, mean, slope, residual

    past, future = stats(-23, -3), stats(3, 23)
    early, late = stats(-43, -23), stats(23, 43)
    wide_past, wide_future = stats(-43, -3), stats(3, 43)
    narrow_valid = (past[0] >= 15) & (future[0] >= 15)
    wide_valid = (wide_past[0] >= 30) & (wide_future[0] >= 30)
    output = np.zeros((len(EXTRA_FEATURES), count), np.float64)
    output[0] = np.where(narrow_valid, np.clip((future[1] - past[1]) / 3, -1, 1), 0)
    output[1] = np.where(wide_valid, np.clip((wide_future[1] - wide_past[1]) / 3, -1, 1), 0)
    output[2] = np.where(narrow_valid, np.sqrt(past[0] * future[0]) / 20, 0)
    output[3] = np.where(wide_valid, np.sqrt(wide_past[0] * wide_future[0]) / 40, 0)
    for destination, side in ((4, past), (5, future)):
        output[destination] = np.where(side[0] >= 15, np.clip(side[2] * 20 / 3, -1, 1), 0)
        output[destination + 2] = np.where(side[0] >= 15, 1 / (1 + (side[3] / .5) ** 2), 0)
    output[8] = np.where((past[0] >= 15) & (early[0] >= 15), np.clip((past[1] - early[1]) / 3, -1, 1), 0)
    output[9] = np.where((future[0] >= 15) & (late[0] >= 15), np.clip((late[1] - future[1]) / 3, -1, 1), 0)

    # Differencing removes a constant edge slope. Exclude octave jumps and gaps;
    # the first supported positive autocorrelation peak avoids subharmonic picks.
    delta = np.r_[0., np.diff(pitch)]
    valid = voiced * np.r_[0., voiced[:-1]] * (np.abs(delta) <= 1.5)
    delta = np.clip(delta, -.5, .5) * valid
    previous2 = np.full(count, -1., np.float64)
    previous = np.zeros(count, np.float64)
    for lag in range(8, 41):
        delayed = np.r_[np.zeros(min(lag, count)), delta[:max(0, count-lag)]]
        delayed_valid = np.r_[np.zeros(min(lag, count)), valid[:max(0, count-lag)]]
        support = valid * delayed_valid
        a, b = delta * support, delayed * support
        left, right = -40 + lag // 2, 41 + lag // 2
        n, sa, sb, aa, bb, ab = [sums(prefix(x), left, right) for x in (support, a, b, a*a, b*b, a*b)]
        divisor = np.maximum(n, 1)
        va, vb = np.maximum(0, aa - sa*sa/divisor), np.maximum(0, bb - sb*sb/divisor)
        correlation = np.where((n >= 53) & (va > 1e-6) & (vb > 1e-6),
                               np.clip((ab-sa*sb/divisor) / np.sqrt(np.maximum(va*vb, 1e-20)), -1, 1), 0)
        if lag > 8:
            chosen = (output[11] == 0) & (previous > .3) & (previous >= previous2) & (previous > correlation)
            output[10, chosen] = previous[chosen]
            output[11, chosen] = 8 / (lag - 1)  # 100 / lag Hz divided by 12.5 Hz.
        previous2, previous = previous, correlation
    chosen = (output[11] == 0) & (previous > .3) & (previous > previous2)
    output[10, chosen] = previous[chosen]
    output[11, chosen] = 8 / 40
    local = stats(-40, 41)
    output[12] = np.where(local[0] >= 53, np.clip(local[3] / 1.5, 0, 1), 0)
    output[13] = sums(prefix(valid), -40, 41) / 81
    return output.astype(np.float32)


class PitchStructureNoteBoundaryNet(voicing.VoicingNoteBoundaryNet):
    def __init__(self):
        super().__init__(FEATURES)

    def initialize_voicing(self, state):
        extended = dict(state)
        weight = torch.zeros_like(self.input_projection[0].weight)
        weight[:, :voicing.FEATURES] = state["input_projection.0.weight"]
        extended["input_projection.0.weight"] = weight
        self.load_state_dict(extended)


def train_new_inputs_only(model):
    """Keep the preferred model exact; optimizer must use zero weight decay."""
    for parameter in model.parameters(): parameter.requires_grad_(False)
    weight = model.input_projection[0].weight
    weight.requires_grad_(True)

    def mask_old_inputs(gradient):
        gradient = gradient.clone()
        gradient[:, :voicing.FEATURES] = 0
        return gradient

    weight.register_hook(mask_old_inputs)
    return [weight]


def feature_metadata():
    metadata = voicing.feature_metadata()
    metadata.update(schema=SCHEMA, feature_contract=CONTRACT, input_features=FEATURES,
                    pitch_structure_contract=STRUCTURE_CONTRACT,
                    extra_features=metadata["extra_features"] + EXTRA_FEATURES)
    return metadata
