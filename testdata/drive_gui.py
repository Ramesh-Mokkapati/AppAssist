"""Drive AppAssist.exe like a user (Windows UI Automation): fill in paths, pick an engine, Analyze.

usage: python drive_gui.py <AppAssist.exe> <kb.kdb> <log> <screenshot.png> [--model <file.gguf>] [--timeout S]
"""
import subprocess
import sys
import time

from pywinauto import Application

exe, kdb, log, png = sys.argv[1:5]
model = sys.argv[sys.argv.index("--model") + 1] if "--model" in sys.argv else None
timeout = int(sys.argv[sys.argv.index("--timeout") + 1]) if "--timeout" in sys.argv else 300

proc = subprocess.Popen([exe])
app = Application(backend="uia").connect(process=proc.pid, timeout=20)
win = app.window(title_re="AppAssist.*")
win.wait("visible", timeout=20)
print("window:", win.window_text(), flush=True)


def edit(control_id, text):
    box = win.child_window(auto_id=str(control_id), control_type="Edit")
    box.set_edit_text(text)


edit(1003, kdb)    # IDC_KB_PATH
edit(1011, log)    # IDC_LOG_PATH
if model:
    win.child_window(auto_id="1013", control_type="RadioButton").click_input()   # Embedded model
    edit(1014, model)
else:
    win.child_window(auto_id="1020", control_type="RadioButton").click_input()   # No AI

win.child_window(auto_id="1030", control_type="Button").click_input()     # Analyze log
status = win.child_window(auto_id="1034", control_type="Text")
start = time.time()
seen = set()
while time.time() - start < timeout:
    text = status.window_text()
    if text not in seen:
        print(f"  {time.time() - start:6.1f}s status: {text}", flush=True)
        seen.add(text)
    if text.startswith(("Analysis finished", "Cancelled")) or "rror" in text or "not found" in text:
        break
    time.sleep(0.5)
output = win.child_window(auto_id="1035", control_type="Edit").window_text()
print("output chars:", len(output), "| has evidence:", "[E1]" in output, "| has AI section:", "AI analysis" in output)
print(output[output.find("=== AI analysis"):][:1500] if "=== AI analysis" in output else "", flush=True)
win.set_focus()
time.sleep(0.5)
win.capture_as_image().save(png)
print("saved", png)
win.close()
time.sleep(2)
if proc.poll() is None:
    proc.kill()

