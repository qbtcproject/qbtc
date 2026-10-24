"""Compatibility entry point for the desktop packager."""
import runpy
from pathlib import Path
runpy.run_path(str(Path(__file__).with_name("package-desktop.py")), run_name="__main__")
