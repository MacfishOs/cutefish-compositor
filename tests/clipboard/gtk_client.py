#!/usr/bin/python3
"""Test-owned GTK 3 client using the normal toolkit clipboard, never user data."""
import sys
import gi

gi.require_version("Gtk", "3.0")
from gi.repository import Gtk, Gdk, GLib

TEXT = "Generated clipboard 🐟 中文\nsecond line"
clipboard = Gtk.Clipboard.get(Gdk.SELECTION_CLIPBOARD)
window = Gtk.Window(title=sys.argv[1])
window.set_default_size(320, 200)
result = 3


def received(_clipboard, text, _data):
    global result
    result = 0 if text == TEXT else 2
    print("PASS" if result == 0 else "FAIL", flush=True)
    Gtk.main_quit()


def key(_window, event):
    global result
    if event.keyval in (Gdk.KEY_q, Gdk.KEY_Q):
        result = 0
        Gtk.main_quit()
    elif event.keyval in (Gdk.KEY_c, Gdk.KEY_C):
        clipboard.set_text(TEXT, -1)
        print("PUBLISHED", flush=True)
    elif event.keyval in (Gdk.KEY_v, Gdk.KEY_V):
        clipboard.request_text(received, None)
    return True


window.connect("key-press-event", key)
window.connect("destroy", Gtk.main_quit)
window.show_all()
GLib.timeout_add_seconds(15, lambda: (Gtk.main_quit(), False)[1])
Gtk.main()
sys.exit(result)
