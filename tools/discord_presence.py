"""Discord Rich Presence for the launcher: "Playing God of War: Chains of Olympus", with the time played.

Talks to the Discord desktop app over its local IPC pipe (\\\\.\\pipe\\discord-ipc-N), standard library only. A
frame is op (int32 LE), length (int32 LE), then JSON: op 0 is the handshake, op 1 a command. Nothing goes over the
network from here; Discord shows the activity to the player's friends according to Discord's own settings. When
Discord isn't running, nothing happens. Text only: no game art (the launcher never ships any).
"""
import json
import os
import struct
import threading
import time
import uuid

APP_ID = "1563951390202662953"  # the project's Discord application
OP_HANDSHAKE, OP_FRAME, OP_CLOSE = 0, 1, 2


def encode(op, payload):
    data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    return struct.pack("<II", op, len(data)) + data


def read_frame(f):
    """(op, payload) of the next frame from a file-like pipe."""
    head = f.read(8)
    if len(head) < 8:
        raise OSError("Discord closed the pipe")
    op, n = struct.unpack("<II", head)
    return op, json.loads(f.read(n).decode("utf-8") or "null")


class Presence:
    def __init__(self, app_id=APP_ID, opener=None):
        self.app_id, self.f = app_id, None
        self.opener = opener or self._open_pipe

    @staticmethod
    def _open_pipe():
        for i in range(10):
            try:
                return open(rf"\\.\pipe\discord-ipc-{i}", "r+b", buffering=0)
            except OSError:
                continue
        return None

    def connect(self):
        """True when Discord answered the handshake with READY."""
        self.f = self.opener()
        if not self.f:
            return False
        self.f.write(encode(OP_HANDSHAKE, {"v": 1, "client_id": self.app_id}))
        op, msg = read_frame(self.f)
        if op != OP_FRAME or (msg or {}).get("evt") != "READY":
            self.close()
            return False
        return True

    def set_activity(self, activity):
        """Shows activity (a dict; None clears it); returns Discord's answer."""
        self.f.write(encode(OP_FRAME, {"cmd": "SET_ACTIVITY", "args": {"pid": os.getpid(), "activity": activity},
                                       "nonce": str(uuid.uuid4())}))
        return read_frame(self.f)[1]

    def close(self):
        if self.f:
            try:
                self.f.write(encode(OP_CLOSE, {}))
                self.f.close()
            except OSError:
                pass
        self.f = None


def activity_for(title, started):
    return {"details": title, "state": "Native PC port (unofficial)", "timestamps": {"start": int(started)}}


def show_while_running(proc, title, log=print, app_id=APP_ID):
    """In a background thread: shows "Playing <title>" on Discord until proc exits. Never raises."""
    def run():
        p = Presence(app_id)
        try:
            if not p.connect():
                return
            p.set_activity(activity_for(title, time.time()))
            log(f"discord: showing {title}")
            proc.wait()
            p.set_activity(None)
        except (OSError, ValueError) as e:
            log(f"discord: {e}")
        finally:
            p.close()
    t = threading.Thread(target=run, daemon=True, name="discord-presence")
    t.start()
    return t
