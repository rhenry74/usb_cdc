# usb_cdc – Program Overview

This project runs on the ESP32‑S3 and combines **USB Serial JTAG throughput testing**, **high‑speed GPIO edge sampling**, and a **48 kHz PWM output**. It uses FreeRTOS tasks and GPIO interrupts to capture timing data and send buffered samples over USB Serial JTAG, while also providing a button‑triggered test burst and UART debug logs.

---

## Hardware/Pin Map
**Inputs:**
- **GPIO2, GPIO4, GPIO5, GPIO13**: High‑speed edge capture channels (interrupt on any edge).
- **GPIO17**: Index input (advances sample index, resets counter, flips buffers).
- **GPIO0**: Button input (active‑low; triggers test burst).

**Outputs:**
- **GPIO18**: 48 kHz PWM output (LEDC low‑speed mode, 50% duty).

> Note: GPIO12 is avoided because it can be a strapping pin on some ESP32‑S3 devkits.

---

## 1) Initialization (in `app_main()`)
1. **UART debug init** (`init_uart()`)
   - Sets up UART0 at 115200 baud for debug output.
2. **USB Serial JTAG init** (`init_usb_serial_jtag()`)
   - Installs the USB Serial JTAG driver for host communication.
3. **PWM init** (`init_pwm_48khz()`)
   - Configures LEDC low‑speed timer/channel to output **48 kHz** PWM at **50% duty** on **GPIO18**.

---

## 2) High‑Speed GPIO Sampling (Globals)
- **`counter`**: Fast‑incrementing time base used to timestamp edges.
- **`channel_buffers[2][NUM_CHANNELS][BUFFER_LEN]`**: Double‑buffered timestamp storage.
- **`buffer_index`**: Current sample index (0…`BUFFER_LEN-1`).
- **`active_buffer`**: Which buffer is currently being filled.
- **`paused`**: Temporarily disables sampling during test bursts.

### Buffering Model
Each channel interrupt stores a timestamp into the active buffer at `buffer_index`. The **index pin** increments `buffer_index`; when it wraps to 0, the inactive buffer is considered “complete” and sent over USB.

---

## 3) GPIO ISRs
- **`gpio_isr_handler`** (channel pins):
  - On any edge, stores the current `counter` value in the channel’s buffer slot.
- **`index_isr_handler`** (index pin):
  - Advances `buffer_index` and flips the active buffer on wrap.
  - Resets `counter` on each index edge to measure relative edge timing.
- **`button_isr_handler`** (button pin):
  - Increments a diagnostic counter and notifies the button task.

---

## 4) FreeRTOS Tasks
### `counter_task` (core 0)
- Tight loop incrementing `counter` (high‑resolution time base).

### `gpio_setup_task` (core 1)
- Configures GPIO input modes and attaches ISRs.
- Periodically polls the button level and logs changes.

### `buffer_monitor_task` (core 1)
- Detects buffer wrap events and pushes the completed buffer over USB Serial JTAG.

### `button_task` (core 1)
- On button press:
  1. Pauses sampling
  2. Sends ~64 KiB burst over USB Serial JTAG
  3. Resumes sampling

---

## 5) USB Serial JTAG Throughput Loop
Inside the main loop:
- Reads up to 64 bytes from USB Serial JTAG.
- If data arrives:
  - Echoes it back 100 times.
  - Measures elapsed time.
  - Logs throughput to UART.
- If no data:
  - Logs idle status and sleeps 1s.

---

## 6) PWM Output
A constant PWM is produced on **GPIO18**:
- **Frequency**: 48 kHz
- **Duty**: 50%
- **LEDC low‑speed mode**
- **Resolution**: 10‑bit (trade‑off between frequency and resolution)

---

## Data Format Sent Over USB
When a buffer completes, a single packed payload is sent:
- **Sync bytes**: `0x55 0xAA`
- Followed by **NUM_CHANNELS × BUFFER_LEN** samples
- Each sample is a **24‑bit timestamp** (3 bytes, little‑endian)

Payload length = `2 + NUM_CHANNELS * BUFFER_LEN * 3` bytes.

---

## Summary
✅ Outputs 48 kHz PWM on GPIO18
✅ Samples 4 GPIO inputs with ISR‑based timestamps
✅ Index pin gates buffer boundaries
✅ Sends buffers over USB Serial JTAG
✅ Button triggers USB burst test
✅ USB echo loop reports throughput
