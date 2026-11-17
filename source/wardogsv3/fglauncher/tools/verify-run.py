"""verify-run.py -- assert the launcher's JSONL evidence against the expected flow.

Usage:  python verify-run.py wardog-launcher.jsonl [--mode run|scan|probe]

It never touches the driver or the kernel; it only reads the log produced by
WardogLauncher.exe and checks the same invariants the reference investigation
used for its own native trials.
"""
import json
import sys


def load(path):
    events = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError:
                events.append({"type": "unparsable", "raw": line})
    return events


def first(events, kind, **filters):
    for e in events:
        if e.get("type") != kind:
            continue
        if all(e.get(k) == v for k, v in filters.items()):
            return e
    return None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    mode = "run"
    if "--mode" in sys.argv:
        mode = sys.argv[sys.argv.index("--mode") + 1]
    events = load(path)
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))

    start = first(events, "start")
    check("log has start event", start is not None)
    if mode != "selftest":
        check("process was elevated", start and start.get("elevated") is True,
              "" if not start else f"elevated={start.get('elevated')}")

    if mode == "selftest":
        steps = [e for e in events if e.get("type", "").startswith("selftest_step")]
        failed = [e for e in steps if e["type"] == "selftest_step_failed"]
        check("selftest steps executed", len(steps) > 0, f"{len(steps)} steps")
        check("no failed selftest step", not failed, ", ".join(e.get("step", "?") for e in failed))
        done = first(events, "selftest_complete_ok")
        check("selftest completed ok", done is not None)
        report(checks)
        return 0 if all(ok for _, ok, _ in checks) else 1

    helper = first(events, "helper_loaded")
    check("helper (PGR) loaded", helper is not None)

    selftest = first(events, "selftest_ok")
    check("physical read/write self test passed", selftest is not None)
    if selftest:
        check("  mapping equals virtual page", selftest.get("physical_read_matches_virtual") is True)
        check("  physical write visible in virtual page",
              selftest.get("physical_write_visible_in_virtual") is True)
        check("  physical restore verified", selftest.get("physical_restore_ok") is True)

    if mode == "probe":
        check("probe completed", first(events, "probe_complete") is not None)
        cleanup = first(events, "cleanup")
        check("PGR unloaded and service removed",
              cleanup and cleanup.get("service_removed") is True and cleanup.get("device_present") is False,
              "" if not cleanup else str(cleanup))
        report(checks)
        return 0 if all(ok for _, ok, _ in checks) else 1

    check("translation ready", first(events, "translation_ready") is not None)
    verified = first(events, "translation_verified")
    check("translated read matches live kernel", verified is not None)
    candidates = [e for e in events if e.get("type") == "ci_candidate"]
    check("CI target candidates located", len(candidates) > 0, f"{len(candidates)} candidates")

    baseline = first(events, "control_result", case="baseline_unsigned")
    check("baseline unsigned load refused", baseline is not None and baseline.get("load_status") == "0xC0000428",
          "" if not baseline else str(baseline.get("load_status")))
    check("baseline device absent", baseline is not None and baseline.get("device") is False)

    if mode == "scan":
        check("scan completed", first(events, "scan_complete") is not None)
        cleanup = first(events, "cleanup")
        check("PGR unloaded and service removed",
              cleanup and cleanup.get("service_removed") is True and cleanup.get("device_present") is False)
        report(checks)
        return 0 if all(ok for _, ok, _ in checks) else 1

    gate = first(events, "gate_confirmed")
    check("code-integrity gate confirmed by trial", gate is not None,
          "" if not gate else f"va={gate.get('va')} section={gate.get('section')}")

    saved = first(events, "original_content_saved") or first(events, "temporary_content_written")
    check("original content saved before write", saved is not None)
    written = first(events, "temporary_content_written")
    check("temporary content written", written is not None,
          "" if not written else f"readback={written.get('readback', written.get('value'))}")

    follow = first(events, "followup_load_result")
    check("follow-up driver admitted", follow is not None and follow.get("load_status") == "0x00000000",
          "" if not follow else str(follow.get("load_status")))
    check("follow-up driver actually ran", follow is not None and follow.get("device") is True)

    restored = first(events, "original_content_restored")
    check("original content restored", restored is not None and restored.get("match") is True)

    after = first(events, "control_result", case="after_restore")
    check("unsigned load refused again after restore",
          after is not None and after.get("load_status") == "0xC0000428",
          "" if not after else str(after.get("load_status")))

    summary = first(events, "summary")
    check("summary reports success", summary is not None and summary.get("result") == "success")

    cleanup = first(events, "cleanup")
    check("PGR unloaded and service removed",
          cleanup and cleanup.get("service_removed") is True and cleanup.get("device_present") is False,
          "" if not cleanup else str(cleanup))

    report(checks)
    return 0 if all(ok for _, ok, _ in checks) else 1


def report(checks):
    width = max(len(n) for n, _, _ in checks) + 2
    ok_all = True
    for name, ok, detail in checks:
        ok_all &= ok
        print(("PASS  " if ok else "FAIL  ") + name.ljust(width) + (detail or ""))
    print()
    print("RESULT:", "all checks passed" if ok_all else "SOME CHECKS FAILED")


if __name__ == "__main__":
    sys.exit(main())

