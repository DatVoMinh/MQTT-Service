#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if ! command -v g++ >/dev/null 2>&1; then
  echo "g++ not found in PATH" >&2
  exit 1
fi

NEED_BUILD=0
if [[ ! -f "$SCRIPT_DIR/mqtt_monitor" ]]; then
  NEED_BUILD=1
elif [[ "$SCRIPT_DIR/mqtt_monitor.cpp" -nt "$SCRIPT_DIR/mqtt_monitor" ]]; then
  NEED_BUILD=1
fi

if [[ "$NEED_BUILD" -eq 1 ]]; then
  echo "Building mqtt_monitor (C++)..."
  set +e
  g++ -std=c++17 -O2 -o mqtt_monitor mqtt_monitor.cpp -lpaho-mqttpp3 -lpaho-mqtt3a -lpthread
  STATUS=$?
  if [[ $STATUS -ne 0 ]]; then
    echo "Retry building with libpaho-mqtt3c..."
    g++ -std=c++17 -O2 -o mqtt_monitor mqtt_monitor.cpp -lpaho-mqttpp3 -lpaho-mqtt3c -lpthread
    STATUS=$?
  fi
  set -e
  if [[ $STATUS -ne 0 ]]; then
    echo "Build failed. Ensure libpaho-mqttpp3 and libpaho-mqtt3(a|c) are installed." >&2
    exit 1
  fi
fi

exec "$SCRIPT_DIR/mqtt_monitor"
