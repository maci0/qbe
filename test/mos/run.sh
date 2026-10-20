#!/bin/sh

dir=`dirname "$0"`
bin=${bin:-$dir/../../qbe}
tmp=/tmp/qbe.mos.zzzz

asm=$tmp.s
startup_o=$tmp.startup.o
startup_6510_o=$tmp.startup_6510.o
test_o=$tmp.test.o
exe=$tmp.bin

fail=0
count=0
skip=0

if ! test -x "$bin"
then
	echo "qbe not found at $bin"
	exit 1
fi

if ! ca65 --version >/dev/null 2>&1
then
	echo "ca65 not found"
	exit 1
fi

if ! sim65 --version >/dev/null 2>&1
then
	echo "sim65 not found"
	exit 1
fi

# assemble startups once
ca65 -t none "$dir/startup.s" -o "$startup_o" 2>/dev/null
if test $? -ne 0
then
	echo "startup.s assembly failed"
	exit 1
fi

ca65 -t none -D IS_6510=1 "$dir/startup.s" -o "$startup_6510_o" 2>/dev/null
if test $? -ne 0
then
	echo "startup.s (6510) assembly failed"
	exit 1
fi

# run_one TEST_FILE QBE_TARGET CA65_CPU SIM65_CPU
run_one() {
	_t=$1 _tgt=$2 _ca65cpu=$3 _simcpu=$4 _num=$5
	_name=`basename "$_t"`

	count=`expr $count + 1`
	printf "  %-40s [%s] " "$_name..." "$_tgt"

	if ! "$bin" -t "$_tgt" "$_t" > "$asm" 2>/dev/null
	then
		echo "[qbe fail]"
		fail=`expr $fail + 1`
		return
	fi

	# 65816: only verify assembly (sim65 lacks 65816 support)
	if test "$_tgt" = "mos_65816"
	then
		if ca65 -t none --cpu "$_ca65cpu" "$asm" -o "$test_o" 2>/dev/null
		then
			echo "[asm ok, sim skip]"
		else
			echo "[ca65 fail]"
			fail=`expr $fail + 1`
		fi
		skip=`expr $skip + 1`
		count=`expr $count - 1`
		return
	fi

	if ! ca65 -t none --cpu "$_ca65cpu" "$asm" -o "$test_o" 2>/dev/null
	then
		echo "[ca65 fail]"
		fail=`expr $fail + 1`
		return
	fi

	if test "$_tgt" = "mos_6510"
	then
		_start_o="$startup_6510_o"
	else
		_start_o="$startup_o"
	fi

	if ! ld65 -C "$dir/sim65.cfg" \
		"$_start_o" "$test_o" -o "$exe" 2>/dev/null
	then
		echo "[ld65 fail]"
		fail=`expr $fail + 1`
		return
	fi

	sim65 -x 10000000 --cpu "$_simcpu" "$exe" \
		>/dev/null 2>&1
	got=$?

	if test "$got" -eq "$_num"
	then
		echo "[ok]"
	else
		echo "[fail: expected $_num got $got]"
		fail=`expr $fail + 1`
	fi
}

for t in "$dir"/*.ssa
do
	name=`basename "$t"`
	hdr=`sed -e 1q "$t"`
	val=`echo "$hdr" | sed -n 's/^# expected: *\(.*\)/\1/p'`

	if test -z "$val"
	then
		printf "  %-40s [skip: no expected]\n" "$name"
		skip=`expr $skip + 1`
		continue
	fi

	# parse: "VALUE" or "VALUE VARIANT"
	num=`echo "$val" | awk '{print $1}'`
	variant=`echo "$val" | awk '{print $2}'`

	if test -n "$variant"
	then
		# variant-specific: run only on that target
		case "$variant" in
		mos)   run_one "$t" "mos"       "6502"  "6502"  "$num" ;;
		2a07)  run_one "$t" "mos_2a07"  "6502"  "6502"  "$num" ;;
		6510)  run_one "$t" "mos_6510"  "6502"  "6502"  "$num" ;;
		65c02) run_one "$t" "mos_65c02" "65C02" "65C02" "$num" ;;
		65816) run_one "$t" "mos_65816" "65816" "65C02" "$num" ;;
		*)
			printf "  %-40s [bad variant: %s]\n" \
				"$name" "$variant"
			fail=`expr $fail + 1`
			;;
		esac
	else
		# no variant: run on all targets
		run_one "$t" "mos"       "6502"  "6502"  "$num"
		run_one "$t" "mos_2a07"  "6502"  "6502"  "$num"
		run_one "$t" "mos_6510"  "6502"  "6502"  "$num"
		run_one "$t" "mos_65c02" "65C02" "65C02" "$num"
		run_one "$t" "mos_65816" "65816" "65C02" "$num"
	fi
done

echo
if test $fail -ge 1
then
	echo "$fail of $count tests failed! ($skip skipped)"
	exit 1
else
	echo "All $count tests passed! ($skip skipped)"
	exit 0
fi
