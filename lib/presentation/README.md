# libzclpresentation

`lib/presentation` is the reusable C23 native-window layer for ZClassic23.
It accepts one bounded, tightly packed RGB/RGBA bitmap and presents it through
the operating system's native window surface. QR deposits are the first
consumer; charts, Metaverse property views, and reviewed ZCode/App output can
render into the same pixel contract without acquiring wallet, network,
filesystem, or process-launch authority.

The stable public surface is
`include/presentation/presentation.h`. RGFW is a private implementation detail,
pinned under `vendor/rgfw`; callers never include its header. The backend uses
Win32 on Windows, Cocoa on macOS, and dynamically loaded X11 on Linux. Those are
OS/desktop APIs, not application dependencies. Rendering is software-only and
does not require OpenGL, GTK, Qt, libqrencode, Python, or a browser.

The library and example build with:

```sh
make presentation-lib
make presentation-demo
make presentation-relaunch
make presentation-desktop-install   # Linux, per-user application identity
make presentation-portability
```

`presentation-relaunch` is the visual edit loop: it incrementally rebuilds
only stale package objects, replaces the prior demo window, and returns to the
developer immediately. Whole-node LTO and the full test suite remain release
gates, not per-pixel iteration steps.

Linux task managers associate the stable
`org.zclassic.ZClassic23` `WM_CLASS` with the packaged desktop entry and SVG.
The install target publishes those two files to the operator's per-user data
directory; it is not a runtime dependency and the presentation ABI itself does
not gain filesystem access.

`presentation-portability` performs strict native compilation and, when the
installed MinGW compiler is present, a strict Windows cross-link. Hosted
portability CI repeats the link on Linux, Windows, and macOS.

ZCode boundary: audited built-ins such as Metaverse may call this library
directly. Fetched/third-party ZCode remains out-of-process by project policy;
its future broker may translate an explicitly granted local-presentation
capability into this ABI, but packages do not receive RGFW, process launch, or
raw desktop handles.
