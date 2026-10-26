"""Read the URLs of the tabs open in a Chromium-based browser.

Standard library only. Chromium browsers (Opera GX, Chrome, Brave, Edge, ...)
log the session to <profile>/Sessions/Session_<n> in the "SNSS" format: a
header, then length-prefixed commands. Replaying the commands gives exactly
the tabs that are open (or were open when the browser last quit), each at its
current page: no back/forward history and no closed tabs. The browser stays
untouched; nothing needs to be installed or relaunched.

Windows is the exception: a running browser locks its live session file, so
only an older snapshot can be read (see read_tabs). The page asks the ytmerge
tabs extension (extension/ in the repo) first, which reads tabs live.
"""

from __future__ import annotations

import os
import re
import struct
import sys
from pathlib import Path

# Session command ids, from Chromium's session_service_commands.cc.
CMD_SET_TAB_WINDOW = 0
CMD_PRUNED_FROM_BACK = 5
CMD_UPDATE_NAVIGATION = 6
CMD_SELECTED_NAVIGATION = 7
CMD_TAB_CLOSED = 16
CMD_WINDOW_CLOSED = 17
CMD_NAVIGATION_PATH_PRUNED = 24

URL_RE = re.compile(rb"https?://[\x21-\x7e]+")


def _profile_roots() -> dict[str, list[Path]]:
    """Browser name -> user-data dirs to search, for this platform."""
    home = Path.home()
    if sys.platform == "darwin":
        sup = home / "Library" / "Application Support"
        return {
            "opera-gx": [sup / "com.operasoftware.OperaGX"],
            "opera": [sup / "com.operasoftware.Opera"],
            "chrome": [sup / "Google" / "Chrome"],
            "brave": [sup / "BraveSoftware" / "Brave-Browser"],
            "edge": [sup / "Microsoft Edge"],
            "vivaldi": [sup / "Vivaldi"],
        }
    if sys.platform.startswith("win"):
        roaming = Path(os.environ.get("APPDATA", home / "AppData" / "Roaming"))
        local = Path(os.environ.get("LOCALAPPDATA", home / "AppData" / "Local"))
        return {
            "opera-gx": [roaming / "Opera Software" / "Opera GX Stable"],
            "opera": [roaming / "Opera Software" / "Opera Stable"],
            "chrome": [local / "Google" / "Chrome" / "User Data"],
            "brave": [local / "BraveSoftware" / "Brave-Browser" / "User Data"],
            "edge": [local / "Microsoft" / "Edge" / "User Data"],
            "vivaldi": [local / "Vivaldi" / "User Data"],
        }
    cfg = Path(os.environ.get("XDG_CONFIG_HOME", home / ".config"))
    return {
        "opera-gx": [],  # no Linux build
        "opera": [cfg / "opera"],
        "chrome": [cfg / "google-chrome", cfg / "chromium"],
        "brave": [cfg / "BraveSoftware" / "Brave-Browser"],
        "edge": [cfg / "microsoft-edge"],
        "vivaldi": [cfg / "vivaldi"],
    }


BROWSERS = ["opera-gx", "opera", "chrome", "brave", "edge", "vivaldi"]


def session_files(browser: str) -> list[Path]:
    """Every Session_* file for a browser, across profiles, newest first.
    Opera keeps its profile in the root (Windows) or Default/ (macOS);
    Chrome-likes use Default/ and "Profile N"/."""
    found: list[Path] = []
    for root in _profile_roots().get(browser, []):
        for sessions in [root / "Sessions", *root.glob("*/Sessions")]:
            found += [p for p in sessions.glob("Session_*") if p.is_file()]
    return sorted(found, key=lambda p: p.stat().st_mtime, reverse=True)


def latest_session(browser: str = "auto") -> tuple[str, Path] | None:
    """(browser, newest session file). "auto" picks whichever browser was
    used most recently."""
    names = BROWSERS if browser == "auto" else [browser]
    best: tuple[str, Path] | None = None
    for name in names:
        files = session_files(name)
        if files and (best is None or files[0].stat().st_mtime > best[1].stat().st_mtime):
            best = (name, files[0])
    return best


def _pickle_string(p: bytes, off: int) -> str:
    (n,) = struct.unpack_from("<i", p, off)
    if n < 0 or off + 4 + n > len(p):
        raise ValueError("bad string")
    return p[off + 4:off + 4 + n].decode("utf-8", "replace")


def parse_snss(data: bytes) -> list[str]:
    """Replay an SNSS session log -> the current URL of every open tab,
    in the order the tabs first appear. Raises ValueError if it isn't one."""
    if data[:4] != b"SNSS":
        raise ValueError("not an SNSS session file")
    tabs: dict[int, dict] = {}
    closed_tabs: set[int] = set()
    closed_windows: set[int] = set()

    def tab(tid: int) -> dict:
        return tabs.setdefault(tid, {"window": None, "navs": {}, "selected": None})

    pos = 8  # "SNSS" + int32 version
    while pos + 2 <= len(data):
        (size,) = struct.unpack_from("<H", data, pos)
        pos += 2
        if size == 0 or pos + size > len(data):
            break  # truncated tail: the browser was mid-write
        cid, p = data[pos], data[pos + 1:pos + size]
        pos += size
        try:
            if cid == CMD_SET_TAB_WINDOW:
                window, tid = struct.unpack_from("<ii", p)
                tab(tid)["window"] = window
            elif cid == CMD_UPDATE_NAVIGATION:
                # A base::Pickle: uint32 payload size, tab id, nav index, url, ...
                tid, index = struct.unpack_from("<ii", p, 4)
                tab(tid)["navs"][index] = _pickle_string(p, 12)
            elif cid == CMD_SELECTED_NAVIGATION:
                tid, index = struct.unpack_from("<ii", p)
                tab(tid)["selected"] = index
            elif cid == CMD_PRUNED_FROM_BACK:
                tid, index = struct.unpack_from("<ii", p)
                navs = tab(tid)["navs"]
                for k in [k for k in navs if k >= index]:
                    del navs[k]
            elif cid == CMD_NAVIGATION_PATH_PRUNED:
                tid, index, count = struct.unpack_from("<iii", p)
                navs = tab(tid)["navs"]
                tab(tid)["navs"] = {(k if k < index else k - count): v
                                    for k, v in navs.items() if not index <= k < index + count}
            elif cid == CMD_TAB_CLOSED:
                closed_tabs.add(struct.unpack_from("<i", p)[0])
            elif cid == CMD_WINDOW_CLOSED:
                closed_windows.add(struct.unpack_from("<i", p)[0])
        except (struct.error, ValueError):
            continue  # one malformed command shouldn't lose the rest
    urls = []
    for tid, t in tabs.items():
        if tid in closed_tabs or t["window"] in closed_windows or not t["navs"]:
            continue
        sel = t["selected"]
        urls.append(t["navs"].get(sel) if sel in t["navs"] else t["navs"][max(t["navs"])])
    return urls


def _tab_urls(data: bytes) -> list[str]:
    try:
        return parse_snss(data)
    except ValueError:
        # Unknown format: every URL in the file, which can include history.
        return [m.group().decode() for m in URL_RE.finditer(data)]


def read_tabs(path: Path) -> tuple[list[str], float | None]:
    """(open tab URLs, snapshot time) from the session file at `path`.

    On Windows a running Chromium browser keeps its live session file locked
    against all readers. The browser starts a new file every so often and
    unlocks the previous one, so we fall back to the newest readable file in
    the same folder and return its time (epoch seconds) so the caller can say
    how old the list is. The time is None when the live file was read."""
    older = sorted((p for p in path.parent.glob("Session_*") if p != path and p.is_file()),
                   key=lambda p: p.stat().st_mtime, reverse=True)
    for candidate in [path, *older]:
        try:
            data = candidate.read_bytes()
        except PermissionError:
            continue
        return _tab_urls(data), (None if candidate == path else candidate.stat().st_mtime)
    raise LookupError("the browser has its session file locked (Windows does this while it's "
                      "open) and there's no older snapshot yet; the ytmerge tabs extension "
                      "reads open tabs live")


def open_tab_urls(browser: str = "auto") -> tuple[str, list[str], float | None]:
    """(browser used, URLs of its open tabs, snapshot time or None if live).
    Raises LookupError when no readable session file is found."""
    hit = latest_session(browser)
    if hit is None:
        which = "any supported browser" if browser == "auto" else browser
        raise LookupError(f"no open-tab session found for {which} "
                          f"(supported: {', '.join(BROWSERS)})")
    name, path = hit
    urls, as_of = read_tabs(path)
    return name, urls, as_of
