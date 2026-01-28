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
write_to_file(
    path="README.md",
    content="""<img width="800" height="600" alt="image" src="https://github.com/user-attachments/assets/99fad94b-808d-4054-b466-ca2f1cd697b9" />


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
- Circuitry external of the ESP32 includes a ramp generator and a comparator for each channel.
- Comparator outputs are tied to **MCPWM capture channel pins** on the MCU for precise hardware timestamping.

***

# usb_cdc – Program Overview

This project runs on the ESP32‑S3 and combines **USB Serial JTAG throughput testing**, **precise MCPWM-based edge sampling**, and a **48 kHz PWM output**. It uses FreeRTOS tasks and hardware capture peripherals to obtain accurate timing data and send buffered samples over USB Serial JTAG, while also providing a button‑triggered test burst and UART debug logs.

---

## Hardware/Pin Map
**Inputs:**
- **GPIO2, GPIO4, GPIO5, GPIO13**: High‑speed comparator inputs (routed to MCPWM capture channels).
- **GPIO17**: Index input (syncs capture timers, advances sample index, flips buffers).
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
4. **MCPWM Capture init** (`init_mcpwm_capture()`)
   - Configures two MCPWM capture timers (one per group) to handle 4 channels.
   - Sets up hardware synchronization to reset timers on the **INDEX** pin edge.

---

## 2) High‑Speed MCPWM Capture (Globals)
- **`channel_buffers[2][NUM_CHANNELS][BUFFER_LEN]`**: Double‑buffered timestamp storage.
- **`buffer_index`**: Current sample index (0…`BUFFER_LEN-1`).
- **`active_buffer`**: Which buffer is currently being filled.
- **`paused`**: Temporarily disables sampling during test bursts.

### Architecture
The system uses the ESP32-S3's **MCPWM Capture** peripheral for precise edge timing, replacing software ISR jitter with hardware latches.
- **Capture Timers**: Two MCPWM capture timers (Group 0 & 1) run continuously.
- **Synchronization**: The **INDEX_PIN** triggers a hardware sync that resets both timers to 0, ensuring all timestamps are relative to the ramp start.
- **Capture Channels**: Comparator outputs are routed to MCPWM capture channels. On a negative edge, the hardware latches the timer value into a register and triggers a lightweight callback (`cap_cb`) to store it in the buffer.

---

## 3) Interrupts & Callbacks
- **`cap_cb`** (MCPWM Capture Callback):
  - Triggered by hardware when a comparator edge is detected.
  - Reads the hardware-latched timestamp (`cap_value`) and stores it in the active buffer.
- **`index_isr_handler`** (Index Pin ISR):
  - Advances `buffer_index`.
  - Swaps buffers when a full set of samples is collected.
  - Note: Timing reset is handled by MCPWM hardware sync, not this ISR.
- **`button_isr_handler`** (Button Pin):
  - Increments a diagnostic counter and notifies the button task.

---

## 4) FreeRTOS Tasks
### (No counter task)
- Timing is derived from **hardware capture timers**, so no software counter task is needed.

### `gpio_setup_task` (core 1)
- Configures GPIO input modes and attaches ISRs/callbacks.
- Periodically polls the button level and logs changes.

### `buffer_monitor_task` (core 1)
- Detects buffer wrap events and pushes the completed buffer over USB Serial JTAG.

### `button_task` (core 1)
- On button press:
  1. Pauses sampling.
  2. Sends a **1 kHz sine wave burst** (generated mathematically) over USB Serial JTAG for ~5 seconds.
  3. Resumes sampling.

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
✅ Samples 4 comparator inputs using **MCPWM hardware capture**
✅ **Hardware sync** on Index pin for jitter-free reference
✅ Sends buffers over USB Serial JTAG
✅ Button triggers **1 kHz sine wave test burst**
✅ USB echo loop reports throughput