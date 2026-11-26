#!/system/bin/sh
MODDIR=${0%/*}
until [ "$(getprop sys.boot_completed)" = 1 ]; do sleep 2; done

# Supervisor: restart the daemon if it ever exits.
(
  while true; do
    "$MODDIR/bin/fpdtap"
    log -t FpDoubleTap "fpdtap exited ($?), restarting in 5s"
    sleep 5
  done
) </dev/null >/dev/null 2>&1 &
