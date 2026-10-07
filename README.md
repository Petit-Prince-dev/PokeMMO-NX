# PokeMMO-NX

Plays PokeMMO natively on a Nintendo Switch, in Horizon OS, the console's official OS. No need to install Android or Linux anymore :D

The official Linux ARM64 PokeMMO client runs as is, through a loader written in C on top of libnx.

Unofficial project, not affiliated with PokeMMO. The release zip contains the official unmodified PokeMMO client (Linux ARM64 part only), which belongs to the PokeMMO team. The ROMs are not included, you must use your own.

Version 1.1.3 - author: Petit_Prince

## Just want to play?

Download `PokeMMO-NX-XXXXX.zip` from the Releases page. It contains everything needed.

1. Copy the `switch` folder of the zip to the root of the SD card.
2. Put your ROMs in `switch/PokeMMO/isolate-root/game/roms`.
3. Start hbmenu **in application mode** (hold R while launching a game) and launch "PokeMMO". The game starts at once.

The first launch builds its caches from your ROMs: 3 to 5 minutes of black screen, then it is fast.

**Launch from the home menu (optional).** The zip also holds `switch/PokeMMO/PokeMMO-forwarder.nsp`. Install it with DBI (Install from the SD card): PokeMMO then appears in the home menu and starts directly, no need to hold R. Like any forwarder it needs the usual signature patches, and it launches `switch/PokeMMO/PokeMMO.nro`, so keep the folder where it is.

## What is adapted to the Switch

- **Dynamic Resolution:** 1280x720 in handheld mode, 1920x1080 when docked. It changes while you play when you dock or undock the console.
- **Touch screen:** a tap is a left click, a long press (finger held in place) is a right click, moving the finger drags.
- **Cursor with the controller:** click the left stick (L3) to turn a cursor on or off. While it is on, the left stick moves it, ZR is the left click, ZL the right click, and the game does not see the controller.
- **D-pad:** it is now mirrored on the left stick, so it moves your character and selects in menus like the stick does. Because of this, you lose the 4 D-pad hotkeys (1 to 4), but you can rebind 2 hotkeys to L and R, which the game doesn't use!
- **Keyboard:** click the right stick (R3) to open or close the console's keyboard (login, chat).
- **File chooser:** buttons such as "Select File" open a chooser drawn over the game. It shows the game's folders and the whole SD card (folder `sd`).

## Performance

Even at default clocks the game can run at 120 fps. However, it suffers from loading stutters. I would recommend keeping it at 60 fps and using a light overclock to remove the stutters.

## Diagnostics

Nothing is written by default. To get a log, create an empty file named `debug.enabled` in `switch/PokeMMO` on the SD card, restart, reproduce the problem, leave the game, then read `switch/PokeMMO/diagnostics.log`.

## Build (Windows, devkitPro)

Requirements: devkitPro with devkitA64, libnx and the Switch portlibs (Mesa/EGL, zlib); Python 3.

```powershell
python tools/fetch_client.py            # the official client into private/ (once)
python tools/fetch_runtime_libs.py      # libstdc++ / libgcc_s from Debian 12 (once)
powershell -File tools/build.ps1        # native/PokeMMO.nro
python tools/prepare_sd.py --client private/PokeMMO-Client.zip   # artifacts/sdmc and the release zip artifacts/PokeMMO-NX-<client revision>.zip
```

The home menu launcher is an NSP made with an online NSP forwarder generator (NRO path `/switch/PokeMMO/PokeMMO.nro`). It is signed with the keys of whoever generates it, so it is not in the repository: add `--forwarder <file.nsp>` to put it in the zip.

The zip holds the application, the Linux ARM64 part of the client and the two C++ runtime libraries (unmodified, from Debian, see `NOTICE.txt` in it). Without `--client` the zip has no client. The C code is formatted with `clang-format` (`native/.clang-format`).

## License

MIT, see `LICENSE`. The libraries linked into the application have their own licenses, see `THIRD-PARTY-NOTICES.md`. The PokeMMO client in the release zip belongs to the PokeMMO team.

## How it works

- `main.c`, `game.c`: start the client's `main` on its own thread, wait for it, and leave the way a regular game does.
- `elf_*.c`, `linux_dl.c`: ELF loader for the client and its libraries.
- `linux_*.c`: the Linux libc the client expects (files, memory, threads, sockets...), adapted to Horizon. An unsupported call is refused cleanly.
- `linux_sdl*.c`, `linux_audio*.c`, `linux_al.c`: SDL3 (window, events, controller, keyboard) and OpenAL written in C, with Mesa/EGL for the graphics.
- `linux_gtk.c`, `linux_file_picker.c`: the file chooser.
- `diagnostics.c`: the optional log.
