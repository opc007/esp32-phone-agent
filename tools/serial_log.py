"""Reset the board and capture its serial log for N seconds."""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM9"
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0

ser = serial.Serial(PORT, 115200, timeout=1)
time.sleep(0.3)
ser.reset_input_buffer()
# Ctrl-C twice then Ctrl-D = soft reset into main.py
ser.write(b"\x03\x03")
time.sleep(0.3)
ser.write(b"\x04")
started = time.time()
out = bytearray()
while time.time() - started < SECONDS:
    chunk = ser.read(4096)
    if chunk:
        out.extend(chunk)
ser.close()
text = out.decode("utf-8", "replace")
print("===== SERIAL LOG =====")
print(text)
print("===== END (%d bytes) =====" % len(out))
