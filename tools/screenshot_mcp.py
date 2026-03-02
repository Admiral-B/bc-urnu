"""MCP server for capturing Bridge Command simulator screenshots.

The simulator checks for 'screenshot_request.txt' in its working directory
each frame. When found, it saves 'screenshot.png' via WickedEngine's
GPU back-buffer readback and deletes the request file.

This MCP server exposes a 'take_screenshot' tool that triggers this
mechanism and returns the resulting image.
"""

import asyncio
import base64
import os
import time
from pathlib import Path

from mcp.server.fastmcp import FastMCP

# The simulator runs from bin/ directory
BIN_DIR = Path(__file__).resolve().parent.parent / "bin"
REQUEST_FILE = BIN_DIR / "screenshot_request.txt"
OUTPUT_FILE = BIN_DIR / "screenshot.png"

mcp = FastMCP("bridge-command-screenshot")


@mcp.tool()
async def take_screenshot(timeout_seconds: float = 5.0) -> str:
    """Capture a screenshot from the running Bridge Command simulator.

    Triggers the simulator's built-in screenshot mechanism (WickedEngine GPU
    back-buffer readback) and returns the resulting PNG image.

    The simulator must be running for this to work.

    Args:
        timeout_seconds: Max seconds to wait for the screenshot (default 5).

    Returns:
        Base64-encoded PNG image data, or an error message.
    """
    # Clean up any stale output from a previous request
    if OUTPUT_FILE.exists():
        OUTPUT_FILE.unlink()

    # Write the request file to trigger the simulator
    REQUEST_FILE.write_text("screenshot")

    # Poll for the output file
    start = time.monotonic()
    while time.monotonic() - start < timeout_seconds:
        if OUTPUT_FILE.exists() and not REQUEST_FILE.exists():
            # Small delay to ensure the file is fully written
            await asyncio.sleep(0.1)
            try:
                data = OUTPUT_FILE.read_bytes()
                if len(data) > 0:
                    b64 = base64.standard_b64encode(data).decode("ascii")
                    return f"data:image/png;base64,{b64}"
            except OSError:
                pass  # File still being written
        await asyncio.sleep(0.1)

    # Cleanup on timeout
    if REQUEST_FILE.exists():
        REQUEST_FILE.unlink()

    return "ERROR: Screenshot timed out. Is the simulator running?"


@mcp.tool()
async def is_simulator_running() -> str:
    """Check if the Bridge Command simulator process is running."""
    import subprocess
    result = subprocess.run(
        ["tasklist", "/FI", "IMAGENAME eq bridgecommand-bc.exe", "/NH"],
        capture_output=True, text=True
    )
    if "bridgecommand-bc.exe" in result.stdout:
        return "Bridge Command simulator is running."
    return "Bridge Command simulator is NOT running."


if __name__ == "__main__":
    mcp.run(transport="stdio")
