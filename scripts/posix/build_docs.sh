#!/bin/sh
# Build the project documentation: Doxygen HTML (docs/html) + the LaTeX manual
# compiled to PDF (docs/latex/refman.pdf).
# POSIX sh (Linux / WSL / macOS / MSYS2 shell): run it from anywhere; the
# script resolves the repo root. Usage:
#     sh scripts/posix/build_docs.sh
# Requires: doxygen on PATH (or the standard Windows/Unix install dirs) and a
# TeX toolchain — make + pdflatex/makeindex, latexmk, or bare pdflatex
# (MiKTeX's per-user install under %LOCALAPPDATA% is found automatically).
#
# The LaTeX steps run SILENTLY: the (very chatty) pdflatex/make/makeindex
# stdout+stderr goes to a throwaway log file instead of the console. On
# failure the tail of that log is printed so a broken build still says why;
# pdflatex additionally keeps its full transcript in docs/latex/refman.log.

set -eu

# project root = two levels up from scripts/<platform>/ holding this script
root=$(CDPATH= cd "$(dirname "$0")/../.." && pwd)
cd "$root"

# normalize a (possibly Windows-style) path for POSIX tests
to_posix() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -u "$1" 2>/dev/null || printf '%s' "$1" | tr '\\' '/'
    else
        printf '%s' "$1" | tr '\\' '/'
    fi
}

# ---------- 1. doxygen: HTML + LaTeX sources ----------
doxygen_cmd=""
if command -v doxygen >/dev/null 2>&1; then
    doxygen_cmd=$(command -v doxygen)
else
    for candidate in "/c/Program Files/doxygen/bin/doxygen.exe" "/usr/bin/doxygen" "/opt/homebrew/bin/doxygen"; do
        if [ -x "$candidate" ]; then
            doxygen_cmd=$candidate
            break
        fi
    done
fi
if [ -z "$doxygen_cmd" ]; then
    echo "error: doxygen not found on PATH (install Doxygen or add its bin dir)." >&2
    exit 1
fi

echo "== doxygen: $doxygen_cmd =="
"$doxygen_cmd" Doxyfile
echo "html written to docs/html/index.html"

# ---------- 2. LaTeX manual -> refman.pdf ----------
# put a TeX toolchain on PATH when it lives at a standard location
add_tex_to_path() {
    [ -n "$1" ] || return 0
    d=$(to_posix "$1")
    [ -d "$d" ] || return 0
    case ":$PATH:" in
        *":$d:"*) ;;
        *) PATH="$d:$PATH" ;;
    esac
}
# manual rerun loop replicating the generated Makefile: pdflatex, makeindex,
# repeat pdflatex while the log asks for another pass, makeindex, pdflatex
# (each pass is captured to latex_pass.log; failures show its tail)
#
# "ASKS FOR ANOTHER PASS" means LaTeX's and hyperref's prose requests ("Rerun to
# get cross-references right", "Rerun to get outlines right", "Label(s) may have
# changed"), NOT the bare word: the rerunfilecheck package prints
# "... Rerun checks for auxiliary files (HO)" in its banner on every pass, so a
# grep for "Rerun" never stopped, the loop always ran to its cap and a converged
# manual was typeset ten times (~10 minutes, eight passes changing nothing).
rerun_wanted() {
    grep -qs -E "Rerun to get|may have changed" refman.log
}
# the auxiliary state a pass converges; unchanged after a pass means that pass
# changed nothing, whatever its log says
aux_fingerprint() {
    for f in refman.aux refman.toc refman.out refman.idx; do
        if [ -f "$f" ]; then md5sum "$f" 2>/dev/null || cksum "$f"; else echo "-"; fi
    done
}
run_pdflatex() { # $1 = DRAFT for the passes that do not write the PDF
    if [ "$1" = "DRAFT" ]; then
        set -- -draftmode
    else
        set --
    fi
    if pdflatex "$@" -interaction=nonstopmode -halt-on-error refman.tex >latex_pass.log 2>&1; then
        return 0
    fi
    echo "error: pdflatex failed (see docs/latex/refman.log for the full transcript):" >&2
    tail -n 30 latex_pass.log >&2
    exit 1
}
run_makeindex() {
    if [ -f refman.idx ] && command -v makeindex >/dev/null 2>&1; then
        if makeindex refman.idx >latex_makeindex.log 2>&1; then
            rm -f latex_makeindex.log
        else
            echo "error: makeindex failed:" >&2
            tail -n 20 latex_makeindex.log >&2
            exit 1
        fi
    fi
}
compile_latex_manually() {
    # only the last pass writes the PDF; the earlier ones run in -draftmode and
    # still produce the .aux/.toc/.out/.idx state they exist to converge
    run_pdflatex DRAFT
    run_makeindex
    count=0
    fingerprint=$(aux_fingerprint)
    while rerun_wanted; do
        run_pdflatex DRAFT
        count=$((count + 1))
        state=$(aux_fingerprint)
        if [ "$state" = "$fingerprint" ] || [ "$count" -ge 4 ]; then
            break
        fi
        fingerprint=$state
    done
    run_makeindex
    run_pdflatex
    rm -f latex_pass.log
}

add_tex_to_path "${LOCALAPPDATA:-}/Programs/MiKTeX/miktex/bin/x64"
add_tex_to_path "${PROGRAMFILES:-}/MiKTeX/miktex/bin/x64"
# TeX Live installs versioned bin dirs: <root>/<year>/bin/<arch>
for texlive_root in "/c/texlive" "/usr/local/texlive" "/opt/texlive"; do
    for d in "$texlive_root"/*/bin/*; do
        [ -d "$d" ] && add_tex_to_path "$d"
    done
done

cd docs/latex

if command -v make >/dev/null 2>&1; then
    # doxygen generates docs/latex/Makefile with 'all' -> refman.pdf
    echo "== latex via make (docs/latex/Makefile, output suppressed) =="
    if make >latex_make.log 2>&1; then
        rm -f latex_make.log
    else
        rc=$?
        echo "error: make failed (exit $rc), tail of docs/latex/latex_make.log:" >&2
        tail -n 40 latex_make.log >&2
        exit "$rc"
    fi
elif command -v pdflatex >/dev/null 2>&1; then
    echo "== latex via pdflatex (no make found, output suppressed) =="
    compile_latex_manually
else
    echo "error: no LaTeX toolchain found. Install MiKTeX/TeX Live (or make), then rerun." >&2
    exit 1
fi

echo
echo "documentation built:"
echo "  html: docs/html/index.html"
echo "  pdf:  docs/latex/refman.pdf"
