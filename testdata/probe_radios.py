"""Debug helper: click each engine radio button and report radio/edit states."""
import subprocess
import sys
import time

from pywinauto import Application

proc = subprocess.Popen([sys.argv[1]])
app = Application(backend="uia").connect(process=proc.pid, timeout=20)
win = app.window(title_re="AppAssist.*")
win.wait("visible", timeout=20)
win.set_focus()


def state():
    radios = {rid: win.child_window(auto_id=rid, control_type="RadioButton") for rid in ("1013", "1016", "1020")}
    sel = {rid: r.is_selected() for rid, r in radios.items()}
    model_edit = win.child_window(auto_id="1014", control_type="Edit").is_enabled()
    url_edit = win.child_window(auto_id="1017", control_type="Edit").is_enabled()
    return f"selected={sel} model_edit_enabled={model_edit} url_edit_enabled={url_edit}"


print("start:        ", state())
for rid, name in (("1013", "GGUF model"), ("1016", "Local server"), ("1020", "No AI"), ("1013", "GGUF model")):
    r = win.child_window(auto_id=rid, control_type="RadioButton")
    print(f"  rect of {name}: {r.rectangle()}")
    r.click_input()
    time.sleep(0.5)
    print(f"after {name:12}", state())
win.close()
time.sleep(1)
if proc.poll() is None:
    proc.kill()
