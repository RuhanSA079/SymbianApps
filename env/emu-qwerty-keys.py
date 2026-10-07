#!/usr/bin/env python3
"""Give EKA2L1 a 'qwerty' keybind profile so the PC keyboard types into the
emulated E7 (the default profile maps only softkeys, Enter and arrows), and
make it the current profile. Run on the HOST while the emulator is closed.

Bindings map Qt key codes to Symbian scan codes (e32keys.h). EKA2L1 turns
scan codes 0x1d..0x5f straight into the same character code (and lower ones
through its key table), so every printable key from 0x20 to 0x5f is bound to
its own ASCII value: that covers digits, upper-case letters (rSSH lower-cases
them unless Shift is held) and most symbols, including '@'. Characters above
0x5f other than letters (` { | } ~) cannot be typed this way."""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, 'emu-data', 'EKA2L1')
BIND = os.path.join(DATA, 'bindings')
CONF = os.path.join(DATA, 'config.yml')

QT = {'Escape': 0x01000000, 'Tab': 0x01000001, 'Backspace': 0x01000003,
      'Return': 0x01000004, 'Enter': 0x01000005, 'Insert': 0x01000006,
      'Delete': 0x01000007, 'Home': 0x01000010, 'End': 0x01000011,
      'Left': 0x01000012, 'Up': 0x01000013, 'Right': 0x01000014,
      'Down': 0x01000015, 'PageUp': 0x01000016, 'PageDown': 0x01000017,
      'Shift': 0x01000020, 'Control': 0x01000021, 'Alt': 0x01000023,
      'F1': 0x01000030, 'F2': 0x01000031, 'F3': 0x01000032, 'F4': 0x01000033}
STD = {'Backspace': 0x01, 'Tab': 0x02, 'Enter': 0x03, 'Escape': 0x04,
       'Space': 0x05, 'Home': 0x08, 'End': 0x09, 'PageUp': 0x0a,
       'PageDown': 0x0b, 'Insert': 0x0c, 'Delete': 0x0d, 'Left': 0x0e,
       'Right': 0x0f, 'Up': 0x10, 'Down': 0x11, 'LeftShift': 0x12,
       'LeftAlt': 0x14, 'LeftCtrl': 0x16, 'Comma': 0x79, 'FullStop': 0x7a,
       'Slash': 0x7b, 'BackSlash': 0x7c, 'SemiColon': 0x7d,
       'SingleQuote': 0x7e, 'Hash': 0x7f, 'BracketL': 0x80,
       'BracketR': 0x81, 'Minus': 0x82, 'Equals': 0x83,
       'Device0': 0xa4, 'Device1': 0xa5, 'Device3': 0xa7,
       'Application0': 0xb4, 'Application1': 0xb5}

binds = [
    # what the default profile had: softkeys, menu keys, select, arrows
    (QT['F1'], STD['Device0']), (QT['F2'], STD['Device1']),
    (QT['F3'], STD['Application0']), (QT['F4'], STD['Application1']),
    # Return -> Enter (the select key, Device3, never reaches the app's
    # controls on Belle); keypad Enter stays the select key for dialogs.
    (QT['Return'], STD['Enter']), (QT['Enter'], STD['Device3']),
    (QT['Up'], STD['Up']), (QT['Down'], STD['Down']),
    (QT['Left'], STD['Left']), (QT['Right'], STD['Right']),
    # non-printing keys use Symbian scan codes (mapped by EKA2L1's key table)
    (QT['Backspace'], STD['Backspace']), (QT['Tab'], STD['Tab']),
    (QT['Escape'], STD['Escape']),
    (QT['Shift'], STD['LeftShift']), (QT['Control'], STD['LeftCtrl']),
    (QT['Alt'], STD['LeftAlt']),
    (QT['Home'], STD['Home']), (QT['End'], STD['End']),
    (QT['PageUp'], STD['PageUp']), (QT['PageDown'], STD['PageDown']),
    (QT['Insert'], STD['Insert']), (QT['Delete'], STD['Delete']),
]
# printable: Qt's key code for these is the character itself (letters arrive
# as upper case), and EKA2L1 passes scan codes 0x1d..0x5f through unchanged.
binds += [(c, c) for c in range(0x20, 0x60)]

if not os.path.isfile(CONF):
    sys.exit('no %s - run the emulator once first' % CONF)
os.makedirs(BIND, exist_ok=True)
with open(os.path.join(BIND, 'qwerty.yml'), 'w') as f:
    for qt, std in binds:
        f.write('- source:\n    type: key\n    data:\n      keycode: %d\n'
                '  target: %d\n' % (qt, std))
conf = open(CONF).read()
conf, n = re.subn(r'(?m)^current-keybind-profile:.*$',
                  'current-keybind-profile: qwerty', conf)
if not n:
    conf += '\ncurrent-keybind-profile: qwerty\n'
open(CONF, 'w').write(conf)
print('wrote bindings/qwerty.yml (%d keys); current profile = qwerty' % len(binds))
