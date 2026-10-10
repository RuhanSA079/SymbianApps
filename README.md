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
4. Fetch third-party sources, each checked against a pinned commit, signature or SHA-256: PuTTY 0.85 for rSSH; mbedTLS and cJSON for `apps/common`; minimp3 for rInternetRadio; NetSurf (git, pinned in `env/netsurf.lock`), expat, libpng and libjpeg for NetSurf.
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
                               rsym_net.lib (RSocket TCP, HTTPS client, cJSON, debug log,
                               remote debug log), rsym_audio.lib + rsym_mp3.lib (audio/:
                               MP3 stream player, minimp3; minimp3/ fetched)
apps/rdrive/                   rDrive: Google Drive client (work in progress)
apps/netsurf/                  NetSurf browser: symbian/ (app, fetcher, display surface),
                               patches/ (applied to NetSurf), tools/, data/;
                               src/, expat/, libpng/, libjpeg/ (fetched)
apps/rinternetradio/           rInternetRadio: src/ (app), data/, prebuild.sh
apps/rjellyfin/                rJellyfin: Jellyfin client (music), src/, data/, prebuild.sh
apps/rsharp/                   rSharp: C# scratchpad; engine/ (the interpreter, portable C++),
                               src/ (app), tests/ (host tests of the interpreter)
apps/rwifisniffdumper/         rWiFiSniffDumper (work in progress): driver/ (receive-only
                               RTL8188CUS/8192CU driver, portable C++), host/ (Linux
                               libusb tool), tests/, tools/gen-tables.py
env/test-sshd/                 throwaway SSH server for testing rSSH
env/test-jellyfin.sh           throwaway Jellyfin server (with test music) for rJellyfin
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
  - *Settings*: debug logging (off by default), export or clear the log, the [remote debug log](#remote-debug-log), forget all host keys.
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
- **App icon:** `data/netsurf-svgrepo-com.svg`, from [SVG Repo](https://www.svgrepo.com/). `prebuild.sh` puts it into `netsurf_aif.mif` with `mifconv`, unchanged: GnuPoc's `mifconv` cannot encode SVG to the binary SVG-T format, but the phone and EKA2L1 both read plain SVG. It must stay within SVG Tiny (no arcs in paths, for example), which the phone's icon engine reads; EKA2L1's app list shows only SVG icons, not bitmap ones. rSSH's icon, `apps/rssh/data/ssh-svgrepo-com.svg` (also from SVG Repo), is packed the same way by `apps/rssh/prebuild.sh`.
- **Building:** NetSurf and its libraries are not built by abld. abld names object files by file name only, and NetSurf has many files with the same name. Instead, `apps/netsurf/prebuild.sh`, which `env/build.sh` runs first, compiles them with abld's exact GCC flags into `ns_*.lib` archives that the `.mmp` links.
- **Using it:**
  - Tap a link to click it; drag on the page to scroll.
  - The toolbar has back, history, forward, stop, reload and the address bar.
  - *Options*: go to address (or search), back, forward, reload, stop, home page, screen orientation, Settings, About, Exit.
  - *Options → Settings* is a full-screen page: home page, zoom, text size, load images, block adverts, Do Not Track, screen orientation, the debug log (on/off, location, export to `E:\NetSurf\`, clear) and the [remote debug log](#remote-debug-log). Browsing options are saved to `X:\private\E5A1E030\Choices`, which is read over the defaults in `res/Choices`. Zoom applies at once; text size and images apply when you leave Settings (the page reloads).
  - *Options → Screen orientation*: landscape (the default, for the E7's keyboard), portrait or automatic. The choice is remembered.
  - The keyboard types into pages and the address bar, including Ctrl+C, Ctrl+V and other Ctrl shortcuts.
- **Testing:**
  - `C:\Data\netsurf-url.txt` (first line) opens that page at start.
  - With the debug log on, the app writes `netsurf-debug.log` and NetSurf's own `netsurf.log` to `X:\private\E5A1E030\` on the drive NetSurf is installed on. In EKA2L1 that is usually `emu-data/EKA2L1/data/drives/e/private/E5A1E030/`.
- **In EKA2L1:** pages work but load slowly. Each TLS handshake takes 10–40 s there (rDrive's lone request takes about 5 s), and the emulator delivers timer events about once a second. Expect real hardware to be much faster, but it has not been tested yet.
- **Not yet:** JavaScript (Duktape is in NetSurf's tree, but not built yet); proper fonts (NetSurf's built-in bitmap font is used for now); connection reuse; downloads; certificate-error override; testing on a real phone.

## rInternetRadio

Internet radio for Symbian^3: MP3 streams over HTTP or HTTPS, found through the [radio-browser.info](https://www.radio-browser.info/) directory.

- **Using it:**
  - The start screen lists your favourites, with *Search stations* (by name) and *Top stations* (the directory's most played MP3 stations) at the top. A few SomaFM channels are there on first run.
  - The first row always shows what is playing (station, song title, bitrate, volume). Select it to stop, or to play the last station again.
  - *Options*: play, stop, add to or remove from favourites, add a station by address (a stream, `.pls` or `.m3u`), volume, Settings (volume, screen orientation — landscape, portrait or automatic, remembered — debug log, [remote debug log](#remote-debug-log)), About.
  - Volume: left/right keys, the phone's volume keys or *Options*. It keeps playing in the background.
- **How it works:**
  - The player is shared with rJellyfin, in `apps/common/audio`. In `radio_engine.cpp`, a worker thread fetches the stream with `apps/common`'s HTTP(S) client, follows redirects and playlists, and takes out the ICY metadata (the song title). It decodes the MP3 with [minimp3](https://github.com/lieff/minimp3) (CC0; `env/fetch-minimp3.sh`, pinned) and queues about 1.5 s of PCM. Playback starts once 1 s is buffered. `audio_out.h` plays the PCM on the UI thread through `CMdaAudioOutputStream`.
  - `src/rinternetradio.cpp`: the Avkon UI. It reads the volume keys through RemCon. Directory searches run on a worker thread and are parsed with cJSON.
  - minimp3 decodes in floating point, which is far too slow with soft-float library calls. `apps/common/prebuild.sh` therefore builds it as `rsym_mp3.lib` with VFP code (`-mfloat-abi=softfp -mfpu=vfp`), because abld always adds `-msoft-float` after an `.mmp`'s own options.
  - HTTPS streams are checked against Mozilla's root certificates (NetSurf's copy, installed with the app).
- **Testing:** `C:\Data\rinternetradio-autotest.txt` with a stream URL on its first line plays it at start; `search:<name>` searches the directory instead. In EKA2L1 a 128 kbps SomaFM stream decodes faster than real time and plays without dropouts.
- **S60 3rd (E90):** in EKA2L1's E90 it plays a 128 kbps stream (see [Older phones](#older-phones-s60-3rd-edition-nokia-e90-e71-n95-)). minimp3 needs the phone's VFP unit, which the E90's ARM11 has; whether it keeps up on the real 330 MHz CPU is untested.
- **Not yet:** AAC, Ogg Vorbis and Opus streams (many stations use AAC; the directory search shows only MP3 stations for now); HLS; sleep timer; testing on a real phone.

## rJellyfin

A client for a [Jellyfin](https://jellyfin.org/) media server: sign in, browse the libraries with their artwork, and play music. Video is not supported yet.

- **Using it:**
  - First, set the server address (for example `http://192.168.1.10:8096`) and the user name, then *Sign in*. The password is asked for each time and never stored; the app keeps only the token the server gives it.
  - Browse *Libraries*. A music library has *Albums*, *Album artists* and *Songs*. Other libraries, playlists and folders open as they are. A list shows 100 items at a time, with *Load more* at the end.
  - Selecting a song plays it and then the rest of that list (an album in track order). The first row shows what is playing, with the time, the artist and the position in the queue. Select it to stop, or to play again.
  - *Options*: next and previous track, volume, Settings, About. Volume and track skipping also work with the phone's volume keys and with headset buttons.
  - *Settings*: sign out, volume, screen orientation, *Accept any certificate*, the debug log and the [remote debug log](#remote-debug-log). *Accept any certificate* is for an `https://` server with a self-signed certificate: the connection is still encrypted, but not checked.
- **How it works:**
  - Jellyfin's REST API, as JSON. Every request carries a `MediaBrowser` authorization header with the token and a device ID.
  - Artwork is fetched one image at a time, already resized by the server to the list's icon size. The phone's own image decoders (`CImageDecoder`) decode it.
  - Music comes from `/Audio/<id>/universal`, asking for MP3. MP3 files are sent as they are; FLAC, AAC, Opus and so on are converted by the server. They play through the player in `apps/common/audio`. Its *finished* state, and the audio output's "drained" callback, start the next track once the last one has been heard.
- **Testing:** `env/test-jellyfin.sh start` runs Jellyfin 10.10.7 in Docker on `127.0.0.1:8096`. On first start it creates user `test` (password `test`) and a Music library with an album of test tones: two FLAC tracks and one MP3, with a cover. `C:\Data\rjellyfin-autotest.txt` with `server=`, `user=` and `password=` lines signs in at start; `play=1` then plays the first album. In EKA2L1 it signs in, browses, shows the artwork and plays the whole album, FLAC (converted) and MP3.
- **S60 3rd (E90):** in EKA2L1's E90 it signs in, browses and plays the whole test album. On a real E90, a 192 kbps stream (the app's transcoding limit) is the heaviest load for its 330 MHz CPU; this is untested.
- **Not yet:** video (the E7's player wants an MP4 it can stream, so this probably needs a conversion on the server side); seeking; telling the server what is playing; testing on a real phone.

## rSharp

A C# scratchpad, in the spirit of LINQPad and RoslynPad: type C# expressions, statements and LINQ queries on the phone, run them, see the result. There is no .NET on Symbian, so rSharp has its own interpreter for a subset of C#.

- **Using it:**
  - The last line without `;` is the result, shown as LINQPad would (sequences as lists, objects as `{ A = 1 }`). `Console.WriteLine` and `x.Dump()` write to the output too. Integer results also show their hex and binary digits; *Options > Hex/binary of results* turns that off.
  - *Run* is the right softkey (or the navigation key's centre). On the output, up/down and left/right scroll and *Back* returns to the code. *Stop* stops a long run.
  - Errors: the line is shaded red, the cursor goes to it, and the message shows in a red strip above the softkeys until the code is edited. A compile error (a misspelt name or member, a syntax error) stays on the code screen; an exception shows the output first, and the line is marked on *Back*.
  - In EKA2L1 the editor's cursor isn't drawn (the emulator doesn't implement the text cursor yet); typing still goes where it is.
  - *Options*: examples, clear, screen orientation, *What works*, About. The code is kept between runs.
- **The language:**
  - Supported:
    - The types `int`, `uint`, `long`, `ulong`, `short`, `ushort`, `byte`, `sbyte`, `char`, `bool`, `float`, `double` and `string`, with C#'s rules: numeric promotion, wrap-around (`OverflowException` in `checked`), constants checked at compile time, integer division, shifts masked to the operand width, `>>>`.
    - Statements: `var` and typed variables, `if`, `for`, `foreach`, `while`, `do`, `switch` (with patterns), `break`, `continue`, `return`, `throw`.
    - Local functions, lambdas and closures, `Func<...>`, method groups (`.Select(Math.Sqrt)`).
    - Tuples (including `(a, b) = (b, a)`), anonymous objects, `is` patterns, switch expressions, `?.`, `??`, `^1`, `a..b`.
    - Arrays, collection expressions, `List`, `Dictionary`, `HashSet`, `StringBuilder`, string methods, `$"..."` with alignment and formats (`X8`, `N2`, `0.00`, `B` (binary), ...).
    - `Math`, `Convert` (including bases 2/8/16), `BitOperations` and `int.PopCount` and friends, `Parse` and `TryParse`.
    - LINQ: methods, lazy as in .NET (`Range(1, int.MaxValue).Where(...).First()` works), and query syntax (`from`, `where`, `let`, `join` (`into`), `orderby`, `group ... by` (`into`)).
  - Not supported: classes, structs and interfaces, `try`/`catch`, `decimal`, `async`, user generics, reflection, files, the network.
  - Static typing is approximate: variables keep the type they were declared with (so `double d = 1; d / 2` is 0.5), but some C# compile-time errors (a misspelt member, a wrong argument count) are only found when the line runs; they are then reported as compile errors, and the run's output is dropped, as if it hadn't run.
- **How it works:**
  - `apps/rsharp/engine` (about 9,000 lines of portable C++, built as gnu++98 like the rest): lexer, parser, a tree-walking evaluator, the library and LINQ.
  - All memory a run allocates is in an arena (20 MB at most on the phone), freed when the run ends. Errors `longjmp` back out. Lambda scopes and query rows are reused when nothing captured them, so long LINQ pipelines don't fill the arena.
  - It runs on a worker thread with a 256 KB stack. Recursion deeper than the stack allows ends with `InsufficientExecutionStackException` instead of a crash.
- **Testing:**
  - `apps/rsharp/tests/run-tests.sh` builds the interpreter on the host (with AddressSanitizer and UBSan) and runs `tests/cases.txt`: C# snippets with the output C# gives. `build/rs_test -e 'code'` runs one snippet.
  - `C:\Data\rsharp-autotest.txt` is run at start, and its output written to `C:\Data\rsharp-autotest-out.txt`. In EKA2L1, the E7 and the E90 give the same output as the host.
- **Not yet:** syntax colouring; a monospace font; copying the output; testing on a real phone.

## rWiFiSniffDumper (work in progress)

A WiFi scanner and monitor-mode sniffer for the E7, using a cheap Realtek RTL8188CUS/RTL8192CU USB adapter on the USB On-The-Go port. It only listens: it never transmits.

- **The driver:** `apps/rwifisniffdumper/driver` is a receive-only port of Linux rtlwifi's `rtl8192cu` (GPL-2.0), in portable gnu++98 C++.
  - It loads no firmware. On this chip the 8051 firmware only does rate adaptation and power saving, so a monitor should work without it, but that is untested on hardware.
  - The MAC's TX engine and TX DMA are never switched on, and all TX queues stay paused, so the adapter cannot send anything, ACKs included. IQ calibration (it loops the transmitter back) and TX power setup are skipped.
  - The platform provides register access (USB vendor control transfers) and passes in bulk-IN data. The driver returns frames with rate and signal, writes pcap files (radiotap), and reads beacons (SSID, channel, security).
  - `tools/gen-tables.py` makes `driver/rtlu_tables.cpp` from Linux's `table.c`.
- **Testing:**
  - `apps/rwifisniffdumper/tests/run-tests.sh` builds the driver on the host (with AddressSanitizer and UBSan). It tests the EFUSE, RX descriptor, radiotap and beacon parsing, and runs the whole bring-up against a simulated chip, checking that TX is never enabled.
  - `apps/rwifisniffdumper/host/build.sh` builds `build/host/rwifisniffdumper`, the driver on Linux over libusb. For example, `sudo build/host/rwifisniffdumper -H 1-13 -w scan.pcap` lists the networks heard and writes every frame for Wireshark. Run `lsusb` first to check the adapter is a 92C-family chip (`0bda:8176`, `0bda:8178`, ...): many "8188CUS" dongles are really RTL8188EUS/8188FTV, which this driver does not support.
- **Not yet:** a test with a real adapter; the Symbian side, which needs a USB host Function Driver Controller plugin (ProtServ + CommDD capabilities, so a hacked phone) plus the USBDI headers from the Symbian Foundation sources; the app.

## Remote debug log

rSSH, NetSurf, rInternetRadio, rJellyfin (and rDrive, through `rsym_log`) can send their debug log over the network to a PC as it is written, which is the easiest way to follow what happens on a real phone.

```sh
env/rlog-server.py                 # on the PC: listens on port 7865, prints this machine's IP
```

On the phone, in the app's *Settings*, set *Remote debug host* to the PC's IP address, leave *Remote debug port* at 7865, and turn *Remote debug log* on. Lines appear in the terminal and are saved to `out/rlog/<app>-<date>.log`. In EKA2L1 the PC is `127.0.0.1`.

- **Independent of the file log:** the remote log works whether or not *Debug log* is on. With it on at start-up, NetSurf also sends its own verbose log (lines starting `ns`).
- **Settings file:** `X:\private\<SID>\remote-debug.cfg`, keys `remote-debug`, `remote-debug-host`, `remote-debug-port`. Writing it into the emulator's drive is a quick way to turn it on there.
- **How it works:** `apps/common/net/rsym_rlog.cpp` queues lines (from any thread) into a 32 KB buffer that a background thread sends over TCP, reconnecting every 5 s while the server cannot be reached. When the buffer is full, newer lines are dropped and the server is told how many. It starts one network connection and keeps it, so a phone set to *Always ask* asks for an access point only once.
- **Protocol:** plain text, one line per log line; each connection starts with `#rlog1 app=<name>`. `nc -lk 7865` works as a bare-bones server.
- **Server options:** `--port`, `--bind`, `--grep REGEX` (print only matching lines; the saved file keeps everything), `--no-save`, `--log-dir`.

## Older phones: S60 3rd Edition (Nokia E90, E71, N95, ...)

rSSH, rInternetRadio, rJellyfin and rSharp also build for S60 3rd Edition FP1 (Symbian OS 9.2). Symbian apps run on newer releases too, so those packages should also install on FP2, 5th Edition and Symbian^3 phones.

```sh
env/fetch-s60v31.sh                      # SDK, Open C and Nokia's pips.sis -> downloads/s60v31/
./sym sh env/install-sdk-s60v31.sh       # -> sdk/s60v31 (1.2 GB)
SYM_SDK=s60v31 ./sym sh env/build.sh apps/common  # shared libraries, first
SYM_SDK=s60v31 ./sym sh env/build.sh apps/rssh    # -> out/rssh_s60v31.sisx
SYM_SDK=s60v31 ./sym sh env/build.sh apps/rinternetradio   # -> out/rinternetradio_s60v31.sisx
SYM_SDK=s60v31 ./sym sh env/build.sh apps/rjellyfin         # -> out/rjellyfin_s60v31.sisx
SYM_SDK=s60v31 ./sym sh env/build.sh apps/rsharp            # -> out/rsharp_s60v31.sisx
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
