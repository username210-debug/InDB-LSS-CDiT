#!/usr/bin/env python3
"""Create WindTunnel *_pre.csv files using the project's supplied preprocessing rules.

The source CSV files must be directly inside data/windtunnel_data/ (not nested).
The script leaves timestamp unchanged and z-scores the 21 measured variables.
Only Python's standard library is required.
"""
from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path

VARIABLES = [
    "hatch", "pot_1", "pot_2", "load_in", "load_out", "v_1", "v_2", "v_mic", "v_in", "v_out",
    "current_in", "current_out", "rpm_in", "rpm_out", "pressure_upwind", "pressure_downwind",
    "pressure_ambient", "pressure_intake", "mic", "signal_1", "signal_2",
]
REQUIRED = ["timestamp"] + VARIABLES


def parse_number(text: str) -> tuple[bool, float]:
    try:
        return True, float(text.strip())
    except ValueError:
        return False, 0.0


def fmt10(value: float) -> str:
    return format(value, ".10g")


def fmt15(value: float) -> str:
    return format(value, ".15g")


def process_file(path: Path, overwrite: bool) -> tuple[bool, str]:
    if path.suffix.lower() != ".csv" or path.name.endswith("_pre.csv"):
        return False, "skip non-raw/preprocessed file"
    output = path.with_name(path.stem + "_pre.csv")
    if output.exists() and not overwrite:
        return False, "output already exists"

    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames is None:
            return False, "empty file"
        header = list(reader.fieldnames)
        missing = [name for name in REQUIRED if name not in header]
        if missing:
            return False, "missing required columns: " + ", ".join(missing)
        rows = []
        for row in reader:
            timestamp_ok, timestamp = parse_number(row.get("timestamp", ""))
            if not timestamp_ok:
                continue
            values = []
            for name in VARIABLES:
                ok, value = parse_number(row.get(name, ""))
                values.append(value if ok else 0.0)
            rows.append((timestamp, values))

    if not rows:
        with output.open("w", encoding="utf-8", newline="") as stream:
            csv.writer(stream, lineterminator="\n").writerow(header)
        return True, str(output)

    # Match the supplied C++ Welford sample-variance calculation (n - 1).
    means = []
    stds = []
    n = len(rows)
    for col in range(len(VARIABLES)):
        mean = 0.0
        m2 = 0.0
        count = 0
        for _, values in rows:
            count += 1
            delta = values[col] - mean
            mean += delta / count
            delta2 = values[col] - mean
            m2 += delta * delta2
        variance = m2 / (count - 1) if count > 1 else 0.0
        std = math.sqrt(variance) if variance >= 0.0 else float("nan")
        if not (std > 1e-12):
            std = 1.0
        means.append(mean)
        stds.append(std)

    with output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        # The C++ preprocessor writes the input header, then timestamp + canonical variable order.
        writer.writerow(header)
        for timestamp, values in rows:
            out = [fmt15(timestamp)]
            out.extend(fmt10((values[i] - means[i]) / stds[i]) for i in range(len(VARIABLES)))
            writer.writerow(out)

    return True, str(output)


def main() -> int:
    parser = argparse.ArgumentParser(description="Preprocess WindTunnel CSV files into *_pre.csv files.")
    parser.add_argument("data_dir", nargs="?", default="data/windtunnel_data",
                        help="directory containing raw WindTunnel CSV files")
    parser.add_argument("--no-overwrite", action="store_true",
                        help="keep an existing *_pre.csv instead of replacing it")
    args = parser.parse_args()
    base = Path(args.data_dir).expanduser().resolve()
    if not base.is_dir():
        parser.error(f"WindTunnel data directory does not exist: {base}")

    processed = skipped = 0
    for path in sorted(base.glob("*.csv")):
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
