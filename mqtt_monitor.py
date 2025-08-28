#!/usr/bin/env python3

import paho.mqtt.client as mqtt
import json
import time
import subprocess
import signal
import os
from datetime import datetime
import threading
import logging
import builtins
import glob
import socket
from typing import Optional

# MQTT configuration (follow dummy's test file)
BROKER_HOST = "127.0.0.1"
BROKER_PORT = 9001
PROTOCOL = "websockets"
BASE_PATH = "/mqtt"

# Topics
# SUB_TOPIC = "amr/+/rtamrtf"
SUB_TOPIC = "fleetmanagement/3/robot/info"

# Logging configuration
LOG_FILE_MAX_BYTES = 1 * 1024 * 1024          # 1 MB per file
LOG_TOTAL_CAP_BYTES = 50 * 1024 * 1024        # 0.05 GB total cap
LOG_FILE_PREFIX = "mqtt_monitor_timeout_kill_adapter_"

AUTO_KILL_LOG_DIR = "/home/wcs2/AMR/auto_start/logs"

# Timeouts (seconds)
NO_MESSAGE_TIMEOUT = 20.0        # Kill if no messages at all for 20s
RESTART_WAIT_TIME = 120.0        # Wait up to 120s for adapter to restart

MAX_CONSECUTIVE_KILLS = 3       # Maximum kills allowed in a row

class MQTTMonitor:
    def __init__(self):
        # Setup log directory and logger first so all prints get recorded
        self.log_dir = self._resolve_log_dir()
        os.makedirs(self.log_dir, exist_ok=True)
        self.logger = self._create_file_logger()
        # Redirect prints to also write into log file
        self._orig_print = builtins.print
        builtins.print = self._tee_print

        # MQTT client with explicit unique client_id to avoid broker rejection
        self.client_id = f"rtamrtf-monitor-{socket.gethostname()}-{os.getpid()}"
        self.client = mqtt.Client(client_id=self.client_id, protocol=mqtt.MQTTv5, transport=PROTOCOL)
        self.client.ws_set_options(path=BASE_PATH)
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message
        self.client.on_disconnect = self.on_disconnect
        self.client.on_subscribe = self.on_subscribe
        self.client.on_log = self.on_log
        # Make reconnects fast but bounded
        try:
            self.client.reconnect_delay_set(min_delay=1, max_delay=5)
        except Exception:
            pass
        # Enable internal client logging
        try:
            logging.basicConfig(level=logging.INFO)
            self.client.enable_logger()
        except Exception:
            pass

        # Optional auth/TLS from environment variables
        self._configure_security_from_env()

        # State
        now = time.time()
        self.last_any_msg_time = now
        self.kill_in_progress = False
        self.monitoring = True
        self.consecutive_kill_count = 0       # Number of kills performed without a healthy message in between

        # Threads
        self.no_msg_thread = None

    # ---------- MQTT callbacks ----------
    def on_connect(self, client, userdata, flags_dict, reason, properties):
        print(f"[{datetime.now()}] Connected to MQTT broker at {BROKER_HOST}:{BROKER_PORT} (client_id={self.client_id})")
        client.subscribe(SUB_TOPIC)
        print(f"[{datetime.now()}] Subscribed to {SUB_TOPIC}")

    def _env_true(self, value: Optional[str]) -> bool:
        if value is None:
            return False
        return value.strip().lower() in ("1", "true", "yes", "on")

    def _configure_security_from_env(self) -> None:
        try:
            username = os.getenv("MQTT_USERNAME")
            password = os.getenv("MQTT_PASSWORD")
            if username:
                self.client.username_pw_set(username, password or None)
                print(f"[{datetime.now()}] MQTT auth enabled (username from env)")

            use_tls = self._env_true(os.getenv("MQTT_USE_TLS"))
            if use_tls:
                ca = os.getenv("MQTT_CA_CERT")
                certfile = os.getenv("MQTT_CERTFILE")
                keyfile = os.getenv("MQTT_KEYFILE")
                # tls_set can be called with None to use system CA store
                self.client.tls_set(ca_certs=ca if ca else None,
                                    certfile=certfile if certfile else None,
                                    keyfile=keyfile if keyfile else None)
                insecure = self._env_true(os.getenv("MQTT_TLS_INSECURE"))
                if insecure:
                    self.client.tls_insecure_set(True)
                print(f"[{datetime.now()}] MQTT TLS enabled (use_tls={use_tls}, insecure={insecure if 'insecure' in locals() else False})")
        except Exception as e:
            print(f"[{datetime.now()}] MQTT security config error: {e}")

    def on_disconnect(self, client, userdata, reasonCode, properties=None):
        try:
            print(f"[{datetime.now()}] Disconnected from MQTT broker. reasonCode={reasonCode}")
        except Exception:
            print(f"[{datetime.now()}] Disconnected from MQTT broker.")

    def on_subscribe(self, client, userdata, mid, granted_qos, properties=None):
        print(f"[{datetime.now()}] Subscribed mid={mid}, granted_qos={granted_qos}")

    def on_log(self, client, userdata, level, buf):
        # Reduce verbosity; still useful for disconnect causes
        if level >= 16:  # WARNING and above
            print(f"[{datetime.now()}] MQTT LOG level={level}: {buf}")

    # ---------- Logging helpers ----------
    def _resolve_log_dir(self) -> str:
        if AUTO_KILL_LOG_DIR:
            log_dir = AUTO_KILL_LOG_DIR
        else:
            log_dir = os.path.dirname(os.path.abspath(__file__))
        return os.path.join(log_dir, "log_mqtt_monitor")

    class _TimestampSizeRotator(logging.Handler):
        def __init__(self, log_dir: str, prefix: str, max_bytes: int, total_cap_bytes: int):
            super().__init__(level=logging.INFO)
            self.log_dir = log_dir
            self.prefix = prefix
            self.max_bytes = max_bytes
            self.total_cap_bytes = total_cap_bytes
            self._lock = threading.Lock()
            self._open_new_file()

        def _timestamp_name(self) -> str:
            ts = datetime.now().strftime("%d_%b_%Y_%H_%M_%S")
            return os.path.join(self.log_dir, f"{self.prefix}{ts}.log")

        def _open_new_file(self):
            self.current_path = self._timestamp_name()
            self.stream = open(self.current_path, "a", buffering=1)

        def _should_rotate(self) -> bool:
            try:
                return os.path.getsize(self.current_path) >= self.max_bytes
            except Exception:
                return False

        def _cleanup_total_size(self):
            try:
                files = sorted(
                    glob.glob(os.path.join(self.log_dir, f"{self.prefix}*.log")),
                    key=lambda p: os.path.getmtime(p)
                )
                total = 0
                sizes = []
                for p in files:
                    try:
                        sz = os.path.getsize(p)
                    except Exception:
                        sz = 0
                    sizes.append((p, sz))
                    total += sz
                idx = 0
                while total > self.total_cap_bytes and idx < len(sizes):
                    p, sz = sizes[idx]
                    try:
                        os.remove(p)
                    except Exception:
                        pass
                    total -= sz
                    idx += 1
            except Exception:
                pass

        def emit(self, record: logging.LogRecord):
            try:
                msg = self.format(record)
                with self._lock:
                    # rotate if needed
                    if self._should_rotate():
                        try:
                            self.stream.close()
                        except Exception:
                            pass
                        self._open_new_file()
                        self._cleanup_total_size()
                    self.stream.write(msg + "\n")
                    self.stream.flush()
            except Exception:
                pass

    def _create_file_logger(self) -> logging.Logger:
        logger = logging.getLogger("auto_kill_adapter")
        logger.propagate = False
        logger.setLevel(logging.INFO)
        # Remove old handlers
        for h in list(logger.handlers):
            logger.removeHandler(h)
        handler = self._TimestampSizeRotator(
            log_dir=self.log_dir,
            prefix=LOG_FILE_PREFIX,
            max_bytes=LOG_FILE_MAX_BYTES,
            total_cap_bytes=LOG_TOTAL_CAP_BYTES,
        )
        formatter = logging.Formatter("%(message)s")
        handler.setFormatter(formatter)
        logger.addHandler(handler)
        return logger

    def _tee_print(self, *args, **kwargs):
        sep = kwargs.get("sep", " ")
        end = kwargs.get("end", "\n")
        msg = sep.join(str(a) for a in args)
        try:
            self.logger.info(msg)
        except Exception:
            pass
        self._orig_print(*args, **kwargs)

    def on_message(self, client, userdata, msg):
        # Any received message updates the last seen timestamp. Content is ignored.
        self.last_any_msg_time = time.time()
        # Receiving any message resets the consecutive kill counter.
        if self.consecutive_kill_count != 0:
            self.consecutive_kill_count = 0

    # ---------- Watchdogs ----------
    def start_watchdogs(self):
        if not self.no_msg_thread or not self.no_msg_thread.is_alive():
            self.no_msg_thread = threading.Thread(target=self._watch_no_message, daemon=True)
            self.no_msg_thread.start()

    def _watch_no_message(self):
        while self.monitoring:
            # Avoid triggering while we are killing/waiting
            if self.kill_in_progress:
                time.sleep(0.1)
                continue
            if time.time() - self.last_any_msg_time >= NO_MESSAGE_TIMEOUT:
                print(f"[{datetime.now()}] No MQTT messages for {NO_MESSAGE_TIMEOUT}s. Initiating kill.")
                self.kill_robot_adapter()
                # Reset timer to avoid repeated triggers while waiting
                self.last_any_msg_time = time.time()
            time.sleep(0.1)

    # ---------- Kill and restart handling ----------
    def kill_robot_adapter(self):
        # Enforce maximum consecutive kills
        if self.consecutive_kill_count >= MAX_CONSECUTIVE_KILLS:
            print(f"[{datetime.now()}] Kill suppressed: reached max consecutive kills ({MAX_CONSECUTIVE_KILLS}).")
            return
        if self.kill_in_progress:
            return
        self.kill_in_progress = True

        print(f"[{datetime.now()}] Killing robot_adapter processes (single pass)...")
        # Prevent no-message watchdog from firing during kill
        self.last_any_msg_time = time.time()
        try:
            res = subprocess.run(["pgrep", "-f", "robot_adapter"], capture_output=True, text=True)
            if res.returncode == 0:
                pids = [p for p in res.stdout.strip().split("\n") if p.strip()]
                for pid in pids:
                    try:
                        os.kill(int(pid), signal.SIGKILL)
                        print(f"[{datetime.now()}] Killed PID {pid}")
                    except Exception as e:
                        print(f"[{datetime.now()}] Failed to kill PID {pid}: {e}")
            else:
                print(f"[{datetime.now()}] No robot_adapter process found.")
        except Exception as e:
            print(f"[{datetime.now()}] Kill pass error: {e}")

        # Count this kill attempt
        self.consecutive_kill_count += 1
        print(f"[{datetime.now()}] Consecutive kills so far: {self.consecutive_kill_count}/{MAX_CONSECUTIVE_KILLS}")

        # After single-pass kill, wait fixed 1 minute (message-based evaluation resumes after)
        print(f"[{datetime.now()}] Waiting {RESTART_WAIT_TIME}s before resuming evaluation...")
        start_wait = time.time()
        while time.time() - start_wait < RESTART_WAIT_TIME:
            # keep watchdogs quiet during wait
            self.last_any_msg_time = time.time()
            time.sleep(0.5)
        print(f"[{datetime.now()}] Resume monitoring by message evaluation.")
        self.kill_in_progress = False

    def _wait_for_restart(self):
        print(f"[{datetime.now()}] Waiting up to {RESTART_WAIT_TIME}s for robot_adapter to restart...")
        start = time.time()
        while time.time() - start < RESTART_WAIT_TIME:
            res = subprocess.run(["pgrep", "-f", "robot_adapter"], capture_output=True, text=True)
            if res.returncode == 0:
                print(f"[{datetime.now()}] robot_adapter appears to be running again. Resuming monitoring.")
                # Reset state and continue monitoring
                self.last_any_msg_time = time.time()
                self.kill_in_progress = False
                return
            time.sleep(1)
        print(f"[{datetime.now()}] Restart wait timed out. Continuing to monitor and will kill again if needed.")
        self.kill_in_progress = False

    # ---------- Run ----------
    def connect(self):
        try:
            # Use a shorter keepalive to keep websockets session fresh
            self.client.connect(BROKER_HOST, BROKER_PORT, 30)
            self.client.loop_start()
            return True
        except Exception as e:
            print(f"[{datetime.now()}] MQTT connect failed: {e}")
            return False

    def start(self):
        if not self.connect():
            return
        print(f"[{datetime.now()}] MQTT Monitor started.\n  - Subscribed: {SUB_TOPIC}\n  - Kill if: no messages for {NO_MESSAGE_TIMEOUT}s\n  - Restart wait: {RESTART_WAIT_TIME}s\n  - Max consecutive kills: {MAX_CONSECUTIVE_KILLS}")
        self.start_watchdogs()
        try:
            while self.monitoring:
                time.sleep(1)
        except KeyboardInterrupt:
            print(f"\n[{datetime.now()}] Stopping monitor...")
        finally:
            self.monitoring = False
            try:
                self.client.loop_stop()
                self.client.disconnect()
            except Exception:
                pass

if __name__ == "__main__":
    MQTTMonitor().start()
