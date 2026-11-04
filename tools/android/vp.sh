#!/system/bin/sh
# vp.sh CMD... : send commands to vpad and wait until they have run
F=/data/local/tmp/vpad.fifo
D=/data/local/tmp/vpad.done
rm -f $D
{ for c in "$@"; do echo "$c"; done; echo "done $D"; } > $F
n=0
while [ ! -e $D ] && [ $n -lt 400 ]; do sleep 0.05; n=$((n+1)); done
