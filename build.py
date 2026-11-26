"""Package module/ into build/FpDoubleTapFix-<version>.zip (LF line endings, exec bits set)."""
import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.join(HERE, "module")
props = dict(l.split("=", 1) for l in open(os.path.join(MOD, "module.prop"), encoding="utf-8").read().splitlines() if "=" in l)
out = os.path.join(HERE, "build", f"FpDoubleTapFix-{props['version']}.zip")
os.makedirs(os.path.dirname(out), exist_ok=True)

with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk(MOD):
        for f in sorted(files):
            p = os.path.join(root, f)
            arc = os.path.relpath(p, MOD).replace(os.sep, "/")
            data = open(p, "rb").read()
            if f.endswith((".sh", ".prop")):
                data = data.replace(b"\r\n", b"\n")
            zi = zipfile.ZipInfo(arc, (2026, 11, 26, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = (0o755 if f == "fpdtap" or f.endswith(".sh") else 0o644) << 16
            z.writestr(zi, data)
print(out)
for n in zipfile.ZipFile(out).namelist():
    print("  ", n)
