"""Baseline note-detector training package."""

from .dataset import NoteDetectorDataset
from .model import NoteDetectorModel, NoteDetectorLoss

__all__ = [
    "NoteDetectorDataset",
    "NoteDetectorModel",
    "NoteDetectorLoss",
]
