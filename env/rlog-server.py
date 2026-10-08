#!/usr/bin/env python3
"""Remote debug log server for the Symbian apps (rSSH, NetSurf, rDrive).

Runs on the HOST (any machine on the phone's network; Python 3, no extra
modules). On the phone, open the app's Settings and set:

    Remote debug host   this machine's IP address (printed at start-up)
    Remote debug port   7865 (the default)
    Remote debug log    On

Every log line the app writes then appears here as it happens, and is also
saved under out/rlog/<app>-<date>.log.

    env/rlog-server.py                   # listen on 0.0.0.0:7865
    env/rlog-server.py --port 9000 --grep fetch
    env/rlog-server.py --no-save

Protocol (apps/common/net/rsym_rlog.h): plain TCP text, one line per log
line; each connection starts with "#rlog1 app=<name>". Several phones or apps
may be connected at once.
"""
import argparse
import datetime
import os
import re
import socket
import sys
import threading

DEFAULT_PORT = 7865
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

COLOURS = ['36', '33', '35', '32', '34', '31']   # per connection
print_lock = threading.Lock()


def local_addresses():
    """This machine's IPv4 addresses, best effort (for the phone's settings)."""
    addrs = set()
    try:
        # the address used for the default route (nothing is sent)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(('192.0.2.1', 9))
        addrs.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            addrs.add(info[4][0])
    except OSError:
        pass
    return sorted(a for a in addrs if not a.startswith('127.'))


class Client(threading.Thread):
    counter = 0

    def __init__(self, sock, addr, args):
        super().__init__(daemon=True)
        self.sock, self.addr, self.args = sock, addr, args
        Client.counter += 1
        self.colour = COLOURS[(Client.counter - 1) % len(COLOURS)]
        self.app = '?'
        self.file = None

    def out(self, text, meta=False):
        now = datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]
        tag = '%s %s' % (self.addr[0], self.app)
        if self.args.colour:
            line = '\033[2m%s\033[0m \033[%sm%s\033[0m %s%s%s' % (
                now, self.colour, tag, '\033[1m' if meta else '', text,
                '\033[0m' if meta else '')
        else:
            line = '%s %s %s' % (now, tag, text)
        with print_lock:
            print(line, flush=True)
        if self.file:
            self.file.write('%s %s\n' % (now, text))
            self.file.flush()

    def open_file(self):
        if self.args.no_save:
            return
        d = os.path.join(self.args.log_dir)
        os.makedirs(d, exist_ok=True)
        name = re.sub(r'[^A-Za-z0-9_.-]', '_', self.app)
        path = os.path.join(d, '%s-%s.log' % (name, datetime.date.today().isoformat()))
        self.file = open(path, 'a', encoding='utf-8')
        self.file.write('---- %s connected from %s\n' % (
            datetime.datetime.now().isoformat(timespec='seconds'), self.addr[0]))

    def run(self):
        self.out('connected', meta=True)
        buf = b''
        try:
            while True:
                data = self.sock.recv(4096)
                if not data:
                    break
                buf += data
                while b'\n' in buf:
                    raw, buf = buf.split(b'\n', 1)
                    self.line(raw.decode('utf-8', 'replace').rstrip('\r'))
        except OSError as e:
            self.out('connection error: %s' % e, meta=True)
        finally:
            if buf:
                self.line(buf.decode('utf-8', 'replace'))
            self.out('disconnected', meta=True)
            if self.file:
                self.file.close()
            self.sock.close()

    def line(self, text):
        if text.startswith('#rlog1 '):
            m = re.search(r'app=(\S+)', text)
            self.app = m.group(1) if m else '?'
            self.open_file()
            self.out('hello (%s)' % text[7:], meta=True)
            return
        if text.startswith('#rlog: '):
            self.out(text[7:], meta=True)
            return
        if self.args.grep and not self.args.grep.search(text):
            if self.file:          # the file keeps everything
                self.file.write(text + '\n')
            return
        self.out(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bind', default='0.0.0.0', help='address to listen on (default all)')
    ap.add_argument('--port', type=int, default=DEFAULT_PORT,
                    help='TCP port (default %d)' % DEFAULT_PORT)
    ap.add_argument('--log-dir', default=os.path.join(ROOT, 'out', 'rlog'),
                    help='where to save the logs (default out/rlog/)')
    ap.add_argument('--no-save', action='store_true', help='only print, do not save')
    ap.add_argument('--grep', type=re.compile, help='only print lines matching this regex')
    ap.add_argument('--no-colour', dest='colour', action='store_false',
                    default=sys.stdout.isatty(), help='plain output')
    args = ap.parse_args()
    sys.stdout.reconfigure(line_buffering=True)     # also when piped to a file

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.bind, args.port))
    srv.listen(8)
    print('rlog server listening on %s:%d' % (args.bind, args.port))
    addrs = local_addresses()
    if addrs:
        print('on the phone, set Remote debug host to: %s' % ' or '.join(addrs))
    print('(the EKA2L1 emulator reaches this machine as 127.0.0.1)')
    if not args.no_save:
        print('saving to %s/' % os.path.relpath(args.log_dir))
    try:
        while True:
            sock, addr = srv.accept()
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
            Client(sock, addr, args).start()
    except KeyboardInterrupt:
        print()


if __name__ == '__main__':
    main()
