"""Puts a new gear in place of the running one: windows_update.py FOLDER
Run on Windows in the signed-in user's session, where tools/windows-update sends it with deskrun.
FOLDER, in the user's home folder unless it is a whole path, holds gear.exe and, beside it, the new
files in a folder named new. The running gear is ended, the new files take the old ones' places, and
gear is started again; it goes back to the agent it last reached (target.txt).
"""
import os
import shutil
import subprocess
import sys
import time
import winreg

folder = os.path.join(os.path.expanduser("~"), sys.argv[1])
subprocess.run(["taskkill", "/IM", "gear.exe", "/F"], capture_output=True, creationflags=0x08000000)
new = os.path.join(folder, "new")
error = None
for attempt in range(120):  # (the program's file can be replaced once the program has gone)
    try:
        for name in os.listdir(new):
            shutil.copyfile(os.path.join(new, name), os.path.join(folder, name))
        break
    except OSError as e:
        error = e
        time.sleep(0.25)
else:
    sys.exit(f"the new files could not be put in place: {error!r}")
shutil.rmtree(new)
# The user's own variables as they are now (GEAR_DEBUG, say), which deskrun's worker, started before a
# change, would not pass on.
env = dict(os.environ)
with winreg.OpenKey(winreg.HKEY_CURRENT_USER, "Environment") as key:
    for i in range(winreg.QueryInfoKey(key)[1]):
        name, value, kind = winreg.EnumValue(key, i)
        if name.upper() != "PATH" and kind in (winreg.REG_SZ, winreg.REG_EXPAND_SZ):
            env[name] = os.path.expandvars(value)
subprocess.Popen([os.path.join(folder, "gear.exe")], cwd=folder, env=env, creationflags=0x00000008 | 0x00000200)  # (on its own, not the worker's child)
