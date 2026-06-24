<img width="800" height="600" alt="image" src="https://github.com/user-attachments/assets/99fad94b-808d-4054-b466-ca2f1cd697b9" />


The diagram illustrates **how a ramp‑based analog‑to‑digital conversion (ADC) method works** by converting signal amplitude into a **time measurement**.

Here’s what it is showing:

***

## 🔍 **Overall Concept**

A **ramp reference signal** (blue line) increases linearly over time.  
A sampled analog signal value is represented by a **horizontal threshold level**.

As the ramp rises, the ADC waits until the ramp intersects the sampled signal value.  
The **time it takes to reach that intersection** becomes the *digital representation* of the sample.

This is why this technique is sometimes called:

*   **Time‑to‑Digital Conversion (TDC)**
*   **Single‑Slope (Ramp) ADC**

***

## 📈 **What happens in the diagram**

### 1. **A ramp signal resets to zero and starts rising**

Each “sample window” begins with a reset (the left gray vertical line), then the ramp climbs at a constant rate.

### 2. **Different signal amplitudes intersect the ramp at different times**

Green dots labeled **S1**, **S2**, **S3** represent sampled analog voltages.

Because the ramp increases linearly:

*   A **lower amplitude** intersects **sooner**
*   A **higher amplitude** intersects **later**

Thus, time ↔ amplitude relationship is linear.

### 3. **The ADC measures the time to intersection**

Next to the diagram, the values:

*   S1 = 334
*   S2 = 662
*   S3 = 509

represent the *time count* (or digital output) when each sample intersected the ramp.

### 4. **One full ramp period = one sample**

The horizontal arrow labeled **“1 Sample”** shows that each ramp cycle provides one full ADC measurement.

***

## 🧠 **In simpler words**

The diagram shows an ADC that:

1.  Generates a rising reference signal (a ramp)
2.  Compares the ramp to a sampled analog voltage
3.  Converts that voltage into a **digital value** by measuring **how long** until the ramp reaches it

**Voltage → Time → Digital Count**

***

## **Hardware**

- Circuitry external of the ESP32-S3 includes a ramp generator and a comparator for each channel.
- Comparator outputs are tied to **MCPWM capture channel pins** on the MCU for precise hardware timestamping.
- The ESP32-S3 provides **6 MCPWM capture channels** across 2 hardware groups (3 channels per group).

***

# usb_cdc – Program Overview

This project runs on the ESP32‑S3 and combines **USB Serial JTAG throughput testing**, **precise MCPWM-based edge sampling**, and a **44.1 kHz PWM output**. It uses FreeRTOS tasks and hardware capture peripherals to obtain accurate timing data and send buffered samples over USB Serial JTAG, while also providing a button‑triggered test burst and UART debug logs.

---

## Hardware/Pin Map
**Inputs:**
- **GPIO2, GPIO4, GPIO5**: Group 0 MCPWM capture channels 0–2.
- **GPIO13, GPIO14, GPIO15**: Group 1 MCPWM capture channels 0–2.
- **GPIO17**: Index input (hardware-syncs both capture timers, advances sample index, flips buffers).
- **GPIO0**: Button input (active‑low; triggers test burst).

**Outputs:**
- **GPIO18**: 44.1 kHz PWM output (LEDC low‑speed mode, 50% duty, 9‑bit resolution).

> Note: GPIO12 is avoided because it can be a strapping pin on some ESP32‑S3 devkits.

---

## 1) Initialization (in `app_main()`)
1. **UART debug init** (`init_uart()`)
   - Sets up UART0 at 115200 baud for debug output.
2. **USB Serial JTAG init** (`init_usb_serial_jtag()`)
   - Installs the USB Serial JTAG driver for host communication.
3. **PWM init** (`init_pwm_44k1()`)
   - Configures LEDC low‑speed timer/channel to output **44.1 kHz** PWM at **10% duty** on **GPIO18** (9‑bit resolution).
4. **MCPWM Capture init** (`init_mcpwm_capture()`)
   - Configures both MCPWM capture timers (Group 0 & Group 1) to handle **all 6 capture channels**.
   - Sets up hardware synchronization to reset both timers on the **INDEX** pin edge.

---

## 2) High‑Speed MCPWM Capture (Globals)
- **`channel_buffers[2][NUM_CHANNELS][BUFFER_LEN]`** (6 channels × 16 samples, double‑buffered): Timestamp storage.
- **`buffer_index`**: Current sample index (0…`BUFFER_LEN-1`).
- **`active_buffer`**: Which buffer is currently being filled.
- **`paused`**: Temporarily disables sampling during test bursts.

### Architecture
The system uses the ESP32-S3's **MCPWM Capture** peripheral for precise edge timing, replacing software ISR jitter with hardware latches.
- **Capture Timers**: Two MCPWM capture timers (Group 0 & 1) run continuously at ~89 MHz.
- **Synchronization**: The **INDEX_PIN** triggers a hardware sync that resets both timers to 0, ensuring all timestamps are relative to the ramp start.
- **Capture Channels**: 6 comparator outputs are routed to MCPWM capture channels (3 on Group 0, 3 on Group 1). On a negative edge, the hardware latches the timer value into a register and triggers a lightweight callback (`cap_cb`) to store it in the buffer.

---

## 3) Interrupts & Callbacks
- **`cap_cb`** (MCPWM Capture Callback):
  - Triggered by hardware when a comparator edge is detected.
  - Reads the hardware-latched timestamp (`cap_value`) and stores it in the active buffer.
- **`index_isr_handler`** (Index Pin ISR):
  - Advances `buffer_index`.
  - Swaps buffers when a full set of 16 samples is collected.
  - Note: Timing reset is handled by MCPWM hardware sync, not this ISR.
- **`button_isr_handler`** (Button Pin):
  - Increments a diagnostic counter and notifies the button task.

---

## 4) FreeRTOS Tasks
### (No counter task)
- Timing is derived from **hardware capture timers**, so no software counter task is needed.

### `burst_task` (core 0)
- Dedicated burst executor with a 4096-byte stack to avoid overflow.
- Owns **capture pause/resume** while a burst is running.
- Handles **drop‑then‑queue** behavior:
  - If a burst is active, one request is queued.
  - Additional overlapping requests are dropped until the queued burst runs.

### `button_task` (core 0)
- On button press:
  1. Queues a **1 kHz sine wave burst** (generated mathematically) for **5 seconds**.
  2. Burst execution (pause/resume + send) is handled by `burst_task`.

### `gpio_setup_task` (core 0)
- Configures GPIO input modes and attaches ISRs/callbacks.
- Periodically polls the button level and logs changes.
- Enables button interrupts after a 2-second boot delay to avoid spurious triggers.

### `buffer_monitor_task` (core 1)
- Waits for buffer swap notification from `index_isr_handler`.
- Packs all 6 channels of captured data and sends over USB Serial JTAG.
- Logs throughput statistics (kB/s, packet counts, index trigger deltas) every 3 seconds.

---

## 5) UART Command Loop
Inside `app_main()` (core 0):
- Reads bytes from UART0.
- Buffers a line until `\r` or `\n`.
- Parses commands:
  - `TONE <freq_hz> <duration_ms>`
    - Example: `TONE 1000 5000`
    - Requests a burst from `burst_task` (shared tone generator).
    - Capture pause/resume is handled inside `burst_task`.

---

## 6) PWM Output
A constant PWM is produced on **GPIO18**:
- **Frequency**: 44.1 kHz
- **Duty**: 10% (configurable via `PWM_DUTY_PERCENT`)
- **LEDC low‑speed mode**
- **Resolution**: 9‑bit (trade‑off between frequency and resolution)

---

## 7) Capture‑Miss Correction
When a buffer slot contains zero (no capture event for that sample), the `send_buffers_over_usb()` function fills it with:
- The **next** value if at the start of the buffer.
- The **previous** value if at the end of the buffer.
- The **average** of previous and next values otherwise.

This corrects for missed captures that can occur during USB sends or due to timing.

---

## Data Format Sent Over USB
When a buffer completes, a single packed payload is sent:
- **Sync bytes**: `0x55 0xAA`
- Followed by **NUM_CHANNELS × BUFFER_LEN** samples (6 × 16 = 96 samples)
- Each sample is a **16‑bit timestamp** (2 bytes, little‑endian) — raw capture timer counts scaled by ×36 to fit the 16-bit range.

Payload length = `2 + NUM_CHANNELS * BUFFER_LEN * 2` bytes.

With `NUM_CHANNELS=6` and `BUFFER_LEN=16`, each packet is **194 bytes**.

---

## Summary
| Feature | Status |
|---------|--------|
| PWM output (44.1 kHz on GPIO18) | ✅ |
| 6-channel MCPWM hardware capture (all ESP32-S3 capture channels) | ✅ |
| 2 MCPWM capture timer groups with INDEX hardware sync | ✅ |
| Sample rate locked to 44.1 kHz (index pin driven by ramp reset) | ✅ |
| Double-buffered capture (one fills, one sends) | ✅ |
| USB Serial JTAG data output (194-byte packets) | ✅ |
| Button-triggered 1 kHz sine wave test burst | ✅ |
| UART command loop for arbitrary test tones (`TONE <freq> <dur>`) | ✅ |
| Capture-miss correction (interpolation of zero-value slots) | ✅ |
| Throughput statistics reporting (every 3 seconds via UART) | ✅ |

## Data Flow Summary
```
External Comparators (6 ch)
        ↓  (falling edges on GPIO2,4,5,13,14,15)
MCPWM Capture Hardware (Group 0 & 1 timers @ ~89 MHz)
        ↓  (hardware-latched timestamps)
cap_cb() → channel_buffers[active_buffer][ch][idx]
        ↓  (buffer wrap at 16 samples)
index_isr_handler() → swap buffer → notify buffer_monitor_task
        ↓
send_buffers_over_usb() → [0x55 0xAA] [6ch × 16 samples × 2 bytes]
        ↓
USB Serial JTAG → Host PC