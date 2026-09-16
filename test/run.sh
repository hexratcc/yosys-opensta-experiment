#!/usr/bin/env bash
# Local test runner: every check is automated; nothing relies on eyeballing.
set -u

SPIKE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
YOSYS="${YOSYS:-$SPIKE_DIR/deps/yosys/build/yosys}"
STA="${STA:-$SPIKE_DIR/deps/OpenSTA/build/sta}"
PLUGIN="$SPIKE_DIR/opensta_net.so"
cd "$SPIKE_DIR"

pass=0; fail=0
ok()   { echo "PASS $1"; pass=$((pass + 1)); }
bad()  { echo "FAIL $1${2:+ ($2)}"; fail=$((fail + 1)); }

run_ys() { "$YOSYS" -m "$PLUGIN" -s "$1" 2>&1; }

# kh/kh2: keep_hierarchy marking, asserted by select inside the scripts;
# kh additionally pins the write_sdc output to a golden.
for t in kh kh2; do
	if run_ys test/$t.ys > /dev/null; then ok $t; else bad $t; fi
done
if diff -q test/kh_out.sdc test/kh_out.golden > /dev/null 2>&1; then
	ok kh_write_sdc_golden
else
	bad kh_write_sdc_golden "test/kh_out.sdc differs from golden"
fi

# m1: exact expected summary values.
m1_log=$(run_ys test/m1.ys)
if echo "$m1_log" | grep -q "insts: 4 pins: 11 nets: 6" &&
   echo "$m1_log" | grep -q "slack (MET)"; then
	ok m1
else
	bad m1 "summary values changed"
fi

# m4: the three slacks must be exactly baseline/replaced/rewired.
m4_slacks=$(run_ys test/m4.ys | grep -oE "slack [a-z_ ]*: [0-9.]+" | awk '{print $NF}' | tr '\n' ' ')
if [ "$m4_slacks" = "8.60 8.20 8.70 " ]; then
	ok m4
else
	bad m4 "slacks: $m4_slacks (want 8.60 8.20 8.70)"
fi

# relink: second link must reflect the persisted replace_cell (8.20 twice).
relink_slacks=$(run_ys test/relink.ys | grep -oE "slack [a-z_ ]*: [0-9.]+" | awk '{print $NF}' | tr '\n' ' ')
if [ "$relink_slacks" = "8.60 8.20 8.20 8.20 " ]; then
	ok relink
else
	bad relink "slacks: $relink_slacks (want 8.60 8.20 8.20 8.20)"
fi

# optflow: post-opt numbers from the auto-relinked run must equal a fresh run.
opt_incr=$(run_ys test/optflow.ys | grep -E "^(insts|slack):" | tail -2)
opt_fresh=$(run_ys test/optflow_fresh.ys | grep -E "^(insts|slack):")
if [ -n "$opt_incr" ] && [ "$opt_incr" = "$opt_fresh" ]; then
	ok optflow
else
	bad optflow "incremental [$opt_incr] vs fresh [$opt_fresh]"
fi

# memo: a Monitor-silent mutation issued through the `yosys` Tcl command must
# drop the memoized staleness check, so the next OpenSTA command refuses.
memo_log=$(run_ys test/memo.ys)
if echo "$memo_log" | grep -q "linked before: 1" &&
   echo "$memo_log" | grep -q "linked after: 0" &&
   echo "$memo_log" | grep -q "refused: .*No network has been linked"; then
	ok memo
else
	bad memo "stale view not refused after yosys-command mutation"
fi

# top: adding a module and switching the top must both force a relink.
top_log=$(run_ys test/top.ys)
if [ "$(echo "$top_log" | grep -c "design changed since link")" = 1 ] &&
   [ "$(echo "$top_log" | grep "^insts:" | tr '\n' ' ')" = "insts: 2 insts: 3 " ]; then
	ok top
else
	bad top "top change not detected"
fi

# tie: an assign-to-constant wire is an alias of the constant net.
if run_ys test/tie.ys | grep -q "tie nets: 1"; then
	ok tie
else
	bad tie "constant-tied wire not findable by name"
fi

# hier: a driver swap inside a child must drop the parent net's cached driver
# set, or a later -through exception binds to the dead driver.
hier_log=$(run_ys test/hier.ys)
if echo "$hier_log" | grep -q "No paths found" && ! echo "$hier_log" | grep -q "u_sub/i[12]/Y"; then
	ok hier
else
	bad hier "stale driver cache across hierarchy"
fi

# editlog: the adapter's own edits and Monitor-reported rewires are logged by name.
run_ys test/editlog.ys > /dev/null
if grep -q "^cell_retyped edit top u_inv1 INV$" test/editlog_out.txt &&
   grep -q "^disconnected monitor top u_inv2 A 0 n1$" test/editlog_out.txt &&
   grep -q "^connected monitor top u_inv2 A 0 q0$" test/editlog_out.txt; then
	ok editlog
else
	bad editlog "edit log entries missing"
fi

# handoff: Verilog + SDC written from yosys must time identically when OpenSTA's
# own reader loads them (the P0 handoff).
run_ys test/handoff.ys | awk '/^HANDOFF_BEGIN$/{f=1;next} /^HANDOFF_END$/{f=0} f' > test/handoff_yosys.out
"$STA" -no_init -no_splash -exit test/handoff_ref.tcl 2>&1 | awk '/^HANDOFF_BEGIN$/{f=1;next} /^HANDOFF_END$/{f=0} f' > test/handoff_ref.out
if [ -s test/handoff_ref.out ] && grep -q "slack" test/handoff_ref.out && diff -q test/handoff_ref.out test/handoff_yosys.out > /dev/null; then
	ok handoff
else
	bad handoff "exported netlist + SDC time differently in the reference binary"
fi

# dup: duplicating a flop through NetworkEdit keeps timing, and the edit
# persists in RTLIL across a relink.
dup_log=$(run_ys test/dup.ys)
dup_slacks=$(echo "$dup_log" | grep -oE "slack [a-z ]*: [0-9.]+" | awk '{print $NF}' | tr '\n' ' ')
if [ "$dup_slacks" = "8.60 8.60 8.60 " ] && echo "$dup_log" | grep -q "dff count: 2"; then
	ok dup
else
	bad dup "slacks: $dup_slacks (want 8.60 x3), or flop not persisted"
fi

# bus*: bused leaf-cell bit binding (descending and ascending liberty buses)
# and bus-bit edits must match the OpenSTA binary exactly.
for t in bus bus_asc bus_edit; do
	"$STA" -no_init -no_splash -exit test/${t}_ref.tcl > test/${t}_ref.out 2>&1
	run_ys test/$t.ys > test/${t}_yosys.log
	awk '/^u1|^slack/' test/${t}_yosys.log > test/${t}_yosys.out
	if [ -s test/${t}_ref.out ] && diff -q test/${t}_ref.out test/${t}_yosys.out > /dev/null; then
		ok $t
	else
		bad $t "bit binding differs from reference"
	fi
done

# stress: regenerate the reference from the OpenSTA binary each run; the diff
# must be EXACTLY the documented alias-collapse deviation (see stress_waiver).
"$STA" -no_init -no_splash -exit test/stress_ref.tcl > test/stress_ref.out 2>&1
run_ys test/stress.ys > test/stress_yosys.log
awk '/^--- ports ---$/{flag=1} flag' test/stress_yosys.log |
	sed '/^End of script/,$d' | sed '${/^$/d}' > test/stress_yosys.out
diff <(sed 's/line [0-9]*/line N/' test/stress_ref.out) \
     <(sed 's/line [0-9]*/line N/' test/stress_yosys.out) > test/stress.diff
if diff -q test/stress.diff test/stress_waiver > /dev/null 2>&1; then
	ok stress
else
	bad stress "diff vs reference is not exactly the documented waiver"
fi

echo "---"
echo "pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
