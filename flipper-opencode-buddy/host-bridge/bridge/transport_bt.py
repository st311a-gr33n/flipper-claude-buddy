"""
Bluetooth BLE transport — uses bleak to talk to the Flipper Zero BLE serial service.

Connection flow:
  1. Scan for a BLE device advertising config.FLIPPER_ADV_UUID (0x3082), falling back
     to a name prefix match against config.BT_DEVICE_NAME ("Flipper")
  2. On Linux: pre-pair / trust the device via bluetoothctl so BlueZ allows
     GATT service discovery (best-effort; pairing failures are non-fatal)
  3. Connect to the device (up to 3 retries with backoff for BlueZ GATT issues)
  4. Look for the Flipper's Serial-over-BLE GATT service
  5. Verify the known serial characteristics for notify (RX) and write (TX)
  6. Subscribe to notifications on the serial characteristic
  7. Incoming notifications are buffered; readline() returns complete lines

Flipper BLE Serial UUIDs (official and Momentum firmware):
  Adv UUID: 00003082-0000-1000-8000-00805f9b34fb  (advertised service, used for scan)
  TX char:  19ed82ae-ed21-4c9d-4145-228e61fe0000  (Flipper→host, notify)
  RX char:  19ed82ae-ed21-4c9d-4145-228e62fe0000  (host→Flipper, write)

Install dependency:  pip install bleak   (or: pip install ".[bt]")
"""

import asyncio
import logging

from . import config
from .transport import Transport

log = logging.getLogger(__name__)


FLIPPER_SERIAL_TX_UUID = "19ed82ae-ed21-4c9d-4145-228e61fe0000"  # Flipper→host (notify)
FLIPPER_SERIAL_RX_UUID = "19ed82ae-ed21-4c9d-4145-228e62fe0000"  # host→Flipper (write)


# ---------------------------------------------------------------------------
# BlueZ cache helper (Linux only)
# ---------------------------------------------------------------------------

async def _bluetoothctl_session(cmds: list[str], timeout: float = 10.0) -> tuple[int, str]:
    """Run bluetoothctl commands in a session with a NoInputNoOutput agent.

    Commands are sent via stdin and the process stays alive until they
    complete.  For the ``pair`` command specifically, use
    ``_bluetoothctl_pair()`` instead — it needs a dedicated process so
    the agent isn't torn down before the async pairing handshake finishes.

    Returns ``(rc, output_text)``.
    """
    import shutil
    if not shutil.which("bluetoothctl"):
        return -1, "bluetoothctl not found"
    try:
        proc = await asyncio.create_subprocess_exec(
            "bluetoothctl", "--agent", "NoInputNoOutput",
            "--timeout", str(int(timeout)),
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        input_str = "\n".join(cmd for cmd in cmds if cmd) + "\nquit\n"
        stdout, stderr = await asyncio.wait_for(
            proc.communicate(input_str.encode()), timeout=timeout + 5,
        )
        return proc.returncode or 0, (stdout + stderr).decode(errors="replace")
    except asyncio.TimeoutError:
        return -1, "timed out"
    except Exception as exc:
        return -1, str(exc)


async def _bluetoothctl_pair(address: str, timeout: float = 25.0) -> str:
    """Pair with a BLE device.  Runs ``pair`` in a dedicated bluetoothctl
    process whose ``--agent`` stays alive for the full pairing handshake.

    Returns the raw stdout+stderr output text (exit code is not meaningful
    because ``pair`` success is determined by output keywords).
    """
    proc = await asyncio.create_subprocess_exec(
        "bluetoothctl", "--agent", "NoInputNoOutput",
        "--timeout", str(int(timeout)),
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    stdout, stderr = await asyncio.wait_for(
        proc.communicate(f"pair {address}\n".encode()), timeout=timeout + 5,
    )
    return (stdout + stderr).decode(errors="replace")


async def _bluetoothctl_clear(address: str) -> None:
    """Remove any stale BlueZ device entry.

    A stale (possibly incomplete) GATT database from a previous
    connection can cause "Failed to discover services" on later
    connects.  Agent/pairing setup is handled by ``--agent`` on the
    bluetoothctl session; we don't pre-pair here because the bond
    can confuse the Flipper's BLE stack on reconnect
    (UNLIKELY_ERROR on CCCD writes).
    """
    rc, out = await _bluetoothctl_session(["pairable on", f"remove {address}"])
    log.debug("BT: bluetoothctl session (pairable on, remove %s) → rc=%d", address, rc)


async def _bluetoothctl_pair_fallback(address: str) -> bool:
    """Pair + trust a BLE device for BlueZ versions that refuse service
    discovery on unpaired devices.  Returns True if pairing succeeded.

    Call ONLY as a fallback when the first connection attempt fails with
    a service-discovery error.  Pairing creates a bond that can cause
    ``UNLIKELY_ERROR`` on CCCD writes against the Flipper's BLE stack,
    so we avoid it unless necessary.
    """
    log.info("BT: fallback — pairing %s via bluetoothctl…", address)
    out = await _bluetoothctl_pair(address)
    ok = "Pairing successful" in out or "Paired: yes" in out
    if ok:
        log.info("BT: paired with %s", address)
        await _bluetoothctl_session([f"trust {address}"])
    else:
        log.info("BT: pair %s FAILED — output:\n%s", address, out.strip())
    # Give the Flipper time to restart advertising after the pair→disconnect.
    await asyncio.sleep(1.0)
    return ok


# ---------------------------------------------------------------------------
# BtTransport
# ---------------------------------------------------------------------------

class BtTransport(Transport):
    def __init__(self):
        self._client = None
        self._tx_uuid = FLIPPER_SERIAL_TX_UUID  # subscribe for notify
        self._rx_uuid = FLIPPER_SERIAL_RX_UUID  # write to
        self._rx_buf = bytearray()
        self._rx_event = asyncio.Event()
        self._closed = True

    # ── Transport interface ────────────────────────────────────────

    async def connect(self) -> bool:
        try:
            from bleak import BleakScanner, BleakClient
        except ImportError:
            log.error("BT transport requires bleak:  pip install bleak")
            return False

        adv_uuid = config.FLIPPER_ADV_UUID.lower()
        name_prefix = config.BT_DEVICE_NAME

        def _is_flipper(device, adv_data) -> bool:
            # Primary: 0xFEAF is broadcast in every Flipper advertisement packet.
            if adv_uuid in [u.lower() for u in adv_data.service_uuids]:
                return True
            # Fallback: name prefix, for OS-level ad caches that strip service UUIDs.
            return bool(name_prefix and device.name and device.name.startswith(name_prefix))

        log.info(
            "BT: scanning for Flipper (UUID %s or name prefix %r, timeout %.0fs)…",
            config.FLIPPER_ADV_UUID, name_prefix, config.BT_SCAN_TIMEOUT,
        )
        device = await BleakScanner.find_device_by_filter(
            _is_flipper,
            timeout=config.BT_SCAN_TIMEOUT,
        )
        if device is None:
            log.warning("BT: Flipper not found — is Bluetooth enabled and advertising?")
            return False

        log.info("BT: found %s (%s)", device.name, device.address)

        # Clear any stale BlueZ device cache so GATT handles are
        # re-discovered fresh.  We deliberately do NOT pre-pair here:
        # pairing creates a bond that can confuse the Flipper's BLE
        # stack on reconnect (UNLIKELY_ERROR on CCCD writes).
        await _bluetoothctl_clear(device.address)

        # Connect with retries for BlueZ GATT flakiness.
        # Up to 3 attempts with increasing backoff (1.5s, 3s, 4.5s).
        # Retry loop covers connect, GATT service discovery, and notify
        # subscription — all three can fail transiently on BlueZ.
        paired_fallback = False
        last_error = ""
        for attempt in range(3):
            self._closed = False  # reset from any previous attempt's disconnect
            self._client = BleakClient(
                device.address, disconnected_callback=self._on_disconnect,
            )
            try:
                await self._client.connect()
            except Exception as exc:
                last_error = str(exc)
                self._client = None
                # Some BlueZ versions refuse service discovery on unpaired
                # devices ("failed to discover services, device
                # disconnected").  Pair+trust once as a fallback and
                # retry — the bond will persist for the next attempt.
                if ("discover" in last_error.lower()) and not paired_fallback:
                    log.warning(
                        "BT: connect attempt %d/3 failed (needs pairing?): %s",
                        attempt + 1, exc,
                    )
                    paired_fallback = await _bluetoothctl_pair_fallback(device.address)
                    continue
                if "device disconnected" in last_error.lower() or \
                   "discover" in last_error.lower():
                    wait = 1.5 * (attempt + 1)
                    log.warning(
                        "BT: connect attempt %d/3 failed (BlueZ GATT issue): %s — "
                        "retrying in %.1fs…",
                        attempt + 1, exc, wait,
                    )
                    await asyncio.sleep(wait)
                    continue
                log.error("BT: connect to %s failed: %s", device.name, exc)
                return False

            # GATT connect succeeded — verify characteristics and subscribe.
            # These can also fail transiently (e.g. "Unlikely Error" during
            # start_notify on BlueZ).  Treat them as retryable, not fatal.
            try:
                tx_char = self._client.services.get_characteristic(self._tx_uuid)
                rx_char = self._client.services.get_characteristic(self._rx_uuid)
            except Exception:
                tx_char = rx_char = None
            if tx_char is None or rx_char is None:
                log.warning(
                    "BT: attempt %d/3 — serial characteristics not found on %s, "
                    "retrying…",
                    attempt + 1, device.name,
                )
                try:
                    await self._client.disconnect()
                except Exception:
                    pass
                self._client = None
                await asyncio.sleep(1.5 * (attempt + 1))
                continue

            log.info("BT: serial service found  TX=%s  RX=%s", self._tx_uuid, self._rx_uuid)
            log.info("BT: TX char properties: %s", tx_char.properties)
            log.info("BT: RX char properties: %s", rx_char.properties)
            mtu = getattr(self._client, "mtu_size", 23)
            log.info("BT: negotiated MTU=%d  (write chunk=%d)",
                     mtu, max(1, min(mtu - 3, config.BT_WRITE_CHUNK)))

            try:
                await self._client.start_notify(self._tx_uuid, self._on_notify)
            except Exception as exc:
                err_str = str(exc)
                # If we paired and still get UNLIKELY_ERROR, the bond is
                # the culprit — remove it and retry without bonding.
                if "unlikely" in err_str.lower() and paired_fallback:
                    log.warning(
                        "BT: attempt %d/3 — start_notify failed after pairing "
                        "(UNLIKELY_ERROR, bond interference): %s — "
                        "removing bond and retrying…",
                        attempt + 1, exc,
                    )
                    paired_fallback = False
                    try:
                        await self._client.disconnect()
                    except Exception:
                        pass
                    self._client = None
                    await _bluetoothctl_clear(device.address)
                    continue
                log.warning(
                    "BT: attempt %d/3 — start_notify failed (BlueZ GATT error): %s — "
                    "retrying…",
                    attempt + 1, exc,
                )
                try:
                    await self._client.disconnect()
                except Exception:
                    pass
                self._client = None
                await asyncio.sleep(1.5 * (attempt + 1))
                continue

            # All good — finish handshake
            self._closed = False
            self._rx_buf.clear()
            log.info("BT: connected to %s", device.name)
            return True

        # Exhausted all retries
        log.error(
            "BT: connect to %s failed after 3 attempts.\n"
            "Last error: %s\n"
            "Try manually:  bluetoothctl remove %s && "
            "bluetoothctl pair %s && bluetoothctl trust %s\n"
            "Then restart the bridge.",
            device.name, last_error, device.address, device.address, device.address,
        )
        self._client = None
        return False

    async def readline(self) -> bytes:
        """Block until a complete \\n-terminated line arrives via BLE notify."""
        while True:
            if b"\n" in self._rx_buf:
                idx = self._rx_buf.index(b"\n")
                line = bytes(self._rx_buf[: idx + 1])
                del self._rx_buf[: idx + 1]
                return line
            if self._closed:
                return b""
            self._rx_event.clear()
            # Re-check after clear to avoid losing a notification that arrived
            # between the buffer check above and clearing the event.
            if b"\n" in self._rx_buf or self._closed:
                continue
            await self._rx_event.wait()

    async def write(self, data: bytes) -> None:
        """Write to the RX characteristic in MTU-safe chunks.

        Uses write-without-response (Write Command) so the ATT layer does not
        add a second ACK on top of the serial profile's own credit system.
        Chunk size is derived from the negotiated ATT MTU (CoreBluetooth
        negotiates this automatically; MTU minus 3 bytes overhead).
        """
        mtu = getattr(self._client, "mtu_size", 23)
        chunk = max(1, min(mtu - 3, config.BT_WRITE_CHUNK))
        for i in range(0, len(data), chunk):
            await self._client.write_gatt_char(
                self._rx_uuid, data[i : i + chunk], response=False
            )

    async def drain(self) -> None:
        pass  # BLE writes are already awaited

    async def get_rssi(self) -> int | None:
        if not self._client or not self._client.is_connected:
            return None

        backend = getattr(self._client, "_backend", None)
        if backend is None or not hasattr(backend, "get_rssi"):
            return None

        try:
            return int(await backend.get_rssi())
        except Exception as e:
            log.debug("BT: RSSI read failed: %s", e)
            return None

    def close(self) -> None:
        self._closed = True
        self._rx_event.set()  # unblock any waiting readline()
        if self._client:
            asyncio.ensure_future(self._client.disconnect())

    async def aclose(self) -> None:
        """Disconnect BLE and wait for the peer to see it.

        The bridge is restarted every time the user exits and re-enters a
        Claude session.  If we just drop the process without awaiting the
        GATT disconnect, the Flipper's BLE stack has to wait for the
        supervision timeout before it notices and resumes advertising —
        during which the next bridge process can't find it.  Awaiting the
        disconnect here makes handover near-instant.
        """
        self._closed = True
        self._rx_event.set()
        client = self._client
        self._client = None
        if client is None:
            return
        try:
            await client.disconnect()
        except Exception as e:
            log.warning("BT: disconnect error: %s", e)

    @property
    def is_closing(self) -> bool:
        if self._closed:
            return True
        return self._client is None or not self._client.is_connected

    # ── BLE callbacks ──────────────────────────────────────────────

    def _on_notify(self, _handle, data: bytearray) -> None:
        self._rx_buf.extend(data)
        self._rx_event.set()

    def _on_disconnect(self, _client) -> None:
        log.warning("BT: disconnected")
        self._closed = True
        self._rx_event.set()
