"""測定用の同期USBクライアント。"""
import json
import time
import serial


class BoardClient:
    def __init__(self, port):
        self.serial = serial.Serial(port=None, baudrate=115200, timeout=0.2, write_timeout=2)
        self.serial.port = port
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.open()
        self.serial.reset_input_buffer()
        self.sequence = int(time.time()) % 1000000
        self.buffer = bytearray()

    def close(self):
        self.serial.close()

    def responses(self, command, expected=1, timeout=90, **fields):
        self.sequence += 1
        request = dict(id=self.sequence, cmd=command, **fields)
        self.serial.write(("\n" + json.dumps(request) + "\n").encode())
        deadline = time.monotonic() + timeout
        received = 0
        while time.monotonic() < deadline:
            self.buffer.extend(self.serial.read(max(1, min(self.serial.in_waiting, 65536))))
            if len(self.buffer) > 2_000_000:
                raise RuntimeError("USB line exceeds 2 MB")
            while b"\n" in self.buffer:
                line, _, rest = self.buffer.partition(b"\n")
                self.buffer = bytearray(rest)
                try:
                    reply = json.loads(line)
                except (ValueError, UnicodeDecodeError):
                    continue
                if not isinstance(reply, dict) or reply.get("id") != self.sequence:
                    continue
                yield reply
                received += 1
                if not reply.get("ok") or received >= expected:
                    return
        raise TimeoutError(f"{command}: received {received}/{expected}")

    def request(self, command, **fields):
        return next(self.responses(command, **fields))
