"""MCP server for sending input to the Bridge Command simulator.

Uses Win32 SendInput to inject keyboard and mouse events at the hardware
level. The simulator reads input via GetAsyncKeyState() and GetCursorPos(),
both of which respond to SendInput-injected events.

The simulator window must exist (but does not need foreground focus for
GetAsyncKeyState -- it reads global key state).
"""

import asyncio
import ctypes
import ctypes.wintypes
import subprocess
import time
from typing import Optional

from mcp.server.fastmcp import FastMCP

# Win32 constants
INPUT_MOUSE = 0
INPUT_KEYBOARD = 1
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_SCANCODE = 0x0008
MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_ABSOLUTE = 0x8000
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_RIGHTDOWN = 0x0008
MOUSEEVENTF_RIGHTUP = 0x0010
MOUSEEVENTF_WHEEL = 0x0800

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", ctypes.wintypes.LONG),
        ("dy", ctypes.wintypes.LONG),
        ("mouseData", ctypes.wintypes.DWORD),
        ("dwFlags", ctypes.wintypes.DWORD),
        ("time", ctypes.wintypes.DWORD),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    ]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", ctypes.wintypes.WORD),
        ("wScan", ctypes.wintypes.WORD),
        ("dwFlags", ctypes.wintypes.DWORD),
        ("time", ctypes.wintypes.DWORD),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    ]


class INPUT_UNION(ctypes.Union):
    _fields_ = [
        ("mi", MOUSEINPUT),
        ("ki", KEYBDINPUT),
    ]


class INPUT(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.wintypes.DWORD),
        ("union", INPUT_UNION),
    ]


# Virtual key code mapping
VK_MAP = {
    # Letters
    "a": 0x41, "b": 0x42, "c": 0x43, "d": 0x44, "e": 0x45,
    "f": 0x46, "g": 0x47, "h": 0x48, "i": 0x49, "j": 0x4A,
    "k": 0x4B, "l": 0x4C, "m": 0x4D, "n": 0x4E, "o": 0x4F,
    "p": 0x50, "q": 0x51, "r": 0x52, "s": 0x53, "t": 0x54,
    "u": 0x55, "v": 0x56, "w": 0x57, "x": 0x58, "y": 0x59,
    "z": 0x5A,
    # Numbers
    "0": 0x30, "1": 0x31, "2": 0x32, "3": 0x33, "4": 0x34,
    "5": 0x35, "6": 0x36, "7": 0x37, "8": 0x38, "9": 0x39,
    # Special keys
    "escape": 0x1B, "esc": 0x1B,
    "enter": 0x0D, "return": 0x0D,
    "space": 0x20,
    "tab": 0x09,
    "backspace": 0x08,
    "delete": 0x2E,
    "shift": 0x10, "lshift": 0xA0, "rshift": 0xA1,
    "control": 0x11, "ctrl": 0x11,
    "alt": 0x12,
    # Arrow keys
    "left": 0x25, "up": 0x26, "right": 0x27, "down": 0x28,
    # Function keys
    "f1": 0x70, "f2": 0x71, "f3": 0x72, "f4": 0x73,
    "f5": 0x74, "f6": 0x75, "f7": 0x76, "f8": 0x77,
    "f9": 0x78, "f10": 0x79, "f11": 0x7A, "f12": 0x7B,
    # Numpad
    "numpad0": 0x60, "numpad1": 0x61, "numpad2": 0x62,
    "numpad3": 0x63, "numpad4": 0x64, "numpad5": 0x65,
    "numpad6": 0x66, "numpad7": 0x67, "numpad8": 0x68,
    "numpad9": 0x69,
}

WINDOW_CLASSES = ["BridgeCommandWicked", "CIrrlichtWindowsTestDialog"]


def _find_simulator_window() -> Optional[int]:
    """Find the Bridge Command simulator window by class name."""
    for cls in WINDOW_CLASSES:
        hwnd = user32.FindWindowW(cls, None)
        if hwnd and user32.IsWindow(hwnd):
            return hwnd
    return None


def _get_window_rect(hwnd: int) -> tuple:
    """Get window client area in screen coordinates."""
    rect = ctypes.wintypes.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rect))
    point = ctypes.wintypes.POINT(rect.left, rect.top)
    user32.ClientToScreen(hwnd, ctypes.byref(point))
    return (point.x, point.y, rect.right - rect.left, rect.bottom - rect.top)


def _send_key_input(vk: int, key_up: bool = False):
    """Send a single key event via SendInput."""
    inp = INPUT()
    inp.type = INPUT_KEYBOARD
    inp.union.ki.wVk = vk
    inp.union.ki.dwFlags = KEYEVENTF_KEYUP if key_up else 0
    inp.union.ki.time = 0
    inp.union.ki.dwExtraInfo = None
    ctypes.windll.user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def _send_mouse_input(x: int, y: int, flags: int, mouse_data: int = 0):
    """Send a mouse event via SendInput using absolute screen coordinates."""
    screen_w = user32.GetSystemMetrics(0)
    screen_h = user32.GetSystemMetrics(1)
    abs_x = int(x * 65535 / screen_w)
    abs_y = int(y * 65535 / screen_h)

    inp = INPUT()
    inp.type = INPUT_MOUSE
    inp.union.mi.dx = abs_x
    inp.union.mi.dy = abs_y
    inp.union.mi.mouseData = mouse_data
    inp.union.mi.dwFlags = flags | MOUSEEVENTF_ABSOLUTE
    inp.union.mi.time = 0
    inp.union.mi.dwExtraInfo = None
    ctypes.windll.user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def _bring_to_foreground(hwnd: int):
    """Bring the simulator window to the foreground."""
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.05)


mcp = FastMCP("bridge-command-input")


@mcp.tool()
async def send_key(key: str, action: str = "tap", duration_ms: int = 50) -> str:
    """Send a keyboard input to the Bridge Command simulator.

    Args:
        key: Key name (e.g. 'w', 'a', 's', 'd', 'escape', 'f12', 'shift',
             'up', 'down', 'left', 'right', 'space', 'enter', 'o', 'r').
        action: 'tap' (press+release), 'press' (hold down), or 'release'.
        duration_ms: For 'tap', how long to hold the key in milliseconds.

    Returns:
        Status message.
    """
    hwnd = _find_simulator_window()
    if not hwnd:
        return "ERROR: Simulator window not found. Is bridgecommand-bc.exe running?"

    key_lower = key.lower()
    vk = VK_MAP.get(key_lower)
    if vk is None:
        return f"ERROR: Unknown key '{key}'. Valid keys: {', '.join(sorted(VK_MAP.keys()))}"

    _bring_to_foreground(hwnd)

    if action == "tap":
        _send_key_input(vk, key_up=False)
        await asyncio.sleep(duration_ms / 1000.0)
        _send_key_input(vk, key_up=True)
        return f"Tapped '{key}' for {duration_ms}ms"
    elif action == "press":
        _send_key_input(vk, key_up=False)
        return f"Pressed '{key}' (held down)"
    elif action == "release":
        _send_key_input(vk, key_up=True)
        return f"Released '{key}'"
    else:
        return f"ERROR: Unknown action '{action}'. Use 'tap', 'press', or 'release'."


@mcp.tool()
async def send_key_combo(keys: str, duration_ms: int = 50) -> str:
    """Send a key combination (e.g. shift+w for fast movement).

    Args:
        keys: Plus-separated key names (e.g. 'shift+w', 'ctrl+s').
        duration_ms: How long to hold the combo in milliseconds.

    Returns:
        Status message.
    """
    hwnd = _find_simulator_window()
    if not hwnd:
        return "ERROR: Simulator window not found."

    key_names = [k.strip().lower() for k in keys.split("+")]
    vk_codes = []
    for k in key_names:
        vk = VK_MAP.get(k)
        if vk is None:
            return f"ERROR: Unknown key '{k}'."
        vk_codes.append(vk)

    _bring_to_foreground(hwnd)

    for vk in vk_codes:
        _send_key_input(vk, key_up=False)
        await asyncio.sleep(0.01)

    await asyncio.sleep(duration_ms / 1000.0)

    for vk in reversed(vk_codes):
        _send_key_input(vk, key_up=True)
        await asyncio.sleep(0.01)

    return f"Combo '{keys}' held for {duration_ms}ms"


@mcp.tool()
async def hold_key(key: str, duration_ms: int = 1000) -> str:
    """Hold a key down for a specified duration. Useful for continuous
    movement (e.g. hold 'w' for 2 seconds to walk forward).

    Args:
        key: Key name to hold.
        duration_ms: How long to hold in milliseconds (max 10000).

    Returns:
        Status message.
    """
    hwnd = _find_simulator_window()
    if not hwnd:
        return "ERROR: Simulator window not found."

    key_lower = key.lower()
    vk = VK_MAP.get(key_lower)
    if vk is None:
        return f"ERROR: Unknown key '{key}'."

    duration_ms = min(duration_ms, 10000)

    _bring_to_foreground(hwnd)
    _send_key_input(vk, key_up=False)
    await asyncio.sleep(duration_ms / 1000.0)
    _send_key_input(vk, key_up=True)

    return f"Held '{key}' for {duration_ms}ms"


@mcp.tool()
async def click_mouse(x: int, y: int, button: str = "left") -> str:
    """Click the mouse at window-relative coordinates.

    Args:
        x: X position relative to simulator window client area.
        y: Y position relative to simulator window client area.
        button: 'left' or 'right'.

    Returns:
        Status message.
    """
    hwnd = _find_simulator_window()
    if not hwnd:
        return "ERROR: Simulator window not found."

    wx, wy, ww, wh = _get_window_rect(hwnd)
    screen_x = wx + x
    screen_y = wy + y

    if x < 0 or x >= ww or y < 0 or y >= wh:
        return f"ERROR: Coordinates ({x},{y}) outside window ({ww}x{wh})."

    _bring_to_foreground(hwnd)

    if button == "left":
        _send_mouse_input(screen_x, screen_y, MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTDOWN)
        await asyncio.sleep(0.05)
        _send_mouse_input(screen_x, screen_y, MOUSEEVENTF_LEFTUP)
    elif button == "right":
        _send_mouse_input(screen_x, screen_y, MOUSEEVENTF_MOVE | MOUSEEVENTF_RIGHTDOWN)
        await asyncio.sleep(0.05)
        _send_mouse_input(screen_x, screen_y, MOUSEEVENTF_RIGHTUP)
    else:
        return f"ERROR: Unknown button '{button}'. Use 'left' or 'right'."

    return f"Clicked {button} at ({x},{y}) in simulator window"


@mcp.tool()
async def drag_mouse(
    from_x: int, from_y: int, to_x: int, to_y: int,
    button: str = "left", duration_ms: int = 300
) -> str:
    """Drag the mouse between two points in the simulator window.
    Useful for camera look/orbit (left drag) or other drag interactions.

    Args:
        from_x: Start X (window-relative).
        from_y: Start Y (window-relative).
        to_x: End X (window-relative).
        to_y: End Y (window-relative).
        button: 'left' or 'right'.
        duration_ms: Duration of the drag in milliseconds.

    Returns:
        Status message.
    """
    hwnd = _find_simulator_window()
    if not hwnd:
        return "ERROR: Simulator window not found."

    wx, wy, ww, wh = _get_window_rect(hwnd)

    _bring_to_foreground(hwnd)

    screen_from_x = wx + from_x
    screen_from_y = wy + from_y
    screen_to_x = wx + to_x
    screen_to_y = wy + to_y

    down_flag = MOUSEEVENTF_LEFTDOWN if button == "left" else MOUSEEVENTF_RIGHTDOWN
    up_flag = MOUSEEVENTF_LEFTUP if button == "left" else MOUSEEVENTF_RIGHTUP

    _send_mouse_input(screen_from_x, screen_from_y, MOUSEEVENTF_MOVE | down_flag)

    steps = max(5, duration_ms // 16)
    for i in range(1, steps + 1):
        t = i / steps
        cx = int(screen_from_x + (screen_to_x - screen_from_x) * t)
        cy = int(screen_from_y + (screen_to_y - screen_from_y) * t)
        _send_mouse_input(cx, cy, MOUSEEVENTF_MOVE)
        await asyncio.sleep(duration_ms / 1000.0 / steps)

    _send_mouse_input(screen_to_x, screen_to_y, up_flag)

    return f"Dragged {button} from ({from_x},{from_y}) to ({to_x},{to_y})"


@mcp.tool()
async def scroll_wheel(amount: int, x: int = -1, y: int = -1) -> str:
    """Scroll the mouse wheel in the simulator window.
    Positive = scroll up (zoom in), negative = scroll down (zoom out).

    Args:
        amount: Scroll amount (positive=up, negative=down). 120 = one notch.
        x: X position (window-relative). -1 = center of window.
        y: Y position (window-relative). -1 = center of window.

    Returns:
        Status message.
    """
    hwnd = _find_simulator_window()
    if not hwnd:
        return "ERROR: Simulator window not found."

    wx, wy, ww, wh = _get_window_rect(hwnd)

    if x < 0:
        x = ww // 2
    if y < 0:
        y = wh // 2

    screen_x = wx + x
    screen_y = wy + y

    _bring_to_foreground(hwnd)

    _send_mouse_input(screen_x, screen_y, MOUSEEVENTF_MOVE)
    await asyncio.sleep(0.02)

    inp = INPUT()
    inp.type = INPUT_MOUSE
    inp.union.mi.dx = int(screen_x * 65535 / user32.GetSystemMetrics(0))
    inp.union.mi.dy = int(screen_y * 65535 / user32.GetSystemMetrics(1))
    inp.union.mi.mouseData = ctypes.wintypes.DWORD(amount & 0xFFFFFFFF)
    inp.union.mi.dwFlags = MOUSEEVENTF_WHEEL | MOUSEEVENTF_ABSOLUTE
    inp.union.mi.time = 0
    inp.union.mi.dwExtraInfo = None
    ctypes.windll.user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))

    direction = "up" if amount > 0 else "down"
    return f"Scrolled {direction} by {abs(amount)} at ({x},{y})"


@mcp.tool()
async def get_window_info() -> str:
    """Get the simulator window position, size, and state.

    Returns:
        Window information string.
    """
    hwnd = _find_simulator_window()
    if not hwnd:
        return "ERROR: Simulator window not found. Is bridgecommand-bc.exe running?"

    wx, wy, ww, wh = _get_window_rect(hwnd)
    is_foreground = user32.GetForegroundWindow() == hwnd
    is_visible = user32.IsWindowVisible(hwnd)

    title_buf = ctypes.create_unicode_buffer(256)
    user32.GetWindowTextW(hwnd, title_buf, 256)

    return (
        f"Window: {title_buf.value}\n"
        f"Client area: {ww}x{wh} at screen ({wx},{wy})\n"
        f"Foreground: {is_foreground}\n"
        f"Visible: {is_visible}"
    )


if __name__ == "__main__":
    mcp.run(transport="stdio")
