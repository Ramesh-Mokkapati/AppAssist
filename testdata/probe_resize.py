"""Debug helper: resize AppAssist's window and check that anchored controls follow (OnSize layout)."""
import ctypes
import subprocess
import sys
import time

from pywinauto import Application

proc = subprocess.Popen([sys.argv[1]])
app = Application(backend="uia").connect(process=proc.pid, timeout=20)
win = app.window(title_re="AppAssist.*")
win.wait("visible", timeout=20)

CONTROLS = {"output (stretch both)": "1035", "log path (stretch width)": "1011",
            "browse log (move right)": "1012", "save report (move right)": "1033"}


def rects():
    return {name: win.child_window(auto_id=cid).rectangle() for name, cid in CONTROLS.items()}


before = rects()
r = win.rectangle()
ctypes.windll.user32.MoveWindow(win.handle, r.left, r.top, r.width() + 300, r.height() + 200, True)
time.sleep(1)
after = rects()
for name in CONTROLS:
    b, a = before[name], after[name]
    print(f"{name:26} width {b.width():4} -> {a.width():4}   left {b.left:4} -> {a.left:4}   height {b.height():4} -> {a.height():4}")
win.close()
time.sleep(1)
if proc.poll() is None:
    proc.kill()

