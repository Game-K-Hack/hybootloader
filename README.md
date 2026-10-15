<p align="center">
  <img src="docs/logo_readme.png" alt="HyBootloader" width="380">
</p>

<p align="center">
  <strong>A tiny native launcher for the LG Gen5 head unit: one button per program, a floating icon to come back.</strong>
</p>

<p align="center">
  <a href="https://github.com/Game-K-Hack/hybootloader/releases/latest">
    <img alt="Download HyBootloader" src="https://img.shields.io/badge/Download-latest%20version-7C3AED?style=for-the-badge&logo=windows&logoColor=white">
  </a>
</p>

<p align="center">
  <a href="https://github.com/Game-K-Hack/hybootloader/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/Game-K-Hack/hybootloader?label=version&color=7C3AED"></a>
  <img alt="Windows CE 7" src="https://img.shields.io/badge/Windows%20CE-7.0-0078D4">
  <img alt="ARMv4I" src="https://img.shields.io/badge/CPU-ARMv4I%20(Thumb--2)-5B21B6">
  <img alt="C / Win32" src="https://img.shields.io/badge/code-C%20%2F%20Win32-555555">
  <img alt="No configuration" src="https://img.shields.io/badge/configuration-none-7C3AED">
  <img alt="License" src="https://img.shields.io/badge/license-MIT-2E7D32">
</p>

<p align="center">
  <a href="#features">Features</a> ·
  <a href="#installation">Installation</a> ·
  <a href="#how-it-is-launched">How it is launched</a> ·
  <a href="#behaviour">Behaviour</a> ·
  <a href="#building">Building</a> ·
  <a href="#development">Development</a>
</p>

---

HyBootloader replaces the `Aerith.exe` launcher found on modded SD cards of the
LG Gen5 head unit (Hyundai i20 GB, Windows CE 7, i.MX6). The usual launcher is an
MFC application of 1.3 MB that needs several DLLs and a configuration file;
HyBootloader is a single C/Win32 executable of a few tens of kilobytes, with
**nothing to configure**: drop your programs in a `programmes` folder and each one
gets its own button.

## Features

| | |
|---|---|
| **One button per program** | Every `*.exe` of the `programmes` folder becomes a tile, named after the file (`HyLink.exe` → "HyLink") and sorted alphabetically. Up to 24 programs. |
| **No duplicates** | A program that is already running (started by HyBootloader or elsewhere) is brought back to the front instead of being started twice. A green dot marks running programs. |
| **Floating icon** | As soon as another application comes to the front, HyBootloader steps aside and a small icon stays above everything. Tap it to come back; drag it anywhere on the screen. |
| **Always up to date** | The folder is read again every time HyBootloader comes back to the front: a program copied onto the card shows up without restarting anything. |
| **Back to the LG interface** | **—** returns to the stock interface and keeps the floating icon; **×** quits and restarts the stock interface if it is not running. |
| **Single instance** | Starting it a second time brings the open instance back instead. |
| **Tiny and native** | No MFC, no DLL, no configuration file: one executable built with the Windows CE 7 SDK. |

## Installation

1. [Download the latest release](https://github.com/Game-K-Hack/hybootloader/releases/latest)
   (`Aerith.exe`).
2. Lay out the root of the head unit's SD card like this:

   ```
   Aerith.exe               <- HyBootloader
   eu20_upgrade.lgu         <- the update package, which embeds a copy of Aerith.exe
   programmes\
      HyLink.exe
      SomeLibrary.dll       <- a program's DLLs go next to it
      TotalCommander.exe
      ...
   ```

3. Put the same `Aerith.exe` **inside** `eu20_upgrade.lgu`, with
   [LGU-Manager](https://github.com/Game-K-Hack/lgu-manager):

   ```
   python lgu_cli.py aerith     eu20_upgrade.lgu              # shows the embedded launcher
   python lgu_cli.py set-aerith eu20_upgrade.lgu Aerith.exe   # replaces it in place
   ```

   The replacement updates the header, the CRC and the executable in place; putting
   the original launcher back gives a file identical to the original, byte for byte.

4. On the head unit, press **Update**: HyBootloader opens.

Only the `.exe` files directly inside `programmes` become buttons (not those in
sub-folders); an `Aerith.exe` found there is ignored. The folder is looked for next
to the running `Aerith.exe` first, then at the root of the card
(`\SDMemory\programmes`), which is the case when started by **Update**.

## How it is launched

The **Update** button of the LG interface does not run `Aerith.exe` from the root of
the card. The stock interface (`Lisa.exe`) opens `eu20_upgrade.lgu`, decrypts its
header, **extracts the launcher embedded in the package**, copies it to
`\Aerith.exe` and runs it. That is why the launcher must be replaced inside the
`.lgu` file.

The copy at the root of the card still matters: the interface checks that it exists
before extracting the embedded one, and other firmware paths (card inserted in
update mode, recovery) run it directly. Keep the **same** executable in both places.

While a window of class `AERITH` exists, the stock interface locks the physical
keys. HyBootloader uses other class names, so the keys keep working.

## Behaviour

| Action | Effect |
|---|---|
| Tap a tile | starts the program, or brings its window back if it is already running |
| Green dot on a tile | the program is running |
| Another application comes to the front | HyBootloader hides, the floating icon appears |
| Drag the icon | moves it (kept on screen), without starting anything |
| Tap the icon | HyBootloader comes back to the front, the icon disappears |
| **—** button | back to the LG interface, the icon stays |
| **×** button | quits everything (icon included); restarts `Lisa.exe` if it is not running |
| Second start of `Aerith.exe` | brings the open instance back |

A program without a window (a background agent) is never started twice: tapping its
tile again does nothing.

## Building

Requirements: Visual Studio 2008 with its Windows CE cross-compiler, the head unit's
Windows CE 7 SDK (ARMv4I), and `rc.exe` from the Windows SDK.

```
rc /dUNDER_CE /d_WIN32_WCE=0x700 /foAerith.res Aerith.rc

cl /nologo /c /O1 /W3 /GS- /QRarch4T /QRinterwork-return ^
   /D_WIN32_WCE=0x700 /DUNDER_CE=0x700 /DWINCE /DARM /D_ARM_ /DARMV4I ^
   /DUNICODE /D_UNICODE Aerith.c

link /nologo /subsystem:windowsce,7.00 /machine:THUMB ^
     /nodefaultlib:oldnames.lib /nodefaultlib:libc.lib /out:Aerith.exe ^
     Aerith.obj Aerith.res coredll.lib corelibc.lib toolhelp.lib
```

`cl` and `link` are the ARM cross-tools (`VC\ce\bin\x86_arm`), with `INCLUDE` and
`LIB` pointing to the SDK's `Armv4i` folders. The result is a Thumb-2 PE for the
Windows CE GUI subsystem. Its file version is `9.9.9.9`, above the one of the
launcher it replaces, so a tool that compares versions before copying keeps
HyBootloader.

## Development

```
Aerith.c       the whole launcher: program list, main window, floating icon
Aerith.rc      version information
ui/            application icon (hybootloader.ico, hybootloader.png)
docs/          README logo
```

The code is C89 (declarations at the top of each block) for the Visual Studio 2008
C compiler. A few Windows CE gaps are worked around in the code: no
`CreateEllipticRgn` (shapes are built from one-pixel bands), no `CreateFontW`
(`CreateFontIndirectW` instead), and a Toolhelp snapshot is closed with
`CloseToolhelp32Snapshot`.

**Publishing a version**: raise `PRODUCTVERSION` in `Aerith.rc`, build, then create
the GitHub release `x.y.z` (no "v") with `Aerith.exe` attached.

## License

MIT.

![](https://dawn-wave-6f61.hack-h.workers.dev/?project=github%2Fhybootloader)
