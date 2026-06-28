from __future__ import annotations

from dataclasses import dataclass
from typing import Any


EVENT_NAMES = ("phoneme", "syllable", "breath", "silence")
SPECIAL_BREATH = "<AP>"
SPECIAL_SILENCE = "<SP>"

_ARPABET_VOWELS = {
    "AA",
    "AE",
    "AH",
    "AO",
    "AW",
    "AY",
    "EH",
    "ER",
    "EY",
    "IH",
    "IY",
    "OW",
    "OY",
    "UH",
    "UW",
}
_VOWEL_SYMBOLS = frozenset("aeiouɑɐɒæəɚɛɜɞɘɤɪɨɔɵœøɶʊʉʌɯy")


@dataclass(frozen=True)
class DerivedLabels:
    events: dict[str, list[float]]
    intervals: dict[str, list[list[float]]]
    counts: dict[str, int]


def is_vowel_phone(phone: str, language: str | None = None) -> bool:
    """Recognize vowel nuclei in the ARPABET, IPA, and romanized inventories."""
    normalized = phone.strip()
    if normalized.upper() in {SPECIAL_BREATH, SPECIAL_SILENCE}:
        return False
    arpabet_base = normalized.upper().rstrip("0123456789")
    if language == "English":
        return arpabet_base in _ARPABET_VOWELS
    if arpabet_base in _ARPABET_VOWELS:
        return True
    if normalized.isascii() and normalized.isupper():
        return False
    return any(character.lower() in _VOWEL_SYMBOLS for character in normalized)


def _as_float_list(value: Any, field_name: str) -> list[float]:
    if not isinstance(value, list):
        raise ValueError(f"{field_name} must be a list")
    try:
        return [float(item) for item in value]
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{field_name} contains a non-numeric value") from exc


def _deduplicate_sorted(values: list[float], epsilon: float = 1.0e-5) -> list[float]:
    result: list[float] = []
    for value in sorted(values):
        if not result or abs(value - result[-1]) > epsilon:
            result.append(round(value, 6))
    return result


def derive_labels(
    annotation: Any,
    duration_seconds: float,
    language: str | None = None,
) -> DerivedLabels:
    if not isinstance(annotation, list):
        raise ValueError("annotation root must be a list")

    events = {name: [] for name in EVENT_NAMES}
    intervals = {"breath": [], "silence": []}

    for word_index, word in enumerate(annotation):
        if not isinstance(word, dict):
            raise ValueError(f"word {word_index} must be an object")

        token = str(word.get("word", "")).strip()
        try:
            start = float(word["start_time"])
            end = float(word["end_time"])
        except (KeyError, TypeError, ValueError) as exc:
            raise ValueError(f"word {word_index} has invalid timing") from exc

        if start < 0.0 or end < start or end > duration_seconds + 0.05:
            raise ValueError(
                f"word {word_index} timing [{start}, {end}] is outside audio duration "
                f"{duration_seconds}"
            )

        special = token.upper()
        if special in {SPECIAL_BREATH, SPECIAL_SILENCE}:
            label = "breath" if special == SPECIAL_BREATH else "silence"
            events[label].append(start)
            intervals[label].append([round(start, 6), round(end, 6)])
            continue

        phones = word.get("ph")
        if not isinstance(phones, list):
            raise ValueError(f"word {word_index} ph must be a list")
        starts = _as_float_list(word.get("ph_start"), f"word {word_index} ph_start")
        ends = _as_float_list(word.get("ph_end"), f"word {word_index} ph_end")
        if not (len(phones) == len(starts) == len(ends)):
            raise ValueError(f"word {word_index} phone arrays have different lengths")

        for phone_index, (phone, phone_start, phone_end) in enumerate(
            zip(phones, starts, ends, strict=True)
        ):
            phone_text = str(phone).strip()
            if (
                phone_start < start - 0.05
                or phone_end < phone_start
                or phone_end > end + 0.05
            ):
                raise ValueError(
                    f"word {word_index} phone {phone_index} has invalid timing"
                )
            if phone_text.upper() in {SPECIAL_BREATH, SPECIAL_SILENCE}:
                continue
            events["phoneme"].append(phone_start)
            if is_vowel_phone(phone_text, language):
                events["syllable"].append(phone_start)

    normalized_events = {
        name: _deduplicate_sorted(values) for name, values in events.items()
    }
    return DerivedLabels(
        events=normalized_events,
        intervals=intervals,
        counts={name: len(values) for name, values in normalized_events.items()},
    )
