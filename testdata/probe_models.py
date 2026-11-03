"""Debug helper: drive the Model Advisor dialog.

    python probe_models.py <AppAssist.exe> <screenshot_dir> [download_dir]

Prints the startup hint, the advisor's labels/rows/advice, selects the 30B model with "Use this model",
and, if download_dir is given, starts a 7B download there, cancels it, and reports the partial file.
"""
import os
import subprocess
import sys
import time

from pywinauto import Application
from pywinauto.controls.common_controls import ListViewWrapper

exe, shots = sys.argv[1], sys.argv[2]
download_dir = sys.argv[3] if len(sys.argv) > 3 else None
os.makedirs(shots, exist_ok=True)

proc = subprocess.Popen([exe])
app = Application(backend="uia").connect(process=proc.pid, timeout=20)
win = app.window(title_re="AppAssist.*")
win.wait("visible", timeout=20)
win.set_focus()


def text_of(w, auto_id, control_type="Edit"):
    return w.child_window(auto_id=auto_id, control_type=control_type).window_text()


def output():
    return win.child_window(auto_id="1035", control_type="Edit").iface_value.CurrentValue


print("== startup output ==")
print(output())


def open_advisor():
    win.child_window(auto_id="1043", control_type="Button").click_input()
    # UI Automation shows an owned dialog as a child of its owner window.
    dlg = win.child_window(title_re="Model Advisor.*", control_type="Window")
    dlg.wait("visible", timeout=20)
    return dlg


def dump(dlg):
    print("system:", text_of(dlg, "1100", "Text"))
    print("kb:    ", text_of(dlg, "1101", "Text"))
    lv = list_view(dlg)
    for i in range(lv.item_count()):
        cells = [lv.get_item(i, c).text() for c in range(lv.column_count())]
        print("  row:", " | ".join(cells), "(selected)" if lv.is_selected(i) else "")
    print("advice:\n" + dlg.child_window(auto_id="1103", control_type="Edit").iface_value.CurrentValue)
    print("status:", text_of(dlg, "1108", "Text"))
    print("download button:", dlg.child_window(auto_id="1109", control_type="Button").window_text(),
          "use enabled:", dlg.child_window(auto_id="1111", control_type="Button").is_enabled())


def list_view(dlg):
    # The Win32 wrapper reads every column and the selection reliably.
    return ListViewWrapper(dlg.child_window(auto_id="1102", control_type="List").handle)


def select_row(dlg, index):
    list_view(dlg).get_item(index).click_input()
    time.sleep(0.5)


dlg = open_advisor()
time.sleep(1)
print("\n== advisor ==")
dump(dlg)
dlg.capture_as_image().save(os.path.join(shots, "model-advisor.png"))

print("\n== select 30B, Use this model ==")
select_row(dlg, 3)
print("download button:", dlg.child_window(auto_id="1109", control_type="Button").window_text())
dlg.child_window(auto_id="1111", control_type="Button").click_input()
time.sleep(1)
print("model path now:", text_of(win, "1014"))
print("status:", text_of(win, "1034", "Text"))

if download_dir:
    print("\n== download 7B, then cancel ==")
    dlg = open_advisor()
    folder = dlg.child_window(auto_id="1105", control_type="Edit")
    folder.set_edit_text(download_dir)
    select_row(dlg, 1)  # also moves focus off the folder box, which rescans
    select_row(dlg, 1)
    dlg.child_window(auto_id="1109", control_type="Button").click_input()
    time.sleep(12)
    print("status while downloading:", text_of(dlg, "1108", "Text"))
    print("cancel enabled:", dlg.child_window(auto_id="1110", control_type="Button").is_enabled(),
          "download enabled:", dlg.child_window(auto_id="1109", control_type="Button").is_enabled())
    dlg.capture_as_image().save(os.path.join(shots, "model-advisor-downloading.png"))
    dlg.child_window(auto_id="1110", control_type="Button").click_input()
    time.sleep(3)
    print("status after cancel:", text_of(dlg, "1108", "Text"))
    dump(dlg)
    print("files:", [(f, os.path.getsize(os.path.join(download_dir, f))) for f in os.listdir(download_dir)])
    dlg.child_window(auto_id="2", control_type="Button").click_input()  # IDCANCEL, not the title bar's Close
    time.sleep(1)

win.close()
time.sleep(1)
if proc.poll() is None:
    proc.kill()
