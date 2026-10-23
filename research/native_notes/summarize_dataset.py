"""Validate and export a portable dataset inventory after/resuming local generation."""
from __future__ import annotations

import argparse
import collections
import importlib.metadata
import json
from pathlib import Path

from .augmentation import read_prepared
from .corrections import digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--allow-partial", action="store_true")
    args = parser.parse_args()
    root = args.dataset.resolve()
    plan = json.loads((root / "plan.json").read_text())
    results, missing = [], []
    for job in plan["jobs"]:
        path = root / "synthetic" / job["profile"] / job["id"] / "result.json"
        if path.exists(): results.append(json.loads(path.read_text()))
        else: missing.append(job["id"])
    errors = [r for r in results if r["status"] == "error"]
    complete = not missing and not errors
    if not complete and not args.allow_partial: raise ValueError(f"Still pending: {len(missing)} jobs, {len(errors)} errors")
    rows = read_prepared(root / "prepared.json")
    originals = [r for r in rows if "augmentation" not in r]
    synthetic = [r for r in rows if "augmentation" in r]
    buckets = {}
    for split in ("train", "validation", "test"):
        group = [r for r in rows if r["split"] == split]
        buckets[split] = {"clips": len(group), "seconds": sum(r["seconds"] for r in group),
            "notes": sum(len(r["notes"]) for r in group), "synthetic_clips": sum("augmentation" in r for r in group),
            "supervised_seconds": sum(r["seconds"] - sum(b-a for a,b in r.get("ignore_intervals", [])) for r in group)}
    profiles = {}
    for profile in plan["profiles"]:
        group = [r for r in synthetic if r["augmentation"]["profile"] == profile]
        profiles[profile] = {"clips": len(group), "seconds": sum(r["seconds"] for r in group),
            "notes": sum(len(r["notes"]) for r in group),
            "supervised_seconds": sum(r["seconds"] - sum(b-a for a,b in r.get("ignore_intervals", [])) for r in group)}
    summary = {"schema": plan["version"], "complete": complete, "original_clips": len(originals),
        "synthetic_clips": len(synthetic), "planned_derivatives": len(plan["jobs"]),
        "quarantined_clips": sum(r["status"]=="quarantined" for r in results), "missing_jobs": len(missing),
        "errors": len(errors), "splits": buckets, "profiles": profiles,
        "prepared_manifest_sha256": digest(root / "prepared.json"),
        "base_manifest_sha256": digest(root / "base-originals.json"),
        "generation_plan_sha256": digest(root / "plan.json"),
        "source_performances_not_new_recordings": True, "synthetic_labels_not_human_reviewed": True,
        "boundary_model_used_to_label": False, "validation_and_test_used_for_generation": False,
        "versions": {package: importlib.metadata.version(package) for package in
                     ("torch", "transformers", "librosa", "numpy", "scipy", "soundfile")}}
    control_path = root / "provenance/long-alignment-control.json"
    control_text = ""
    if control_path.exists():
        control = json.loads(control_path.read_text())
        summary["known_time_warp_control"] = {k:v for k,v in control.items() if k!="errors_ms"}
        control_text = (f"На отдельном контроле длиной {control['source_seconds']:.2f} с с известным сдвигом до 80 мс "
                        f"MAE переноса {control['before_mae_ms']:.2f} → {control['after_mae_ms']:.2f} мс, "
                        f"p95 {control['p95_ms']:.2f} мс. Это оценка положительного контроля, "
                        "а не ручной точности меток Seed-VC синтетики.")
    (root / "dataset-summary.json").write_text(json.dumps(summary, indent=2))
    with (root / "metadata.jsonl").open("w") as stream:
        for record in rows:
            stream.write(json.dumps({"file_name": str(Path(record["audio"]).relative_to(root)),
                "annotation": str(Path(record["annotation"]).relative_to(root)), "split": record["split"],
                "id": record["id"], "seconds": record["seconds"], "sample_rate": record["sample_rate"],
                "label_origin": "automatic_alignment" if "augmentation" in record else "human_original",
                "source_id": record.get("augmentation", {}).get("parent_id", record["id"]),
                "audio_sha256": record["audio_sha256"], "notes": [{"id": n["id"], "start": n["start"], "end": n["end"]} for n in record["notes"]],
                "ignore_intervals": record.get("ignore_intervals", [])}) + "\n")
    with (root / "quarantine.jsonl").open("w") as stream:
        for result in results:
            if result["status"] != "accepted":
                stream.write(json.dumps({k:v for k,v in result.items() if k!="record"}) + "\n")
    table = ["| Часть | Клипов | Аудио, мин | Из них синтетических |", "| --- | ---: | ---: | ---: |"]
    for name, value in buckets.items(): table.append(f"| {name} | {value['clips']} | {value['seconds']/60:.2f} | {value['synthetic_clips']} |")
    text = f'''# Sasha: ручная разметка и выровненная синтетика

Статус: **{'готов' if complete else 'генерация продолжается, это промежуточный снимок'}**.

{len(originals)} замороженных оригиналов, {len(synthetic)} принятых производных записей.
Для обучения: {buckets['train']['seconds']/3600:.2f} ч аудио, из них
{buckets['train']['supervised_seconds']/3600:.2f} ч вне масок неуверенной разметки.
Это расширение тех же 53 обучающих исполнений; количество независимых
человеческих записей от синтеза не увеличилось. Автоматические метки
не объявлены проверенными человеком.

{chr(10).join(table)}

## Содержимое

- `originals/` — побайтовые копии исходных WAV и JSON с прежними ролями.
- `synthetic/` — WAV/JSON, карты времени и отчёт качества каждого варианта.
- `prepared.json` — проверяемый вход для `research.native_notes.train`.
- `metadata.jsonl` — переносимый общий индекс с относительными путями и границами.
- `quarantine.jsonl` — не включённые в обучение результаты.
- `plan.json` — все задания, seeds и SHA-256 весов моделей.
- `references.json`, `references/` — 6 акустических референсов из train;
  это не утверждение о шести разных певцах.
- `provenance/` — снимки кода, которым созданы варианты.
- `cache/`, `work/` — промежуточные данные; в `prepared.json` они не включены.

Четыре режима Seed-VC: низкий, средний и высокий регистр, а также
замена тембра в исходном регистре. Дополнительно два варианта обычного питча.
Число нот и порядок исходной ручной разметки сохраняются. Промежуточные
признаки Whisper, RMVPE и уровень сигнала используются для монотонного
выравнивания времени. Проверяемая модель границ не участвует в переносе.

Слабое совпадение содержания, большой сдвиг, расхождение признаков,
потеря озвученности и изменение контура высоты вызывают пометки.
Ненадёжные ноты с запасом 80 мс входят в `ignore_intervals`: loss всех трёх
голов на этих кадрах отключается. Для принятия требуется не менее 70%
надёжных нот и 70% доступного времени. Критерии эвристические, поэтому
нужна выборочная ручная проверка. Остальные варианты остаются на диске
для исследования, но в обучающий манифест не попадают.

{control_text}

Для прослушивания одного исполнения во всех режимах:
`preview/comparison.wav`, порядок отрезков в `preview/playlist.json`.

vox_243 — только validation; исходники validation/test не применялись
ни для синтеза, ни как голосовые референсы. Прежние роли сохранены;
связанные фрагменты в старых ролях возможны, поэтому результаты будущей
оценки нельзя автоматически считать обобщением на независимые песни.

## Использование

Датасет переносим целиком: пути внутри манифестов относительные.
Каждая `clip.annotation.json` с соседним `clip.wav` открывается в JUCE
Vocal Note Annotator. Оригинальная папка `notes/` не менялась.

При дообучении использовать подготовленный манифест и маски, а не читать
все JSON как проверенные человеком. Рекомендуемый начальный вес выборки
синтетических окон — 0.25, чтобы множество копий не вытеснило оригиналы.

```sh
research/.venv_seed_vc/bin/python -m research.native_notes.train \\
  --prepared-manifest "{root}/prepared.json" \\
  --output data/native_note_boundary_augmented_experiment \\
  --resume data/native_note_boundary_v7/selected/checkpoint.pt \\
  --name "Sasha notes — augmented experiment" --model-id sasha-notes-augmented \\
  --voicing --synthetic-weight 0.25 --learning-rate-scale 0.25 \\
  --native-runner build-native-note/tools/native_note_annotator/NativeNoteAnnotatorTests_artefacts/Release/NativeNoteAnnotatorTests \\
  --epochs 100 --skip-test --save-epoch-checkpoints
```

Это пример будущего запуска: сборка датасета не запускает дообучение
и не переключает рабочую v7. C++/JUCE, audio thread, параметры и форматы
модели не менялись. Всё выполнено локально на существующих зависимостях.
'''
    (root / "README.md").write_text(text)
    print(json.dumps(summary, indent=2))


if __name__ == "__main__": main()
