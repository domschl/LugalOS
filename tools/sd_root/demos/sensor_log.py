"""Async sensor logger demo using uasyncio, lugal.sensor, and json."""
import json
import os
import time
import uasyncio
import lugal


def _log_path():
    try:
        os.stat("/nvme0")
        return "/nvme0/sensor_log.jsonl"
    except OSError:
        return "/sd0/demos/sensor_log.jsonl"


async def sample_loop(count=5, interval_ms=500):
    sensors = lugal.sensor.list()
    path = _log_path()
    print("Logging", len(sensors), "sensor(s) to", path)

    with open(path, "w") as f:
        for i in range(count):
            entry = {"t_ms": time.ticks_ms(), "seq": i, "readings": {}}
            if sensors:
                for s in sensors:
                    name = s["name"] if isinstance(s, dict) else str(s)
                    entry["readings"][name] = lugal.sensor.read(name)
            else:
                entry["readings"]["sim_temp_c"] = 22.5 + 0.1 * i
            line = json.dumps(entry)
            print(line)
            f.write(line + "\n")
            await uasyncio.sleep_ms(interval_ms)


def main():
    uasyncio.run(sample_loop())


if __name__ == "__main__":
    main()
