"""Build check: merge package/Shaders + features/*/Shaders like the AIO and run slot_audit.py with the allowlist."""
import glob, os, shutil, subprocess, sys, tempfile
src = sys.argv[1]; here = os.path.dirname(os.path.abspath(__file__))
tmp = tempfile.mkdtemp(prefix="bf_slots_")
shutil.copytree(f"{src}/package/Shaders", tmp, dirs_exist_ok=True)
for d in sorted(glob.glob(f"{src}/features/*/Shaders")):
    shutil.copytree(d, tmp, dirs_exist_ok=True)
args = [sys.executable, f"{here}/slot_audit.py", tmp, "--allow", f"{here}/slot_allowlist.txt"] + (["--log", sys.argv[2]] if len(sys.argv) > 2 else [])
rc = subprocess.run(args).returncode
shutil.rmtree(tmp, ignore_errors=True)
sys.exit(rc)
