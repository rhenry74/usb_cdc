#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include <cstring>    // C++ header for strlen
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "DUAL_USB";

// Configure UART0 (bridge port) for debug logs
void init_uart() {
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_0, &uart_config);
    uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

void init_usb_serial_jtag(void) {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %s", esp_err_to_name(err));
    }
}

// Logs a null‑terminated C string to UART0 safely.
static inline void log_uart(const char *msg) {
    if (!msg) {
        return;  // avoid null pointer crash
    }

    size_t len = strlen(msg);   // safe because msg is a proper C‑string
    if (len == 0) {
        return;
    }

    uart_write_bytes(UART_NUM_0, msg, len);
}

// -------------------- High-speed GPIO sampling --------------------

#define NUM_CHANNELS 4
#define BUFFER_LEN 8
#define INDEX_PIN 17 // GPIO17
#define BUTTON_PIN 0  // BOOT button; change if your board uses a different button/pin
#define PWM_PIN 18   // GPIO18 (free pin for 48 kHz PWM output)

#define PWM_LEDC_TIMER LEDC_TIMER_0
#define PWM_LEDC_CHANNEL LEDC_CHANNEL_0
#define PWM_LEDC_SPEED_MODE LEDC_LOW_SPEED_MODE
#define PWM_FREQ_HZ 48000
#define PWM_DUTY_RES LEDC_TIMER_10_BIT

static void init_pwm_48khz(void) {
    ledc_timer_config_t ledc_timer = {};
    ledc_timer.speed_mode = PWM_LEDC_SPEED_MODE;
    ledc_timer.duty_resolution = PWM_DUTY_RES;
    ledc_timer.timer_num = PWM_LEDC_TIMER;
    ledc_timer.freq_hz = PWM_FREQ_HZ;
    ledc_timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {};
    ledc_channel.speed_mode = PWM_LEDC_SPEED_MODE;
    ledc_channel.channel = PWM_LEDC_CHANNEL;
    ledc_channel.timer_sel = PWM_LEDC_TIMER;
    ledc_channel.intr_type = LEDC_INTR_DISABLE;
    ledc_channel.gpio_num = PWM_PIN;
    ledc_channel.duty = (1 << PWM_DUTY_RES) / 2; // 50% duty
    ledc_channel.hpoint = 0;
    ledc_channel_config(&ledc_channel);
}

static volatile uint32_t counter = 0;
static volatile uint32_t channel_buffers[2][NUM_CHANNELS][BUFFER_LEN];
static volatile uint32_t buffer_index = 0;
static volatile uint8_t active_buffer = 0; // 0 or 1

// Control flag to pause ISR-driven sampling while test burst runs
static volatile bool paused = false;

// Debug counter incremented from ISR when button pressed (diagnostic only)
static volatile uint32_t button_press_count = 0;

// Task handle for button handler (signalled from ISR)
static TaskHandle_t buttonTaskHandle = nullptr;

// Forward declarations
static void counter_task(void* pv);
static void gpio_setup_task(void* pv);
static void buffer_monitor_task(void* pv);
static void button_task(void* pv);

static void debug_print(const char* msg) {
    if (!msg) {
        return;
    }
    log_uart(msg);
    log_uart("\r\n");
}

// Fast counter task (core 0)
static void counter_task(void *pv) {
    (void) pv;
    while (true) {
        counter++;
        // No delay: tight loop for high resolution
    }
}

// ISR for GPIO interrupts
static void IRAM_ATTR gpio_isr_handler(void* arg) {
    if (paused) {
        return; // ignore samples while paused
    }
    int channel = (int)(uintptr_t)arg;
    uint32_t idx = buffer_index % BUFFER_LEN;
    channel_buffers[active_buffer][channel][idx] = counter;
}

// ISR for incrementing buffer_index
static void IRAM_ATTR index_isr_handler(void* arg) {
    (void)arg;
    if (paused) {
        return; // don't advance buffer index while paused
    }
    buffer_index = (buffer_index + 1) % BUFFER_LEN;
    if (buffer_index == 0) {
        // Swap buffers when a full set is collected
        active_buffer ^= 1;
    }
    counter = 0; // Reset counter on index increment
}

// ISR for button press: notify the button handling task (keep ISR short)
static void IRAM_ATTR button_isr_handler(void* arg) {
    (void)arg;
    button_press_count++;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(buttonTaskHandle, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static void send_test_burst(void) {
    debug_print("Starting test burst (~64KiB) over USB Serial JTAG");

    const int CHUNK = 4096;
    static uint8_t burst[CHUNK];
    for (int i = 0; i < CHUNK; i++) {
        burst[i] = (uint8_t)(i & 0xFF);
    }
    debug_print("burst array filled");

    const int CHUNKS = 16; // ~64KiB total
    for (int k = 0; k < CHUNKS; k++) {
        usb_serial_jtag_write_bytes(burst, CHUNK, 20 / portTICK_PERIOD_MS);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    debug_print("send_test_burst: all chunks sent");
}

// Task that handles button press: pause sampling, send messages & test burst, resume
static void button_task(void* pv) {
    (void) pv;
    debug_print("button_task started");
    for (;;) {
        // Wait indefinitely until ISR notifies us
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // Immediately report notification and ISR counter (safe in task context)
        {
            char buf[96];
            snprintf(buf, sizeof(buf), "button_task: notified (ISR count=%u)", (unsigned)button_press_count);
            debug_print(buf);
        }
        // Debounce grace period
        vTaskDelay(pdMS_TO_TICKS(20));

        // Pause sampling
        paused = true;
        debug_print("Sampling paused - starting test burst");

        // Send test data
        send_test_burst();

        debug_print("Test burst complete - resuming sampling");

        // Prevent bouncing / repeated triggers
        vTaskDelay(pdMS_TO_TICKS(500));

        // Resume sampling
        paused = false;
    }
}

// Pins to use for interrupts (change as needed)
// NOTE: GPIO12 can be a strapping/boot pin on some devkits; use GPIO13 instead on ESP32-S3 DevKitC-1-N16R8
static const gpio_num_t channel_pins[NUM_CHANNELS] = {
    GPIO_NUM_2, GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_13
};

// Task to configure GPIO interrupts (core 1)
static void gpio_setup_task(void *pv) {
    (void) pv;
    debug_print("gpio_setup_task started");

    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = 0;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_ANYEDGE;

    // install ISR service once
    gpio_install_isr_service(0);

    for (int i = 0; i < NUM_CHANNELS; ++i) {
        io_conf.pin_bit_mask = (1ULL << channel_pins[i]);
        gpio_config(&io_conf);
        gpio_isr_handler_add(channel_pins[i], gpio_isr_handler, (void*)(uintptr_t)i);
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "ISR attached: channel %d -> pin %d", i, (int)channel_pins[i]);
            debug_print(buf);
        }
    }

    // Configure the index increment pin
    io_conf.pin_bit_mask = (1ULL << INDEX_PIN);
    gpio_config(&io_conf);
    gpio_isr_handler_add((gpio_num_t)INDEX_PIN, index_isr_handler, nullptr);
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "Index pin configured: %d", (int)INDEX_PIN);
        debug_print(buf);
    }

    // Configure the user button pin (press to run test burst)
    io_conf.intr_type = GPIO_INTR_NEGEDGE; // button pulls pin low when pressed
    io_conf.pin_bit_mask = (1ULL << BUTTON_PIN);
    gpio_config(&io_conf);
    gpio_isr_handler_add((gpio_num_t)BUTTON_PIN, button_isr_handler, nullptr);
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "Button pin configured: %d", (int)BUTTON_PIN);
        debug_print(buf);
    }
    // restore intr type for other pins (if we need it later)
    io_conf.intr_type = GPIO_INTR_ANYEDGE;

    // Loop: poll the button level periodically to detect changes and show ISR counter
    int prev_level = 1; // pulled-up by default
    while (true) {
        int level = gpio_get_level((gpio_num_t)BUTTON_PIN);
        if (level != prev_level) {
            prev_level = level;
            char buf[96];
            snprintf(buf, sizeof(buf), "Button level changed: %d (ISR count=%u)", level, (unsigned)button_press_count);
            debug_print(buf);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void send_buffers_over_usb(uint8_t send_buffer) {
    // Pack sync + all channels into a single buffer and send once (better throughput)
    const size_t payload_len = 2 + (size_t)NUM_CHANNELS * BUFFER_LEN * 3;
    uint8_t tx_buf[2 + NUM_CHANNELS * BUFFER_LEN * 3];
    tx_buf[0] = 0x55;
    tx_buf[1] = 0xAA;

    size_t off = 2;
    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        for (int i = 0; i < BUFFER_LEN; ++i) {
            uint32_t sample = channel_buffers[send_buffer][ch][i];
            tx_buf[off++] = (sample >> 0) & 0xFF;
            tx_buf[off++] = (sample >> 8) & 0xFF;
            tx_buf[off++] = (sample >> 16) & 0xFF;
        }
    }

    // {
    //     char buf[64];
    //     snprintf(buf, sizeof(buf), "Sending buffer %d, %u bytes", send_buffer, (unsigned)payload_len);
    //     debug_print(buf);
    // }

    usb_serial_jtag_write_bytes(tx_buf, payload_len, 20 / portTICK_PERIOD_MS);
}

static void buffer_monitor_task(void *pv) {
    (void) pv;

    debug_print("Buffer monitor starting");

    uint32_t last_index = 0;
    while (true) {
        uint32_t idx = buffer_index;
        uint8_t send_buffer = active_buffer ^ 1;
        // Detect wrap (index went from BUFFER_LEN-1 -> 0)
        if (idx == 0 && last_index == BUFFER_LEN - 1) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Buffer wrap detected - sending buffer %d", send_buffer);
            debug_print(buf);
            send_buffers_over_usb(send_buffer);
        }
        last_index = idx;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

extern "C" void app_main(void) {
    init_uart();
    init_usb_serial_jtag();
    init_pwm_48khz();

    log_uart("Dual USB Echo + Throughput Example Started\r\n");

    TaskHandle_t h_counter = nullptr;
    TaskHandle_t h_gpio = nullptr;
    TaskHandle_t h_bufmon = nullptr;

    xTaskCreatePinnedToCore(counter_task, "counter_task", 2048, nullptr, 10, &h_counter, 0);
    xTaskCreatePinnedToCore(button_task, "button_task", 2048, nullptr, 10, &buttonTaskHandle, 1);
    xTaskCreatePinnedToCore(gpio_setup_task, "gpio_setup_task", 4096, nullptr, 10, &h_gpio, 1);
    xTaskCreatePinnedToCore(buffer_monitor_task, "buffer_monitor_task", 4096, nullptr, 10, &h_bufmon, 1);

    char buf[256];
    snprintf(buf, sizeof(buf),
             "Tasks created: ctr=%p btn=%p gpio=%p buf=%p",
             (void*)h_counter, (void*)buttonTaskHandle, (void*)h_gpio, (void*)h_bufmon);
    debug_print(buf);

    while (true) {
        uint8_t data[64];
        int len = usb_serial_jtag_read_bytes(data, sizeof(data), 20 / portTICK_PERIOD_MS);
        if (len > 0) {
            log_uart("len > 0\r\n");
            // Timestamp before sending
            int64_t start_us = esp_timer_get_time();

            // Echo back 100 times
            for (int i = 0; i < 100; i++) {
                usb_serial_jtag_write_bytes(data, len, 20 / portTICK_PERIOD_MS);
            }

            // Timestamp after sending
            int64_t end_us = esp_timer_get_time();
            int64_t elapsed_us = end_us - start_us;

            // Calculate throughput
            int total_bytes = len * 100;
            double kbps = (double)total_bytes / (double)elapsed_us * 1000.0;

            // Format debug line
            int outlen = snprintf(buf, sizeof(buf),
                     "START: %lld us, END: %lld us, Elapsed: %lld us, "
                     "Bytes: %d, Rate: %.2f kB/s\r\n",
                     start_us, end_us, elapsed_us, total_bytes, kbps);

            // Write to UART bridge safely
            if (outlen > 0) {
                uart_write_bytes(UART_NUM_0, buf, outlen);
            }
        }
        else
        {
            log_uart("len == 0\r\n");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}
