import time

from platformio.device.monitor.filters.base import DeviceMonitorFilterBase

# The ESP32-C3 can't tell when the serial port is opened, so lines printed
# while nobody was reading sit in its USB buffer and arrive in one burst
# as soon as the monitor connects. Drop that burst and show only new lines.
STALE_WINDOW = 0.5


class Fresh(DeviceMonitorFilterBase):
    NAME = "fresh"

    def __call__(self):
        self.started = time.monotonic()
        return self

    def rx(self, text):
        if time.monotonic() - self.started < STALE_WINDOW:
            return ""
        return text
