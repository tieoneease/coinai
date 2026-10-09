import pathlib
import sys

HOST = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HOST))
