#!/bin/sh
# Start/stop a throwaway OpenSSH server for testing rSSH, on 127.0.0.1:2222
# only (it has a passwordless "test" account). Runs on the HOST.
#   env/test-sshd.sh start | stop | status
# In rSSH (emulator): connect to  test@127.0.0.1:2222  and press Enter at the
# password prompt.
set -e
NAME=rssh-test-sshd
case "${1:-start}" in
start)
    docker build -q -t $NAME "$(dirname "$0")/test-sshd" >/dev/null
    docker rm -f $NAME >/dev/null 2>&1 || true
    docker run -d --rm --name $NAME -p 127.0.0.1:2222:22 $NAME >/dev/null
    echo "test SSH server running: test@127.0.0.1:2222 (empty password)"
    ;;
stop)
    docker rm -f $NAME >/dev/null 2>&1 && echo "stopped" || echo "not running"
    ;;
status)
    docker ps --filter name=$NAME --format '{{.Names}} {{.Status}} {{.Ports}}'
    ;;
*)
    echo "usage: $0 start|stop|status" >&2; exit 1 ;;
esac
