# libzclpresentation

`lib/presentation` is the reusable C23 native-window layer for ZClassic23.
It accepts one bounded, tightly packed RGB/RGBA bitmap and presents it through
the operating system's native window surface. QR deposits are the first
consumer; charts, Metaverse property views, and reviewed ZCode/App output can
render into the same pixel contract without acquiring wallet, network,
filesystem, or process-launch authority.

The stable window surface is `include/presentation/presentation.h`. The
renderer-neutral agent surface is
`lib/presentation/include/presentation/model.h`: a closed,
bounded document for status, tables, progress, charts, timelines, code diffs,
evidence graphs, choices, confirmations, forms, canvases, and QR cards. Its
wire format carries inert text, fractions, graph edges, exact-root labels, and
bounded action IDs—never callbacks, executable names, paths, sockets, wallet
objects, or native handles. A returned action is only an observation; the full
node must independently recheck its root and policy before acting.

Models larger than one fixed viewport are deterministically partitioned into
at most 16 pre-rendered pages. Page Up/Down, arrow keys, Home/End, and the mouse
wheel select among those inert bitmaps inside the display host; pagination is
local visual state and never becomes a software-authority event. The fixed
layout makes the same bounded item sequence produce the same page count and
pixels independently of window size, while aspect-fit scaling and clipping
remain backend concerns.

The companion `include/presentation/canvas.h` is a bounded caller-owned RGB canvas
with clipped rectangles, lines, alpha logo blits, and embedded Basic Latin
text. It is the reusable layer for deposit cards, current balances, metadata,
and small software-rendered graphs. RGFW is a private implementation detail,
pinned under `vendor/rgfw`; callers never include its header. The backend uses
Win32 on Windows, Cocoa on macOS, and dynamically loaded X11 on Linux. Those are
OS/desktop APIs, not application dependencies. Rendering is software-only and
does not require OpenGL, GTK, Qt, libqrencode, Python, or a browser.

The full binary's resident boundary is
`app/views/src/ui_present_host.c`. On Linux it binds a mode-0600 filesystem
AF_UNIX endpoint inside a validated mode-0700 per-user runtime directory,
accepts only same-UID peers, binds every reply to a fresh 128-bit request
nonce, and forks disposable window workers from one warm same-binary parent.
It never opens the canonical datadir. QR cards and renderer-neutral models share this transport without
changing their separate deterministic compositors. The first software blit is
acknowledged separately from a later numbered action/dismissal event.
One 16-slot table owns every display-only and interactive worker. Capacity
exhaustion is a named refusal on Linux; it cannot escape into the detached
cold launcher. An unresolved interactive request ID is busy and cannot be
replaced by a display update, so the original exact decision channel remains
intact.
Non-interactive models with the same bounded request ID replace only their
prior display worker, so an agent can publish live reproduction/progress
frames without accumulating windows or putting authority in the visual
process. The replacement worker renders first, then the host retires the old
owned worker before acknowledging the new frame. This keeps display-server
teardown outside the new window's creation path without acknowledging while a
stale prior frame remains. Child ownership is reaped before replacement,
preventing PID reuse from redirecting the replacement signal. Linux display
workers also bind a
parent-death signal before creating a window, so a crashed resident host cannot
leave orphan instruments behind; the authoritative caller can resubmit the
same exact request ID and latest inert progress frame to a fresh host. The event
carries no authority: the calling
node or agent command must recheck the exact root, authentication, capability,
local policy, and plan/commit state. This early-dispatch host code path opens
no Internet socket or canonical datadir and calls no wallet, package execution,
publication, deployment, or consensus surface. Other desktop platforms retain
the existing same-binary native cold path while the
resident transport is ported; the renderer/model library itself remains
cross-linked on Linux, Windows, and macOS.

The canvas embeds a Basic-Latin-only Noto Sans subset (SIL OFL 1.1) and uses a
pinned stb_truetype snapshot (MIT/public domain) for antialiased software text.
Both are source-controlled under `vendor/typography`; neither adds a runtime or
system dependency.

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
