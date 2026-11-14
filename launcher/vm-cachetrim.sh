#!/bin/sh
# Runs as root inside the muvm guest (muvm -x). The guest keeps a page cache of every
# game file it reads and never hands those pages back to the host, so the VM grows to
# its --mem cap and the host (which caches the same files) pays twice. Dropping the
# guest's clean page cache lets libkrun's free-page reporting return it to the host.
setsid sh -c 'while sleep 15; do echo 1 > /proc/sys/vm/drop_caches; done' </dev/null >/dev/null 2>&1 &
exit 0
