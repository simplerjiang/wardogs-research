"""Assert a Wardog case run log.

Two modes, chosen by what the log contains:

  offline  (--mode case --check)   : case file vs the image and kernel on disk
  online   (--mode case ...)       : the same case, read through the target's
                                     own page tables via the memory provider

Usage:
    python tools/verify-case.py build/case-check.jsonl --expect offline
    python tools/verify-case.py build/case-run.jsonl   --expect online

Exit code 0 means every assertion held; 1 lists what did not.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def load(path: Path) -> list[dict]:
    events = []
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return events


def by_type(events: list[dict]) -> dict[str, list[dict]]:
    grouped: dict[str, list[dict]] = {}
    for event in events:
        grouped.setdefault(event.get("type", ""), []).append(event)
    return grouped


class Report:
    def __init__(self) -> None:
        self.failures: list[str] = []
        self.checks = 0

    def check(self, ok: bool, label: str, detail: str = "") -> None:
        self.checks += 1
        mark = "ok  " if ok else "FAIL"
        print(f"[{mark}] {label}" + (f"  {detail}" if detail else ""))
        if not ok:
            self.failures.append(label)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--expect", choices=["offline", "online"], default="offline")
    ap.add_argument("--min-verified", type=int, default=2)
    args = ap.parse_args()

    path = Path(args.log)
    if not path.exists():
        print(f"log not found: {path}")
        return 2

    events = load(path)
    groups = by_type(events)
    report = Report()

    def first(kind: str) -> dict | None:
        items = groups.get(kind)
        return items[0] if items else None

    loaded = first("case_loaded")
    report.check(loaded is not None, "case_loaded event present")
    if loaded:
        report.check(bool(loaded.get("case_id")), "case has an id", str(loaded.get("case_id")))
        report.check(
            int(loaded.get("case_observation_count", 0)) > 0,
            "case declares observations",
            str(loaded.get("case_observation_count")),
        )

    report.check(first("case_image_verified") is not None, "case image SHA256 matches the pin")
    report.check(
        (first("case_image_pe") or {}).get("case_image_base_matches") is True,
        "image preferred base equals the case base",
    )

    summary = first("case_offline_summary") if args.expect == "offline" else first("case_summary")
    report.check(summary is not None, "summary event present")
    if summary:
        verified = int(summary.get("verified", 0))
        report.check(
            verified >= args.min_verified,
            f"at least {args.min_verified} byte-exact observations",
            f"verified={verified}",
        )
        report.check(
            int(summary.get("mismatched", 0)) == 0,
            "no observation mismatched",
            f"mismatched={summary.get('mismatched')}",
        )
        report.check(
            int(summary.get("out_of_image", 0)) == 0,
            "no observation pointed outside the image",
        )
        report.check(summary.get("result") == "success", "summary result is success")

    if args.expect == "offline":
        report.check(
            first("case_offline_gate_export_verified") is not None,
            "gate routine RVA matches the kernel export table",
        )
        report.check(
            first("case_offline_gate_flag_verified") is not None,
            "gate flag RVA is backed by file data",
        )
        runtime = groups.get("case_offline_runtime_only", [])
    else:
        report.check(first("target_process_ready") is not None, "target process resolved")
        report.check(first("target_address_space_ready") is not None, "target address space found")
        report.check(first("case_gate_verified") is not None, "gate RVA matches the live kernel")
        runtime = groups.get("case_observation_runtime", [])
        for event in groups.get("case_observation_verified", []) + runtime:
            report.check(
                str(event.get("pa", "0x0")) not in ("0x0", "0x0000000000000000"),
                f"observation {event.get('observation')} resolved to a physical address",
                str(event.get("pa")),
            )

    report.check(
        not groups.get("case_observation_unreadable") and not groups.get("case_offline_expectation_unbacked"),
        "no unreadable observations",
    )
    report.check(len(runtime) > 0, "runtime-only slots were reported", f"count={len(runtime)}")

    print()
    if report.failures:
        print(f"{len(report.failures)} of {report.checks} assertions failed")
        return 1
    print(f"all {report.checks} assertions passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
