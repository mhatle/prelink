#!/bin/bash
. `dirname $0`/functions.sh
# First check if __thread is supported by ld.so/gcc/ld/as:
rm -f tlstest
echo '__thread int a; int main (void) { return a; }' \
  | $RUN_HOST $CCLINK -xc - -o tlstest > /dev/null 2>&1 || exit 77
if [ "x$CROSS" = "x" ]; then
 ( $RUN LD_LIBRARY_PATH=. ./tlstest || { rm -f tlstest; exit 77; } ) 2>/dev/null || exit 77
fi
rm -f tls7 tls7.log
rm -f prelink.cache
BINS="tls7"
LIBS=""
$RUN_HOST $CCLINK -o tls7 $srcdir/tls7.c
savelibs
echo $PRELINK ${PRELINK_OPTS--vm} ./tls7 > tls7.log
$RUN_HOST $PRELINK ${PRELINK_OPTS--vm} ./tls7 >> tls7.log 2>&1 || exit 1
grep -q ^`echo $PRELINK | sed 's/ .*$/: /'` tls7.log && exit 2
if [ "x$CROSS" = "x" ]; then
 $RUN LD_LIBRARY_PATH=. ./tls7 || exit 3
fi
$RUN_HOST $READELF -a ./tls7 >> tls7.log 2>&1 || exit 4
# So that it is not prelinked again
chmod -x ./tls7
comparelibs >> tls7.log 2>&1 || exit 5
