#!/bin/sh
# Starts the launcher with the SDK's python and the bundled PySide6.
for d in /app/lib/python3*/site-packages; do
    PYTHONPATH="$d${PYTHONPATH:+:$PYTHONPATH}"
done
export PYTHONPATH
exec python3 /app/share/forza-reimagined/main.py "$@"
