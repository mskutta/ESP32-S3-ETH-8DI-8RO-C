#!/usr/bin/env python3
"""Exercise production network adapters with fake transports and flash under sanitizers."""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shutil.which('clang++') or shutil.which('g++')
if not compiler:
    raise SystemExit('Install clang++ or g++ to run native tests')
with tempfile.TemporaryDirectory(prefix='spookio-tests-') as tmp:
    binary = Path(tmp) / 'tests'
    subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                    '-fsanitize=address,undefined', '-I', str(root / 'tests/stubs'),
                    '-I', str(root / 'src'), str(root / 'tests/test_network.cpp'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

with tempfile.TemporaryDirectory(prefix='spookio-config-tests-') as tmp:
    binary = Path(tmp) / 'tests'
    subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                    '-fsanitize=address,undefined', '-I', str(root / 'tests/stubs'),
                    '-I', str(root / 'src'), str(root / 'src/WS_Config.cpp'),
                    str(root / 'src/WS_OscText.cpp'), str(root / 'tests/test_config.cpp'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
