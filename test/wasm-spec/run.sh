#!/bin/sh

dir=`dirname "$0"`
bin=${bin:-$dir/../../qbe}
tmp=/tmp/qbe.wasm-spec.zzzz

wat=$tmp.wat
wast=$tmp.wast

fail=0
count=0

if ! test -x "$bin"
then
	echo "qbe not found at $bin"
	exit 1
fi

for t in "$dir"/*.ssa
do
	name=`basename "$t" .ssa`
	asserts="$dir/$name.asserts"
	count=`expr $count + 1`
	printf "%-45s" "$name..."

	if ! test -f "$asserts"
	then
		echo "[skip: no asserts]"
		continue
	fi

	if ! "$bin" -t wasm "$t" > "$wat" 2>/dev/null
	then
		echo "[qbe fail]"
		fail=`expr $fail + 1`
		continue
	fi

	cat "$wat" "$asserts" > "$wast"

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
	echo "$fail of $count tests failed!"
	exit 1
else
	echo "All $count tests passed!"
	exit 0
fi
