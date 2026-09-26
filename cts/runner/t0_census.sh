#!/usr/bin/env bash
# T0 — the symbol census, for one platform.
#
#   cts/runner/t0_census.sh <platform>        # e.g. posix, byai, hub8735
#
# Compiles the generated census against the platform's declared backends and
# LINKS it, so a seam function with no implementation shows up here, on a
# host, rather than on the board.  Reports every missing symbol at once: ld
# lists all unresolved references, which is the reason this links rather than
# running nm over objects one at a time.
#
# What a green T0 means: the backend is COMPLETE.  It says nothing about
# whether any of it works -- the BlueZ backend had every symbol and deadlocked
# on its first write.  Do not quote a green census as evidence about
# behaviour.
#
# The platform's declaration lives in cts/platforms/<platform>.yaml.  Only a
# few flat keys are read, with grep rather than a YAML library, so this has no
# dependency beyond a shell and the toolchain; anything structural in those
# files is for the matrix generator, not for this script.
set -uo pipefail
cd "$(dirname "$0")/../.."          # repo root

PLATFORM="${1:-}"
[ -n "$PLATFORM" ] || { echo "usage: $0 <platform>" >&2; exit 2; }
DECL="cts/platforms/$PLATFORM.yaml"
[ -r "$DECL" ] || { echo "t0: no declaration at $DECL" >&2; exit 2; }

# --- read the declaration -------------------------------------------------
# "extends: <parent>" takes every BUILD key (cc, nm, cflags, ldflags,
# linkable, defines, skip_groups, sources, known_missing) from the parent
# declaration unless this file sets it.  Two boards on one SoC share a
# backend, so duplicating twenty lines of SDK flags would only let them
# drift -- but they get separate declarations so that results are recorded
# against the board that actually earned them.
PARENT=$(sed -n 's/^extends:[[:space:]]*//p' "$DECL" | sed 's/[[:space:]]#.*$//' | head -1)
if [ -n "$PARENT" ]; then
    PDECL="cts/platforms/$PARENT.yaml"
    [ -r "$PDECL" ] || { echo "t0: $DECL extends $PARENT, but $PDECL is missing" >&2; exit 2; }
    MERGED="${TMPDIR:-/tmp}/nn-cts-decl-$PLATFORM.yaml"
    # child keys first: val() and list() take the FIRST match, so the child wins
    cat "$DECL" "$PDECL" > "$MERGED"
    DECL_SHOWN="$DECL (extends $PARENT)"
    DECL="$MERGED"
fi
# Deliberately small YAML: flat scalars and one level of "- item" lists.
# Inline "# ..." comments MUST be stripped -- leaving them in silently
# produced defines like -DNN_CTS_SKIP_NN_OSAL_SYNC_H#FREERTOS..., i.e. a
# census that skipped nothing it claimed to and something it did not. If a
# declaration ever needs more structure than this, parse it with a real YAML
# library rather than extending these two lines.
strip_comment() { sed 's/[[:space:]]#.*$//; s/[[:space:]]*$//'; }
val() {
    v=$(sed -n "s/^$1:[[:space:]]*//p" "$DECL" | head -1 | strip_comment | tr -d '"')
    # A lone > or | is a YAML block scalar, which this parser does not read.
    # Use the list form instead; returning the marker would hand the compiler
    # a literal ">" as a filename, which is how this was found.
    case "$v" in ">"|"|"|">-"|"|-") v="" ;; esac
    printf '%s' "$v"
}

# Paths to a vendor SDK do NOT belong in a tracked file: an absolute path in a
# declaration is the bug that broke the BeagleY image build on every machine
# but one, twice.  Declarations write ${NN_SOMETHING} and the environment
# supplies it, so a checkout builds anywhere the toolchain is installed.
expand() {
    out=$1
    while :; do
        case "$out" in
            *'${'*)
                var=${out#*\$\{}; var=${var%%\}*}
                eval "sub=\${$var-}"
                out=$(printf '%s' "$out" | sed "s|\${$var}|$sub|g")
                ;;
            *) break ;;
        esac
    done
    printf '%s' "$out"
}

# Check every ${VAR} the declaration mentions BEFORE expanding any of them.
# This has to run in the main shell: expand() is called inside $(...), and an
# exit there kills only the subshell -- the script sails on with the variable
# expanded to nothing and fails later with a confusing "FreeRTOS.h: No such
# file", which is exactly what happened the first time.
missing_env=""
for var in $(grep -oE '\$\{[A-Za-z_][A-Za-z_0-9]*\}' "$DECL" | tr -d '${}' | sort -u); do
    eval "v=\${$var-__NN_UNSET__}"
    [ "$v" = __NN_UNSET__ ] && missing_env="$missing_env $var"
done
if [ -n "$missing_env" ]; then
    echo "t0: $DECL needs environment variable(s):$missing_env" >&2
    for var in $missing_env; do
        echo "t0:   export $var=/path/to/sdk" >&2
    done
    echo "t0: declarations reference vendor SDKs by variable on purpose -- an" >&2
    echo "t0: absolute path in a tracked file breaks the build on every" >&2
    echo "t0: machine but the one it was written on." >&2
    exit 3
fi
list() {
    # FIRST "key:" block only.  With "extends:" the child is concatenated
    # ahead of the parent, and a sed range would print BOTH blocks -- merging a
    # child's list into its parent's instead of overriding it.
    awk -v k="$1" '
        index($0, k":") == 1 { inblk = 1; next }
        inblk && /^[a-z_]+:/ { exit }
        inblk' "$DECL" \
    | sed -n 's/^[[:space:]]*-[[:space:]]*//p' \
    | strip_comment | sed '/^$/d'
}

CC=$(val cc);            CC=${CC:-cc}
EXTRA_CFLAGS=$(val cflags)
[ -n "$EXTRA_CFLAGS" ] || EXTRA_CFLAGS=$(list cflags | tr '\n' ' ')
EXTRA_CFLAGS=$(expand "$EXTRA_CFLAGS")
EXTRA_LDFLAGS=$(val ldflags)
[ -n "$EXTRA_LDFLAGS" ] || EXTRA_LDFLAGS=$(list ldflags | tr '\n' ' ')
EXTRA_LDFLAGS=$(expand "$EXTRA_LDFLAGS")
LINKABLE=$(val linkable); LINKABLE=${LINKABLE:-yes}
# Cross targets: nm on the compiled objects instead of a hosted link.
NM=$(val nm); NM=${NM:-${CC%gcc}nm}

DEFINES=""; for d in $(list defines); do DEFINES="$DEFINES -D$d"; done

# Validate every skip group against a real header.  A typo here would skip
# nothing while looking like it skipped something, which is the same silent
# narrowing the comment above is about -- so it is a hard error.
SKIPS=""
for g in $(list skip_groups); do
    seam=${g%%/*}; hdr=${g#*/}
    if [ ! -r "modules/libs/$seam/include/$seam/$hdr" ]; then
        echo "t0: $DECL skips '$g', which is not a seam header." >&2
        echo "t0: expected modules/libs/$seam/include/$seam/$hdr" >&2
        exit 2
    fi
    SKIPS="$SKIPS -DNN_CTS_SKIP_$(echo "$g" | tr 'a-z/.' 'A-Z__')"
done
SRCS=""; for f in $(list sources); do
    # shellcheck disable=SC2086
    m=$(echo $f); [ -n "$m" ] && SRCS="$SRCS $m"
done

command -v "$CC" >/dev/null 2>&1 || {
    echo "t0: $PLATFORM declares cc=$CC, which is not on PATH." >&2
    echo "t0: this platform's census needs its vendor toolchain; install it" >&2
    echo "t0: or run the census in the container that has it." >&2
    exit 3
}

echo "── T0 symbol census: $PLATFORM  (cc=$CC)"

# --- the census must match the headers ------------------------------------
# A stale census reports "complete" about symbols it no longer covers, which
# is the one way this tier can lie while staying green.
python3 cts/runner/gen_t0_symbols.py --check || exit 1

INC="-I modules/libs/nn_osal/include -I modules/libs/nn_pal/include
     -I modules/libs/nn_infer/include -I modules/libs"
OUT="${TMPDIR:-/tmp}/nn-cts-t0-$PLATFORM"
mkdir -p "$OUT"

# --- objects mode: completeness WITHOUT a full firmware link ---------------
# A freestanding target cannot link a hosted main(), and its real link needs
# the vendor's linker script and libraries.  But completeness does not need a
# link at all: compile the census and the backend to objects, then ask whether
# every seam symbol the census REFERENCES is DEFINED somewhere in the backend.
# That is the same question, answered with nm, and it gives a cross target a
# real verdict instead of "inconclusive".
# shellcheck disable=SC2086
if [ "$LINKABLE" = objects ]; then
    command -v "$NM" >/dev/null 2>&1 || {
        echo "t0: objects mode needs nm ($NM), which is not on PATH" >&2; exit 3; }
    rm -rf "$OUT/obj"; mkdir -p "$OUT/obj"
    $CC $EXTRA_CFLAGS $DEFINES $SKIPS $INC -c cts/runner/t0_symbols.c \
        -o "$OUT/obj/census.o" || exit 1
    nsrc=0
    for f in $SRCS; do
        $CC $EXTRA_CFLAGS $DEFINES $INC -c "$f" \
            -o "$OUT/obj/$(basename "${f%.c}").o" || exit 1
        nsrc=$((nsrc + 1))
    done
    [ "$nsrc" -gt 0 ] || {
        echo "t0: $DECL declares no sources, so there is nothing to census" >&2
        echo "t0: (an empty backend would otherwise 'pass' by having nothing" >&2
        echo "t0:  to be missing from)" >&2
        exit 2; }

    "$NM" --undefined-only "$OUT/obj/census.o" \
        | awk '{print $NF}' | grep -E '^nn_(osal|pal|infer)_' | sort -u > "$OUT/want.txt"
    "$NM" --defined-only "$OUT/obj"/*.o \
        | awk '{print $NF}' | grep -E '^nn_(osal|pal|infer)_' | sort -u > "$OUT/have.txt"
    comm -23 "$OUT/want.txt" "$OUT/have.txt" > "$OUT/missing.txt"
    echo "   census references $(grep -c . "$OUT/want.txt") seam symbols;" \
         "backend defines $(grep -c . "$OUT/have.txt") across $nsrc file(s)"
    nmiss=$(grep -c . "$OUT/missing.txt")
    rc=0
fi

# shellcheck disable=SC2086
if [ "$LINKABLE" = no ]; then
    # Cross targets whose final link needs the vendor's own linker script and
    # libraries: compiling the census still proves the headers are coherent
    # and the declared skips are right, which is worth having before the
    # board arrives.  It does NOT prove the symbols exist.
    echo "   (declaration says linkable: no — compiling only)"
    $CC $EXTRA_CFLAGS $DEFINES $SKIPS $INC -c cts/runner/t0_symbols.c \
        -o "$OUT/t0.o" || exit 1
    echo "   census COMPILES; link census not run, so completeness is UNPROVEN"
    echo "   T0: INCONCLUSIVE for $PLATFORM (compile-only)"
    exit 0
fi

if [ "$LINKABLE" = yes ]; then
cat > "$OUT/t0_main.c" <<'EOF'
#include <stdio.h>
#include <stddef.h>
struct nn_cts_symbol { const char *seam, *header, *name; void *addr; };
extern const struct nn_cts_symbol nn_cts_symbols[];
extern const size_t nn_cts_symbol_count;
extern const char *const nn_cts_skipped[];
int main(void)
{
    size_t n = 0;
    for (size_t i = 0; i < nn_cts_symbol_count; i++)
        if (nn_cts_symbols[i].addr) n++;
    printf("   %zu/%zu censused symbols have an address\n",
           n, nn_cts_symbol_count);
    for (const char *const *s = nn_cts_skipped; *s; s++)
        printf("   SKIPPED GROUP (declared, not censused): %s\n", *s);
    return n == nn_cts_symbol_count ? 0 : 1;
}
EOF

# shellcheck disable=SC2086
$CC $EXTRA_CFLAGS $DEFINES $SKIPS $INC \
    cts/runner/t0_symbols.c "$OUT/t0_main.c" $SRCS \
    $EXTRA_LDFLAGS -o "$OUT/t0" 2> "$OUT/link.err"
rc=$?

grep -oE "undefined reference to \`[A-Za-z_][A-Za-z_0-9]*'" "$OUT/link.err" \
    | sed "s/undefined reference to .//; s/'$//" | sort -u > "$OUT/missing.txt"
nmiss=$(grep -c . "$OUT/missing.txt")

if [ "$rc" -ne 0 ] && [ "$nmiss" -eq 0 ]; then
    echo "   link failed for a reason other than missing seam symbols:" >&2
    tail -20 "$OUT/link.err" >&2
    exit 1
fi
fi

# Symbols the platform declares as known-absent, with a reason, in its yaml.
# Recording them lets T0 gate NEW holes without a red CI on the day it is
# introduced -- and unlike a silent pass, every one of them is written down in
# the declaration and shows in the matrix.  A recorded gap that later gets
# implemented also fails, so the list cannot rot.
list known_missing > "$OUT/known.txt"
nknown=$(grep -c . "$OUT/known.txt" || true)
comm -23 "$OUT/missing.txt" <(sort "$OUT/known.txt") > "$OUT/new.txt"
comm -13 "$OUT/missing.txt" <(sort "$OUT/known.txt") > "$OUT/fixed.txt"
nnew=$(grep -c . "$OUT/new.txt"); nfixed=$(grep -c . "$OUT/fixed.txt")

if [ "$nknown" -gt 0 ]; then
    echo "   $nknown recorded gap(s) in $DECL:"
    awk '{ printf "     (known) %s\n", $0 }' "$OUT/known.txt"
fi

if [ "$nfixed" -gt 0 ]; then
    echo "   $nfixed recorded gap(s) NO LONGER missing -- remove them from"
    echo "   known_missing in $DECL so the list keeps meaning something:"
    awk '{ printf "     %s\n", $0 }' "$OUT/fixed.txt"
    echo "   T0: FAIL for $PLATFORM (stale known_missing)"
    exit 1
fi

if [ "$nnew" -gt 0 ]; then
    echo "   $nnew NEW declared seam function(s) have NO implementation:"
    awk '{ printf "     %s\n", $0 }' "$OUT/new.txt"
    echo
    echo "   Each of these is a MISSING SYMBOL, not a call that returns"
    echo "   -ENOTSUP.  The contract is that a backend which cannot do"
    echo "   something still defines it and returns -ENOTSUP: a missing symbol"
    echo "   is indistinguishable from a typo, while an explicit -ENOTSUP is a"
    echo "   decision a reader can check.  Either implement them, stub them"
    echo "   -ENOTSUP, or declare the whole group in skip_groups so the"
    echo "   narrowing is recorded in $DECL."
    echo "   T0: FAIL for $PLATFORM"
    exit 1
fi

if [ "$nmiss" -gt 0 ]; then
    echo "   T0: PASS for $PLATFORM with $nmiss recorded gap(s)"
    echo "   (complete except where declared; says nothing about behaviour)"
    exit 0
fi

[ "$LINKABLE" = yes ] && { "$OUT/t0" || exit 1; }
echo "   T0: PASS for $PLATFORM (complete, says nothing about behaviour)"
