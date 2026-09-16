#!/usr/bin/env bash
# Run OpenSTA network/test conformance cases against the YosysNetwork adapter.
# The original tests read the design with OpenSTA's verilog reader; here every
# read_verilog/link_design (initial or mid-test) is rewritten to load through
# the Yosys frontend and relink the adapter, via the shared-interp `yosys`
# command. Liberty is read into OpenSTA once, up front, and persists.
set -u

SPIKE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
STA_TEST_DIR="${STA_TEST_DIR:-$SPIKE_DIR/deps/OpenSTA/network/test}"
YOSYS="${YOSYS:-$SPIKE_DIR/deps/yosys/build/yosys}"
PLUGIN="$SPIKE_DIR/opensta_net.so"
WORK="$SPIKE_DIR/conformance/work"
mkdir -p "$WORK"

pass=0; fail=0; err=0; total=0
filter="${1:-}"
rm -f "$WORK"/*.out "$WORK"/*.diff "$WORK"/*.ok.norm

while IFS='|' read -r name libs; do
	case "$name" in \#*|"") continue;; esac
	if [ -n "$filter" ] && [ "$name" != "$filter" ]; then continue; fi
	total=$((total + 1))

	tcl="$WORK/$name.tcl"
	ys="$WORK/$name.ys"
	out="$WORK/$name.out"
	log="$WORK/$name.log"

	if [ ! -f "$STA_TEST_DIR/$name.tcl" ] || [ ! -s "$STA_TEST_DIR/$name.ok" ]; then
		echo "ERR  $name (missing test .tcl or .ok)"
		err=$((err + 1))
		continue
	fi

	{
		echo "set ::ysta_libs {$libs}"
		cat <<'EOF'
proc ysta_load {verilog top} {
	yosys tee -q design -reset
	foreach lib $::ysta_libs { yosys tee -q read_liberty -lib -ignore_miss_func $lib }
	yosys tee -q read_verilog $verilog
	yosys tee -q hierarchy -top $top
	yosys tee -q uniquify
	yosys tee -q opensta_net -link
}
EOF
		echo 'puts YSTA_BEGIN'
		# read_liberty stays verbatim (the OpenSTA tcl command works in the
		# shared interp, and its warnings must appear in reference order).
		awk '
			/^read_verilog / { vfile = $2; next }
			/^link_design /  { print "ysta_load {" vfile "} {" $2 "}"; next }
			{ print }
		' "$STA_TEST_DIR/$name.tcl"
		echo 'puts YSTA_END'
	} > "$tcl"

	{
		echo "opensta_net"
		echo "opensta_net -source $tcl"
	} > "$ys"

	# The rewrite must have replaced every link_design with a ysta_load.
	want=$(grep -c '^link_design ' "$STA_TEST_DIR/$name.tcl" || true)
	got=$(grep -c '^ysta_load ' "$tcl" || true)
	if [ "$want" != "$got" ]; then
		echo "ERR  $name (design-load rewrite mismatch: $want link_design vs $got ysta_load)"
		err=$((err + 1))
		continue
	fi

	# Run from the STA test dir: tests source ../../test/helpers.tcl etc.
	if ! (cd "$STA_TEST_DIR" && "$YOSYS" -m "$PLUGIN" -s "$ys" < /dev/null) > "$log" 2>&1; then
		echo "ERR  $name (yosys failed, see $log)"
		err=$((err + 1))
		continue
	fi

	awk '/^YSTA_BEGIN$/{flag=1;next} /^YSTA_END$/{flag=0} flag' "$log" > "$out"
	if [ ! -s "$out" ]; then
		echo "FAIL $name (empty extracted output)"
		fail=$((fail + 1))
		continue
	fi

	# Reader-specific messages have no equivalent in the yosys frontend path.
	grep -v '^Warning [0-9]*:.*Creating black box' "$STA_TEST_DIR/$name.ok" > "$WORK/$name.ok.norm"

	if diff -u "$WORK/$name.ok.norm" "$out" > "$WORK/$name.diff" 2>&1; then
		echo "PASS $name"
		pass=$((pass + 1))
	else
		lines=$(grep -c -E '^[+-][^+-]' "$WORK/$name.diff")
		echo "FAIL $name ($lines diff lines, see $WORK/$name.diff)"
		fail=$((fail + 1))
	fi
done < "$SPIKE_DIR/conformance/tests.conf"

echo "---"
echo "pass=$pass fail=$fail err=$err total=$total"
if [ "$total" -eq 0 ] || [ $((pass + fail + err)) -ne "$total" ]; then
	echo "ERROR: test accounting mismatch"
	exit 2
fi
[ $((fail + err)) -eq 0 ]
