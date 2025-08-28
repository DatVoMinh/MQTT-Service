#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if ! command -v python3 >/dev/null 2>&1; then
  echo "Python3 not found in PATH" >&2
  exit 1
fi

# Ensure paho-mqtt is installed
if ! python3 -c "import paho.mqtt.client" >/dev/null 2>&1; then
  echo "Installing paho-mqtt..."
  pip3 install --user paho-mqtt || {
    echo "Failed to install paho-mqtt" >&2
    exit 1
  }
fi

chmod +x mqtt_monitor.py
exec python3 mqtt_monitor.py
