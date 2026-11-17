"""Validate an ESP report (and optionally the captured frame) against the math."""
import argparse
import json
import os
import sys

from PIL import Image
import numpy as np

CHARACTER = (110, 155, 255)
VEHICLE = (190, 160, 255)


def load_report(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def check_report(report, tolerance, problems):
    viewport = report["viewport"]
    width, height = float(viewport[0]), float(viewport[1])
    if report["frames"] <= 0:
        problems.append("no frame was presented")
    if report["frames_with_quads"] <= 0:
        problems.append("no frame contained quads")
    quads = report["quads"]
    drawn = report["stats"]["drawn"]
    expected = drawn * 4
    if quads != expected:
        problems.append("quads %d != drawn %d * 4" % (quads, drawn))
    for index, box in enumerate(report["boxes"]):
        if box["status"] == 0:
            problems.append("box %d was skipped" % index)
            continue
        left, top, right, bottom = [float(v) for v in box["box"]]
        box_width = right - left
        box_height = bottom - top
        if abs(box_width - box["box_width"]) > 0.01 or abs(box_height - box["box_height"]) > 0.01:
            problems.append("box %d extents disagree with the reported size" % index)
        ratio = report["settings"]["width_ratio"]
        minimum_height = 3.0
        if box_height > minimum_height + tolerance:
            if abs(box_width - ratio * box["box_height"]) > tolerance:
                problems.append("box %d does not follow width = %.2f * height (%.3f vs %.3f)"
                                % (index, ratio, box_width, ratio * box_height))
        if abs(box["box_width_130"] - box["box_width"] * 1.3) > 0.02:
            problems.append("box %d 130 percent width is not 1.3x (%.3f vs %.3f)"
                            % (index, box["box_width_130"], box["box_width"] * 1.3))
        if abs(box["box_height_130"] - box["box_height"] * 1.3) > 0.02:
            problems.append("box %d 130 percent height is not 1.3x" % index)
        if right < -64 or left > width + 64 or bottom < -64 or top > height + 64:
            problems.append("box %d lies outside the viewport" % index)


def check_capture(path, report, problems):
    image = np.array(Image.open(path).convert("RGB")).astype(int)
    height, width = image.shape[0], image.shape[1]
    if [width, height] != list(report["viewport"]):
        problems.append("capture is %dx%d but the viewport is %s" % (width, height, report["viewport"]))
    lit = image.sum(axis=2) > 0
    if not lit.any():
        problems.append("capture is empty")
        return
    ys, xs = np.nonzero(lit)
    for index, box in enumerate(report["boxes"]):
        if box["status"] == 0:
            continue
        left, top, right, bottom = [float(v) for v in box["box"]]
        inside_x = (xs >= left - 2) & (xs <= right + 2)
        inside_y = (ys >= top - 2) & (ys <= bottom + 2)
        if not (inside_x & inside_y).any():
            problems.append("box %d produced no pixel in the capture" % index)
    kinds = {box["kind"] for box in report["boxes"] if box["status"] != 0}
    for kind, colour in ((0, CHARACTER), (1, VEHICLE)):
        if kind not in kinds:
            continue
        mask = (np.abs(image[:, :, 0] - colour[0]) <= 2) & \
               (np.abs(image[:, :, 1] - colour[1]) <= 2) & \
               (np.abs(image[:, :, 2] - colour[2]) <= 2)
        if int(mask.sum()) == 0:
            problems.append("kind %d colour is missing from the capture" % kind)
    stray = int((lit & ~(np.abs(image[:, :, 2] - 255) <= 2)).sum())
    if stray:
        problems.append("%d lit pixels are not a box colour" % stray)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--report", required=True)
    parser.add_argument("--capture")
    parser.add_argument("--tolerance", type=float, default=0.05)
    args = parser.parse_args()

    problems = []
    if not os.path.exists(args.report):
        print("report_missing %s" % args.report)
        return 2
    report = load_report(args.report)
    check_report(report, args.tolerance, problems)
    if args.capture:
        if not os.path.exists(args.capture):
            problems.append("capture_missing %s" % args.capture)
        else:
            check_capture(args.capture, report, problems)

    print("esp_report mode=%s frames=%d boxes=%d quads=%d drawn=%d"
          % (report.get("source", "?"), report["frames"], len(report["boxes"]), report["quads"],
             report["stats"]["drawn"]))
    if problems:
        for problem in problems:
            print("PROBLEM %s" % problem)
        return 1
    print("esp_report_ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
