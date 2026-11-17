"""Build a controlled child -> ReadProcessMemory -> existing overlay/GPU test.

Run in an x64 MSVC developer shell. Generated adapters live only in --out;
Production files are not modified; the driver is not executed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def function(text, signature):
    begin = text.index(signature)
    tokens = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*[\s\S]*?\*/|//[^\n]*|[{}]')
    depth = 0
    for match in tokens.finditer(text, begin):
        if match[0] == "{":
            depth += 1
        elif match[0] == "}":
            depth -= 1
            if not depth:
                return text[begin:match.end()]
    raise ValueError(signature)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--out", required=True, type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=True)
src = root / "2_src_cheat"
original = (src / "wd_esp_full.c").read_text(encoding="utf-8-sig")
adapted = original
replacements = {
    "static int rd(uint64_t a, void* o, size_t sz)": """
static HANDLE controlled_process;
static unsigned long long controlled_reads, controlled_bytes;
static DWORD controlled_read_error;
static int rd(uint64_t a, void* o, size_t sz) {
    SIZE_T received = 0;
    BOOL ok = ReadProcessMemory(controlled_process, (LPCVOID)(ULONG_PTR)a, o, sz, &received);
    controlled_reads++; controlled_bytes += received;
    controlled_read_error = ok ? ERROR_SUCCESS : GetLastError();
    return ok && received == sz;
}
""",
    # These paths are outside the test. Fail visibly if accidentally reached.
    "static int drv_open(void)": 'static int drv_open(void) { abort(); return 0; }',
    "static uint64_t drv_base(void)": 'static uint64_t drv_base(void) { abort(); return 0; }',
    "static int wr(": 'static int wr(uint64_t a, const void* d, size_t n) { abort(); return 0; }',
    "int WINAPI WinMain(": "",
}
for signature, replacement in replacements.items():
    adapted = adapted.replace(function(adapted, signature), replacement, 1)

preserved = {}
for signature in ("static void rot_matrix(", "static int w2s_a(",
                  "static void project_all(", "static void DrawOverlay("):
    before = function(original, signature)
    after = function(adapted, signature)
    assert before == after
    preserved[signature] = hashlib.sha256(before.encode()).hexdigest()

test = Path(__file__).with_name("controlled_render.c").read_text(encoding="utf-8")
(out / "controlled_render.generated.c").write_text(adapted + "\n" + test, encoding="utf-8")
gpu = (src / "gpu.cpp").read_text(encoding="utf-8-sig")
probe = Path(__file__).with_name("controlled_gpu_probe.cpp").read_text(encoding="utf-8")
(out / "controlled_gpu.generated.cpp").write_text(gpu + "\n" + probe, encoding="utf-8")

objects = []
for name, source in (("controlled_render", out / "controlled_render.generated.c"),
                     ("controlled_gpu", out / "controlled_gpu.generated.cpp"),
                     ("chams", src / "chams/chams.c")):
    obj = out / (name + ".obj")
    subprocess.run(["cl", "/nologo", "/utf-8", "/O2", "/W3", "/EHsc", "/c",
                    "/I" + str(src), str(source), "/Fo" + str(obj)], cwd=out, check=True)
    objects.append(str(obj))
subprocess.run(["link", "/nologo", "/SUBSYSTEM:WINDOWS", "/MACHINE:X64",
                "/OUT:" + str(out / "controlled_render.exe"), *objects,
                "kernel32.lib", "user32.lib", "gdi32.lib", "comctl32.lib", "advapi32.lib",
                "ole32.lib", "oleaut32.lib", "uuid.lib", "d3d11.lib", "dxgi.lib",
                "d2d1.lib", "dwrite.lib", "dcomp.lib", "d3dcompiler.lib"], cwd=out, check=True)
(out / "source-manifest.json").write_text(json.dumps({
    "source_root": str(root),
    "unmodified_functions_sha256": preserved,
    "gpu_original_sha256": hashlib.sha256(gpu.encode()).hexdigest(),
    "adapters": ["rd -> ReadProcessMemory on owned child", "driver open/base/write -> abort",
                 "WinMain -> controlled test", "menu -> private config, no menu thread",
                 "GPU probe appended for readback before presentation"],
    "kernel_tested": False,
    "game_discovery_or_scan_tested": False,
}, ensure_ascii=False, indent=2), encoding="utf-8")
print("PASS controlled render build; production projection/drawing functions preserved")
