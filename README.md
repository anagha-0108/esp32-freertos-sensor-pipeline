# Multi-Sensor Data Acquisition & Monitoring System (ESP32, FreeRTOS, UART)

No external sensors needed. Fake-but-realistic sensor values are generated
on-chip with `esp_random()`, so you can build, flash, and watch real
multi-task FreeRTOS behavior with nothing but the WROOM dev board and a
USB cable.

## Project layout
```
project/
├── CMakeLists.txt          (top-level)
└── main/
    ├── CMakeLists.txt
    └── main.c
```

## 1. One-time VS Code setup
1. Install the **"ESP-IDF"** extension from the VS Code marketplace (by Espressif).
2. Run command palette → `ESP-IDF: Configure ESP-IDF Extension` → choose
   **"Express"** → pick **release v5.2** (gives you 5.2.7) → let it download
   the toolchain. This takes a while the first time.
3. Open this `project/` folder in VS Code (`File > Open Folder`).

## 2. Set the target chip
Command palette → `ESP-IDF: Set Espressif Device Target` → choose `esp32`
(plain WROOM, not -S2/-S3/-C3).

## 3. Build
Command palette → `ESP-IDF: Build your project`
(or click the little "cylinder" build icon in the bottom status bar)

## 4. Flash
Plug in the board via USB.
Command palette → `ESP-IDF: Select Port to Use` → pick your COM/tty port.
Then → `ESP-IDF: Flash your device`.

## 5. Watch it run
Command palette → `ESP-IDF: Monitor your device`
You should see something like:
```
[SYS] Waiting for all sensors to come online...
[SYS] All sensors ready. Starting aggregation.
TEMP,28.40
HUMID,52.10
VIBRO,1.30
TEMP,31.90
HEARTBEAT,ALIVE
[FAULT] Temperature out of range!
TEMP,36.10
...
```
(Press `Ctrl+]` to exit the monitor.)

## What's actually happening (for your resume / interview talking points)

| Mechanism | Where | Why it's there |
|---|---|---|
| 3 separate Queues | `q_temp`, `q_humid`, `q_vibro` | Each sensor task is independent; no shared state to corrupt |
| Queue Set | `qset_sensors` | One consumer task (`AggregatorTask`) blocks on *all three* queues at once instead of polling each one — efficient, event-driven |
| Event Group (sync barrier) | `eg_ready` | Aggregator won't start consuming until all 3 sensors have announced readiness — avoids reading from an uninitialized pipeline |
| Task Notification | `fault_task_handle` | Out-of-range readings skip the queue entirely and go straight to `FaultTask` via a lightweight bitmask notification — cheaper than a queue for rare, urgent signals |
| Mutex | `mtx_console` | Protects `safe_print()` so System/Fault log lines never interleave mid-line |
| Software Timer | `heartbeat_timer` | Periodic 5s heartbeat runs independently of sensor cadence, proving the system is alive even with quiet sensors |
| Message Buffer | `msgbuf_uart` | Single serialization point before the only task allowed to write to the UART/stdout — avoids any peripheral race condition |
| Watchdog | `esp_task_wdt_add/reset` in `AggregatorTask` | If the aggregator ever deadlocks or stalls, the watchdog reboots the chip instead of silently hanging forever |

## Notes
Sensor data is simulated with `esp_random()` instead of real hardware
readings, and telemetry is written to the existing console UART (UART0)
rather than a dedicated peripheral. Both choices were made deliberately
to keep the project runnable on bare hardware with no extra components,
while still exercising the full set of FreeRTOS primitives above.
