#!/usr/bin/env python3
"""Generate Basin *_pre.csv files using the preprocessing used by this project.

Expected input columns: time,tavg,prec,Qobs.
The original CSV files are never modified; output is written beside each input as
<stem>_pre.csv. Run this before generating faulted variants.
"""

from __future__ import annotations

import argparse
import csv
import math
from datetime import datetime
from pathlib import Path

MISSING = {"", "NA", "NaN", "nan", "NULL", "null"}
EXPECTED = ("time", "tavg", "prec", "Qobs")


def parse_number(raw: str) -> float:
    value = raw.strip()
    if value in MISSING:
        return math.nan
    try:
        return float(value)
    except (TypeError, ValueError):
        return math.nan


def format_date(raw: str) -> str:
    value = raw.strip()
    for fmt in ("%Y/%m/%d", "%Y-%m-%d", "%Y/%m/%d "):
        try:
            d = datetime.strptime(value, fmt).date()
            return f"{d.year}/{d.month}/{d.day}"
        except ValueError:
            pass
    # Avoid silently rewriting an unrecognised timestamp.
    parts = value.replace("-", "/").split("/")
    if len(parts) != 3:
        raise ValueError(f"unsupported date value: {raw!r}")
    y, m, d = (int(x) for x in parts)
    if not (y > 0 and 1 <= m <= 12 and 1 <= d <= 31):
        raise ValueError(f"invalid date value: {raw!r}")
    return f"{y}/{m}/{d}"


def interpolate(values: list[float], prefix_zero: bool = False) -> list[float]:
    """Match the supplied C++ linear-imputation policy."""
    n = len(values)
    if n == 0:
        return values
    valid = [i for i, x in enumerate(values) if not math.isnan(x)]
    if not valid:
        return [0.0] * n

    first = valid[0]
    prefix_value = 0.0 if prefix_zero else values[first]
    for i in range(first):
        values[i] = prefix_value

    last = first
    for i in valid[1:]:
        gap = i - last
        if gap > 1:
            a, b = values[last], values[i]
            for j in range(1, gap):
                weight = j / gap
                values[last + j] = a + (b - a) * weight
        last = i

    for i in range(last + 1, n):
        values[i] = values[last]
    return values


def fmt10(value: float) -> str:
    # C++ std::setprecision(10) in the supplied implementation uses 10 significant digits.
    return format(value, ".10g")


def process_file(path: Path, overwrite: bool) -> tuple[bool, str]:
    stem = path.stem
    # Do not treat previously generated clean/fault files as raw source files.
    if "_pre" in stem:
        return False, "skip generated/preprocessed file"
    output = path.with_name(stem + "_pre.csv")
    if output.exists() and not overwrite:
        return False, "output already exists"

    with path.open("r", encoding="utf-8-sig", newline="") as f:
        reader = csv.reader(f)
        try:
            header = next(reader)
        except StopIteration:
            return False, "empty file"
        header = [x.strip() for x in header]
        if tuple(header[:4]) != EXPECTED:
            return False, "expected columns time,tavg,prec,Qobs"

        dates: list[str] = []
        tavg: list[float] = []
        prec: list[float] = []
        qobs: list[float] = []
        for row in reader:
            if not row or not any(x.strip() for x in row):
                continue
            if len(row) < 4:
                continue
            try:
                dates.append(format_date(row[0]))
            except ValueError:
                continue
            tavg.append(parse_number(row[1]))
            prec.append(parse_number(row[2]))
            qobs.append(parse_number(row[3]))

    if not dates:
        return False, "no valid dated records"

    interpolate(tavg)
    prec = [0.0 if math.isnan(x) else x for x in prec]
    interpolate(qobs, prefix_zero=True)

    # The supplied preprocessing applies log1p to non-negative precipitation and runoff.
    prec = [math.log1p(x) if x >= 0.0 else x for x in prec]
    qobs = [math.log1p(x) if x >= 0.0 else x for x in qobs]

    with output.open("w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f, lineterminator="\n")
        writer.writerow(EXPECTED)
        for row in zip(dates, tavg, prec, qobs):
            writer.writerow([row[0], fmt10(row[1]), fmt10(row[2]), fmt10(row[3])])
    return True, str(output)


def main() -> int:
    parser = argparse.ArgumentParser(description="Preprocess Basin CSV files into *_pre.csv files.")
    parser.add_argument("data_dir", nargs="?", default="data/basin_data", help="directory containing the original Basin CSV files")
    parser.add_argument("--no-overwrite", action="store_true", help="keep an existing *_pre.csv instead of replacing it")
    args = parser.parse_args()

    data_dir = Path(args.data_dir).expanduser().resolve()
    if not data_dir.is_dir():
        parser.error(f"data directory does not exist: {data_dir}")

    processed = 0
    skipped = 0
    for path in sorted(data_dir.glob("*.csv")):
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
