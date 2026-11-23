#!/usr/bin/env python3
"""Create FLUXNET *_pre.csv files using the project's supplied preprocessing rules.

Input layout: data/fluxnet_data/<site-directory>/*.csv
Output layout: each source file's directory, with the suffix _pre.csv.
Only Python's standard library is required.
"""
from __future__ import annotations

import argparse
import csv
import math
import re
from datetime import datetime
from pathlib import Path

EPOCH = datetime(1970, 1, 1)
MISSING_TEXT = {"", "NA", "NaN", "nan"}


def parse_number(text: str) -> tuple[bool, float]:
    value = "".join(ch for ch in text if not ch.isspace())
    if value in MISSING_TEXT:
        return False, -9999.0
    try:
        return True, float(value)
    except ValueError:
        return False, -9999.0


def timestamp_minutes(text: str) -> int:
    digits = re.sub(r"\D", "", text)
    try:
        if len(digits) == 12:
            dt = datetime(int(digits[:4]), int(digits[4:6]), int(digits[6:8]),
                          int(digits[8:10]), int(digits[10:12]))
        elif len(digits) == 8:
            dt = datetime(int(digits[:4]), int(digits[4:6]), int(digits[6:8]))
        elif len(digits) == 6:
            dt = datetime(int(digits[:4]), int(digits[4:6]), 1)
        elif len(digits) == 4:
            dt = datetime(int(digits[:4]), 1, 1)
        else:
            return 0
        return int((dt - EPOCH).total_seconds() // 60)
    except ValueError:
        return 0


def fmt10(value: float) -> str:
    return format(value, ".10g")


def process_file(path: Path, overwrite: bool) -> tuple[bool, str]:
    if path.suffix.lower() != ".csv" or "_pre" in path.stem:
        return False, "skip non-raw/preprocessed file"
    output = path.with_name(path.stem + "_pre.csv")
    if output.exists() and not overwrite:
        return False, "output already exists"

    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        reader = csv.reader(stream)
        try:
            header = next(reader)
        except StopIteration:
            return False, "empty file"
        try:
            ts_index = header.index("TIMESTAMP")
        except ValueError:
            return False, "TIMESTAMP column not found"

        rows: list[tuple[int, int, str, list[float], list[bool]]] = []
        sequence = 0
        for fields in reader:
            if not fields or not any(field.strip() for field in fields):
                continue
            if len(fields) < len(header):
                fields = fields + [""] * (len(header) - len(fields))
            elif len(fields) > len(header):
                fields = fields[:len(header)]

            ts_text = fields[ts_index]
            numeric = [0.0] * len(header)
            valid = [False] * len(header)
            any_numeric = False
            for col, field in enumerate(fields):
                if col == ts_index:
                    continue
                ok, value = parse_number(field)
                if ok:
                    numeric[col] = value
                    valid[col] = True
                    any_numeric = True
                else:
                    numeric[col] = -9999.0
            if not any_numeric:
                continue
            rows.append((timestamp_minutes(ts_text), sequence, ts_text, numeric, valid))
            sequence += 1

    if not rows:
        with output.open("w", encoding="utf-8", newline="") as stream:
            csv.writer(stream, lineterminator="\n").writerow(header)
        return True, str(output)

    # Stable timestamp ordering; duplicate timestamps retain the first source row.
    rows.sort(key=lambda item: item[0])
    deduplicated = []
    previous_time = None
    for row in rows:
        if previous_time is not None and row[0] == previous_time:
            continue
        deduplicated.append(row)
        previous_time = row[0]
    rows = deduplicated

    n = len(rows)
    numeric_columns = [c for c in range(len(header)) if c != ts_index]
    means_raw: dict[int, float] = {}
    counts_raw: dict[int, int] = {}

    for col in numeric_columns:
        values = [row[3][col] for row in rows if row[3][col] > -9990.0]
        means_raw[col] = sum(values) / len(values) if values else 0.0
        counts_raw[col] = len(values)

    # Forward-fill; the first missing value uses the column's raw mean (or zero).
    for col in numeric_columns:
        last = means_raw[col]
        has_last = counts_raw[col] > 0
        for row in rows:
            value = row[3][col]
            if value <= -9990.0:
                row[3][col] = last if has_last else 0.0
            else:
                last = value
                has_last = True

    # Population z-score, then winsorize to [-5, 5].
    means: dict[int, float] = {}
    sigmas: dict[int, float] = {}
    for col in numeric_columns:
        values = [row[3][col] for row in rows]
        mean = sum(values) / max(1, n)
        variance = sum((value - mean) ** 2 for value in values) / max(1, n)
        sigma = math.sqrt(max(1e-12, variance))
        means[col] = mean
        sigmas[col] = sigma

    for row in rows:
        for col in numeric_columns:
            z = (row[3][col] - means[col]) / sigmas[col]
            row[3][col] = min(5.0, max(-5.0, z))

    with output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(header)
        for _, _, ts_text, values, _ in rows:
            out = []
            for col in range(len(header)):
                out.append(ts_text if col == ts_index else fmt10(values[col]))
            writer.writerow(out)

    return True, str(output)


def main() -> int:
    parser = argparse.ArgumentParser(description="Preprocess FLUXNET CSV files into *_pre.csv files.")
    parser.add_argument("data_dir", nargs="?", default="data/fluxnet_data",
                        help="directory containing site subdirectories")
    parser.add_argument("--no-overwrite", action="store_true",
                        help="keep an existing *_pre.csv instead of replacing it")
    args = parser.parse_args()
    base = Path(args.data_dir).expanduser().resolve()
    if not base.is_dir():
        parser.error(f"FLUXNET data directory does not exist: {base}")

    processed = skipped = 0
    for site_dir in sorted(p for p in base.iterdir() if p.is_dir()):
        # SPACETIME 使用的 95 个站点中，ZA-Kru 是唯一的 FLUXNET2015 Tier Two 站点。
        # 为避免意外生成或分发受限站点的派生文件，预处理时自动跳过整个 ZA-Kru 目录。
        folder_name = site_dir.name
        site_id = folder_name[4:].split("_", 1)[0] if folder_name.startswith("FLX_") else folder_name
        if site_id == "ZA-Kru":
            print(f"[SKIP] excluded FLUXNET2015 Tier Two site: {folder_name}")
            skipped += len(list(site_dir.glob("*.csv"))) or 1
            continue
        for path in sorted(site_dir.glob("*.csv")):
            ok, message = process_file(path, overwrite=not args.no_overwrite)
            if ok:
                processed += 1
                print(f"[OK] {path.name} -> {message}")
            else:
                skipped += 1
                print(f"[SKIP] {path.name}: {message}")
    print(f"Finished: {processed} file(s) processed, {skipped} file(s) skipped.")
    return 0 if processed else 1


if __name__ == "__main__":
    raise SystemExit(main())
