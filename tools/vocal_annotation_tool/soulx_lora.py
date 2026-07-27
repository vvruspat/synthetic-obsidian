#!/usr/bin/env python3
"""Small dependency-free LoRA adapter helpers for SoulX-Singer-SVC."""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Iterable

import torch
from safetensors.torch import load_file, save_file
from torch import nn
from torch.nn import functional as F


@dataclass(frozen=True)
class LoRAConfig:
    rank: int = 8
    alpha: float = 8.0
    dropout: float = 0.05
    target_suffixes: tuple[str, ...] = ("q_proj", "v_proj")
    module_prefix: str = "cfm_decoder.model.diff_estimator.layers."

    @classmethod
    def from_dict(cls, value: dict[str, Any]) -> "LoRAConfig":
        return cls(
            rank=int(value.get("rank", 8)),
            alpha=float(value.get("alpha", value.get("rank", 8))),
            dropout=float(value.get("dropout", 0.0)),
            target_suffixes=tuple(value.get("target_suffixes", ("q_proj", "v_proj"))),
            module_prefix=str(
                value.get(
                    "module_prefix",
                    "cfm_decoder.model.diff_estimator.layers.",
                )
            ),
        )


class LoRALinear(nn.Module):
    """Frozen base linear layer plus a trainable low-rank residual."""

    def __init__(
        self,
        base_layer: nn.Linear,
        *,
        rank: int,
        alpha: float,
        dropout: float,
    ) -> None:
        super().__init__()
        if rank <= 0:
            raise ValueError("LoRA rank must be positive")

        self.base_layer = base_layer
        self.rank = rank
        self.alpha = alpha
        self.scale = alpha / rank
        self.dropout = nn.Dropout(dropout) if dropout > 0.0 else nn.Identity()
        self.lora_a = nn.Parameter(
            torch.empty(
                rank,
                base_layer.in_features,
                device=base_layer.weight.device,
                dtype=base_layer.weight.dtype,
            )
        )
        self.lora_b = nn.Parameter(
            torch.zeros(
                base_layer.out_features,
                rank,
                device=base_layer.weight.device,
                dtype=base_layer.weight.dtype,
            )
        )
        nn.init.kaiming_uniform_(self.lora_a, a=5**0.5)
        for parameter in self.base_layer.parameters():
            parameter.requires_grad_(False)

    def forward(self, inputs: torch.Tensor) -> torch.Tensor:
        base = self.base_layer(inputs)
        update = F.linear(F.linear(self.dropout(inputs), self.lora_a), self.lora_b)
        return base + update * self.scale


def _parent_and_name(model: nn.Module, qualified_name: str) -> tuple[nn.Module, str]:
    parts = qualified_name.split(".")
    parent = model
    for part in parts[:-1]:
        parent = getattr(parent, part)
    return parent, parts[-1]


def inject_lora(model: nn.Module, config: LoRAConfig) -> list[str]:
    """Inject adapters and return stable qualified module names."""
    matched: list[str] = []
    for name, module in list(model.named_modules()):
        if not isinstance(module, nn.Linear):
            continue
        if not name.startswith(config.module_prefix):
            continue
        if not any(name.endswith(f".{suffix}") for suffix in config.target_suffixes):
            continue

        parent, attribute = _parent_and_name(model, name)
        setattr(
            parent,
            attribute,
            LoRALinear(
                module,
                rank=config.rank,
                alpha=config.alpha,
                dropout=config.dropout,
            ),
        )
        matched.append(name)

    if not matched:
        raise RuntimeError("No SoulX attention layers matched the LoRA configuration")
    return matched


def remove_lora(model: nn.Module) -> int:
    """Restore the frozen base layers without changing their weights."""
    removed = 0
    for name, module in list(model.named_modules()):
        if not isinstance(module, LoRALinear):
            continue
        parent, attribute = _parent_and_name(model, name)
        setattr(parent, attribute, module.base_layer)
        removed += 1
    return removed


def trainable_lora_parameters(model: nn.Module) -> Iterable[nn.Parameter]:
    for module in model.modules():
        if isinstance(module, LoRALinear):
            yield module.lora_a
            yield module.lora_b


def adapter_state_dict(model: nn.Module) -> dict[str, torch.Tensor]:
    result: dict[str, torch.Tensor] = {}
    for name, module in model.named_modules():
        if not isinstance(module, LoRALinear):
            continue
        result[f"{name}.lora_a"] = module.lora_a.detach().float().cpu().contiguous()
        result[f"{name}.lora_b"] = module.lora_b.detach().float().cpu().contiguous()
    if not result:
        raise RuntimeError("The model has no LoRA weights to save")
    return result


def save_adapter(
    adapter_path: Path,
    model: nn.Module,
    config: LoRAConfig,
    metadata: dict[str, Any],
) -> None:
    adapter_path.parent.mkdir(parents=True, exist_ok=True)
    save_file(
        adapter_state_dict(model),
        str(adapter_path),
        metadata={
            "format": "synthetic-obsidian-soulx-lora-v1",
            "preset_name": str(metadata.get("presetName", "")),
        },
    )
    config_path = adapter_path.with_suffix(".json")
    payload = {
        "schemaVersion": 1,
        "adapterFile": adapter_path.name,
        "lora": asdict(config),
        **metadata,
    }
    temporary = config_path.with_suffix(config_path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    temporary.replace(config_path)


def load_adapter(
    model: nn.Module,
    adapter_path: Path,
    *,
    device: torch.device | str,
) -> tuple[LoRAConfig, list[str]]:
    config_path = adapter_path.with_suffix(".json")
    payload = json.loads(config_path.read_text(encoding="utf-8"))
    config = LoRAConfig.from_dict(payload["lora"])

    remove_lora(model)
    modules = inject_lora(model, config)
    weights = load_file(str(adapter_path), device="cpu")
    for name in modules:
        module = model.get_submodule(name)
        if not isinstance(module, LoRALinear):
            raise RuntimeError(f"LoRA module was not injected: {name}")
        module.lora_a.data.copy_(weights[f"{name}.lora_a"].to(module.lora_a))
        module.lora_b.data.copy_(weights[f"{name}.lora_b"].to(module.lora_b))
    return config, modules
