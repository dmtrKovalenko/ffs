#!/usr/bin/env bash

set -uo pipefail
TARGET_USER="${TARGET_USER:-${SUDO_USER:-$(logname 2>/dev/null || echo "$USER")}}"
TARGET_HOME="$(getent passwd "$TARGET_USER" 2>/dev/null | cut -d: -f6)"
TARGET_HOME="${TARGET_HOME:-/home/$TARGET_USER}"

PATTERN="${1:-${PATTERN:-TODO}}"
ITERS="${ITERS:-3}"
THREADS="${THREADS:-$(nproc)}"
MODES="${MODES:-cold warm}"

FFS_DIR="${FFS_DIR:-$TARGET_HOME/dev/ffs}"
FFS="${FFS:-$FFS_DIR/ffs}"
RG="${RG:-$(command -v rg || true)}"
OUTDIR="${OUTDIR:-$FFS_DIR/bench-out}"

# Default datasets: progressively more files. "label:absolute_path"
if [ -n "${DATASETS:-}" ]; then
    read -r -a DATASETS <<<"$DATASETS"
else
    DATASETS=(
        "repos:$TARGET_HOME/dev/fff-demo/repos"
        "dev:$TARGET_HOME/dev"
        "home:$TARGET_HOME"
    )
fi

# ripgrep flags chosen to mirror ffs semantics as closely as possible:
#   -F            fixed string (ffs is always literal)
#   --no-ignore   ffs does not honor .gitignore/.ignore
#   --hidden      ffs walks dotfiles/dotdirs too
#   --one-file-system   ffs reads a single device / stays in one fs (& subvol)
#   --no-heading -H -n  produce  path:line:content  like ffs
# (binary files are skipped by BOTH tools by default, so that stays default.)
RG_FLAGS=(-F --no-heading -H -n --no-ignore --hidden --one-file-system --no-messages)

# ----------------------------------------------------------------------------
# preflight
# ----------------------------------------------------------------------------
die() { printf 'bench: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "must run as root (try: sudo $0 ...)"
[ -n "$RG" ] && [ -x "$RG" ] || die "ripgrep (rg) not found; set RG=/path/to/rg"

if [ ! -x "$FFS" ]; then
    echo "bench: ffs binary not found at $FFS — building..." >&2
    make -C "$FFS_DIR" >/dev/null || die "failed to build ffs in $FFS_DIR"
fi

mkdir -p "$OUTDIR"
CSV="$OUTDIR/results.csv"
HTML="$OUTDIR/chart.html"

echo "bench: target user=$TARGET_USER home=$TARGET_HOME"
echo "bench: pattern='$PATTERN'  iters=$ITERS  threads=$THREADS  modes='$MODES'"
echo "bench: ffs=$FFS"
echo "bench: rg=$RG ($("$RG" --version | head -1))"
echo

# ----------------------------------------------------------------------------
# helpers
# ----------------------------------------------------------------------------
now_ns() { date +%s%N; }

human() {
    awk -v n="$1" 'BEGIN{
        if (n>=1e6) printf "%.2fM", n/1e6;
        else if (n>=1e3) printf "%.0fk", n/1e3;
        else printf "%d", n
    }'
}

drop_caches() { sync; echo 3 >/proc/sys/vm/drop_caches; }

# runners — both write matches to stdout in  path:line:content  form
run_ffs() { ( cd "$1" && OMP_NUM_THREADS="$THREADS" "$FFS" "$PATTERN" ); }
run_rg()  { "$RG" "${RG_FLAGS[@]}" -- "$PATTERN" "$1"; }

# time a runner: echoes elapsed seconds (3dp), discards all output
time_run() {
    local tool="$1" path="$2" s e
    s=$(now_ns)
    if [ "$tool" = ffs ]; then run_ffs "$path" >/dev/null 2>&1
    else                       run_rg  "$path" >/dev/null 2>&1; fi
    e=$(now_ns)
    awk -v s="$s" -v e="$e" 'BEGIN{ printf "%.3f", (e-s)/1e9 }'
}

# count matches (untimed, warm) — sanity check that both find similar amounts
count_run() {
    local tool="$1" path="$2"
    if [ "$tool" = ffs ]; then run_ffs "$path" 2>/dev/null | wc -l
    else                       run_rg  "$path" 2>/dev/null | wc -l; fi
}

median() { printf '%s\n' "$@" | sort -n |
    awk '{a[NR]=$1} END{ if(NR%2) printf "%.3f",a[(NR+1)/2];
                         else printf "%.3f",(a[NR/2]+a[NR/2+1])/2 }'; }
minv()   { printf '%s\n' "$@" | sort -n | head -1; }

# ----------------------------------------------------------------------------
# benchmark
# ----------------------------------------------------------------------------
declare -A MED   # MED[tool|mode|label] = median seconds
declare -A MIN   # MIN[tool|mode|label] = best  seconds
declare -A MATCH # MATCH[tool|label]    = match count
declare -A FILES # FILES[label]         = file count
ORDER=()         # dataset labels in display order
ENTRIES=()       # raw per-iteration rows for the csv

echo "dataset,path,files,tool,mode,iter,seconds,matches" >"$CSV"

for spec in "${DATASETS[@]}"; do
    label="${spec%%:*}"
    path="${spec#*:}"
    if [ ! -d "$path" ]; then
        echo "bench: skip '$label' — no such directory: $path" >&2
        continue
    fi
    ORDER+=("$label")

    echo "==> dataset '$label'  ($path)"
    printf '    counting files... '
    nfiles=$(find "$path" -xdev -type f 2>/dev/null | wc -l)
    FILES[$label]=$nfiles
    echo "$nfiles ($(human "$nfiles"))"

    for tool in ffs rg; do
        # one untimed warm pass for the match count
        MATCH[$tool|$label]=$(count_run "$tool" "$path")

        for mode in $MODES; do
            times=()
            for ((i=1; i<=ITERS; i++)); do
                [ "$mode" = cold ] && drop_caches
                t=$(time_run "$tool" "$path")
                times+=("$t")
                echo "$label,$path,$nfiles,$tool,$mode,$i,$t,${MATCH[$tool|$label]}" >>"$CSV"
            done
            m=$(median "${times[@]}")
            mn=$(minv "${times[@]}")
            MED[$tool|$mode|$label]=$m
            MIN[$tool|$mode|$label]=$mn
            printf '    %-4s %-4s  median=%7ss  min=%7ss  (runs: %s)\n' \
                   "$tool" "$mode" "$m" "$mn" "${times[*]}"
        done
        printf '         matches=%s\n' "${MATCH[$tool|$label]}"
    done
    echo
done

[ "${#ORDER[@]}" -gt 0 ] || die "no datasets benchmarked"

# ----------------------------------------------------------------------------
# markdown table (copy/paste into chat or docs)
# ----------------------------------------------------------------------------
echo "================================================================"
echo "RESULTS (median seconds; lower is better)"
echo "================================================================"
echo
{
    printf '| dataset | files |'
    for tool in ffs rg; do for mode in $MODES; do printf ' %s/%s |' "$tool" "$mode"; done; done
    printf ' ffs matches | rg matches |\n'
    printf '|---|---|'
    for tool in ffs rg; do for mode in $MODES; do printf '---|'; done; done
    printf '---|---|\n'
    for label in "${ORDER[@]}"; do
        printf '| %s | %s |' "$label" "$(human "${FILES[$label]}")"
        for tool in ffs rg; do for mode in $MODES; do
            printf ' %s |' "${MED[$tool|$mode|$label]}"
        done; done
        printf ' %s | %s |\n' "${MATCH[ffs|$label]}" "${MATCH[rg|$label]}"
    done
} | column -t -s'|' -o'|'
echo

# ----------------------------------------------------------------------------
# ASCII chart — bars scaled to the global max median
# ----------------------------------------------------------------------------
gmax=0
for label in "${ORDER[@]}"; do
    for tool in ffs rg; do for mode in $MODES; do
        v=${MED[$tool|$mode|$label]}
        gmax=$(awk -v a="$gmax" -v b="$v" 'BEGIN{print (b>a)?b:a}')
    done; done
done

for label in "${ORDER[@]}"; do
    echo
    echo "[$label — $(human "${FILES[$label]}") files]"
    for tool in ffs rg; do for mode in $MODES; do
        v=${MED[$tool|$mode|$label]}
        bar=$(awk -v v="$v" -v m="$gmax" 'BEGIN{
            w=(m>0)?int(v/m*50+0.5):0; s=""; for(i=0;i<w;i++) s=s"#"; print s }')
        printf '  %-4s %-4s |%-50s| %ss\n' "$tool" "$mode" "$bar" "$v"
    done; done
done
echo

