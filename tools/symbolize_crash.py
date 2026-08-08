#!/usr/bin/env python3
"""Decode an ac6recomp crash report to exact guest instructions, offline.

The crash handler ships raw host RVAs (exe+0x...) because users don't have
the PDB. This tool runs where the PDB exists (a dev machine; the
reproducible build regenerates it byte-identical for any released commit)
and rewrites each host frame as:

    exe+0x00123456  rex_sub_8209ABCD  generated/ac6recomp_recomp.7.cpp:1234
        guest 0x8209ABF0:  lwz r11,0(r3)        <- the exact guest instruction

Usage:
    python tools/symbolize_crash.py <crash-....txt>
        [--exe out/build/win-amd64-relwithdebinfo/ac6recomp.exe]
        [--generated generated] [--force]

Requires llvm-symbolizer (and llvm-readobj for the PDB match check) on PATH
or in C:/Program Files/LLVM/bin. The PDB must MATCH the exe (same build):
the tool compares the exe's RSDS debug-directory GUID against the PDB's and
refuses to print symbols from a mismatched pair unless --force is given -
wrong symbols are worse than none.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

LLVM_DIRS = [r"C:\Program Files\LLVM\bin", "/usr/bin", "/usr/local/bin"]


def find_tool(name):
    path = shutil.which(name)
    if path:
        return path
    for d in LLVM_DIRS:
        candidate = os.path.join(d, name + (".exe" if os.name == "nt" else ""))
        if os.path.isfile(candidate):
            return candidate
    return None


def pe_pdb_guid(readobj, exe):
    """The exe's RSDS GUID+age and referenced PDB path, via llvm-readobj."""
    out = subprocess.run([readobj, "--coff-debug-directory", exe],
                         capture_output=True, text=True).stdout
    guid = re.search(r"PDBGUID:\s*\(([^)]*)\)", out)
    age = re.search(r"PDBAge:\s*(\d+)", out)
    pdb = re.search(r"PDBFileName:\s*(.+)", out)
    if not guid:
        # Older llvm-readobj prints "GUID: {...}"
        guid = re.search(r"GUID:\s*\{?([0-9A-Fa-f-]+)\}?", out)
    return (guid.group(1).strip() if guid else None,
            age.group(1) if age else None,
            pdb.group(1).strip() if pdb else None)


def pdb_guid(pdbutil, pdb_path):
    out = subprocess.run([pdbutil, "dump", "--summary", pdb_path],
                         capture_output=True, text=True).stdout
    guid = re.search(r"GUID:\s*\{?([0-9A-Fa-f-]+)\}?", out)
    age = re.search(r"Age:\s*(\d+)", out)
    return (guid.group(1).strip() if guid else None, age.group(1) if age else None)


def normalize_guid(g):
    if not g:
        return None
    return re.sub(r"[^0-9A-F]", "", g.upper())


def crash_build_identity(crash_path):
    """The 'Build ... commit <hash> built <stamp>' line the handler writes."""
    with open(crash_path, "r", errors="replace") as f:
        for _ in range(40):
            line = f.readline()
            if not line:
                break
            m = re.match(r"Build\s+(\S+)\s+commit\s+(\S+)\s+built\s+(\S+)", line.strip())
            if m:
                return m.group(2), m.group(3)
    return None, None


def exe_contains_tokens(exe_path, tokens):
    """Are these build strings compiled into this exe? They come from
    version.h, so a binary from another build carries different ones."""
    try:
        with open(exe_path, "rb") as f:
            blob = f.read()
    except OSError:
        return None
    return all(t.encode("ascii", "ignore") in blob for t in tokens if t)


MNEMONIC_RE = re.compile(r"^\s*//\s+(\S.*)$")
LABEL_RE = re.compile(r"^\s*loc_([0-9A-Fa-f]{8}):")
LR_RE = re.compile(r"ctx\.lr\s*=\s*0x([0-9A-Fa-f]{8})")


def guest_instruction_at(generated_root, rel_file, line_number):
    """The PPC mnemonic comment directly above `line_number`, plus the
    nearest preceding guest-address anchor (loc_ label or ctx.lr store)."""
    path = rel_file
    if not os.path.isabs(path):
        path = os.path.join(generated_root, os.path.basename(rel_file))
    if not os.path.isfile(path):
        return None, None
    try:
        with open(path, "r", errors="replace") as f:
            lines = f.readlines()
    except OSError:
        return None, None
    if line_number < 1 or line_number > len(lines):
        return None, None
    mnemonic = None
    anchor = None
    for i in range(line_number - 1, max(-1, line_number - 400), -1):
        text = lines[i]
        if mnemonic is None:
            m = MNEMONIC_RE.match(text)
            if m and not m.group(1).startswith(("=", "-", "TODO")):
                mnemonic = m.group(1).strip()
        if anchor is None:
            m = LABEL_RE.match(text)
            if m:
                anchor = "after loc_%s" % m.group(1).upper()
            else:
                m = LR_RE.search(text)
                if m:
                    anchor = "after call site lr=0x%s" % m.group(1).upper()
        if mnemonic and anchor:
            break
        # A function boundary ends the walk - anything above belongs elsewhere.
        if text.startswith("PPC_FUNC_IMPL("):
            break
    return mnemonic, anchor


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("crash", help="crash-....txt written by the game")
    parser.add_argument("--exe", default="out/build/win-amd64-relwithdebinfo/ac6recomp.exe")
    parser.add_argument("--pdb", default=None, help="default: next to the exe")
    parser.add_argument("--generated", default="generated")
    parser.add_argument("--force", action="store_true",
                        help="symbolize even if the PDB does not match the exe")
    args = parser.parse_args()

    symbolizer = find_tool("llvm-symbolizer")
    if not symbolizer:
        sys.exit("llvm-symbolizer not found (install LLVM or add it to PATH)")
    pdb_path = args.pdb or os.path.splitext(args.exe)[0] + ".pdb"

    # --- PDB<->exe match check. Wrong symbols are worse than none.
    readobj = find_tool("llvm-readobj")
    pdbutil = find_tool("llvm-pdbutil")
    if readobj and pdbutil and os.path.isfile(pdb_path):
        exe_guid, exe_age, exe_pdb_name = pe_pdb_guid(readobj, args.exe)
        got_guid, got_age = pdb_guid(pdbutil, pdb_path)
        if normalize_guid(exe_guid) and normalize_guid(got_guid) and \
           normalize_guid(exe_guid) != normalize_guid(got_guid):
            print("*" * 72)
            print("PDB MISMATCH: %s does not belong to %s" % (pdb_path, args.exe))
            print("  exe RSDS GUID: %s (age %s, references %s)" % (exe_guid, exe_age,
                                                                   exe_pdb_name))
            print("  pdb GUID:      %s (age %s)" % (got_guid, got_age))
            print("  Rebuild the released commit (reproducible build) to regenerate the")
            print("  matching PDB. Symbols below would be WRONG.")
            print("*" * 72)
            if not args.force:
                sys.exit(2)
    else:
        print("note: PDB match check skipped (llvm-readobj/llvm-pdbutil or the PDB "
              "itself unavailable) - trust the symbols accordingly")

    # --- Does this exe actually correspond to this crash file? The PDB check
    # above only proves the PDB belongs to the exe; decoding a crash from a
    # DIFFERENT build against it yields confident, wrong answers.
    crash_commit, crash_built = crash_build_identity(args.crash)
    if crash_commit or crash_built:
        present = exe_contains_tokens(args.exe, [crash_commit, crash_built])
        if present is False:
            print("*" * 72)
            print("BUILD MISMATCH: %s was not produced by %s" % (args.crash, args.exe))
            print("  crash file says: commit %s built %s" % (crash_commit, crash_built))
            print("  those build strings are not present in that exe.")
            print("  Rebuild that commit and point --exe/--pdb at it.")
            print("  Symbols below would be WRONG.")
            print("*" * 72)
            if not args.force:
                sys.exit(3)
    else:
        print("note: the crash file carries no build identity line - cannot "
              "confirm it belongs to this exe")

    with open(args.crash, "r", errors="replace") as f:
        crash_lines = f.readlines()

    rva_re = re.compile(r"exe\+0x([0-9A-Fa-f]+)")
    decoded_any = False
    for line in crash_lines:
        stripped = line.rstrip("\n")
        m = rva_re.search(stripped)
        if not m:
            print(stripped)
            continue
        rva = int(m.group(1), 16)
        result = subprocess.run(
            [symbolizer, "--obj=" + args.exe, "--relative-address",
             "--no-inlines", "--output-style=LLVM", "0x%X" % rva],
            capture_output=True, text=True).stdout.strip().splitlines()
        function = result[0].strip() if result else "??"
        location = result[1].strip() if len(result) > 1 else "??"
        print(stripped)
        print("        => %s  %s" % (function, location))
        loc_match = re.match(r"(.+):(\d+):\d*", location)
        if loc_match and "recomp" in loc_match.group(1):
            mnemonic, anchor = guest_instruction_at(args.generated, loc_match.group(1),
                                                    int(loc_match.group(2)))
            if mnemonic:
                extra = (" (%s)" % anchor) if anchor else ""
                print("        => guest instruction: %s%s" % (mnemonic, extra))
            decoded_any = True

    if not decoded_any:
        print("\n(no host frame resolved into generated code - the fault may be "
              "entirely host-side; the crash file's own rex_sub_* naming and the "
              "guest backchain still apply)")


if __name__ == "__main__":
    main()
