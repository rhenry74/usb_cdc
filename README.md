# usb_cdc – Program Overview

This project runs on the ESP32‑S3 and combines **USB Serial JTAG throughput testing**, **high‑speed GPIO edge sampling**, and a **48 kHz PWM output**. It uses FreeRTOS tasks and GPIO interrupts to capture timing data and send buffered samples over USB Serial JTAG, while also providing a button‑triggered test burst and UART debug logs.

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
- **`counter`**: Fast‑incrementing time base.
- **`channel_buffers[2][NUM_CHANNELS][BUFFER_LEN]`**: Double buffer storing timestamps.
- **`buffer_index`**: Current sample index.
- **`active_buffer`**: Which buffer is being filled.
- **`paused`**: Stops sampling during test bursts.

**Pins used**:
- **Channel inputs**: GPIO2, GPIO4, GPIO5, GPIO13
- **Index input**: GPIO17
- **Button input**: GPIO0
- **PWM output**: GPIO18

---

## 3) GPIO ISRs
- **`gpio_isr_handler`** (channel pins):
  - On any edge, store `counter` in the channel buffer.
- **`index_isr_handler`** (index pin):
  - Increments `buffer_index`.
  - Swaps buffers when wrapping to 0.
  - Resets `counter` each index edge.
- **`button_isr_handler`** (button pin):
  - Notifies the button task to run the test burst.

---

## 4) FreeRTOS Tasks
### `counter_task` (core 0)
- Tight loop incrementing `counter` (high‑resolution timing base).

### `gpio_setup_task` (core 1)
- Configures GPIOs and attaches ISRs.
- Polls button level changes and logs them.

### `buffer_monitor_task` (core 1)
- Detects buffer wrap and sends the completed buffer via USB Serial JTAG.

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

---

## Summary
✅ Outputs 48 kHz PWM on GPIO18
✅ Samples 4 GPIO inputs with ISR‑based timestamps
✅ Index pin gates buffer boundaries
✅ Sends buffers over USB Serial JTAG
✅ Button triggers USB burst test
✅ USB echo loop reports throughput
