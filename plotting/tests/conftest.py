"""Make the plotting scripts importable as modules from the tests."""

import sys
from pathlib import Path

PLOTTING_DIR = Path(__file__).resolve().parent.parent
if str(PLOTTING_DIR) not in sys.path:
    sys.path.insert(0, str(PLOTTING_DIR))
