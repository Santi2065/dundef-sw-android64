"""Regenerates the diagrams shown in the README.

    pip install matplotlib
    python docs/figures/make_figures.py
"""
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

HERE = Path(__file__).resolve().parent
for f in Path("/usr/share/fonts/lm").glob("lm*10-*.otf"):  # Latin Modern, if installed
    font_manager.fontManager.addfont(str(f))
plt.style.use(HERE / "paper.mplstyle")
C = plt.rcParams["axes.prop_cycle"].by_key()["color"]
INK, GRAY = "#1a1a1a", "#8c8c8c"


def tint(hex_color, a):
    """Mixes a palette color with white (a = share of the color)."""
    rgb = [int(hex_color[i:i + 2], 16) for i in (1, 3, 5)]
    return "#" + "".join(f"{round(255 - a * (255 - c)):02x}" for c in rgb)


def save(fig, name):
    fig.savefig(HERE / name, metadata={"Date": None})
    plt.close(fig)


def canvas(w, h, xmax, ymax):
    fig, ax = plt.subplots(figsize=(w, h))
    ax.set_xlim(0, xmax)
    ax.set_ylim(0, ymax)
    ax.set_aspect("equal")
    ax.axis("off")
    return fig, ax


def box(ax, x, y, w, h, title, body=None, color=None, fill=0.10, dashed=False, title_top=False,
        size=8.5, body_size=7.4, mono_title=False):
    ec = color or INK
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0,rounding_size=0.8",
                                fc=tint(color, fill) if color else "white", ec=ec,
                                lw=0.7, ls=(0, (4, 2)) if dashed else "-"))
    fam = "monospace" if mono_title else None
    if title_top:
        ax.text(x + 1.2, y + h - 1.0, title, ha="left", va="top", size=size, weight="bold",
                color=ec, family=fam)
        if body:
            ax.text(x + 1.2, y + h - 3.6, body, ha="left", va="top", size=body_size, color=INK,
                    linespacing=1.35)
        return
    cy = y + h / 2
    if body:
        ax.text(x + w / 2, y + h - 1.3, title, ha="center", va="top", size=size, weight="bold",
                color=ec, family=fam)
        ax.text(x + w / 2, y + h - 4.2, body, ha="center", va="top", size=body_size, color=INK,
                linespacing=1.35)
    else:
        ax.text(x + w / 2, cy, title, ha="center", va="center", size=size, weight="bold",
                color=ec, family=fam)


def arrow(ax, p, q, label=None, both=False, rad=0.0, loff=(0, 0), ha="center", color=INK):
    ax.add_patch(FancyArrowPatch(p, q, arrowstyle="<|-|>" if both else "-|>", mutation_scale=7,
                                 lw=0.7, color=color, shrinkA=0, shrinkB=0,
                                 connectionstyle=f"arc3,rad={rad}"))
    if label:
        mx, my = (p[0] + q[0]) / 2 + loff[0], (p[1] + q[1]) / 2 + loff[1]
        ax.text(mx, my, label, ha=ha, va="center", size=7, style="italic", color=INK,
                bbox=dict(fc="white", ec="none", pad=0.6))


# ---- Figure 1: loader architecture inside the app process
fig, ax = canvas(7.6, 5.6, 100, 73)
box(ax, 1, 1, 98, 71, "", color=GRAY, fill=0.0, dashed=True)
ax.text(2.5, 70.6, "One 64-bit Android app process", ha="left", va="top", size=8, style="italic",
        color="#4d4d4d")

box(ax, 4, 57, 92, 10, "Java layer: the game's own Activity, patched by build.py",
    "data downloader pointed at the mirror  ·  EGL config fix  ·  full resolution, 144 Hz request\n"
    "LAN multicast lock  ·  single-line text input  ·  tablet aspect fix",
    color=C[3], fill=0.08)

box(ax, 4, 16, 58, 37, "", color=C[0], fill=0.03)
ax.text(60.5, 52, "libDunDef-Android.so: 64-bit loader", ha="right", va="top", size=8.2,
        weight="bold", color=C[0])
B = dict(color=C[0], fill=0.12, size=8.2, body_size=7.0)
box(ax, 6, 33.5, 25.5, 14, "JNI bridge",
    "fake JavaVM / JNIEnv in guest\nmemory; Java objects become\n32-bit handles; 18 natives", **B)
box(ax, 34.5, 33.5, 25.5, 14, "ELF loader",
    "maps the ARM32 library,\napplies relocations, runs its\nconstructors, stubs imports", **B)
box(ax, 6, 18, 25.5, 13.5, "High-level imports",
    "~280 functions: libc, math,\nsockets, pthreads, zlib,\nGLES 2, EGL", **B)
box(ax, 34.5, 18, 25.5, 13.5, "dynarmic JIT",
    "ARM/Thumb code to arm64,\none JIT per guest thread", **B)

box(ax, 67, 16, 29, 37, "", color=C[1], fill=0.03)
ax.text(94.5, 52, "Guest memory, low 4 GB", ha="right", va="top", size=8.2, weight="bold",
        color=C[1])
R = dict(color=C[1], fill=0.12, size=8.2, body_size=7.0)
box(ax, 69, 33.5, 25, 14, "libDunDefGuest.so",
    "the game's original 32-bit\nARM engine, unmodified,\nloaded as data", mono_title=True, **R)
box(ax, 69, 26.5, 25, 5, "guest heap (dlmalloc)", color=C[1], fill=0.12, size=7.4)
box(ax, 69, 18, 25, 6.5, "soft-float calls patched\nto VFP / IDIV code", color=C[1], fill=0.12,
    size=7.2)

box(ax, 4, 3, 92, 8.5, "Android system libraries",
    "bionic libc  ·  libGLESv2  ·  libEGL  ·  libz  ·  liblog", color=C[2], fill=0.10)

arrow(ax, (18.75, 57), (18.75, 47.5), both=True)
ax.text(20, 55, "JNI", ha="left", va="center", size=7, style="italic")
arrow(ax, (60, 40.5), (69, 40.5))
ax.text(64.5, 42, "maps", ha="center", va="center", size=7, style="italic")
arrow(ax, (60, 26), (69, 37.5))
ax.text(62.6, 33.4, "runs", ha="center", va="center", size=7, style="italic")
arrow(ax, (34.5, 24.75), (31.5, 24.75))
ax.text(33, 33.0 - 6.9, "svc", ha="center", va="bottom", size=7, style="italic")
arrow(ax, (18.75, 18), (18.75, 11.5))
ax.text(20, 13.7, "direct calls: a guest pointer is a host pointer", ha="left", va="center",
        size=7, style="italic")
save(fig, "fig1-loader-architecture.svg")


# ---- Figure 2: what build.py does to the user's APK
fig, ax = canvas(7.6, 2.75, 100, 36)
steps = [
    (1, "Your APK", "com.trendy-7.6.apk\n32-bit ARM only\n(SHA-256 checked)", C[5]),
    (21, "Decode", "apktool d\nsmali, manifest,\nassets, lib/", C[0]),
    (41, "Patch", "regex patches that\nmust match, plus\nfiles from patches/", C[0]),
    (61, "Swap native code", "loader takes the\nlibrary's name; the\noriginal is renamed", C[0]),
    (81, "Rebuild and sign", "apktool b,\nuber-apk-signer,\nlocal keystore", C[0]),
]
for x, title, body, col in steps:
    box(ax, x, 15, 18, 15, title, body, color=col, fill=0.10, size=8.2, body_size=7.2)
for x in (19, 39, 59, 79):
    arrow(ax, (x, 22.5), (x + 2, 22.5))
box(ax, 57, 1.5, 26, 9, "64-bit loader (port/)",
    "release build, SHA-256 checked,\nor --from-source (NDK r28)", color=C[1], fill=0.10,
    size=8, body_size=7.0)
arrow(ax, (70, 10.5), (70, 15))
box(ax, 85, 3, 14, 6, "DunDefSW-64.apk", color=C[2], fill=0.12, size=7.2, mono_title=True)
arrow(ax, (92, 15), (92, 9))
ax.text(1, 6.2, "lib/armeabi/libDunDef-Android.so → lib/arm64-v8a/libDunDefGuest.so\n"
        "64-bit loader                    → lib/arm64-v8a/libDunDef-Android.so",
        ha="left", va="center", size=6.2, family="monospace", color=INK, linespacing=1.5)
save(fig, "fig2-build-pipeline.svg")
