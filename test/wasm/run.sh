#!/bin/sh

dir=`dirname "$0"`
bin=${bin:-$dir/../../qbe}
tmp=/tmp/qbe.wasm.zzzz

wat=$tmp.wat
wast=$tmp.wast

fail=0
count=0
skip=0

if ! test -x "$bin"
then
	echo "qbe not found at $bin"
	exit 1
fi

for t in "$dir"/*.ssa
do
	name=`basename "$t"`
	hdr=`sed -e 1q "$t"`
	val=`echo "$hdr" | sed -n 's/^# expected: *\(.*\)/\1/p'`
	if test -z "$val"
	then
		printf "%-45s[skip: no expected]\n" "$name"
		skip=`expr $skip + 1`
		continue
	fi

	num=`echo "$val" | awk '{print $1}'`
	typ=`echo "$val" | awk '{print $2}'`
	addr=`echo "$val" | awk '{print $3}'`
	mval=`echo "$val" | awk '{print $4}'`

	count=`expr $count + 1`
	printf "%-45s" "$name..."

	if ! "$bin" -t wasm "$t" > "$wat" 2>/dev/null
	then
		echo "[qbe fail]"
		fail=`expr $fail + 1`
		continue
	fi

	case "$typ" in
	w)
		wt="i32"
		;;
	l)
		wt="i64"
		;;
	s)
		wt="f32"
		;;
	d)
		wt="f64"
		;;
	*)
		echo "[bad type: $typ]"
		fail=`expr $fail + 1`
		continue
		;;
	esac

	if test "$num" = "void" -a -n "$addr" -a -n "$mval"
	then
		# void function: check memory at addr after call
		cat "$wat" > "$wast"
		cat >> "$wast" << EOF
(invoke "test")
(assert_return (invoke "check") (i32.const $mval))
EOF
	else
		cat "$wat" > "$wast"
		echo "(assert_return (invoke \"test\") ($wt.const $num))" >> "$wast"
	fi

	if wasmtime wast "$wast" > /dev/null 2>&1
	then
		echo "[ok]"
	else
		echo "[wasm fail]"
		fail=`expr $fail + 1`
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
