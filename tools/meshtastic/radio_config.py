"""Single source of truth for mesh-wide radio settings.

Every node MUST agree on channel, region, preset and PSK or the mesh won't
communicate — defined here once and imported by configure_params.py and
check_node_info.py.

The channel PSKs are shared secrets: read from the environment (the `.env` file
in this directory) and never committed. Set LORA_TELEMETRY_CHANNEL_PSK and
LORA_MSG_CHANNEL_PSK there (base64).
"""
import os
from pathlib import Path

_ENV_PATH = Path(__file__).resolve().parent / ".env"  # tools/meshtastic/.env


def _load_dotenv(path: Path) -> None:
    """Minimal .env loader (no external dependency). Won't override existing env vars."""
    if not path.exists():
        return
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, val = line.partition("=")
        os.environ.setdefault(key.strip(), val.strip())


_load_dotenv(_ENV_PATH)

# ── Shared, non-secret radio settings (node and gateway must match) ───────────
CHANNEL_TELEMETRY_IDX  = 0
CHANNEL_TELEMETRY_NAME = "telCPS_RTC"
CHANNEL_MSG_IDX = 1
CHANNEL_MSG_NAME = "msgPUC_NET"
LORA_REGION  = "ANZ"
LORA_PRESET  = "MEDIUM_FAST"
REBROADCAST_MODE = "LOCAL_ONLY"

# SX126x RX Boosted Gain: trades a little extra power for higher RX
# sensitivity. Only the SX126x radio series honours it — the LilyGO T-Beam
# uses an SX127x, so the field is stored but ignored there (harmless no-op).
SX126X_RX_BOOSTED_GAIN = True

# ── Shared secret: channel PSK (base64). From env; never commit the real value. ──
CHANNEL_TELEMETRY_PSK_B64 = os.environ.get("LORA_TELEMETRY_CHANNEL_PSK")
CHANNEL_MSG_PSK_B64 = os.environ.get("LORA_MSG_CHANNEL_PSK")
_missing = [
    name
    for name, value in (
        ("LORA_TELEMETRY_CHANNEL_PSK", CHANNEL_TELEMETRY_PSK_B64),
        ("LORA_MSG_CHANNEL_PSK", CHANNEL_MSG_PSK_B64),
    )
    if not value
]
if _missing:
    raise RuntimeError(
        f"Channel PSK(s) not set: {', '.join(_missing)}. Copy .env.example to "
        ".env and set both base64 keys (shared by every node and the gateway)."
    )
