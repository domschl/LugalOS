"""Live canvas chart demo using lugal.canvas and lugal.sensor."""
import math
import time
import lugal


def main():
    lugal.canvas.open()
    w, h = lugal.canvas.size()
    sensors = lugal.sensor.list()
    history = []

    try:
        for step in range(120):
            if sensors:
                name = sensors[0]["name"] if isinstance(sensors[0], dict) else str(sensors[0])
                val = lugal.sensor.read(name)
                if isinstance(val, dict):
                    val = list(val.values())[0] if val else 0.0
                y_norm = max(0.0, min(1.0, (float(val) - 15.0) / 25.0))
            else:
                y_norm = 0.5 + 0.4 * math.sin(step * 0.18)

            history.append(y_norm)
            if len(history) > w - 20:
                history.pop(0)

            lugal.canvas.fill(0x0841)
            lugal.canvas.rect(10, 20, w - 20, h - 35, 0x18E3)
            lugal.canvas.text(12, 6, "LugalOS MicroPython Chart", 0xFFFF, 0x0841)

            for i in range(1, len(history)):
                x0 = 10 + i - 1
                x1 = 10 + i
                y0 = int(h - 20 - history[i - 1] * (h - 45))
                y1 = int(h - 20 - history[i] * (h - 45))
                lugal.canvas.line(x0, y0, x1, y1, 0x07FF)

            lugal.canvas.swap()
            time.sleep_ms(30)
    finally:
        lugal.canvas.close()


if __name__ == "__main__":
    main()
