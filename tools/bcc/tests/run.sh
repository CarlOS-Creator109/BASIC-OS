#!/bin/sh
# pruebas de BCC: cada tests/t_*.c se compila con gcc y con BCC y las salidas tienen que ser iguales
cd "$(dirname "$0")"
fail=0
for c in t_*.c; do
    t=${c%.c}
    in=/dev/null; [ -f $t.in ] && in=$t.in
    gcc -w -o /tmp/$t.gcc $c && /tmp/$t.gcc < $in > /tmp/$t.exp 2>&1
    if ../bcc $c -o /tmp/$t.BIN > /tmp/$t.log 2>&1 && ../bccrun /tmp/$t.BIN < $in > /tmp/$t.got 2>&1 && cmp -s /tmp/$t.exp /tmp/$t.got; then
        echo "OK     $t"
    else
        echo "FALLA  $t"; cat /tmp/$t.log; diff /tmp/$t.exp /tmp/$t.got | head -20; fail=1
    fi
done
exit $fail
