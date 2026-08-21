#!/usr/bin/env python3
#
# Copyright (c) 2025-2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

"""Desktop viewer for AURORA flight logs.

A Tkinter front end for the tools that already exist in this directory:
:mod:`binlog` decodes the data logger's binary flight logs (and the
converter's ``.csv`` / ``.influx`` output), and
:mod:`plot_flight_data` turns the decoded streams into the same panels
the command-line plotter produces.  Nothing is re-implemented here — the
GUI only picks a log, picks a window, picks panels, and embeds the
resulting matplotlib figure.

Two ways in:

* **Open Log…** — a single flight log file.
* **Open Folder…** — a data logger ``DATA`` directory.  Every flight log
  in it is listed; selecting one decodes it and lists the flights it
  contains.

Binary logs carry no state-machine audit trail, so the flight window is
recovered from the measurements themselves (the same heuristics the CLI
falls back to).  A ``state_audit`` file sitting next to the log is picked
up automatically and used instead.

Usage::

    python3 tools/flight_log_gui.py [PATH] [--theme {light,dark}]
"""

import argparse
import os
import queue
import sys
import threading
import traceback

import matplotlib

# The figures are embedded in Tk canvases built by hand, so pyplot must
# not try to open windows of its own.
matplotlib.use("Agg")

import matplotlib.pyplot as plt                                 # noqa: E402
from matplotlib.figure import Figure                            # noqa: E402
from matplotlib.backends.backend_tkagg import (                 # noqa: E402
    FigureCanvasTkAgg,
    NavigationToolbar2Tk,
)

import tkinter as tk                                            # noqa: E402
from tkinter import filedialog, messagebox, ttk                 # noqa: E402

import numpy as np                                              # noqa: E402

import binlog                                                   # noqa: E402
import plot_flight_data as pfd                                  # noqa: E402

try:
    from PIL import Image, ImageEnhance, ImageTk
except ImportError:                                    # pragma: no cover
    Image = ImageEnhance = ImageTk = None


TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
LOGO_PATH = os.path.join(TOOLS_DIR, os.pardir, "doc", "img", "logo.png")


class PlotToolbar(NavigationToolbar2Tk):
    """The matplotlib toolbar without its history and subplot buttons.

    Back/Forward navigate a view history that ``update()`` clears every
    time the figure is rebuilt — which here is every panel tick — so they
    spend their life disabled, and Tk renders a disabled image button as
    a stippled block that looks like damage on a dark background.  The
    subplot-parameter dialog is equally pointless: :meth:`_fit_figure`
    re-runs ``tight_layout`` on the next resize and discards whatever it
    set.  Home still resets the view, and Save is kept alongside the
    window's own Export button because it is where people look for it.
    """

    toolitems = [item for item in NavigationToolbar2Tk.toolitems
                 if item[0] not in ("Back", "Forward", "Subplots")]


# ---------------------------------------------------------------------------
# Widget palette — mirrors the Furo variables in doc/conf.py
# ---------------------------------------------------------------------------

UI_THEMES = {
    "light": {
        "bg":          "#ffffff",   # color-background-primary
        "surface":     "#f8f9fa",   # color-sidebar-background
        "border":      "#dce0e5",   # color-sidebar-background-border
        "fg":          "#333333",   # color-sidebar-link-text
        "muted":       "#5f6673",
        "brand":       "#1a5fb4",   # color-brand-primary
        "brand_hover": "#0d3a72",   # cover-bg gradient end
        "on_brand":    "#ffffff",
        "select":      "#e8f0fe",   # color-admonition-title-background--note
        "field":       "#ffffff",
        "logo_boost":  1.0,
    },
    "dark": {
        "bg":          "#1e2028",   # color-background-primary
        "surface":     "#111318",   # color-sidebar-background
        "border":      "#3a3d46",   # color-background-border
        "fg":          "#dcdee3",   # color-foreground-primary
        "muted":       "#8890a0",   # color-sidebar-caption-text
        "brand":       "#6ea8fe",   # color-brand-primary
        "brand_hover": "#9cc4ff",
        "on_brand":    "#111318",
        "select":      "#252830",   # color-background-secondary
        "field":       "#252830",
        "logo_boost":  1.45,
    },
}


# ---------------------------------------------------------------------------
# Log loading (worker thread)
# ---------------------------------------------------------------------------

class Flight:
    """One plottable flight window inside a log file."""

    def __init__(self, label, streams, events, boost_ns, end_ns,
                 recovered, boost_source, detail):
        self.label = label
        self.streams = streams
        self.events = events
        self.boost_ns = boost_ns
        self.end_ns = end_ns
        self.recovered = recovered
        self.boost_source = boost_source
        #: (key, value) rows shown in the summary box.
        self.detail = detail


class LoadedLog:
    """A decoded log file and the flights found in it."""

    def __init__(self, path, kind, flights, warnings, audit_path):
        self.path = path
        self.kind = kind
        self.flights = flights
        self.warnings = warnings
        self.audit_path = audit_path


def _stream_summary(streams):
    """``[(name, "N samples"), ...]`` for every non-empty stream."""
    return [(name, f"{t.size} samples")
            for name, (t, _v) in streams.items() if t.size]


def _flight_stats(streams):
    """Headline numbers a flight log is usually opened to look up."""
    rows = []

    t_pose, pose = streams.get("sm_pose", (np.empty(0), np.empty((0, 2))))
    if t_pose.size:
        peak = int(np.argmax(pose[:, 1]))
        rows.append(("Peak altitude", f"{pose[peak, 1]:.1f} m"))
        rows.append(("Max velocity", f"{pose[:, 0].max():.1f} m/s"))

    t_baro, baro = streams.get("baro", (np.empty(0), np.empty((0, 2))))
    if t_baro.size:
        p_pa = baro[:, 0] * 1000.0
        alt = pfd.pressure_to_altitude(p_pa, p_pa[0])
        rows.append(("Peak baro altitude", f"{alt.max():.1f} m"))

    t_acc, accel = streams.get("accel", (np.empty(0), np.empty((0, 3))))
    if t_acc.size:
        mag = np.linalg.norm(accel, axis=1)
        rows.append(("Max |a_body|", f"{mag.max():.1f} m/s²"))

    t_vbat, vbat = streams.get("vbat", (np.empty(0), np.empty((0, 1))))
    if t_vbat.size:
        rows.append(("Battery",
                     f"{vbat[:, 0].max():.2f} → {vbat[:, 0].min():.2f} V"))

    return rows


def _segment_flights(streams, events, label_prefix, base_detail):
    """Split ``streams`` into plottable flights.

    Prefers ``ARMED → BOOST`` transitions from a ``state_audit`` file and
    falls back to the measurement heuristics in :mod:`plot_flight_data`,
    exactly as the CLI does for logs whose audit trail is missing — which
    is every binary log, since the format records sensor data only.
    """
    events = list(events)
    windows = pfd.segment_flights(events)
    recovered = False
    boost_source = None

    if not windows:
        boost_ns, boost_source = pfd.recover_boost_from_measurements(streams)
        recovered = True
        if boost_ns is None:
            first = min((t[0] for t, _v in streams.values() if t.size),
                        default=0)
            windows = [(int(first), None)]
            boost_source = None
        else:
            landed_ns, _src = pfd.recover_landed_from_measurements(
                streams, boost_ns)
            events.append((boost_ns, "transition", "ARMED", "BOOST"))
            if landed_ns is not None:
                events.append((landed_ns, "transition", "FLIGHT", "LANDED"))
            events.sort(key=lambda e: e[0])
            windows = [(boost_ns, landed_ns)]

    flights = []
    for n, (boost_ns, end_ns) in enumerate(windows, start=1):
        detail = list(base_detail)
        if len(windows) > 1:
            label = f"{label_prefix} · flight {n}"
        else:
            label = label_prefix
        if boost_source:
            detail.append(("Boost", f"recovered from {boost_source}"))
        elif recovered:
            detail.append(("Boost", "not found — whole log shown"))
        else:
            detail.append(("Boost", "from state_audit"))
        if end_ns is not None:
            detail.append(("Flight time",
                           f"{(end_ns - boost_ns) / 1e9:.1f} s"))
        detail.extend(_flight_stats(streams))
        detail.extend(_stream_summary(streams))
        flights.append(Flight(label=label, streams=streams, events=events,
                              boost_ns=boost_ns, end_ns=end_ns,
                              recovered=recovered, boost_source=boost_source,
                              detail=detail))
    return flights


def load_log(path, frame_size=None):
    """Decode a log file into a :class:`LoadedLog`. Safe to call off-thread."""
    ext = os.path.splitext(path)[1].lower()
    name = os.path.basename(path)
    warnings = []

    audit_path = binlog.find_state_audit(path)
    events = []
    if audit_path:
        try:
            events = pfd.parse_state_audit(audit_path)
        except OSError as exc:
            warnings.append(f"could not read {audit_path}: {exc}")
            audit_path = None

    flights = []
    if ext == ".bin":
        log = binlog.read_binary_log(path, frame_size=frame_size)
        warnings.extend(log.warnings)
        multi = len(log.segments) > 1
        for i, seg in enumerate(log.segments, start=1):
            prefix = f"segment {i}" if multi else "flight"
            detail = [
                ("Format", f"binary v{binlog.BIN_VERSION}"),
                ("Frame size", f"{log.frame_size} B"),
                ("Frames", f"{seg.frame_count} "
                           f"(seq {seg.first_seq}…{seg.last_seq})"),
                ("Records", f"{seg.record_count}"),
                ("Flight ID", f"{seg.flight_id}"),
                ("Log duration", f"{seg.duration_s:.1f} s"),
            ]
            if seg.split_reason:
                detail.append(("Segment start", seg.split_reason))
            flights.extend(_segment_flights(seg.streams, events, prefix,
                                            detail))
    else:
        streams = binlog.load_streams(path)
        total = sum(int(t.size) for t, _v in streams.values())
        if not total:
            raise binlog.BinLogError(f"{name}: no telemetry samples found")
        detail = [("Format", ext.lstrip(".") or "unknown"),
                  ("Samples", f"{total}")]
        flights.extend(_segment_flights(streams, events, "flight", detail))

    if not flights:
        raise binlog.BinLogError(f"{name}: nothing plottable in this log")

    return LoadedLog(path=path, kind=ext.lstrip("."), flights=flights,
                     warnings=warnings, audit_path=audit_path)


# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------

class FlightLogViewer(tk.Tk):
    """Main window."""

    #: Height of one stacked panel, matching plot_raw_flight's figsize.
    PANEL_HEIGHT_IN = 2.4
    #: Resolution used when exporting the figure to a file.
    EXPORT_DPI = 150

    def __init__(self, theme="light", frame_size=None):
        super().__init__()

        self.title("AURORA Flight Log Viewer")
        self.geometry("1440x900")
        self.minsize(960, 600)

        self.theme_name = theme if theme in UI_THEMES else "light"
        self.frame_size = frame_size

        self._logo_source = self._load_logo_image()
        self._logo_images = {}
        self._results = queue.Queue()
        self._cache = {}
        self._current_flight = None
        self._current_path = None
        self._resize_job = None
        self._busy = 0
        # _figure / _fig_canvas / _fig_window / _toolbar / _panel_count are
        # created once by _build_plot_area().

        self._build_style()
        self._build_widgets()
        self._apply_theme()
        self._set_window_icon()
        self.after(60, self._poll_results)

    def _set_window_icon(self):
        """Use the Auxspace logo as the window/taskbar icon."""
        if ImageTk is None or self._logo_source is None:
            return
        try:
            self._icon = ImageTk.PhotoImage(
                self._logo_source.resize((64, 39), Image.LANCZOS))
            self.iconphoto(True, self._icon)
        except tk.TclError:
            pass

    # -- chrome ----------------------------------------------------------

    def _load_logo_image(self):
        """Load ``doc/img/logo.png`` at a header-friendly size."""
        if Image is None or not os.path.isfile(LOGO_PATH):
            return None
        try:
            img = Image.open(LOGO_PATH).convert("RGBA")
        except OSError:
            return None
        height = 42
        width = max(1, round(img.width * height / img.height))
        return img.resize((width, height), Image.LANCZOS)

    def _logo_for(self, theme):
        """Theme-specific logo, brightened so it lifts off a dark header."""
        if self._logo_source is None:
            return None
        if theme not in self._logo_images:
            img = self._logo_source
            boost = UI_THEMES[theme]["logo_boost"]
            if boost != 1.0 and ImageEnhance is not None:
                rgb = ImageEnhance.Brightness(
                    img.convert("RGB")).enhance(boost)
                img = Image.merge("RGBA",
                                  (*rgb.split(), img.getchannel("A")))
            self._logo_images[theme] = ImageTk.PhotoImage(img)
        return self._logo_images[theme]

    def _build_style(self):
        self.style = ttk.Style(self)
        # clam is the only bundled theme that honours background colours
        # on every widget class we restyle below.
        if "clam" in self.style.theme_names():
            self.style.theme_use("clam")

    def _build_widgets(self):
        self.rowconfigure(1, weight=1)
        self.columnconfigure(0, weight=1)

        # -- header ------------------------------------------------------
        header = ttk.Frame(self, style="Header.TFrame", padding=(14, 10))
        header.grid(row=0, column=0, sticky="ew")
        header.columnconfigure(2, weight=1)

        self.logo_label = ttk.Label(header, style="Header.TLabel")
        self.logo_label.grid(row=0, column=0, rowspan=2, padx=(0, 12))

        ttk.Label(header, text="AURORA", style="Brand.TLabel").grid(
            row=0, column=1, sticky="w")
        ttk.Label(header, text="Flight Log Viewer",
                  style="Subtitle.TLabel").grid(row=1, column=1, sticky="w")

        buttons = ttk.Frame(header, style="Header.TFrame")
        buttons.grid(row=0, column=3, rowspan=2, sticky="e")
        ttk.Button(buttons, text="Open Log…", style="Brand.TButton",
                   command=self.open_file).pack(side="left", padx=4)
        ttk.Button(buttons, text="Open Folder…", style="Brand.TButton",
                   command=self.open_folder).pack(side="left", padx=4)
        ttk.Button(buttons, text="Export PNG…", style="Ghost.TButton",
                   command=self.export_png).pack(side="left", padx=4)
        self.theme_button = ttk.Button(buttons, style="Ghost.TButton",
                                       width=10, command=self.toggle_theme)
        self.theme_button.pack(side="left", padx=(4, 0))

        self.header_rule = ttk.Frame(self, height=2, style="Rule.TFrame")
        self.header_rule.grid(row=0, column=0, sticky="sew")

        # -- body --------------------------------------------------------
        body = ttk.PanedWindow(self, orient="horizontal")
        body.grid(row=1, column=0, sticky="nsew")

        sidebar = ttk.Frame(body, style="Sidebar.TFrame", padding=(10, 10))
        body.add(sidebar, weight=0)
        self._build_sidebar(sidebar)

        plot_area = ttk.Frame(body, style="Plot.TFrame")
        body.add(plot_area, weight=1)
        self._build_plot_area(plot_area)

        # PanedWindow ignores the child's requested width unless nudged.
        self.after(50, lambda: body.sashpos(0, 330))

        # -- status bar --------------------------------------------------
        self.status = tk.StringVar(
            value="Open a flight log (.bin) or a DATA folder to start.")
        self.status_label = ttk.Label(self, textvariable=self.status,
                                      style="Status.TLabel",
                                      padding=(12, 6))
        self.status_label.grid(row=2, column=0, sticky="ew")

    def _build_sidebar(self, parent):
        parent.rowconfigure(1, weight=3)
        parent.rowconfigure(4, weight=2)
        parent.rowconfigure(7, weight=2)
        parent.columnconfigure(0, weight=1)

        ttk.Label(parent, text="FLIGHT LOGS",
                  style="Caption.TLabel").grid(row=0, column=0, sticky="w",
                                               pady=(0, 4))

        tree_wrap = ttk.Frame(parent, style="Sidebar.TFrame")
        tree_wrap.grid(row=1, column=0, sticky="nsew")
        tree_wrap.rowconfigure(0, weight=1)
        tree_wrap.columnconfigure(0, weight=1)

        self.tree = ttk.Treeview(tree_wrap, show="tree", selectmode="browse",
                                 height=6)
        self.tree.grid(row=0, column=0, sticky="nsew")
        tree_bar = ttk.Scrollbar(tree_wrap, orient="vertical",
                                 command=self.tree.yview)
        tree_bar.grid(row=0, column=1, sticky="ns")
        self.tree.configure(yscrollcommand=tree_bar.set)
        self.tree.bind("<<TreeviewSelect>>", self._on_tree_select)

        ttk.Label(parent, text="GRAPHS", style="Caption.TLabel").grid(
            row=2, column=0, sticky="w", pady=(12, 4))

        picker = ttk.Frame(parent, style="Sidebar.TFrame")
        picker.grid(row=3, column=0, sticky="ew")
        ttk.Button(picker, text="All", style="Ghost.TButton", width=6,
                   command=lambda: self._select_panels(True)).pack(
                       side="left")
        ttk.Button(picker, text="None", style="Ghost.TButton", width=6,
                   command=lambda: self._select_panels(False)).pack(
                       side="left", padx=(6, 0))

        list_wrap = ttk.Frame(parent, style="Sidebar.TFrame")
        list_wrap.grid(row=4, column=0, sticky="nsew", pady=(6, 0))
        list_wrap.rowconfigure(0, weight=1)
        list_wrap.columnconfigure(0, weight=1)

        self.panel_list = tk.Listbox(list_wrap, selectmode=tk.EXTENDED,
                                     exportselection=False, activestyle="none",
                                     highlightthickness=0, borderwidth=1,
                                     relief="solid", height=6)
        self.panel_list.grid(row=0, column=0, sticky="nsew")
        panel_bar = ttk.Scrollbar(list_wrap, orient="vertical",
                                  command=self.panel_list.yview)
        panel_bar.grid(row=0, column=1, sticky="ns")
        self.panel_list.configure(yscrollcommand=panel_bar.set)
        self.panel_list.bind("<<ListboxSelect>>", self._on_panel_select)
        self._panel_keys = []

        ttk.Label(parent, text="WINDOW", style="Caption.TLabel").grid(
            row=5, column=0, sticky="w", pady=(12, 4))

        window = ttk.Frame(parent, style="Sidebar.TFrame")
        window.grid(row=6, column=0, sticky="ew")
        window.columnconfigure(1, weight=1)

        self.window_mode = tk.StringVar(value="flight")
        ttk.Radiobutton(window, text="Flight window", value="flight",
                        variable=self.window_mode, style="Side.TRadiobutton",
                        command=self._rerender).grid(row=0, column=0,
                                                     columnspan=2, sticky="w")
        ttk.Radiobutton(window, text="Whole log", value="full",
                        variable=self.window_mode, style="Side.TRadiobutton",
                        command=self._rerender).grid(row=1, column=0,
                                                     columnspan=2, sticky="w")

        self.pre_boost = tk.DoubleVar(value=10.0)
        self.post_end = tk.DoubleVar(value=2.0)
        self.r_meas = tk.DoubleVar(value=6.0)
        for row, (text, var, step) in enumerate((
                ("Pre-boost (s)", self.pre_boost, 1.0),
                ("Post-end (s)", self.post_end, 1.0),
                ("Kalman R (m²)", self.r_meas, 0.5)), start=2):
            ttk.Label(window, text=text, style="Side.TLabel").grid(
                row=row, column=0, sticky="w", pady=2)
            spin = ttk.Spinbox(window, textvariable=var, from_=0.0, to=600.0,
                               increment=step, width=8,
                               style="Side.TSpinbox",
                               command=self._rerender)
            spin.grid(row=row, column=1, sticky="e", pady=2)
            spin.bind("<Return>", lambda _e: self._rerender())

        summary_wrap = ttk.Frame(parent, style="Sidebar.TFrame")
        summary_wrap.grid(row=7, column=0, sticky="nsew", pady=(12, 0))
        summary_wrap.rowconfigure(1, weight=1)
        summary_wrap.columnconfigure(0, weight=1)
        ttk.Label(summary_wrap, text="SUMMARY", style="Caption.TLabel").grid(
            row=0, column=0, sticky="w", pady=(0, 4))
        self.summary = tk.Text(summary_wrap, height=8, wrap="none",
                               borderwidth=1, relief="solid",
                               highlightthickness=0, state="disabled",
                               font=("TkFixedFont", 8), padx=8, pady=6)
        self.summary.grid(row=1, column=0, sticky="nsew")
        summary_bar = ttk.Scrollbar(summary_wrap, orient="vertical",
                                    command=self.summary.yview)
        summary_bar.grid(row=1, column=1, sticky="ns")
        self.summary.configure(yscrollcommand=summary_bar.set)

    def _build_plot_area(self, parent):
        parent.rowconfigure(1, weight=1)
        parent.columnconfigure(0, weight=1)

        self.plot_title = tk.StringVar(value="No flight loaded")
        self.title_label = ttk.Label(parent, textvariable=self.plot_title,
                                     style="PlotTitle.TLabel",
                                     padding=(14, 10))
        self.title_label.grid(row=0, column=0, columnspan=2, sticky="ew")

        self.scroll = tk.Canvas(parent, highlightthickness=0, borderwidth=0)
        self.scroll.grid(row=1, column=0, sticky="nsew")
        self.scroll_bar = ttk.Scrollbar(parent, orient="vertical",
                                        command=self.scroll.yview)
        self.scroll_bar.grid(row=1, column=1, sticky="ns")
        self.scroll.configure(yscrollcommand=self.scroll_bar.set)
        self.scroll.bind("<Configure>", self._on_canvas_resize)

        self.toolbar_holder = ttk.Frame(parent, style="Plot.TFrame")
        self.toolbar_holder.grid(row=2, column=0, columnspan=2, sticky="ew")

        # The canvas and toolbar are built once and kept for the lifetime
        # of the window; each render swaps the figure behind them rather
        # than tearing the widgets down, which would flicker on every
        # panel tick and reset the toolbar's pan/zoom mode.
        self._figure = Figure()
        self._panel_count = 1
        self._fig_canvas = FigureCanvasTkAgg(self._figure, master=self.scroll)
        widget = self._fig_canvas.get_tk_widget()
        widget.configure(borderwidth=0, highlightthickness=0)
        self._fig_window = self.scroll.create_window(0, 0, anchor="nw",
                                                     window=widget)

        self._toolbar = PlotToolbar(self._fig_canvas, self.toolbar_holder,
                                    pack_toolbar=False)
        self._toolbar.update()
        self._toolbar.pack(side="left", fill="x")

        for target in (self.scroll, widget):
            target.bind("<MouseWheel>", self._on_wheel)
            target.bind("<Button-4>", self._on_wheel)
            target.bind("<Button-5>", self._on_wheel)

    # -- theming ---------------------------------------------------------

    def _apply_theme(self):
        c = UI_THEMES[self.theme_name]
        s = self.style

        self.configure(background=c["bg"])
        s.configure(".", background=c["bg"], foreground=c["fg"],
                    fieldbackground=c["field"], bordercolor=c["border"],
                    lightcolor=c["border"], darkcolor=c["border"],
                    focuscolor=c["brand"])

        s.configure("TFrame", background=c["bg"])
        s.configure("Plot.TFrame", background=c["bg"])
        s.configure("Header.TFrame", background=c["surface"])
        s.configure("Sidebar.TFrame", background=c["surface"])
        s.configure("Rule.TFrame", background=c["border"])

        s.configure("TLabel", background=c["bg"], foreground=c["fg"])
        s.configure("Header.TLabel", background=c["surface"])
        s.configure("Sidebar.TLabel", background=c["surface"],
                    foreground=c["fg"])
        s.configure("Side.TLabel", background=c["surface"],
                    foreground=c["fg"])
        s.configure("Brand.TLabel", background=c["surface"],
                    foreground=c["brand"],
                    font=("TkDefaultFont", 17, "bold"))
        s.configure("Subtitle.TLabel", background=c["surface"],
                    foreground=c["muted"], font=("TkDefaultFont", 10))
        s.configure("Caption.TLabel", background=c["surface"],
                    foreground=c["muted"],
                    font=("TkDefaultFont", 8, "bold"))
        s.configure("PlotTitle.TLabel", background=c["bg"],
                    foreground=c["brand"],
                    font=("TkDefaultFont", 12, "bold"))
        s.configure("Status.TLabel", background=c["surface"],
                    foreground=c["muted"])

        s.configure("Brand.TButton", background=c["brand"],
                    foreground=c["on_brand"], bordercolor=c["brand"],
                    focusthickness=0, relief="flat", padding=(12, 6))
        s.map("Brand.TButton",
              background=[("pressed", c["brand_hover"]),
                          ("active", c["brand_hover"])],
              foreground=[("disabled", c["muted"])])

        s.configure("Ghost.TButton", background=c["surface"],
                    foreground=c["brand"], bordercolor=c["border"],
                    lightcolor=c["border"], darkcolor=c["border"],
                    focusthickness=0, relief="solid", borderwidth=1,
                    padding=(11, 5))
        s.map("Ghost.TButton",
              background=[("pressed", c["select"]), ("active", c["select"])],
              foreground=[("disabled", c["muted"])])

        s.configure("Treeview", background=c["field"],
                    fieldbackground=c["field"], foreground=c["fg"],
                    bordercolor=c["border"], borderwidth=1, rowheight=24)
        s.map("Treeview", background=[("selected", c["brand"])],
              foreground=[("selected", c["on_brand"])])

        s.configure("TScrollbar", background=c["surface"],
                    troughcolor=c["bg"], bordercolor=c["border"],
                    arrowcolor=c["muted"])
        s.map("TScrollbar", background=[("active", c["border"])])

        # clam names the indicator's fill and dot "indicatorbackground" /
        # "indicatorforeground"; the "indicatorcolor" other themes use is
        # silently ignored here and leaves a white blob on a dark sidebar.
        s.configure("Side.TRadiobutton", background=c["surface"],
                    foreground=c["fg"], focuscolor=c["surface"],
                    indicatorbackground=c["field"],
                    indicatorforeground=c["brand"],
                    upperbordercolor=c["border"],
                    lowerbordercolor=c["border"])
        s.map("Side.TRadiobutton",
              background=[("active", c["surface"])],
              foreground=[("active", c["fg"])],
              indicatorbackground=[("pressed", c["select"]),
                                   ("active", c["select"])])

        s.configure("Side.TSpinbox", fieldbackground=c["field"],
                    background=c["surface"], foreground=c["fg"],
                    arrowcolor=c["brand"], bordercolor=c["border"])

        self.panel_list.configure(background=c["field"], foreground=c["fg"],
                                  selectbackground=c["brand"],
                                  selectforeground=c["on_brand"],
                                  highlightbackground=c["border"])
        self.summary.configure(background=c["field"], foreground=c["fg"],
                               insertbackground=c["fg"],
                               highlightbackground=c["border"])
        self.summary.tag_configure("key", foreground=c["muted"])
        self.summary.tag_configure("warn", foreground=c["brand"])
        self.scroll.configure(background=c["bg"])

        logo = self._logo_for(self.theme_name)
        if logo is not None:
            self.logo_label.configure(image=logo)
        else:
            self.logo_label.configure(style="Brand.TLabel", text="✦")

        self.theme_button.configure(
            text="☀ Light" if self.theme_name == "dark" else "☾ Dark")

        self._style_toolbar()
        if self._current_flight is None:
            self._show_placeholder()

    def _show_placeholder(self, heading="No flight loaded", detail=None):
        """Paint the empty plot area so it reads as part of the app."""
        if detail is None:
            detail = ("Open a flight log, or open the DATA folder from the "
                      "card to browse every flight on it.")
        c = UI_THEMES[self.theme_name]
        self._figure.clear()
        self._figure.set_facecolor(c["bg"])
        self._figure.text(0.5, 0.54, heading, ha="center", va="center",
                          color=c["brand"], fontsize=16, fontweight="bold")
        self._figure.text(0.5, 0.47, detail, ha="center", va="center",
                          color=c["muted"], fontsize=10)
        self._panel_count = 1
        self._fit_figure()

    def _style_toolbar(self):
        """Repaint the matplotlib toolbar, which is plain Tk underneath."""
        if self._toolbar is None:
            return
        c = UI_THEMES[self.theme_name]
        try:
            self._toolbar.configure(background=c["bg"])
        except tk.TclError:
            pass
        for child in self._toolbar.winfo_children():
            for option, value in (("background", c["bg"]),
                                  ("foreground", c["fg"]),
                                  ("highlightbackground", c["bg"]),
                                  ("activebackground", c["select"]),
                                  ("activeforeground", c["fg"]),
                                  ("selectcolor", c["select"])):
                try:
                    child.configure(**{option: value})
                except tk.TclError:
                    continue

        # matplotlib bakes the icon colours in when the button is built,
        # picking a light- or dark-background variant from the colours the
        # button had at that moment.  Recolouring the widget afterwards
        # leaves the old icons behind, so ask it to redraw them.
        recolor = getattr(self._toolbar, "_set_image_for_button", None)
        for button in getattr(self._toolbar, "_buttons", {}).values():
            if recolor is None:
                break
            try:
                recolor(button)
            except Exception:                        # noqa: BLE001
                break

    def toggle_theme(self):
        self.theme_name = "dark" if self.theme_name == "light" else "light"
        self._apply_theme()
        self._rerender()

    # -- opening ---------------------------------------------------------

    def open_file(self):
        path = filedialog.askopenfilename(
            title="Open AURORA flight log",
            filetypes=[("AURORA flight logs", "*.bin *.csv *.influx"),
                       ("Binary flight log", "*.bin"),
                       ("Converter CSV", "*.csv"),
                       ("InfluxDB line protocol", "*.influx"),
                       ("All files", "*")])
        if path:
            self.load_path(path)

    def open_folder(self):
        path = filedialog.askdirectory(
            title="Open a data logger DATA directory")
        if path:
            self.load_path(path)

    def load_path(self, path):
        """Open a log file or a ``DATA`` directory."""
        path = os.path.abspath(path)
        if os.path.isdir(path):
            self._populate_folder(path)
        elif os.path.isfile(path):
            self._populate_files(os.path.dirname(path), [path])
            self._select_first_log(preferred=path)
        else:
            messagebox.showerror("AURORA", f"No such file or directory:\n"
                                           f"{path}")

    def _populate_folder(self, directory):
        logs = binlog.scan_data_dir(directory)
        if not logs:
            messagebox.showwarning(
                "AURORA",
                f"No flight logs (.bin, .csv, .influx) found in\n"
                f"{directory}")
            self.status.set(f"No flight logs in {directory}")
            return
        self._populate_files(directory, logs)
        self.status.set(f"{len(logs)} log(s) in {directory}")
        self._select_first_log()

    def _populate_files(self, directory, paths):
        self.tree.delete(*self.tree.get_children())
        label = os.path.basename(directory) or directory
        root = self.tree.insert("", "end", iid="root", open=True,
                                text=f"  {label}")
        for path in paths:
            size = os.path.getsize(path) / 1024.0
            unit, value = ("MiB", size / 1024.0) if size > 1024 else ("KiB",
                                                                      size)
            iid = f"file:{path}"
            self.tree.insert(root, "end", iid=iid,
                             text=f"  {os.path.basename(path)}"
                                  f"   ({value:.1f} {unit})")
        self.tree.item(root, open=True)

    def _select_first_log(self, preferred=None):
        children = self.tree.get_children("root")
        if not children:
            return
        target = f"file:{preferred}" if preferred else children[0]
        if target not in children:
            target = children[0]
        self.tree.selection_set(target)
        self.tree.focus(target)

    # -- tree events -----------------------------------------------------

    def _on_tree_select(self, _event=None):
        selection = self.tree.selection()
        if not selection:
            return
        iid = selection[0]
        if iid.startswith("file:"):
            self._open_log(iid[len("file:"):])
        elif iid.startswith("flight:"):
            path, index = iid[len("flight:"):].rsplit("#", 1)
            log = self._cache.get(path)
            if log is not None:
                self._show_flight(log, log.flights[int(index)])

    def _open_log(self, path):
        cached = self._cache.get(path)
        if cached is not None:
            self._populate_flights(cached)
            return
        self._set_busy(True, f"Reading {os.path.basename(path)} …")
        thread = threading.Thread(target=self._load_worker, args=(path,),
                                  daemon=True)
        thread.start()

    def _load_worker(self, path):
        try:
            self._results.put(("ok", path, load_log(path, self.frame_size)))
        except Exception as exc:                     # noqa: BLE001
            self._results.put(("error", path,
                               (exc, traceback.format_exc())))

    def _poll_results(self):
        try:
            while True:
                kind, path, payload = self._results.get_nowait()
                self._set_busy(False)
                if kind == "ok":
                    self._cache[path] = payload
                    self._populate_flights(payload)
                else:
                    exc, tb = payload
                    print(tb, file=sys.stderr)
                    self.status.set(f"{os.path.basename(path)}: {exc}")
                    messagebox.showerror(
                        "Cannot read flight log",
                        f"{os.path.basename(path)}\n\n{exc}")
        except queue.Empty:
            pass
        self.after(60, self._poll_results)

    def _populate_flights(self, log):
        """Hang the flights of ``log`` under its file node and show one."""
        iid = f"file:{log.path}"
        if not self.tree.exists(iid):
            return
        self.tree.delete(*self.tree.get_children(iid))
        for i, flight in enumerate(log.flights):
            self.tree.insert(iid, "end", iid=f"flight:{log.path}#{i}",
                             text=f"    {flight.label}")
        self.tree.item(iid, open=True)

        for warning in log.warnings:
            print(f"warning: {os.path.basename(log.path)}: {warning}",
                  file=sys.stderr)
        self._show_flight(log, log.flights[0])

    # -- rendering -------------------------------------------------------

    def _show_flight(self, log, flight):
        self._current_flight = (log, flight)
        self._render()

    def _rerender(self, _event=None):
        if self._current_flight is not None:
            self._render()

    def _slice(self, flight):
        """Apply the window controls to a flight's streams."""
        pre = max(0.0, float(self.pre_boost.get()))
        post = max(0.0, float(self.post_end.get()))

        if self.window_mode.get() == "full":
            # Span the recording end to end.  end_ns has to be the last
            # sample rather than None: slice_real_flight() would otherwise
            # assume a 120 s flight and drop every state transition past
            # it from the markers.
            times = [t for t, _v in flight.streams.values() if t.size]
            first = int(min(t[0] for t in times)) if times else 0
            last = int(max(t[-1] for t in times)) if times else first
            return pfd.slice_real_flight(flight.streams, flight.events,
                                         first, last, pre_boost_s=0.0,
                                         post_end_s=0.0, relax_empty=True)

        return pfd.slice_real_flight(flight.streams, flight.events,
                                     flight.boost_ns, flight.end_ns,
                                     pre_boost_s=pre, post_end_s=post,
                                     relax_empty=flight.recovered
                                     and flight.boost_source is None)

    def _render(self):
        log, flight = self._current_flight
        self._set_busy(True, "Rendering …")
        try:
            sliced, transitions = self._slice(flight)
            usable = pfd.available_panels(sliced)
            if not usable:
                self._set_busy(False)
                self.plot_title.set(f"{os.path.basename(log.path)} — "
                                    f"{flight.label}")
                self._show_placeholder(
                    "Nothing in this window",
                    "Switch to 'Whole log', or widen the pre-boost and "
                    "post-end margins.")
                self._fill_summary(log, flight)
                self.status.set(f"{os.path.basename(log.path)} · "
                                f"{flight.label} · no samples in window")
                return
            wanted = self._sync_panel_list(usable)
            figure = pfd.plot_raw_flight(
                sliced, transitions, self.theme_name, out_path=None,
                title=self._plot_title(log, flight),
                r_meas=float(self.r_meas.get()), panels=wanted)
        except Exception as exc:                     # noqa: BLE001
            self._set_busy(False)
            traceback.print_exc()
            self.status.set(f"Plot failed: {exc}")
            messagebox.showerror("Plot failed", str(exc))
            return

        self._embed(figure, len(wanted))
        self._fill_summary(log, flight)
        self.plot_title.set(f"{os.path.basename(log.path)} — {flight.label}")
        self._set_busy(False)
        self.status.set(
            f"{os.path.basename(log.path)} · {flight.label} · "
            f"{len(wanted)} panel(s)"
            + (f" · audit: {os.path.basename(log.audit_path)}"
               if log.audit_path else " · no state_audit (window recovered)"))

    def _plot_title(self, log, flight):
        title = f"AURORA — {os.path.basename(log.path)}"
        if flight.label != "flight":
            title += f" ({flight.label})"
        if flight.boost_source:
            title += f"  [boost recovered from {flight.boost_source}]"
        return title

    def _sync_panel_list(self, usable):
        """Refresh the panel picker and return the panels to draw."""
        labels = [pfd.PANEL_LABELS.get(key, key) for key in usable]
        previous = {self._panel_keys[i]
                    for i in self.panel_list.curselection()
                    if i < len(self._panel_keys)}

        if usable != self._panel_keys:
            self.panel_list.delete(0, tk.END)
            for label in labels:
                self.panel_list.insert(tk.END, f" {label}")
            self._panel_keys = list(usable)
            keep = [i for i, key in enumerate(usable) if key in previous]
            if not keep:
                keep = range(len(usable))
            for i in keep:
                self.panel_list.selection_set(i)

        chosen = [self._panel_keys[i] for i in self.panel_list.curselection()]
        return chosen or list(usable)

    def _on_panel_select(self, _event=None):
        if self._current_flight is not None and not self._busy:
            self._render()

    def _select_panels(self, everything):
        if everything:
            self.panel_list.selection_set(0, tk.END)
        else:
            self.panel_list.selection_clear(0, tk.END)
            if self._panel_keys:
                self.panel_list.selection_set(0)
        self._rerender()

    def _embed(self, figure, panel_count):
        """Show ``figure`` on the canvas, retiring the one it replaces."""
        previous = self._figure

        self._figure = figure
        self._panel_count = panel_count
        figure.set_canvas(self._fig_canvas)
        self._fig_canvas.figure = figure
        # Clears the pan/zoom history, which belonged to the old figure.
        self._toolbar.update()
        self._style_toolbar()

        if previous is not None and previous is not figure:
            plt.close(previous)

        self._fit_figure()

    def _fit_figure(self):
        """Size the figure to the viewport width, panels at natural height.

        Everything is computed from the figure's *own* dpi, which the Tk
        backend pins to the display's scaling rather than to the 100 dpi
        matplotlib defaults to.  Sizing against anything else renders the
        figure wider than the widget holding it and clips the right-hand
        axis decorations.
        """
        if self._figure is None or self._fig_window is None:
            return
        dpi = self._figure.get_dpi()
        width = max(self.scroll.winfo_width(), 400)
        panel_px = int(round(self.PANEL_HEIGHT_IN * dpi))
        height = max(panel_px * max(self._panel_count, 1),
                     max(self.scroll.winfo_height(), 300))

        self._figure.set_size_inches(width / dpi, height / dpi)
        if self._figure.axes:
            self._figure.tight_layout()
        self._fig_canvas.draw_idle()

        self.scroll.itemconfigure(self._fig_window, width=width,
                                  height=height)
        self.scroll.configure(scrollregion=(0, 0, width, height))

    def _on_canvas_resize(self, _event=None):
        if self._resize_job is not None:
            self.after_cancel(self._resize_job)
        self._resize_job = self.after(150, self._do_resize)

    def _do_resize(self):
        self._resize_job = None
        self._fit_figure()

    def _on_wheel(self, event):
        if event.num == 4 or getattr(event, "delta", 0) > 0:
            self.scroll.yview_scroll(-3, "units")
        elif event.num == 5 or getattr(event, "delta", 0) < 0:
            self.scroll.yview_scroll(3, "units")
        return "break"

    # -- summary / status ------------------------------------------------

    def _fill_summary(self, log, flight):
        self.summary.configure(state="normal")
        self.summary.delete("1.0", tk.END)
        rows = [("File", os.path.basename(log.path))]
        if log.audit_path:
            rows.append(("State audit", os.path.basename(log.audit_path)))
        rows.extend(flight.detail)
        width = max((len(k) for k, _v in rows), default=0)
        for key, value in rows:
            self.summary.insert(tk.END, f"{key.ljust(width)}  ", "key")
            self.summary.insert(tk.END, f"{value}\n")
        for warning in log.warnings:
            self.summary.insert(tk.END, f"\n! {warning}\n", "warn")
        self.summary.configure(state="disabled")

    def _set_busy(self, busy, message=None):
        self._busy = max(0, self._busy + (1 if busy else -1))
        if message:
            self.status.set(message)
        self.configure(cursor="watch" if self._busy else "")
        self.update_idletasks()

    # -- export ----------------------------------------------------------

    def export_png(self):
        if self._current_flight is None:
            messagebox.showinfo("AURORA", "Nothing to export yet.")
            return
        log, flight = self._current_flight
        stem = os.path.splitext(os.path.basename(log.path))[0]
        suggested = f"{stem}_{flight.label.replace(' · ', '_')}" \
                    f"_{self.theme_name}.png".replace(" ", "_")
        path = filedialog.asksaveasfilename(
            title="Export plot", defaultextension=".png",
            initialfile=suggested,
            filetypes=[("PNG image", "*.png"), ("PDF document", "*.pdf"),
                       ("SVG image", "*.svg")])
        if not path:
            return
        try:
            self._figure.savefig(path, dpi=self.EXPORT_DPI,
                                 facecolor=self._figure.get_facecolor())
        except OSError as exc:
            messagebox.showerror("Export failed", str(exc))
            return
        self.status.set(f"Exported {path}")


def main():
    parser = argparse.ArgumentParser(
        description="Browse and plot AURORA flight logs.")
    parser.add_argument("path", nargs="?",
                        help="flight log file (.bin/.csv/.influx) or a data "
                             "logger DATA directory to open on start-up")
    parser.add_argument("--theme", choices=["light", "dark"], default="light",
                        help="initial colour theme (default: light)")
    parser.add_argument("--frame-size", type=int, default=None,
                        help="override the binary log frame-size probe "
                             "(CONFIG_DATA_LOGGER_BIN_FRAME_SIZE)")
    args = parser.parse_args()

    try:
        app = FlightLogViewer(theme=args.theme, frame_size=args.frame_size)
    except tk.TclError as exc:
        print(f"error: cannot open a display ({exc})", file=sys.stderr)
        return 1

    if args.path:
        app.after(120, lambda: app.load_path(args.path))
    app.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
