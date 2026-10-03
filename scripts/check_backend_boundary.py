#!/usr/bin/env python3
"""check_backend_boundary.py - the FLIP GATE, measured from the link artifacts.

WHAT THIS IS
    `dynamic_link` can only replace load-time binding once nothing on the engine side
    references a symbol that `deren_vulkan` defines. That set - not the number of `vk*`
    spellings in the source - is what has to reach zero before the flip. This script
    measures exactly that set:

        symbols DEFINED in deren_vulkan  INTERSECT  symbols UNDEFINED in vulkancorekit

    and it measures it from the ARCHIVES, not from the source and not from the object
    directories. The object directories are not trustworthy here: `build-release-clang64/
    CMakeFiles/vulkancorekit.dir` still holds pre-split objects from before S1-A, and a
    directory-level count reports 130 where the archive-level truth is 78.

WHY A RATCHET
    The same discipline as the 13 frozen render hashes: a checked-in baseline, and the
    gate fails when the number GROWS. While the migration is in flight the count falls;
    lower the baseline with `--update` each time, and the boundary can never quietly
    re-acquire a dependency. At the flip the baseline is 0 and the script is the proof.

THE SECOND NUMBER: OWNING STL ACROSS THE BOUNDARY
    A symbol whose signature carries `std::vector` / `std::string` / an allocator is an
    allocation that one image makes and the other frees (today:
    `init_utils::create_host_buffers(..., std::vector<vk_buffer>&, ...)`). The contract
    forbids it (plan_rhi_v4.md §4.2: no STL across the boundary), so the count is tracked
    as a ratchet of its own - it is the one part of the migration where "it links" is not
    the same as "it is safe".

MEASURED 2026-10-03, build-release-clang64 (Release clang64):
    78 symbols / 227 reference sites / 31 archive members; 0 in the reverse direction;
    1 of the 78 carries owning STL.

BASELINES ARE PER TOOLCHAIN (the symbol sets are not comparable across them):
    backend_boundary_baseline.mingw.json   <- the measured one, from the clang64 tree
    backend_boundary_baseline.msvc.json    <- record before gating the MSVC tree
    backend_boundary_baseline.posix.json   <- same for a Linux tree
    A tree with no baseline records nothing and never fails; `--update` writes one.

USAGE
    python scripts/check_backend_boundary.py                       # gate against the baseline
    python scripts/check_backend_boundary.py --update              # ratchet down to today
    python scripts/check_backend_boundary.py --list                # the worklist, demangled
    python scripts/check_backend_boundary.py --warn                # report, never fail (CI's first phase)
    python scripts/check_backend_boundary.py --build-dir DIR
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from collections import defaultdict

BACKEND_TARGET = "deren_vulkan"
KIT_TARGET = "vulkancorekit"

# Where the two halves land, per toolchain. MinGW/clang64 archives first, MSVC after.
ARCHIVE_NAMES = {
    BACKEND_TARGET: ("libderen_vulkan.a", "deren_vulkan.lib"),
    KIT_TARGET: ("libvulkancorekit.a", "vulkancorekit.lib"),
}

# The nm to use. llvm-nm reads both GNU archives and MSVC .lib; plain nm is the fallback.
NM_CANDIDATES = ("llvm-nm", "llvm-nm-18", "llvm-nm-17", "nm")
FILT_CANDIDATES = ("llvm-cxxfilt", "llvm-cxxfilt-18", "c++filt")

# Reference sites are attributed to an area by the member name they come from. This is
# reporting only; the gate itself is the SET of symbols.
AREA_PATTERNS = (
    ("runtime", re.compile(r"^runtime")),
    ("pass", re.compile(r"^(pass|taa|scene|transparent|character_forward|toon_screen_rim|"
                        r"goo_rim|megalights|ray_traced_shadow|mask_bake|compute_skin|cluster|"
                        r"deferred|post|fxaa|upscale|geometry_buffer_debug|shadow|chain)")),
)

# An allocation that crosses the boundary: libc++'s std::vector / std::string / allocator,
# and their libstdc++ spellings, appear in the MANGLED name of any symbol that passes one.
OWNING_STL_RE = re.compile(r"(6vector|12basic_string|9allocator|6string)")


def category(symbol: str) -> str:
    if re.search(r"13vma_allocator", symbol):
        return "core::vma_allocator"
    if re.search(r"15descriptor_heap", symbol):
        return "core::descriptor_heap"
    if re.search(r"10init_utils", symbol):
        return "init_utils"
    if re.search(r"8pipeline", symbol):
        return "core::pipeline"
    if re.search(r"7filters", symbol):
        return "core::filters"
    # The generic handle bucket comes LAST of the specific ones but before core::core:
    # a handle symbol's signature often spells `core` as well.
    if re.search(r"7handles", symbol) or re.search(r"(8vk_image|9vk_buffer)", symbol):
        return "native RAII handle (vk_*)"
    if re.search(r"4core4core", symbol):
        return "core::core member"
    if symbol.startswith("_ZGI") or symbol.startswith("_ZGV"):
        return "data: init/guard"
    return "other"


def carries_owning_stl(symbol: str) -> bool:
    return OWNING_STL_RE.search(symbol) is not None


def find_tool(candidates) -> str | None:
    for name in candidates:
        path = shutil.which(name)
        if path:
            return path
    return None


def resolve_archive(build_dir: str, target: str) -> str:
    for name in ARCHIVE_NAMES[target]:
        candidate = os.path.join(build_dir, name)
        if os.path.isfile(candidate):
            return candidate
    searched = ", ".join(ARCHIVE_NAMES[target])
    raise SystemExit(
        f"check_backend_boundary: no archive for '{target}' in {build_dir!r} "
        f"(looked for {searched}); build the tree first, or pass --build-dir"
    )


def flavor_of(build_dir: str) -> str:
    """Which baseline this tree's symbol sets are comparable with.

    The mangled names, and therefore the counts, differ per toolchain and per standard
    library, so the baseline is keyed rather than shared.
    """
    if any(os.path.isfile(os.path.join(build_dir, name)) for name in ARCHIVE_NAMES[BACKEND_TARGET][1:]):
        return "msvc"
    return "mingw" if sys.platform.startswith("win") else "posix"


MEMBER_RE = re.compile(r"^(?P<member>[^:\[\]]+\.(?:obj|o)):\s*$")


def read_symbols(archive: str, nm: str, *, defined: bool) -> tuple[dict[str, list[str]], set[str]]:
    """Return ({symbol: [members]}, {members}) for one archive.

    nm's archive output interleaves member headers ("name.obj:") with the member's
    symbols, so the header line is the current member for every symbol that follows.
    """
    flag = "--defined-only" if defined else "--undefined-only"
    proc = subprocess.run([nm, flag, "--no-demangle", archive],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit(f"check_backend_boundary: {nm} failed on {archive}\n{proc.stderr.strip()}")

    by_symbol: dict[str, list[str]] = defaultdict(list)
    members: set[str] = set()
    member = "<archive>"
    for line in proc.stdout.splitlines():
        if not line.strip():
            continue
        header = MEMBER_RE.match(line)
        if header and " " not in line.strip():
            member = os.path.basename(header.group("member"))
            members.add(member)
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        # "                 U symbol" -> last field; "0000 T symbol" -> last field too.
        symbol = parts[-1]
        if symbol in ("U", "*"):
            continue
        if member not in by_symbol[symbol]:
            by_symbol[symbol].append(member)
    return by_symbol, members


def area_of(member: str) -> str:
    for name, pattern in AREA_PATTERNS:
        if pattern.match(member):
            return name
    return "other"


def main() -> int:
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    scripts_dir = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default=os.path.join(repo_root, "build-release-clang64"),
                        help="the build tree holding both archives")
    parser.add_argument("--baseline", default=None,
                        help="baseline json (default: per-toolchain, see the module docstring)")
    parser.add_argument("--update", action="store_true", help="rewrite the baseline to today's count")
    parser.add_argument("--list", action="store_true", help="print the worklist, demangled")
    parser.add_argument("--warn", action="store_true",
                        help="report a growth but exit 0 (the first phase of the CI wiring)")
    parser.add_argument("--quiet", action="store_true", help="only report the verdict")
    args = parser.parse_args()

    if args.baseline is None:
        args.baseline = os.path.join(scripts_dir, f"backend_boundary_baseline.{flavor_of(args.build_dir)}.json")

    nm = find_tool(NM_CANDIDATES)
    if not nm:
        raise SystemExit("check_backend_boundary: no nm found (looked for "
                         + ", ".join(NM_CANDIDATES) + ")")
    filt = find_tool(FILT_CANDIDATES)

    backend_archive = resolve_archive(args.build_dir, BACKEND_TARGET)
    kit_archive = resolve_archive(args.build_dir, KIT_TARGET)

    # A HALF-BUILT TREE UNDER-REPORTS, AND IT DID: rebuilding `deren_vulkan` alone leaves
    # `vulkancorekit.a` referencing symbols the backend no longer defines, so those references drop
    # out of the intersection and the count FALLS without a line of engine code having changed
    # (measured: 78 -> 76 that way, against 78 -> 77 for the change that was actually made). The
    # engine archive is the one that has to be at least as new as the backend's, so that is the
    # direction checked - a warning rather than a failure, because measuring a half-built tree on
    # purpose is legitimate.
    if os.path.getmtime(kit_archive) < os.path.getmtime(backend_archive) - 300.0:
        print("WARNING: the engine archive is more than 5 minutes older than the backend's, so this "
              "tree may be half-built and the count may UNDER-report. Rebuild both halves before "
              "believing the number.")

    defined, _ = read_symbols(backend_archive, nm, defined=True)
    undefined, kit_members = read_symbols(kit_archive, nm, defined=False)

    cross = {s: undefined[s] for s in undefined if s in defined}
    usages = sum(len(undefined[s]) for s in cross)

    symbols = sorted(cross)
    owning = [s for s in symbols if carries_owning_stl(s)]
    report = {
        "backend_archive": os.path.basename(backend_archive),
        "defined_in_backend": len(defined),
        "kit_archive": os.path.basename(kit_archive),
        "kit_members": len(kit_members),
        "cross_boundary_symbols": symbols,
        "count": len(symbols),
        "usages": usages,
        "owning_stl_symbols": owning,
        "owning_stl_count": len(owning),
    }

    if args.update:
        with open(args.baseline, "w", encoding="utf-8") as handle:
            json.dump(report, handle, indent=2, sort_keys=True)
            handle.write("\n")
        print(f"check_backend_boundary: baseline updated to {len(symbols)} symbols "
              f"({usages} sites, {len(owning)} with owning STL, {len(kit_members)} members)"
              f" -> {args.baseline}")
        return 0

    baseline = None
    if os.path.isfile(args.baseline):
        with open(args.baseline, encoding="utf-8") as handle:
            baseline = json.load(handle)

    if not args.quiet:
        print(f"backend  {os.path.basename(backend_archive):<24} "
              f"{len(defined)} defined symbols")
        print(f"engine   {os.path.basename(kit_archive):<24} "
              f"{len(kit_members)} members, {len(cross)} of them reach into the backend")
        print(f"usage    {usages} reference sites; {len(owning)} symbol(s) carry owning STL")
        print()

        by_category: dict[str, int] = defaultdict(int)
        for symbol in symbols:
            by_category[category(symbol)] += 1
        print("by category (symbols):")
        for name, count in sorted(by_category.items(), key=lambda kv: -kv[1]):
            print(f"    {count:>4}  {name}")

        by_area: dict[str, int] = defaultdict(int)
        for symbol in symbols:
            seen = set()
            for member in cross[symbol]:
                area = area_of(member)
                if (area, member) not in seen:
                    by_area[area] += 1
                    seen.add((area, member))
        print("by area (symbol/member pairs, one symbol can appear in several):")
        for name, count in sorted(by_area.items(), key=lambda kv: -kv[1]):
            print(f"    {count:>4}  {name}")
        print()

    if args.list:
        print("worklist:")
        for symbol in symbols:
            shown = symbol
            if filt:
                proc = subprocess.run([filt, symbol], capture_output=True, text=True)
                if proc.returncode == 0 and proc.stdout.strip():
                    shown = proc.stdout.strip()
            marker = "  [owning STL]" if carries_owning_stl(symbol) else ""
            print(f"    {shown}{marker}")
            print(f"        <- {', '.join(sorted(set(cross[symbol])))}")
        print()

    if baseline is None:
        print(f"check_backend_boundary: no baseline at {args.baseline}; "
              f"run --update to record today's {len(symbols)}")
        return 0

    allowed = int(baseline.get("count", 0))
    allowed_owning = int(baseline.get("owning_stl_count", 0))
    failed = False

    if len(symbols) > allowed:
        added = sorted(set(symbols) - set(baseline.get("cross_boundary_symbols", [])))
        print(f"FAIL: the engine reaches into the backend with {len(symbols)} symbols, "
              f"baseline is {allowed}")
        for symbol in added[:20]:
            print(f"    NEW  {symbol}")
        if len(added) > 20:
            print(f"    ... and {len(added) - 20} more")
        failed = True

    if len(owning) > allowed_owning:
        print(f"FAIL: {len(owning)} symbols carry an owning STL type across the boundary, "
              f"baseline is {allowed_owning} (plan §4.2: no STL across the boundary)")
        for symbol in owning:
            print(f"    OWNING  {symbol}")
        failed = True

    if failed:
        return 0 if args.warn else 1

    if len(symbols) < allowed:
        print(f"OK (improved): {len(symbols)} symbols, baseline still {allowed} - "
              f"run --update to ratchet the baseline down")
        return 0

    print(f"OK: {len(symbols)} symbols, baseline {allowed}, {usages} reference sites, "
          f"{len(owning)} with owning STL")
    if allowed == 0:
        print("the flip gate is closed: nothing on the engine side references a backend symbol")
    return 0


if __name__ == "__main__":
    sys.exit(main())
