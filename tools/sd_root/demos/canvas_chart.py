"""Live canvas chart demo using lugal.canvas and lugal.sensor."""
import math
import time
import lugal


def main():
    lugal.canvas.open()
    w, h = lugal.canvas.size()
    if w < 40 or h < 40:
        lugal.canvas.close()
        print("canvas unavailable")
        return
    sensors = lugal.sensor.list()
    history = []
    max_pts = 120
    dx = max(1, (w - 24) // max_pts)

    try:
        lugal.canvas.fill(0x0841)
        lugal.canvas.frame(10, 20, w - 20, h - 35, 0x4A69)
        lugal.canvas.text(12, 6, "LugalOS MicroPython Chart", 0xFFFF)
        lugal.canvas.swap()

        for step in range(max_pts):
            if sensors:
                name = sensors[0]["name"] if isinstance(sensors[0], dict) else str(sensors[0])
                val = lugal.sensor.read(name)
                if isinstance(val, dict):
                    val = list(val.values())[0] if val else 0.0
                base = max(0.2, min(0.8, (float(val) - 15.0) / 25.0))
                y_norm = base + 0.15 * math.sin(step * 0.18)
            else:
                y_norm = 0.5 + 0.4 * math.sin(step * 0.18)

            history.append(y_norm)
            n = len(history)
            if n >= 2:
                x0 = 12 + (n - 2) * dx
                x1 = 12 + (n - 1) * dx
                y0 = int(h - 20 - history[n - 2] * (h - 45))
                y1 = int(h - 20 - history[n - 1] * (h - 45))
                lugal.canvas.line(x0, y0, x1, y1, 0x07FF)
                lugal.canvas.swap()

            time.sleep_ms(30)
    finally:
        lugal.canvas.close()


if __name__ == "__main__":
    main()
