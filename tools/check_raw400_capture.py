"""Read-only serial-log analysis; never flash, provision or certify a device."""
import argparse
import json
from pathlib import Path


def inspect_capture(text, *, minimum_seconds=86400, rate_tolerance=0.5,
                    max_jitter_us=625, max_callback_us=1250, minimum_heap=65536):
    rows = []
    errors = []
    for line in text.splitlines():
        if "adc_profile=ad8232-raw400-v1 " not in line:
            continue
        values = dict(part.split("=", 1) for part in line.split() if "=" in part)
        required = {"target_hz", "interval_us", "raw_samples", "raw_rail", "window_us",
                    "window_start_us", "window_end_us", "raw_invalid", "scale_clipped", "missed_slots", "duplicate_wakeups",
                    "queue_dropped", "max_adc_us", "max_callback_us", "max_jitter_us",
                    "max_interval_us", "heap", "min_heap"}
        try:
            row = {key: int(values[key]) for key in required}
        except (KeyError, ValueError):
            errors.append("malformed_window")
            continue
        if any(value < 0 for value in row.values()):
            errors.append("negative_counter")
            continue
        if row["window_us"] == 0 or row["raw_samples"] == 0:
            errors.append("empty_window")
            continue
        if row["window_end_us"] - row["window_start_us"] != row["window_us"]:
            errors.append("inconsistent_window_time")
        if rows and rows[-1]["window_end_us"] != row["window_start_us"]:
            errors.append("missing_or_restarted_log_window")
        if row["target_hz"] != 400 or row["interval_us"] != 2500:
            errors.append("wrong_profile_clock")
        if row["raw_rail"] > row["raw_samples"]:
            errors.append("invalid_rail_denominator")
        rate = row["raw_samples"] * 1_000_000 / row["window_us"]
        row["observed_hz"] = rate
        if abs(rate - 400) > rate_tolerance:
            errors.append("rate_outside_tolerance")
        for key in ("raw_invalid", "scale_clipped", "missed_slots", "duplicate_wakeups", "queue_dropped"):
            if row[key]:
                errors.append(key)
        if row["max_jitter_us"] > max_jitter_us:
            errors.append("jitter_outside_tolerance")
        if row["max_callback_us"] > max_callback_us or row["max_adc_us"] > max_callback_us:
            errors.append("callback_budget_exceeded")
        if row["max_interval_us"] > 2500 + 2 * max_jitter_us:
            errors.append("sampling_interval_exceeded")
        if min(row["heap"], row["min_heap"]) < minimum_heap:
            errors.append("heap_below_reserve")
        rows.append(row)
    for line in text.splitlines():
        if "direct_dropped=" in line:
            try:
                losses = dict(part.split("=", 1) for part in line.split() if "=" in part)
                if int(losses["direct_dropped"]) or int(losses["timing_dropped"]):
                    errors.append("transport_or_timing_loss")
            except (KeyError, ValueError):
                errors.append("malformed_loss_counters")
    duration = sum(row["window_us"] for row in rows) / 1_000_000
    if not rows or duration < minimum_seconds:
        errors.append("insufficient_observation")
    return {"profile": "ad8232-raw400-v1", "windows": len(rows), "seconds": duration,
            "observed_min_hz": min((r["observed_hz"] for r in rows), default=None),
            "observed_max_hz": max((r["observed_hz"] for r in rows), default=None),
            "minimum_free_heap": min((r["min_heap"] for r in rows), default=None),
            "observed_max_adc_us": max((r["max_adc_us"] for r in rows), default=None),
            "observed_max_callback_us": max((r["max_callback_us"] for r in rows), default=None),
            "observed_max_jitter_us": max((r["max_jitter_us"] for r in rows), default=None),
            "errors": sorted(set(errors)), "capture_criteria_met": not errors,
            "release_authorized": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    report = inspect_capture(args.log.read_text())
    print(json.dumps(report, indent=2))
    return 0 if report["capture_criteria_met"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
