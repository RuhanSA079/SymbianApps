# SymbianApps

Build Symbian^3 apps (Nokia E7, N8, C7, …; Anna/Belle) on Linux. No Windows, no Wine, no IDE. Includes **rSSH**, an experimental SSH client built on PuTTY 0.85.

- **Toolchain:** [GnuPoc](https://github.com/mstorsjo/gnupoc-package), which provides native Linux versions of `elf2e32`, `rcomp`, `makesis`, `signsis` and others. The compiler is **GCC 14.2** for `arm-none-symbianelf`, built from source in the Docker image. The original CodeSourcery GCCE 3.4.3 is still available.
- **Container:** everything runs in an Ubuntu 16.04 Docker image, so the host stays clean.
- **SDK:** the Nokia Symbian^3 SDK v1.0. It is **not** redistributable, so it is never committed or baked into the image. It is downloaded from the archive.org mirror and installed into `./sdk`, which is git-ignored.

## Setup and build, from a fresh checkout

```sh
env/bootstrap.sh
```

The script runs these steps, skipping any that are already done, so it is safe to re-run:

1. Download the SDK zip (866 MB, MD5-checked) into `downloads/`.
2. Build the `symbian3-env` image. The first build takes 30–40 minutes, mostly spent compiling GCC.
3. Unpack and patch the SDK into `sdk/symbian3` (2.5 GB).
4. Fetch PuTTY 0.85 for rSSH, checking its GPG signature and SHA-256, and apply `apps/rssh/patches/`.
5. Generate rSSH's build files.
6. Build every app in `apps/`. Signed packages go to `out/*.sisx`.

It needs `docker`, `wget`, `gpg`, `perl`, `python3` and `md5sum` on the host.

## Build one app

```sh
./sym sh env/build.sh apps/rssh       # -> out/rssh.sisx
```

- **Signing key:** the first build creates a personal self-signing key in `keys/`, which is git-ignored. Back it up: upgrading an installed app requires a package signed with the same key.
- **`./sym`:** runs any command inside the container, in the current directory. Use `./sym` with no arguments for a shell. Inside the container the usual SDK commands work: `bldmake bldfiles`, `abld build gcce urel`, `makesis`, `signsis`.
- **Compiler:** GCC 14 is the default. `GCCE_BIN=/opt/symbian/csl-gcc/bin ./sym sh env/build.sh apps/hello` uses GCCE 3.4.3 instead. `build.sh` does a clean rebuild whenever the compiler changes.
- **Failures:** GnuPoc's `abld` exits 0 even when compiling fails. `build.sh` checks its output instead and stops with `==> BUILD FAILED`.

## Layout

```
apps/hello, apps/helloclaude   minimal Avkon GUI apps (templates)
apps/rssh/                     rSSH: src/ (C++ UI), symbian/ (platform layer),
                               data/ (resources), about.txt, tools/gen-mmp.py,
                               patches/ (applied to PuTTY), putty/ (fetched)
env/Dockerfile                 toolchain image (GnuPoc + GCC 14 + GCCE 3.4.3)
env/bootstrap.sh               everything above in one go
env/*.sh, *.py, fix-perl.pl    fetch / install / build / signing / emulator helpers
env/test-sshd/                 throwaway SSH server for testing rSSH
sym, emu                       wrappers: build container, EKA2L1 emulator
```

To start a new app, copy `apps/hello` and change the UID (`0xE5A1E001`) in the `.mmp`, `_reg.rss`, `.pkg` and `.cpp` files.

## rSSH

An SSH client for Symbian^3, built on PuTTY 0.85.

- **Code:**
  - PuTTY's portable core (SSH, crypto, terminal emulation) is used unmodified, apart from one small patch.
  - `apps/rssh/symbian/` is the platform layer: native `RSocket` networking, the event loop, entropy, and storage (PuTTY's Unix storage code, kept in the app's private folder).
  - `apps/rssh/src/rssh.cpp` is the Avkon UI: the connections list, terminal view, dialogs and About page.
- **Features:**
  - the start screen lists saved connections, with *Quick connect* at the top;
  - connections can be added, edited and deleted;
  - host keys are confirmed and remembered;
  - password prompts appear in the terminal;
  - Ctrl comes from the keyboard or from *Options → Ctrl + next key*;
  - *Back* asks before exiting or disconnecting;
  - *Settings*: debug logging (off by default), export or clear the log, forget all host keys.
- **Not yet:** port forwarding, key-based authentication from the UI, scrollback, a monospace font, and testing on real hardware.
- **After upgrading PuTTY** or editing `about.txt`, regenerate the build files with `python3 apps/rssh/tools/gen-mmp.py`.

## Self-signed limits

A self-signed `.sisx` installs on a stock E7 with these limits:
- **UIDs:** use the unprotected `0xE…` range for test builds and `0xA…` for releases.
- **Capabilities:** only user-grantable ones: `NetworkServices`, `LocalServices`, `ReadUserData`, `WriteUserData`, `UserEnvironment`, `Location`. rSSH needs only `NetworkServices`.
- **Phone settings:** set *Settings → Application manager → Installation settings → Software installation* to **All**, and turn off the online certificate check.
- **Phone clock:** must be correct. The certificate is valid starting from its creation time.

## Testing in the EKA2L1 emulator

```sh
env/fetch-emulator.sh              # EKA2L1 AppImage -> tools/ (SHA-256 checked)
env/fetch-firmware.sh classic      # E7 firmware -> downloads/firmware/ (see below)
./emu                              # GUI: Devices -> install -> "Firmware (VPL)"
env/emu-qwerty-keys.py             # with the emulator closed: PC keyboard -> E7 keys
./emu --device RM-626 --install out/rssh.sisx
DISPLAY=:0 ./emu --device RM-626 --app rSSH
env/test-sshd.sh start             # test@127.0.0.1:2222, empty password
```

- **Emulator data:** `./emu` keeps all its data in `emu-data/`. The emulated `C:` drive is `emu-data/EKA2L1/data/drives/c/`.
- **Exiting apps:** when an app exits, EKA2L1 either closes (if started with `--app`) or restarts the emulated phone, and in this EKA2L1 build that restart crashes. So exiting rSSH closes the emulator: start it again with `./emu`. Apps that use the normal Symbian shutdown, like the hello templates, hang instead, because EKA2L1 never completes the framework teardown; rSSH avoids that by ending its process with `User::Exit` after its own clean-up. None of this applies on a real phone.
- **Debug log:** off by default. Turn it on in *Options → Settings → Debug logging* (or create an empty `C:\Private\E5A1E010\debug-logging.on`). The log, `C:\Private\E5A1E010\rssh-debug.log` (`emu-data/EKA2L1/data/drives/c/Private/E5A1E010/` on the PC), records each step, key presses (never their text) and traffic sizes, and survives crashes. *Settings → Export debug log* copies it to `E:\rSSH\` on a phone.
- **Default target:** `C:\Data\rssh-target.txt` pre-fills Quick connect.
- **Test server:** `env/test-sshd.sh` runs a throwaway OpenSSH container that listens **only on 127.0.0.1**, because its `test` account has no password.
- **Key profile:** EKA2L1's default profile maps only a few keys, and the emulator never reports Shift or Ctrl. `env/emu-qwerty-keys.py` and rSSH work around both. `` ` { | } ~ `` still cannot be typed in the emulator.
- **Crashes:** EKA2L1 segfaults when its window is closed (exit code 139), and when an app calls `abort()`. Both of its logs lose their last lines when that happens. rSSH reports failed assertions to its trace before exiting.
- **Networking:** EKA2L1 lacks the socket `ioctl`s that P.I.P.S. `select()` needs, which is why rSSH uses native `RSocket`s.
- **SDK emulator:** the SDK's own emulator (`epoc.exe`) needs the proprietary WINSCW/CodeWarrior compiler, so it is not supported here.

### Emulator firmware

`env/fetch-firmware.sh` downloads E7 (RM-626) firmware from archive.org, checks its SHA-1, and unpacks it into `downloads/firmware/<variant>/` with `unar` or `unrar`. The firmware is Nokia's copyrighted code: never commit it.

- **`classic`:** Belle Refresh 111.040.1511, LTA variant, SymbianLatino build that is patched to accept unsigned SIS files. It is missing the `emmc` file (factory contents of the phone's built-in storage), which EKA2L1 skips.
- **`stock`:** 111.030.0609 and 111.040.1511, India variant.

## Known quirks

- GnuPoc `makesis` rejects output file names shorter than 8 characters; `build.sh` works around this.
- `env/fix-perl.pl` patches the SDK's Perl scripts for Perl ≥ 5.22 (`defined(@array)` is now fatal). `env/patch-sdk-gcce.sh` adapts the SDK's GCCE build rules to GCC ≥ 4. `install-sdk.sh` runs both.
- GCC's `arm-none-symbianelf` target needs two fixes in `env/build-gcce.sh`: the `newlib-stdint.h` type macros, and a `stdint.h` for building `libsupc++`.
- The warning `Use of uninitialized value $1 in lc at e32variant.pm` is harmless.
