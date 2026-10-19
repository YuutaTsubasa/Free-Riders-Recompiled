"""The standalone online server now lives in server/live_server.py (also the
Docker image's, server/README.md). This path keeps working:

    python scripts/live_server.py --port 47800
"""
import runpy
from pathlib import Path

if __name__ == '__main__':
    runpy.run_path(str(Path(__file__).resolve().parents[1] / 'server' / 'live_server.py'), run_name='__main__')
