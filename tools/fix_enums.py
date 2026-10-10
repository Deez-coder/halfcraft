"""Dumper-7 gave some enums the wrong underlying type for this game (e.g. ERotatorQuantization : uint64 and
ETransitionType : int32, really 1 byte), which shifts every struct and class that holds one (AActor via
FRepMovement). Each member line carries its real size, '// 0xOFFS(0xSIZE)', so give every enum the size it actually
has where it is used. Enums never used as a member are left alone."""
import glob, os, re

SDK = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "hs", "third_party", "sdk", "SDK")
files = glob.glob(os.path.join(SDK, "*.hpp"))
decl = re.compile(r"^enum class (\w+) : (u?int)(8|16|32|64)\b", re.M)
declared = {}
for f in files:
    if f.endswith("Basic.hpp"):
        continue  # EClassCastFlags / EPropertyFlags really are 64-bit
    for m in decl.finditer(open(f, encoding="utf-8").read()):
        declared[m.group(1)] = (f, m.group(2), m.group(3))
use = re.compile(r"^\s+(?:T1ByteEnum<)?(\w+)>?\s+\w+;\s+// 0x[0-9A-Fa-f]+\(0x([0-9A-Fa-f]+)\)", re.M)
sizes = {}
for f in files:
    for m in use.finditer(open(f, encoding="utf-8").read()):
        if m.group(1) in declared:
            sizes.setdefault(m.group(1), set()).add(int(m.group(2), 16))
bits = {1: "8", 2: "16", 4: "32", 8: "64"}
unknown, conflict, by_file = [], [], {}
for name, (f, sign, cur) in declared.items():
    s = sizes.get(name)
    if not s:
        unknown.append(name)
    elif len(s) != 1 or next(iter(s)) not in bits:
        conflict.append((name, sorted(s)))
    elif bits[next(iter(s))] != cur:
        by_file.setdefault(f, []).append((name, sign, cur, bits[next(iter(s))]))
fixed = 0
for f, items in by_file.items():
    text = open(f, encoding="utf-8").read()
    for name, sign, cur, want in items:
        text, k = re.subn(r"^enum class %s : %s%s\b" % (name, sign, cur), "enum class %s : %s%s" % (name, sign, want), text, flags=re.M)
        fixed += k
    open(f, "w", encoding="utf-8").write(text)
print(f"enums: {len(declared)}, resized: {fixed}, unused as members: {len(unknown)}, conflicting sizes: {conflict}")
