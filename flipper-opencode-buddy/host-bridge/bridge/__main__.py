"""Entry point: python -m bridge  [--transport usb|ble] [--flipper NAME]"""

import argparse
import asyncio
import logging
import os
import signal
import sys

from . import config
from .daemon import Daemon


def _make_transport(name: str):
    if name == "ble":
        from .transport_bt import BtTransport
        return BtTransport()
    if name == "usb":
        from .transport_usb import UsbTransport
        return UsbTransport()
    from .transport_auto import AutoTransport
    return AutoTransport()


def main():
    if sys.version_info < (3, 10):
        sys.exit(
            f"flipper-opencode-buddy requires Python 3.10 or higher. "
            f"You are running Python {sys.version.split()[0]}. "
            f"Please upgrade Python, then reinstall the plugin:\n"
            f"Reinstall the flipper-opencode-buddy plugin after upgrading Python."
        )

    parser = argparse.ArgumentParser(description="Flipper OpenCode Buddy bridge")
    parser.add_argument(
        "--transport",
        choices=["auto", "usb", "ble"],
        default=config.TRANSPORT,
        help="Communication transport (default: %(default)s)",
    )
    parser.add_argument(
        "--flipper",
        default="",
        help="BLE device name (e.g. 'Omachal'; overrides FLIPPER_BT_NAME env)",
    )
    parser.add_argument(
        "--log-level",
        choices=["debug", "info", "warning", "error"],
        default=os.environ.get("FLIPPER_LOG_LEVEL", "info"),
        help="Log level (default: info, env: FLIPPER_LOG_LEVEL)",
    )
    args = parser.parse_args()
    if args.flipper:
        config.BT_DEVICE_NAME = args.flipper

    logging.basicConfig(
        level=getattr(logging, args.log_level.upper()),
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    )

    transport = _make_transport(args.transport)
    daemon = Daemon(transport)

    async def _run():
        loop = asyncio.get_running_loop()
        stop_event = asyncio.Event()
        for sig in (signal.SIGINT, signal.SIGTERM):
            loop.add_signal_handler(sig, stop_event.set)

        daemon_task = asyncio.create_task(daemon.run())
        stopper = asyncio.create_task(stop_event.wait())
        done, _ = await asyncio.wait(
            {daemon_task, stopper}, return_when=asyncio.FIRST_COMPLETED
        )
        if daemon_task not in done:
            daemon_task.cancel()
            try:
                await daemon_task
            except asyncio.CancelledError:
                pass
        stopper.cancel()

    try:
        asyncio.run(_run())
    except KeyboardInterrupt:
        pass
    finally:
        logging.info("Bridge stopped.")


if __name__ == "__main__":
    main()
