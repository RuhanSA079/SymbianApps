# SymbianApps

Build Symbian^3 apps (Nokia E7, N8, C7, …; Anna/Belle) on Linux. No Windows, no Wine, no IDE. Includes **rSSH**, an experimental SSH client built on PuTTY 0.85, and a port of the **NetSurf** web browser.

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
4. Fetch third-party sources, each checked against a pinned commit, signature or SHA-256: PuTTY 0.85 for rSSH; mbedTLS and cJSON for `apps/common`; NetSurf (git, pinned in `env/netsurf.lock`), expat, libpng and libjpeg for NetSurf.
5. Generate build files. For NetSurf this includes one Linux build of it in a throwaway Ubuntu container (about 10 minutes), which produces its generated sources.
6. Build every app in `apps/`. Signed packages go to `out/*.sisx`.

It needs `docker`, `git`, `curl`, `wget`, `gpg`, `perl`, `python3` and `md5sum` on the host.

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
apps/common/                   shared static libraries: rsym_mbedtls.lib (mbedTLS 4.1.1),
                               rsym_net.lib (RSocket TCP, HTTPS client, cJSON, debug log)
apps/rdrive/                   rDrive: Google Drive client (work in progress)
apps/netsurf/                  NetSurf browser: symbian/ (app, fetcher, display surface),
                               patches/ (applied to NetSurf), tools/, data/;
                               src/, expat/, libpng/, libjpeg/ (fetched)
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
- **After upgrading PuTTY** regenerate the build files with `python3 apps/rssh/tools/gen-mmp.py`.

## rDrive (work in progress)

A Google Drive client for Symbian^3, on modern TLS from `apps/common`.

- **Working:** HTTPS to Google from the phone. mbedTLS 4.1.1 runs on native sockets on a worker thread; in EKA2L1, *Options → Test HTTPS* reaches `www.googleapis.com` over TLS 1.3 (AES-256-GCM) and gets HTTP 200 in about 1.7 s.
- **Login design:** Symbian's browser can't do a Google sign-in, so you log in once on a PC with `env/rdrive-auth.py` (OAuth desktop flow, PKCE, your own Google Cloud OAuth client). It writes `keys/rdrive-token.json` for the phone. The script is written but untested.
- **Not done yet:** the Drive code on the phone (`src/gdrive.h` is only the interface so far): token import and refresh, folder listing, download and upload, plus the file-list UI.

## NetSurf

A port of the [NetSurf](https://www.netsurf-browser.org/) web browser (current git, pinned in `env/netsurf.lock`): HTML, CSS, PNG, JPEG, GIF, BMP and SVG, over HTTP and HTTPS. There is no JavaScript yet.

- **How it fits together:**
  - NetSurf's framebuffer frontend draws into a libnsfb surface backed by an ordinary bitmap, which the Avkon app (`apps/netsurf/symbian/nsapp.cpp`) shows. An active object runs NetSurf one step at a time, so there is no blocking main loop.
  - Fetching uses `apps/common`'s HTTP(S) client (native sockets and mbedTLS, TLS 1.3) on worker threads, in place of curl. Certificates are checked against NetSurf's CA bundle, which comes from Mozilla's.
  - Changes to NetSurf itself are small, in `apps/netsurf/patches/`. After editing `apps/netsurf/src`, regenerate the patches with `apps/netsurf/tools/mkpatch.sh netsurf libnsfb`.
- **App icon:** NetSurf's own globe logo. `prebuild.sh` converts `frontends/gtk/res/netsurf.xpm` into an 88×88 bitmap with a soft mask (`tools/gen-icon.py`), and `mifconv` packs it into `netsurf_aif.mif`/`.mbm`.
- **Building:** NetSurf and its libraries are not built by abld. abld names object files by file name only, and NetSurf has many files with the same name. Instead, `apps/netsurf/prebuild.sh`, which `env/build.sh` runs first, compiles them with abld's exact GCC flags into `ns_*.lib` archives that the `.mmp` links.
- **Using it:**
  - Tap a link to click it; drag on the page to scroll.
  - The toolbar has back, history, forward, stop, reload and the address bar.
  - *Options*: go to address (or search), back, forward, reload, stop, home page, debug log, About, Exit.
  - *Options → Screen orientation*: landscape (the default, for the E7's keyboard), portrait or automatic. The choice is remembered.
  - The keyboard types into pages and the address bar, including Ctrl+C, Ctrl+V and other Ctrl shortcuts.
- **Testing:**
  - `C:\Data\netsurf-url.txt` (first line) opens that page at start.
  - With the debug log on, the app writes `netsurf-debug.log` and NetSurf's own `netsurf.log` to `X:\private\E5A1E030\` on the drive NetSurf is installed on. In EKA2L1 that is usually `emu-data/EKA2L1/data/drives/e/private/E5A1E030/`.
- **In EKA2L1:** pages work but load slowly. Each TLS handshake takes 10–40 s there (rDrive's lone request takes about 5 s), and the emulator delivers timer events about once a second. Expect real hardware to be much faster, but it has not been tested yet.
- **Not yet:** JavaScript (Duktape is in NetSurf's tree, but not built yet); proper fonts (NetSurf's built-in bitmap font is used for now); connection reuse; downloads; certificate-error override; testing on a real phone.

## Older phones: S60 3rd Edition (Nokia E90, E71, N95, ...)

rSSH also builds for S60 3rd Edition FP1 (Symbian OS 9.2). Symbian apps run on newer releases too, so that package should also install on FP2, 5th Edition and Symbian^3 phones.

```sh
env/fetch-s60v31.sh                      # SDK, Open C and Nokia's pips.sis -> downloads/s60v31/
./sym sh env/install-sdk-s60v31.sh       # -> sdk/s60v31 (1.2 GB)
SYM_SDK=s60v31 ./sym sh env/build.sh apps/rssh    # -> out/rssh_s60v31.sisx
```

- **SDK switch:** `SYM_SDK=s60v31` makes `./sym` use `sdk/s60v31`, and makes `env/build.sh` take its packages from `sis/s60v31/`.
- **One source for both:** code that needs Symbian^3-only APIs checks `#ifdef SYMBIAN_CRYPTOSPI`, which only the Symbian^3 SDK defines.
- **P.I.P.S.:** FP1 phones don't have it, so the S60 3rd package embeds Nokia's Symbian-Signed `pips.sis`. The phone's installer skips it where P.I.P.S. is already present.

### Release builds for both

```sh
env/build-release.sh apps/rssh           # out/release/rssh-<version>-symbian3.sisx
                                         #             rssh-<version>-s60v3.sisx + SHA256SUMS
```

- **Choosing SDKs:** pass `symbian3` or `s60v31` after the app to build only one.
- **Version:** taken from the package header.

### S60 3rd in EKA2L1

```sh
env/fetch-firmware-e90.sh                # emulator closed; builds an E90 (RA-6) device
./emu --device RA-6 --install out/release/rssh-0.1.0-s60v3.sisx
./emu --device RA-6 --app rSSH
```

- **Why a script and not the install wizard:** EKA2L1's firmware installer can't use the E90's 2007 flash files.
  - Its blocks aren't padded to 512 bytes.
  - The core ROM is deflate-compressed.
  - The language variant (ROFx) points back into ROFS1.
- **What the script does instead:**
  - `env/tools/bb5-device.py` builds the device the way the installer would, from Nokia's firmware 400.34.93: `roms/ra-6/SYM.ROM`, the Z: drive, and a `devices.yml` entry.
  - `env/tools/wsini-fix.py` adds a missing alternate-screen-mode line to the E90's `wsini.ini`. Without it EKA2L1 dereferences a missing entry and crashes when an app starts.
- **Alternative:** `env/fetch-firmware-s60v3.sh` adds an E71 (RM-346), also S60 3rd FP1, from a ready-made EKA2L1 dump.
- **Own drives:** both get separate C: and E: drives. Otherwise the 2009 P.I.P.S. libraries from S60 3rd packages would land on the E7's shared E: drive and could shadow its newer ones.
- **Switching back:** `--device` changes the emulator's default phone; use `--device RM-626` to go back to the E7.

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
