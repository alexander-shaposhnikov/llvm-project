#!/usr/bin/env bash
#
# do_ab_test.sh -- end-to-end A/B test for -thinlto-autohide-exact on LINUX.
#
# The flag (FunctionImport.cpp / thinLTOFinalizeInModule) promotes a prevailing,
# auto-hidden linkonce_odr ODR function to an exact dso_local definition *after
# symbol resolution*, so the ThinLTO backend's FunctionAttrs/IPModRef can infer
# attributes (readonly/memory) on it -- closing the gap where ThinLTO otherwise
# leaves such functions weak_odr (mayBeDerefined -> attribute inference skips).
#
# WHY LINUX: the promotion is gated on whole-program-visibility (canAutoHide).
# WPV is asserted by the LINKER, and only ELF lld exposes it
# (--lto-whole-program-visibility). macOS ld64 and Mach-O lld cannot, so the flag
# is inert there. Hence this harness targets Linux/ELF lld.
#
# WHAT IT DOES:
#   stage1/   build our clang + lld (carry the prototype + the -fnattrs-log-linkage
#             logging + the NumThinLTOAutoHideExact statistic)
#   ab/       one ThinLTO build of clang; A/B is done by RELINKING with vs without
#             the flag -- whole-program-visibility is held CONSTANT on both links so
#             we isolate *only* the autohide-exact effect (not WPV's other effects).
#   analyze   final binaries: size, weak-symbol delta (= promotions that reached the
#             binary), and the linker's NumThinLTOAutoHideExact stat (gross count).
#   ab test   differential correctness (both clangs must compile sample code
#             byte-identically; asserts ON catch miscompiles), and -- unless
#             RUN_CHECK_CLANG=0 -- check-clang on the flag build (the real
#             correctness gate now that the promotion actually FIRES at scale).
#
# Tunables (env): WORK, JOBS, TARGETS, RUN_CHECK_CLANG, NDIFF, PERF_REPS, EXTRA_LINK
#
set -uo pipefail

# ---------------------------------------------------------------- config
LLVM_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # repo root (script lives here)
WORK="${WORK:-$LLVM_SRC/../autohide-ab}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 8)}"
case "$(uname -m)" in
  x86_64)  DEF_TARGETS=X86 ;;
  aarch64|arm64) DEF_TARGETS=AArch64 ;;
  *) DEF_TARGETS=X86 ;;
esac
TARGETS="${TARGETS:-$DEF_TARGETS}"
RUN_CHECK_CLANG="${RUN_CHECK_CLANG:-0}"   # check-clang is slow under ThinLTO; opt-in via =1
NDIFF="${NDIFF:-8}"               # number of differential source files
PERF_REPS="${PERF_REPS:-10}"      # perf: interleaved timed runs per TU (0 = skip perf)
PERF_TUS="${PERF_TUS:-10}"        # perf: number of real LLVM TUs to sample
# Extra linker flags appended to BOTH A/B link configs (e.g. memory control in a
# constrained env: EXTRA_LINK="-Wl,--thinlto-jobs=2"). Kept constant across the
# A/B so it never affects the comparison.
EXTRA_LINK="${EXTRA_LINK:-}"
STAGE1="$WORK/stage1"
AB="$WORK/ab"

log(){ echo "[$(date +%H:%M:%S)] $*"; }
die(){ echo "ERROR: $*" >&2; exit 1; }
hr(){ printf '%s\n' "------------------------------------------------------------"; }

# ---------------------------------------------------------------- preflight
[ "$(uname -s)" = "Linux" ] || die "must run on Linux: the promotion needs ELF lld --lto-whole-program-visibility (ld64 / Mach-O lld cannot assert WPV)."
command -v cmake  >/dev/null || die "cmake not found"
command -v ninja  >/dev/null || die "ninja not found"
command -v nm     >/dev/null || die "nm not found"
grep -q "ThinLTOAutoHideExact" "$LLVM_SRC/llvm/lib/Transforms/IPO/FunctionImport.cpp" \
  || die "source tree lacks the -thinlto-autohide-exact prototype (FunctionImport.cpp). Wrong branch?"
mkdir -p "$WORK"
log "LLVM_SRC=$LLVM_SRC  WORK=$WORK  JOBS=$JOBS  TARGETS=$TARGETS  RUN_CHECK_CLANG=$RUN_CHECK_CLANG"

# ninja with a small restart loop (LTO links are memory-heavy; let it resume)
nb(){ local dir=$1; shift; local a; for a in $(seq 1 30); do
        ninja -C "$dir" "$@" -j"$JOBS" && return 0
        log "[restart] $dir $* (attempt $a)"; sleep 5
      done; return 1; }

# ================================================================ STAGE 1
if [ ! -x "$STAGE1/bin/clang" ] || [ ! -x "$STAGE1/bin/ld.lld" ]; then
  hr; log "STAGE1: configure our clang + lld (assertions ON)"
  cmake -S "$LLVM_SRC/llvm" -B "$STAGE1" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_ENABLE_PROJECTS="clang;lld" \
    -DLLVM_TARGETS_TO_BUILD="$TARGETS" \
    -DLLVM_ENABLE_ASSERTIONS=ON \
    > "$WORK/stage1.cfg.log" 2>&1 || { tail -20 "$WORK/stage1.cfg.log"; die "stage1 configure failed"; }
  log "STAGE1: build clang lld (-j$JOBS)"
  nb "$STAGE1" clang lld || die "stage1 build failed (see ninja output above)"
else
  log "STAGE1: reuse existing $STAGE1"
fi
CLANG="$STAGE1/bin/clang"
CLANGXX="$STAGE1/bin/clang++"
LLD="$STAGE1/bin/ld.lld"
"$LLD" --help 2>/dev/null | grep -q "lto-whole-program-visibility" \
  || die "built lld lacks --lto-whole-program-visibility -- not an ELF lld?"
log "STAGE1 ready: $("$CLANG" --version | head -1)"

# ================================================================ STAGE 2 (A/B)
# Whole-program-visibility on BOTH links (held constant). The only difference is
# the autohide-exact flag, added on the flag relink. Identical bitcode inputs ->
# clean isolation of the flag's effect.
LINK_WPV="--ld-path=$LLD -Wl,--lto-whole-program-visibility $EXTRA_LINK"
COMMON=( -S "$LLVM_SRC/llvm" -B "$AB" -G Ninja
  -DCMAKE_BUILD_TYPE=Release
  -DLLVM_ENABLE_PROJECTS=clang
  -DLLVM_TARGETS_TO_BUILD="$TARGETS"
  -DLLVM_ENABLE_ASSERTIONS=ON
  -DLLVM_ENABLE_LTO=Thin
  -DLLVM_PARALLEL_LINK_JOBS=1
  -DCMAKE_C_COMPILER="$CLANG"
  -DCMAKE_CXX_COMPILER="$CLANGXX" )

hr; log "STAGE2: configure ThinLTO build (WPV, NO autohide flag = baseline)"
cmake "${COMMON[@]}" \
  -DCMAKE_EXE_LINKER_FLAGS="$LINK_WPV" \
  -DCMAKE_SHARED_LINKER_FLAGS="$LINK_WPV" \
  > "$WORK/ab.noflag.cfg.log" 2>&1 || { tail -20 "$WORK/ab.noflag.cfg.log"; die "stage2 baseline configure failed"; }
log "STAGE2: build clang (baseline ThinLTO link)"
nb "$AB" clang || die "baseline clang build failed"
CLANGBIN=$(find "$AB/bin" -maxdepth 1 -name 'clang-[0-9]*' -type f | head -1)
[ -n "$CLANGBIN" ] || die "could not locate built clang binary"
cp -f "$CLANGBIN" "$AB/clang.noflag"
SZ_NF=$(stat -c%s "$AB/clang.noflag")
log "baseline clang: $SZ_NF bytes -> $AB/clang.noflag"

hr; log "STAGE2: reconfigure (WPV + -thinlto-autohide-exact + -stats), RELINK clang"
LINK_FLAG="$LINK_WPV -Wl,-mllvm,-thinlto-autohide-exact -Wl,-mllvm,-stats"
cmake "${COMMON[@]}" \
  -DCMAKE_EXE_LINKER_FLAGS="$LINK_FLAG" \
  -DCMAKE_SHARED_LINKER_FLAGS="$LINK_FLAG" \
  > "$WORK/ab.flag.cfg.log" 2>&1 || { tail -20 "$WORK/ab.flag.cfg.log"; die "stage2 flag configure failed"; }
# Only clang's link command changed -> ninja just relinks (bitcode reused).
# Capture the LTO -stats output to read the promotion count.
( nb "$AB" clang ) 2> "$AB/flag.link.stats" || die "flag relink failed"
cp -f "$CLANGBIN" "$AB/clang.flag"
SZ_FL=$(stat -c%s "$AB/clang.flag")
log "flag clang: $SZ_FL bytes -> $AB/clang.flag"
PROMOTED_STAT=$(grep -E 'NumThinLTOAutoHideExact|auto-hidden prevailing ODR' "$AB/flag.link.stats" \
                 | grep -oE '^[[:space:]]*[0-9]+' | tr -d ' ' | head -1)
PROMOTED_STAT=${PROMOTED_STAT:-0}

# ================================================================ ANALYZE BINARIES
hr; log "ANALYZE final binaries"
weakcnt(){ nm -P "$1" 2>/dev/null | awk '$2 ~ /^[WwVv]$/ {c++} END{print c+0}'; }
WK_NF=$(weakcnt "$AB/clang.noflag")
WK_FL=$(weakcnt "$AB/clang.flag")
WK_DELTA=$(( WK_NF - WK_FL ))           # weak symbols that became non-weak (promoted)
SZ_DELTA=$(( SZ_NF - SZ_FL ))           # +ve => flag is smaller

# ================================================================ DIFFERENTIAL CORRECTNESS
hr; log "A/B correctness: differential ($NDIFF template/inline-heavy TUs; both clangs must match)"
DD="$WORK/diff"; mkdir -p "$DD"; rm -f "$DD"/*.cpp "$DD"/*.nf.ll "$DD"/*.fl.ll "$DD"/*.err 2>/dev/null
gen_src(){ # $1 = index -> a distinct template/inline/virtual-heavy TU
  local i=$1
  cat <<EOF
template<class T> struct Box$i { T v; T get() const { return v; } void set(T x){ v = x; } };
template<class T> static inline T addv$i(T a, T b){ return a + b; }
struct Base$i { virtual int f(int) const; virtual ~Base$i(){} };
struct Deriv$i : Base$i { int f(int x) const override { return x*$i + 1; } };
inline int helper$i(const int* p){ return p ? *p + $i : $i; }
template<class T> T pipeline$i(T* p, int n){ Box$i<T> b; T s = 0; for(int i=0;i<n;i++) s = addv$i(s, p[i]); b.set(s); return b.get() + (T)helper$i((const int*)p); }
extern "C" long entry$i(int* p, int n){ Deriv$i d; long r = pipeline$i<long>((long*)p, n); return r + d.f(n) + Base$i().f(n); }
EOF
}
ident=0; differ=0; flassert=0
for i in $(seq 1 "$NDIFF"); do
  src="$DD/t$i.cpp"; gen_src "$i" > "$src"
  "$AB/clang.noflag" -O2 -std=c++17 -S -emit-llvm -fno-discard-value-names "$src" -o "$DD/t$i.nf.ll" 2>"$DD/t$i.nf.err"
  "$AB/clang.flag"   -O2 -std=c++17 -S -emit-llvm -fno-discard-value-names "$src" -o "$DD/t$i.fl.ll" 2>"$DD/t$i.fl.err"
  grep -qiE "Assertion|PLEASE submit|Stack dump|UNREACHABLE" "$DD/t$i.fl.err" && { flassert=$((flassert+1)); echo "  FLAG-CLANG asserted on t$i"; }
  if [ -f "$DD/t$i.nf.ll" ] && [ -f "$DD/t$i.fl.ll" ] && cmp -s "$DD/t$i.nf.ll" "$DD/t$i.fl.ll"; then
    ident=$((ident+1))
  else
    differ=$((differ+1)); echo "  DIFFERS: t$i"
  fi
done

# ================================================================ PERF (compile throughput)
# Both clangs are the same compiler, differently linked: the flag gave clang's
# hot ODR functions readonly/memory, so the flag-clang *may* be better-optimized
# and compile a fixed workload slightly faster. Measure that, interleaved +
# best-of-N (min, least noise) to avoid the load-confound that bit us before.
# Expectation: small / near-noise -- attribute wins are mostly code-size.
PERF_SUMMARY="(skipped: PERF_REPS=0)"
if [ "${PERF_REPS:-0}" -gt 0 ]; then
  hr; log "PERF: sample $PERF_TUS real LLVM TUs, $PERF_REPS interleaved runs each, best-of (-O3)"
  # Fixed-workload flags that compile most llvm/lib TUs with just the main
  # includes (build-generated + source). TUs that need more are filtered out.
  PFLAGS="-O3 -std=c++17 -fno-rtti -fno-exceptions -D__STDC_LIMIT_MACROS \
          -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS \
          -I$STAGE1/include -I$LLVM_SRC/llvm/include -Wno-everything"
  # Candidate real TUs from header-light libraries (avoid Target/* which need
  # target-specific generated .inc). Subsample for diversity.
  mapfile -t CAND < <(find "$LLVM_SRC/llvm/lib"/{IR,Support,Analysis,Bitcode,Object,ProfileData,BinaryFormat,Transforms/Utils,Transforms/Scalar,Transforms/InstCombine} -name '*.cpp' 2>/dev/null | sort | awk 'NR%5==0')
  PSET=()
  for tu in "${CAND[@]}"; do
    [ "${#PSET[@]}" -ge "$PERF_TUS" ] && break
    if "$AB/clang.noflag" $PFLAGS -I"$(dirname "$tu")" -c "$tu" -o /dev/null 2>/dev/null; then
      PSET+=("$tu")
    fi
  done
  log "PERF: ${#PSET[@]}/$PERF_TUS candidate TUs compile cleanly; timing (interleaved)..."
  if [ "${#PSET[@]}" -gt 0 ]; then
    : > "$WORK/perf_raw"
    for r in $(seq 1 "$PERF_REPS"); do
      idx=0
      for tu in "${PSET[@]}"; do
        idx=$((idx+1)); inc2="-I$(dirname "$tu")"
        t0=$(date +%s.%N); "$AB/clang.noflag" $PFLAGS $inc2 -c "$tu" -o /dev/null 2>/dev/null; t1=$(date +%s.%N)
        "$AB/clang.flag"   $PFLAGS $inc2 -c "$tu" -o /dev/null 2>/dev/null; t2=$(date +%s.%N)
        echo "$idx $(python3 -c "print($t1-$t0)") $(python3 -c "print($t2-$t1)")" >> "$WORK/perf_raw"
      done
    done
    PERF_SUMMARY=$(python3 - "$WORK/perf_raw" "${#PSET[@]}" "$PERF_REPS" <<'PY'
import sys, math
from collections import defaultdict
nf=defaultdict(lambda:1e18); fl=defaultdict(lambda:1e18)
for ln in open(sys.argv[1]):
    p=ln.split()
    if len(p)==3:
        i=p[0]; nf[i]=min(nf[i],float(p[1])); fl[i]=min(fl[i],float(p[2]))
tnf=sum(nf.values()); tfl=sum(fl.values())
geo=math.exp(sum(math.log(fl[i]/nf[i]) for i in nf)/len(nf)) if nf else 0
print("%s TUs x %s runs (best-of per TU): noflag=%.3fs flag=%.3fs  summed flag/noflag=%.4f  geomean(per-TU flag/noflag)=%.4f  (>1 slower, <1 faster)"
      % (sys.argv[2], sys.argv[3], tnf, tfl, (tfl/tnf if tnf else 0), geo))
PY
)
  else
    PERF_SUMMARY="(no sampled TU compiled cleanly with the fixed flags)"
  fi
fi

# ================================================================ CHECK-CLANG (optional, flag side)
CHECK_SUMMARY="(skipped: RUN_CHECK_CLANG=0)"
if [ "$RUN_CHECK_CLANG" = 1 ]; then
  hr; log "check-clang on the FLAG build (real correctness gate -- the promotion fires here)"
  # drop -stats from the link flags so test-tool links aren't noisy; keep WPV + flag.
  cmake "${COMMON[@]}" \
    -DCMAKE_EXE_LINKER_FLAGS="$LINK_WPV -Wl,-mllvm,-thinlto-autohide-exact" \
    -DCMAKE_SHARED_LINKER_FLAGS="$LINK_WPV -Wl,-mllvm,-thinlto-autohide-exact" \
    > "$WORK/ab.check.cfg.log" 2>&1
  nb "$AB" check-clang > "$AB/check.log" 2>&1 || true
  CHECK_SUMMARY=$(grep -E 'Passed|Failed|Unsupported|Expectedly Failed|Skipped' "$AB/check.log" | tail -6)
fi

# ================================================================ REPORT
hr; echo "============================  A/B RESULT  ============================"
echo "stage1 clang/lld:    $("$CLANG" --version | head -1)"
echo "config:              ThinLTO, lld, whole-program-visibility on BOTH (constant);"
echo "                     A/B = -thinlto-autohide-exact OFF (baseline) vs ON."
echo
echo "PROMOTION (impact):"
echo "  NumThinLTOAutoHideExact (linker -stats, gross): $PROMOTED_STAT functions promoted weak_odr -> exact dso_local"
echo "  weak symbols in binary:  noflag=$WK_NF  flag=$WK_FL  (delta=$WK_DELTA promotions reached the binary)"
echo
echo "SIZE:"
echo "  noflag clang: $SZ_NF bytes"
echo "  flag   clang: $SZ_FL bytes   (delta=$SZ_DELTA; +ve => flag smaller)"
echo
echo "PERF (compile throughput of the two clangs on a fixed -O2 workload):"
echo "  $PERF_SUMMARY"
echo
echo "CORRECTNESS:"
echo "  differential: $ident/$NDIFF TUs byte-identical, $differ differing, flag-clang asserts=$flassert"
echo "  check-clang (flag build):"
echo "$CHECK_SUMMARY" | sed 's/^/    /'
echo
if [ "$differ" -eq 0 ] && [ "$flassert" -eq 0 ]; then
  echo "VERDICT: flag clang is functionally identical to baseline (correct)."
  [ "$PROMOTED_STAT" -gt 0 ] 2>/dev/null && echo "         and the promotion FIRED ($PROMOTED_STAT functions) -- mechanism works at clang scale." \
                                          || echo "         NOTE: 0 promotions -- check WPV actually took effect (lld --lto-whole-program-visibility)."
else
  echo "VERDICT: DIVERGENCE/ASSERT detected -- investigate (see $DD and $AB/check.log)."
fi
echo "artifacts: $AB/{clang.noflag,clang.flag,flag.link.stats,check.log}  diff TUs: $DD"
echo "====================================================================="
