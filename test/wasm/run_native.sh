#!/bin/sh

dir=`dirname "$0"`
bin=${bin:-$dir/../../qbe}
tmp=/tmp/qbe.wasm-native.zzzz

asm=$tmp.s
drv=$tmp.c
exe=$tmp.exe
cc=${CC:-cc}

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
		skip=`expr $skip + 1`
		continue
	fi

	num=`echo "$val" | awk '{print $1}'`
	typ=`echo "$val" | awk '{print $2}'`

	if test "$num" = "void"
	then
		skip=`expr $skip + 1`
		continue
	fi

	count=`expr $count + 1`
	printf "%-45s" "$name..."

	if ! "$bin" -o "$asm" "$t" 2>/dev/null
	then
		echo "[qbe fail]"
		fail=`expr $fail + 1`
		continue
	fi

	case "$typ" in
	w) ctyp="int";    fmt="%d" ;;
	l) ctyp="long long"; fmt="%lld" ;;
	s) ctyp="float";  fmt="%a" ;;
	d) ctyp="double"; fmt="%a" ;;
	*)
		echo "[bad type: $typ]"
		fail=`expr $fail + 1`
		continue
		;;
	esac

	cat > "$drv" << EOF
#include <stdio.h>
extern $ctyp test(void);
int main(void) {
	$ctyp r = test();
	printf("$fmt\n", r);
	return 0;
}
EOF

	if ! $cc -o "$exe" "$drv" "$asm" 2>/dev/null
	then
		echo "[cc fail]"
		fail=`expr $fail + 1`
		continue
	fi

	got=`"$exe"`
	case "$typ" in
	w|l)
		if test "$got" = "$num"
		then echo "[ok]"
		else
			echo "[fail: got $got, want $num]"
			fail=`expr $fail + 1`
		fi
		;;
	s|d)
		cat > "$drv" << EOF
#include <stdio.h>
int main(void) {
	$ctyp want = $num;
	printf("$fmt\n", want);
	return 0;
}
EOF
		$cc -o "$exe" "$drv" 2>/dev/null
		want=`"$exe"`
		if test "$got" = "$want"
		then echo "[ok]"
		else
			echo "[fail: got $got, want $want]"
			fail=`expr $fail + 1`
		fi
		;;
	esac
done

rm -f "$asm" "$drv" "$exe"

echo
if test $fail -ge 1
then
	echo "$fail of $count tests failed! ($skip skipped)"
	exit 1
else
	echo "All $count tests passed! ($skip skipped)"
	exit 0
fi
