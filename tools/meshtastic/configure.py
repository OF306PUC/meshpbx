import argparse
import base64
import subprocess
import sys
import time
from pathlib import Path

import meshtastic.serial_interface

# All tool scripts live side-by-side in this directory; make sibling modules
# (configure_params, radio_config) importable when run directly.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import configure_params as node_params  # noqa: E402

MAX_RETRIES = 3
RETRY_DELAY = 10  # seconds to wait before retrying
SETTLE_DELAY = 10  # seconds to let the node settle after a successful command

# Keywords that indicate a transient serial error worth retrying
RETRYABLE_ERRORS = [
    "couldn't be opened",
    "Input/output error",
    "OS Error",
    "serial device",
    "write failed",
]

# Channels this node must join: telemetry (primary) + messaging, shared
# mesh-wide via common/radio_config.py. PSKs come from .env, same source of
# truth as node/ and gateway/.
CHANNELS = [
    (node_params.CHANNEL_TELEMETRY_IDX,
     node_params.CHANNEL_TELEMETRY_NAME,
     node_params.CHANNEL_TELEMETRY_PSK_B64),
    (node_params.CHANNEL_MSG_IDX,
     node_params.CHANNEL_MSG_NAME,
     node_params.CHANNEL_MSG_PSK_B64),
]


def is_retryable(stderr: str, stdout: str) -> bool:
    combined = (stderr + stdout).lower()
    return any(keyword.lower() in combined for keyword in RETRYABLE_ERRORS)


def run(cmd, retries=MAX_RETRIES) -> bool:
    """Run a meshtastic CLI command given as an argv list (no shell).

    Returns True on success, False if it still failed after `retries`
    attempts. Transient serial errors (RETRYABLE_ERRORS) are retried.
    """
    print(f"\nRunning: {' '.join(cmd)}")
    for attempt in range(1, retries + 1):
        result = subprocess.run(cmd, capture_output=True, text=True)
        print(result.stdout)
        if result.returncode == 0 and not is_retryable(result.stderr, result.stdout):
            time.sleep(SETTLE_DELAY)
            return True
        # Something went wrong.
        print(f"ERROR (attempt {attempt}/{retries}):", result.stderr or result.stdout)
        if attempt < retries:
            print(f"Retrying in {RETRY_DELAY}s...")
            time.sleep(RETRY_DELAY)
        else:
            print(f"Command failed after {retries} attempts.")
            time.sleep(2)
    return False


def get_config_value(mesh_argv, key: str) -> str:
    """Read one config value back from the node via `meshtastic --get <key>`.

    Returns the CLI's stdout (stripped) so callers can substring-match the
    expected value; returns '' if the read fails.
    """
    result = subprocess.run(mesh_argv + ["--get", key], capture_output=True, text=True)
    if result.returncode != 0:
        return ""
    return (result.stdout or "").strip()


def decode_psk(psk_b64: str) -> bytes:
    """Decode a channel PSK as stored in .env: 'base64:<key>' or bare base64."""
    if psk_b64.startswith("base64:"):
        psk_b64 = psk_b64[len("base64:"):]
    return base64.b64decode(psk_b64)


def main() -> int:
    parser = argparse.ArgumentParser(description="Configure the Meshtastic node attached to the BLE proxy.")
    parser.add_argument("--port", required=True, help="Serial port of the device (e.g. /dev/ttyUSB0)")
    args = parser.parse_args()

    # Per-proxy settings live in configure_params.py (edit before flashing).
    hop_limit = node_params.HOP_LIMIT
    device_role = node_params.DEVICE_ROLE

    # radio_config already enforces the telemetry PSK; the proxy node also
    # needs the messaging channel key, so fail early if it is missing.
    if not node_params.CHANNEL_MSG_PSK_B64:
        print(
            "ERROR: LORA_MSG_CHANNEL_PSK is not set. Copy .env.example to .env "
            "and set the messaging-channel PSK (shared mesh-wide)."
        )
        return 2

    # Base argv for every meshtastic CLI invocation (no shell → no quoting issues).
    mesh = ["meshtastic", "--port", args.port]

    failures = []

    def step(label, cmd):
        """Run a CLI step and record the label if it ultimately fails."""
        if not run(cmd):
            failures.append(label)

    print(f"Starting proxy-node configuration (role={device_role}, hop_limit={hop_limit}) on {args.port}...")

    # LoRa config: region, preset, hop limit. sx126x_rx_boosted_gain is
    # honoured only by SX126x radios; on an SX127x T-Beam it is stored but
    # ignored (harmless), so we set it mesh-wide from radio_config.
    step("LoRa (region/preset/hop_limit)", mesh + [
        "--set", "lora.region", node_params.LORA_REGION,
        "--set", "lora.modem_preset", node_params.LORA_PRESET,
        "--set", "lora.hop_limit", str(hop_limit),
        "--set", "lora.sx126x_rx_boosted_gain", str(node_params.SX126X_RX_BOOSTED_GAIN).lower(),
    ])

    # Device config: role then rebroadcast mode, chained in ONE invocation so
    # the CLI applies both in a single config write. Role is ordered first;
    # rebroadcast is verified afterwards, so a dropped write (role reboots) is
    # still caught by the check below.
    step("device role + rebroadcast_mode", mesh + [
        "--set", "device.role", device_role,
        "--set", "device.rebroadcast_mode", node_params.REBROADCAST_MODE,
    ])
    actual = get_config_value(mesh, "device.rebroadcast_mode")
    if node_params.REBROADCAST_MODE.lower() not in actual.lower():
        print(f"WARNING: device.rebroadcast_mode did not persist "
              f"(expected {node_params.REBROADCAST_MODE}, read '{actual or 'unknown'}').")
        failures.append("device.rebroadcast_mode (verify)")
    else:
        print(f"Verified device.rebroadcast_mode = {node_params.REBROADCAST_MODE}")

    # Bluetooth config: off — the nRF52840 proxy serves the BLE side.
    ble = str(node_params.BLUETOOTH_ENABLE).lower()
    step("bluetooth", mesh + ["--set", "bluetooth.enabled", ble])

    # Channel 0 (primary) already exists — rename it to the telemetry channel.
    # Channel 1 (messaging) must be created with --ch-add; re-running after it
    # exists makes the step fail, which is reported but harmless.
    step(f"channel {node_params.CHANNEL_TELEMETRY_IDX} rename", mesh + [
        "--ch-index", str(node_params.CHANNEL_TELEMETRY_IDX),
        "--ch-set", "name", node_params.CHANNEL_TELEMETRY_NAME,
    ])
    step(f"channel {node_params.CHANNEL_MSG_IDX} add", mesh + [
        "--ch-add", node_params.CHANNEL_MSG_NAME,
    ])

    # Set PSKs via Python API — the CLI assigns the value as a str to a bytes
    # field, causing "expected bytes, str found". The API accepts bytes directly.
    try:
        iface = meshtastic.serial_interface.SerialInterface(devPath=args.port)
        try:
            for ch_idx, ch_name, psk_b64 in CHANNELS:
                iface.localNode.channels[ch_idx].settings.psk = decode_psk(psk_b64)
                iface.localNode.writeChannel(ch_idx)
                print(f"PSK set for channel {ch_idx} ({ch_name}).")
                time.sleep(5)
        finally:
            iface.close()
    except Exception as exc:
        print(f"ERROR setting PSKs via API: {exc}")
        failures.append("channel PSKs (API)")

    # Telemetry config
    dev_meas = str(node_params.TELEMETRY_DEV_MEAS_ENABLED).lower()
    if node_params.TELEMETRY_DEV_MEAS_ENABLED:
        step("telemetry", mesh + [
            "--set", "telemetry.device_telemetry_enabled", dev_meas,
            "--set", "telemetry.device_update_interval", str(node_params.TELEMETRY_DEV_UPDATE_INTERVAL),
        ])
    else:
        step("telemetry", mesh + ["--set", "telemetry.device_telemetry_enabled", dev_meas])

    # Serial module config: Stream API on UART1 → the BLE proxy drives the node.
    serial_en = str(node_params.SERIAL_MODULE_ENABLE).lower()
    if node_params.SERIAL_MODULE_ENABLE:
        step("serial module", mesh + [
            "--set", "serial.enabled", serial_en,
            "--set", "serial.mode", node_params.SERIAL_MODULE_MODE,
            "--set", "serial.txd", str(node_params.SERIAL_MODULE_TXD),
            "--set", "serial.rxd", str(node_params.SERIAL_MODULE_RXD),
            "--set", "serial.baud", node_params.SERIAL_MODULE_BAUDRATE,
            "--set", "serial.timeout", str(node_params.SERIAL_MODULE_TIMEOUT),
        ])
    else:
        step("serial module", mesh + ["--set", "serial.enabled", serial_en])

    # GPS config
    step("GPS", mesh + [
        "--set", "position.gps_mode", node_params.GPS_MODE,
        "--set", "position.gps_update_interval", str(node_params.GPS_UPDATE_INTERNAL_INTERVAL),
        "--set", "position.position_broadcast_secs", str(node_params.GPS_UPDATE_BROADCAST_INTERVAL),
    ])

    # Reboot to apply changes
    step("reboot", mesh + ["--reboot"])

    # Summary + exit status.
    if failures:
        print("\n=== Configuration completed with ERRORS ===")
        for label in failures:
            print(f"  FAILED: {label}")
        print(f"{len(failures)} step(s) failed; the node may be partially configured.")
        return 1

    print("\n=== Configuration completed successfully ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
