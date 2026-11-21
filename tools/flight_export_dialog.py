#!/usr/bin/env python3
#
# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

"""Export dialog of the flight log viewer.

Lists every export with a short explanation of what it writes, so the
choice is made on what the file contains rather than on a menu label.
The dialog only collects the choice; writing is up to the caller (see
:mod:`flight_export`).
"""

import tkinter as tk
from tkinter import ttk

import flight_export as fx

#: Choice for the plot image; the CSV choices are the ``fx.MODE_*`` keys.
CHOICE_IMAGE = "image"

IMAGE_TITLE = "Plot image"
IMAGE_DESCRIPTION = ("The graphs as currently shown, as a PNG picture "
                     "(PDF and SVG also work). For reports and slides, "
                     "not for analysis.")

#: Width in pixels at which the descriptions wrap.
WRAP = 440


class ExportDialog(tk.Toplevel):
    """Modal chooser; :attr:`result` is ``(choice, rate_hz)`` or ``None``.

    ``rate_hz`` is only set for the resampled export.
    """

    def __init__(self, parent, default_rate_hz, rates_text=""):
        super().__init__(parent)
        self.title("Export")
        self.resizable(False, False)
        self.transient(parent)
        self.result = None

        self._choice = tk.StringVar(value=fx.MODE_RESAMPLED)
        self._rate = tk.StringVar(value=f"{default_rate_hz:g}")
        self._rates_text = rates_text

        body = ttk.Frame(self, padding=14)
        body.pack(fill="both", expand=True)
        ttk.Label(body, text="What do you want to export?").pack(
            anchor="w", pady=(0, 8))

        entries = [(fx.MODE_RESAMPLED, fx.MODES[fx.MODE_RESAMPLED]),
                   (fx.MODE_RAW, fx.MODES[fx.MODE_RAW]),
                   (fx.MODE_PER_SENSOR, fx.MODES[fx.MODE_PER_SENSOR])]
        for choice, spec in entries:
            self._add_choice(body, choice, spec.title, spec.description)
            if choice == fx.MODE_RESAMPLED:
                self._add_rate_row(body)
        self._add_choice(body, CHOICE_IMAGE, IMAGE_TITLE, IMAGE_DESCRIPTION)

        ttk.Label(body, wraplength=WRAP, foreground="gray50", text=(
            "CSV files cover the same window as the plot, with time in "
            "seconds and a unit in every column header.")).pack(
                anchor="w", pady=(4, 10))

        buttons = ttk.Frame(body)
        buttons.pack(fill="x")
        ttk.Button(buttons, text="Cancel", command=self.destroy).pack(
            side="right")
        ttk.Button(buttons, text="Export…", command=self._accept).pack(
            side="right", padx=6)

        self.bind("<Return>", lambda _e: self._accept())
        self.bind("<Escape>", lambda _e: self.destroy())
        self._update_rate_state()
        self.grab_set()
        self.wait_visibility()
        self.focus_set()
        self.wait_window()

    def _add_choice(self, parent, choice, title, description):
        ttk.Radiobutton(parent, text=title, value=choice,
                        variable=self._choice,
                        command=self._update_rate_state).pack(anchor="w")
        ttk.Label(parent, text=description, wraplength=WRAP,
                  foreground="gray50").pack(anchor="w", padx=(24, 0),
                                            pady=(0, 8))

    def _add_rate_row(self, parent):
        row = ttk.Frame(parent)
        row.pack(anchor="w", padx=(24, 0), pady=(0, 8))
        ttk.Label(row, text="Rows per second:").pack(side="left")
        self._rate_entry = ttk.Entry(row, textvariable=self._rate, width=8)
        self._rate_entry.pack(side="left", padx=6)
        ttk.Label(row, text="Hz  (computed from the data)").pack(
            side="left")
        if self._rates_text:
            ttk.Label(parent, wraplength=WRAP, foreground="gray50",
                      text=f"Sample rates found: {self._rates_text}. The "
                           "default is the fastest measured sensor, so no "
                           "sample is skipped.").pack(
                               anchor="w", padx=(24, 0), pady=(0, 8))

    def _update_rate_state(self):
        resampled = self._choice.get() == fx.MODE_RESAMPLED
        self._rate_entry.configure(state="normal" if resampled
                                   else "disabled")

    def _accept(self):
        choice = self._choice.get()
        rate = None
        if choice == fx.MODE_RESAMPLED:
            try:
                rate = float(self._rate.get().replace(",", "."))
                if rate <= 0:
                    raise ValueError
            except ValueError:
                self._rate_entry.focus_set()
                self.bell()
                return
        self.result = (choice, rate)
        self.destroy()
