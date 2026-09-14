from __future__ import annotations

import unittest

import numpy as np
import torch

from voice_lora_training_worker import (
    evaluate_adapter,
    flow_matching_loss,
    learning_rate_scale,
    restore_parameters,
    snapshot_parameters,
    split_feature_cache,
    train_adapter,
)


def feature(frames: int, label: str) -> dict[str, object]:
    values = torch.arange(frames * 2, dtype=torch.float32).reshape(1, frames, 2)
    return {
        "mel": values,
        "condition": values + 0.5,
        "label": label,
    }


class CapturingDecoder:
    sigma = 1e-5

    def __init__(self) -> None:
        self.mel: torch.Tensor | None = None
        self.prompt_mask: torch.Tensor | None = None

    def __call__(
        self,
        mel: torch.Tensor,
        _frame_mask: torch.Tensor,
        _condition: torch.Tensor,
        prompt_mask: torch.Tensor,
    ) -> dict[str, tuple[torch.Tensor, ...]]:
        self.mel = mel
        self.prompt_mask = prompt_mask
        noise = torch.zeros_like(mel)
        final_mask = (1.0 - prompt_mask).unsqueeze(-1)
        return {
            "output": (
                noise,
                mel,
                mel,
                final_mask,
                prompt_mask.sum(dim=1),
            )
        }


class RandomDecoder(torch.nn.Module):
    sigma = 1e-5

    def __init__(self) -> None:
        super().__init__()
        self.cfg_drop_prob = 0.15

    def forward(
        self,
        mel: torch.Tensor,
        _frame_mask: torch.Tensor,
        _condition: torch.Tensor,
        prompt_mask: torch.Tensor,
    ) -> dict[str, tuple[torch.Tensor, ...]]:
        noise = torch.randn_like(mel)
        prediction = torch.zeros_like(mel)
        final_mask = (1.0 - prompt_mask).unsqueeze(-1)
        return {
            "output": (
                noise,
                mel,
                prediction,
                final_mask,
                prompt_mask.sum(dim=1),
            )
        }


class TinyDiffusionModel(torch.nn.Module):
    sigma = 1e-5

    def __init__(self) -> None:
        super().__init__()
        layer = torch.nn.Module()
        layer.q_proj = torch.nn.Linear(2, 2, bias=False)
        torch.nn.init.zeros_(layer.q_proj.weight)
        self.diff_estimator = torch.nn.Module()
        self.diff_estimator.layers = torch.nn.ModuleList([layer])
        self.cfg_drop_prob = 0.15

    def forward(
        self,
        mel: torch.Tensor,
        _frame_mask: torch.Tensor,
        _condition: torch.Tensor,
        prompt_mask: torch.Tensor,
    ) -> dict[str, tuple[torch.Tensor, ...]]:
        prediction = self.diff_estimator.layers[0].q_proj(mel)
        noise = torch.zeros_like(mel)
        final_mask = (1.0 - prompt_mask).unsqueeze(-1)
        return {
            "output": (
                noise,
                mel,
                prediction,
                final_mask,
                prompt_mask.sum(dim=1),
            )
        }


class TinySoulXModel(torch.nn.Module):
    def __init__(self) -> None:
        super().__init__()
        self.cfm_decoder = torch.nn.Module()
        self.cfm_decoder.model = TinyDiffusionModel()


class RecordingReporter:
    def __init__(self) -> None:
        self.messages: list[str] = []

    def check_cancelled(self) -> None:
        return

    def log(self, message: str) -> None:
        self.messages.append(message)

    def update(self, **_values: object) -> None:
        return


class LoRATrainingTests(unittest.TestCase):
    def test_training_layout_matches_single_prefix_prompt_inference(self) -> None:
        decoder = CapturingDecoder()

        flow_matching_loss(
            decoder,
            feature(3, "prompt"),
            feature(5, "target"),
            torch.device("cpu"),
        )

        self.assertEqual(tuple(decoder.mel.shape), (1, 8, 2))
        np.testing.assert_array_equal(
            decoder.prompt_mask.numpy(),
            np.asarray([[1, 1, 1, 0, 0, 0, 0, 0]], dtype=np.float32),
        )

    def test_validation_split_is_deterministic_and_disjoint(self) -> None:
        features = [feature(2, f"segment-{index}") for index in range(10)]

        first_train, first_validation = split_feature_cache(
            features,
            validation_fraction=0.15,
            seed=1337,
        )
        second_train, second_validation = split_feature_cache(
            features,
            validation_fraction=0.15,
            seed=1337,
        )

        self.assertEqual(len(first_train), 8)
        self.assertEqual(len(first_validation), 2)
        self.assertEqual(
            [item["label"] for item in first_train],
            [item["label"] for item in second_train],
        )
        self.assertEqual(
            [item["label"] for item in first_validation],
            [item["label"] for item in second_validation],
        )
        self.assertFalse(set(map(id, first_train)) & set(map(id, first_validation)))

    def test_learning_rate_has_warmup_and_cosine_decay(self) -> None:
        self.assertAlmostEqual(learning_rate_scale(1, 100), 0.2)
        self.assertAlmostEqual(learning_rate_scale(5, 100), 1.0)
        self.assertGreater(
            learning_rate_scale(50, 100),
            learning_rate_scale(90, 100),
        )
        self.assertAlmostEqual(learning_rate_scale(100, 100), 0.1)

    def test_validation_uses_repeatable_diffusion_noise(self) -> None:
        decoder = RandomDecoder()
        features = [feature(4, "one"), feature(4, "two")]

        first = evaluate_adapter(
            decoder,
            features,
            device=torch.device("cpu"),
        )
        second = evaluate_adapter(
            decoder,
            features,
            device=torch.device("cpu"),
        )

        self.assertAlmostEqual(first, second)
        self.assertTrue(decoder.training)
        self.assertAlmostEqual(decoder.cfg_drop_prob, 0.15)

    def test_best_parameter_snapshot_can_be_restored(self) -> None:
        parameter = torch.nn.Parameter(torch.tensor([1.0, 2.0]))
        saved = snapshot_parameters([parameter])
        with torch.no_grad():
            parameter.add_(10.0)

        restore_parameters([parameter], saved)

        np.testing.assert_array_equal(
            parameter.detach().numpy(),
            np.asarray([1.0, 2.0], dtype=np.float32),
        )

    def test_training_restores_a_validation_selected_checkpoint(self) -> None:
        model = TinySoulXModel()
        reporter = RecordingReporter()
        request = {
            "training": {
                "steps": 2,
                "learningRate": 0.001,
                "gradientAccumulation": 1,
                "cfgDropProbability": 0.15,
                "validationFraction": 0.5,
                "validationInterval": 10,
                "seed": 1337,
                "lora": {
                    "rank": 1,
                    "alpha": 1.0,
                    "dropout": 0.0,
                    "target_suffixes": ["q_proj"],
                    "module_prefix": "cfm_decoder.model.diff_estimator.layers.",
                },
            }
        }

        _, training_loss, validation_loss = train_adapter(
            model,
            [feature(3, f"segment-{index}") for index in range(4)],
            request,
            device=torch.device("cpu"),
            reporter=reporter,
        )

        self.assertTrue(np.isfinite(training_loss))
        self.assertIsNotNone(validation_loss)
        self.assertTrue(
            any("validation loss" in message for message in reporter.messages)
        )


if __name__ == "__main__":
    unittest.main()
