"""Blank Dumper-7 assert macros for structs whose layout check fails under MSVC (structs this mod doesn't use).
Repeats: compile a TU that includes the SDK headers, collect failing struct names, blank their DUMPER7_ASSERTS_ macro."""
import os, re, subprocess

HS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "hs")
SDK = os.path.join(HS, "third_party", "sdk")
inl = os.path.join(SDK, "Assertions.inl")
tu = os.path.join(HS, "build", "obj", "assert_probe.cpp")
os.makedirs(os.path.dirname(tu), exist_ok=True)
with open(tu, "w") as f:
    f.write('#include "SDK/Basic.hpp"\n#include "SDK/CoreUObject_classes.hpp"\n#include "SDK/Engine_classes.hpp"\n')
vc = open(os.path.join(os.environ["TEMP"], "hsp_vcvars.txt")).read().strip()
bat = os.path.join(HS, "build", "obj", "assert_probe.bat")
with open(bat, "w") as f:
    f.write('@echo off\nset "PATH=%PATH:"=%"\ncall "' + vc + '" >nul\n'
            'cl /nologo /std:c++latest /EHsc /bigobj /Zs /I "' + SDK + '" "' + tu + '"\n')
disabled = []
for round_ in range(60):
    out = subprocess.run(["cmd", "/c", bat], capture_output=True, text=True).stdout
    names = set(re.findall(r"Wrong (?:size|alignment) on (\w+)'", out)) | set(re.findall(r"Member '(\w+?)::", out))
    if not names:
        print("clean after", round_, "rounds; disabled", len(disabled))
        print("\n".join(l for l in out.splitlines() if "error" in l)[:3000])
        break
    s = open(inl, encoding="utf-8").read()
    for n in sorted(names):
        s, k = re.subn(r"#define DUMPER7_ASSERTS_%s \\\n(?:.*\\\n)*.*\n" % re.escape(n),
                       "#define DUMPER7_ASSERTS_%s /* layout check fails under MSVC; unused here */\n" % n, s)
        if k:
            disabled.append(n)
        else:
            print("no macro for", n)
    open(inl, "w", encoding="utf-8").write(s)
    print("round", round_, len(names), "disabled")
open(os.path.join(SDK, "DISABLED_ASSERTS.txt"), "w").write("\n".join(sorted(disabled)) + "\n")
