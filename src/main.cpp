#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/mcpwm_cap.h"
#include "driver/mcpwm_sync.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include <math.h>
#include <cstring>    // C++ header for strlen
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "DUAL_USB";

// Forward declarations
static void debug_print(const char* msg);
static void usb_jtag_init_task(void* pv);

static volatile bool usb_jtag_ready = false;

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
    cfg.tx_buffer_size = 8192;
    cfg.rx_buffer_size = 4096;
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        char buf[128];
        snprintf(buf, sizeof(buf), "usb_serial_jtag_driver_install failed: %s", esp_err_to_name(err));
        debug_print(buf);
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
#define PWM_PIN 18   // GPIO18 (free pin for 44.1 kHz PWM output)

#define PWM_LEDC_TIMER LEDC_TIMER_0
#define PWM_LEDC_CHANNEL LEDC_CHANNEL_0
#define PWM_LEDC_SPEED_MODE LEDC_LOW_SPEED_MODE
#define PWM_FREQ_HZ 44100
#define PWM_DUTY_RES LEDC_TIMER_10_BIT
#define PWM_DUTY_PERCENT 3  // adjust duty cycle here (0-100)

static void init_pwm_44k1(void) {
    ledc_timer_config_t ledc_timer = {};
    ledc_timer.speed_mode = PWM_LEDC_SPEED_MODE;
    ledc_timer.duty_resolution = PWM_DUTY_RES;
    ledc_timer.timer_num = PWM_LEDC_TIMER;
    ledc_timer.freq_hz = PWM_FREQ_HZ;
    ledc_timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&ledc_timer);

    const uint32_t max_duty = (1U << PWM_DUTY_RES) - 1;
    const uint32_t duty = (max_duty * PWM_DUTY_PERCENT) / 100U;

    ledc_channel_config_t ledc_channel = {};
    ledc_channel.speed_mode = PWM_LEDC_SPEED_MODE;
    ledc_channel.channel = PWM_LEDC_CHANNEL;
    ledc_channel.timer_sel = PWM_LEDC_TIMER;
    ledc_channel.intr_type = LEDC_INTR_DISABLE;
    ledc_channel.gpio_num = PWM_PIN;
    ledc_channel.duty = duty;
    ledc_channel.hpoint = 0;
    ledc_channel_config(&ledc_channel);
}

static volatile uint32_t channel_buffers[2][NUM_CHANNELS][BUFFER_LEN];
static volatile uint32_t buffer_index = 0;
static volatile uint8_t active_buffer = 0; // 0 or 1

// Control flag to pause ISR-driven sampling while test burst runs
static volatile bool paused = true;

// Debug counter incremented from ISR when button pressed (diagnostic only)
static volatile uint32_t button_press_count = 0;

// Task handle for button handler (signalled from ISR)
static TaskHandle_t buttonTaskHandle = nullptr;
static TaskHandle_t burstTaskHandle = nullptr;

static mcpwm_cap_timer_handle_t cap_timer = nullptr;
static mcpwm_cap_timer_handle_t cap_timer_aux = nullptr;
static mcpwm_cap_channel_handle_t cap_ch[NUM_CHANNELS] = {};
static mcpwm_sync_handle_t cap_sync = nullptr;
static mcpwm_sync_handle_t cap_sync_aux = nullptr;
static uint32_t cap_resolution_hz = 0;
static uint32_t cap_resolution_aux_hz = 0;

// Forward declarations
static void gpio_setup_task(void* pv);
static void buffer_monitor_task(void* pv);
static void button_task(void* pv);
static void burst_task(void* pv);
static void init_mcpwm_capture(void);

static void debug_print(const char* msg) {
    if (!msg) {
        return;
    }

    log_uart(msg);
    log_uart("\r\n");
}

static void usb_jtag_init_task(void* pv) {
    (void)pv;
    init_usb_serial_jtag();
    usb_jtag_ready = true;
    vTaskDelete(nullptr);
}

static bool IRAM_ATTR cap_cb(mcpwm_cap_channel_handle_t chan,
                             const mcpwm_capture_event_data_t *edata,
                             void *user_ctx) {
    (void)chan;
    if (paused) {
        return false;
    }

    int channel = (int)(intptr_t)user_ctx;
    uint32_t idx = buffer_index % BUFFER_LEN;
    channel_buffers[active_buffer][channel][idx] = edata->cap_value;
    return false;
}

// ISR for incrementing buffer_index (timing comes from MCPWM capture)
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
}

// ISR for button press: notify the button handling task (keep ISR short)
static void IRAM_ATTR button_isr_handler(void* arg) {
    (void)arg;
    button_press_count++;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(buttonTaskHandle, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static constexpr uint32_t kSampleRateHz = 44100;
static constexpr float kDefaultToneHz = 1000.0f;
static constexpr int kDefaultBurstMs = 5000;

static portMUX_TYPE burst_request_mux = portMUX_INITIALIZER_UNLOCKED;

struct BurstRequest {
    float tone_hz;
    int duration_ms;
};

static volatile BurstRequest burst_request = {kDefaultToneHz, kDefaultBurstMs};
static volatile bool burst_active = false;
static volatile bool burst_queued = false;
static volatile BurstRequest burst_queued_request = {kDefaultToneHz, kDefaultBurstMs};

static uint32_t next_sine_sample(float &phase, float phase_inc) {
    float s = sinf(2.0f * 3.14159265f * phase);
    float scaled = 0.5f + 0.499999f * s; // keep within [0,1) to avoid clipping
    uint32_t sample = static_cast<uint32_t>(scaled * 16777215.0f); // 24-bit unsigned
    phase += phase_inc;
    if (phase >= 1.0f) {
        phase -= 1.0f;
    }
    return sample & 0xFFFFFF;
}

static void send_test_burst(float tone_hz, int duration_ms) {
    if (tone_hz <= 0.0f || duration_ms <= 0) {
        debug_print("Invalid tone or duration; expected > 0");
        return;
    }

    debug_print("Starting sine burst over USB Serial JTAG");

    constexpr uint32_t samples_per_packet = BUFFER_LEN; // 8 samples per channel
    constexpr uint32_t channels = NUM_CHANNELS;
    const uint32_t total_samples = (kSampleRateHz * static_cast<uint32_t>(duration_ms)) / 1000U;
    const uint32_t packets = (total_samples + samples_per_packet - 1) / samples_per_packet;
    const float phase_inc = tone_hz / static_cast<float>(kSampleRateHz);

    static uint8_t tx_buf[2 + channels * samples_per_packet * 3];
    int staged_amount = 0;
    uint32_t sample_index = 0;
    float phase = 0.0f;

    for (uint32_t pkt = 0; pkt < packets; ++pkt) {
        tx_buf[0] = 0x55;
        tx_buf[1] = 0xAA;

        size_t off = 2;
        for (uint32_t ch = 0; ch < channels; ++ch) {
            for (uint32_t i = 0; i < samples_per_packet; ++i) {
                uint32_t sample = 0;
                if (sample_index < total_samples) {
                    sample = next_sine_sample(phase, phase_inc);
                    sample_index++;
                }
                tx_buf[off++] = (sample >> 0) & 0xFF;
                tx_buf[off++] = (sample >> 8) & 0xFF;
                tx_buf[off++] = (sample >> 16) & 0xFF;
            }
        }

        int sent = 0;
        const int payload_len = sizeof(tx_buf);
        while (sent < payload_len) {
            int wrote = usb_serial_jtag_write_bytes(tx_buf + sent, payload_len - sent, 20 / portTICK_PERIOD_MS);
            if (wrote > 0) {
                sent += wrote;
                staged_amount += wrote;
            } else {
                debug_print("wrote = 0, delaying before retry");
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
    }

    debug_print("Sine burst complete, total bytes sent:");
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%d bytes sent in burst", staged_amount);
        debug_print(buf);
    }
}

static void request_burst(float tone_hz, int duration_ms) {
    if (!burstTaskHandle) {
        debug_print("burst_task not ready");
        return;
    }

    taskENTER_CRITICAL(&burst_request_mux);
    if (burst_active) {
        if (!burst_queued) {
            burst_queued_request.tone_hz = tone_hz;
            burst_queued_request.duration_ms = duration_ms;
            burst_queued = true;
        }
        taskEXIT_CRITICAL(&burst_request_mux);
        return;
    }

    burst_request.tone_hz = tone_hz;
    burst_request.duration_ms = duration_ms;
    burst_active = true;
    taskEXIT_CRITICAL(&burst_request_mux);

    xTaskNotifyGive(burstTaskHandle);
}

static void burst_task(void* pv) {
    (void) pv;
    debug_print("burst_task started");
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        BurstRequest req;
        taskENTER_CRITICAL(&burst_request_mux);
        req = burst_request;
        taskEXIT_CRITICAL(&burst_request_mux);

        paused = true;
        debug_print("Sampling paused - starting burst");
        send_test_burst(req.tone_hz, req.duration_ms);
        debug_print("Burst complete - resuming sampling");
        paused = false;

        taskENTER_CRITICAL(&burst_request_mux);
        if (burst_queued) {
            burst_request = burst_queued_request;
            burst_queued = false;
            taskEXIT_CRITICAL(&burst_request_mux);
            xTaskNotifyGive(burstTaskHandle);
            continue;
        }
        burst_active = false;
        taskEXIT_CRITICAL(&burst_request_mux);
    }
}

// Pins to use for interrupts (change as needed)
// NOTE: GPIO12 can be a strapping/boot pin on some devkits; use GPIO13 instead on ESP32-S3 DevKitC-1-N16R8
static const gpio_num_t channel_pins[NUM_CHANNELS] = {
    GPIO_NUM_2, GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_13
};

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
            snprintf(buf, sizeof(buf), "button_task: notified (button press count=%u)", (unsigned)button_press_count);
            debug_print(buf);
        }
        // Debounce grace period
        vTaskDelay(pdMS_TO_TICKS(20));

        request_burst(kDefaultToneHz, kDefaultBurstMs);

        // Prevent bouncing / repeated triggers
        vTaskDelay(pdMS_TO_TICKS(500));

        // Resume sampling handled by burst_task
    }
}

static void init_mcpwm_capture(void) {
    mcpwm_capture_event_callbacks_t cbs = {};
    cbs.on_cap = cap_cb;

    const uint32_t desired_cap_resolution_hz = 160000000;

    mcpwm_capture_timer_config_t cap_timer_config = {};
    cap_timer_config.group_id = 0;
    cap_timer_config.clk_src = MCPWM_CAPTURE_CLK_SRC_DEFAULT;
    cap_timer_config.resolution_hz = desired_cap_resolution_hz;
    ESP_ERROR_CHECK(mcpwm_new_capture_timer(&cap_timer_config, &cap_timer));
    ESP_ERROR_CHECK(mcpwm_capture_timer_enable(cap_timer));

    for (int i = 0; i < 3; ++i) {
        mcpwm_capture_channel_config_t cap_ch_config = {};
        cap_ch_config.gpio_num = channel_pins[i];
        cap_ch_config.prescale = 1;
        cap_ch_config.flags.neg_edge = true;
        cap_ch_config.flags.pos_edge = false;
        ESP_ERROR_CHECK(mcpwm_new_capture_channel(cap_timer, &cap_ch_config, &cap_ch[i]));
        ESP_ERROR_CHECK(mcpwm_capture_channel_register_event_callbacks(cap_ch[i], &cbs, (void*)(intptr_t)i));
        ESP_ERROR_CHECK(mcpwm_capture_channel_enable(cap_ch[i]));
    }

    mcpwm_gpio_sync_src_config_t sync_src_cfg = {};
    sync_src_cfg.group_id = 0;
    sync_src_cfg.gpio_num = INDEX_PIN;
    sync_src_cfg.flags.active_neg = true;
    ESP_ERROR_CHECK(mcpwm_new_gpio_sync_src(&sync_src_cfg, &cap_sync));

    mcpwm_capture_timer_sync_phase_config_t sync_phase_cfg = {};
    sync_phase_cfg.sync_src = cap_sync;
    sync_phase_cfg.count_value = 0;
    sync_phase_cfg.direction = MCPWM_TIMER_DIRECTION_UP;
    ESP_ERROR_CHECK(mcpwm_capture_timer_set_phase_on_sync(cap_timer, &sync_phase_cfg));
    ESP_ERROR_CHECK(mcpwm_capture_timer_start(cap_timer));
    ESP_ERROR_CHECK(mcpwm_capture_timer_get_resolution(cap_timer, &cap_resolution_hz));

    {
        char buf[96];
        snprintf(buf, sizeof(buf), "Capture timer group0 resolution: %u Hz", (unsigned)cap_resolution_hz);
        debug_print(buf);
    }

    mcpwm_capture_timer_config_t cap_timer_aux_config = {};
    cap_timer_aux_config.group_id = 1;
    cap_timer_aux_config.clk_src = MCPWM_CAPTURE_CLK_SRC_DEFAULT;
    cap_timer_aux_config.resolution_hz = cap_resolution_hz;
    ESP_ERROR_CHECK(mcpwm_new_capture_timer(&cap_timer_aux_config, &cap_timer_aux));
    ESP_ERROR_CHECK(mcpwm_capture_timer_enable(cap_timer_aux));

    mcpwm_capture_channel_config_t cap_ch_config = {};
    cap_ch_config.gpio_num = channel_pins[3];
    cap_ch_config.prescale = 1;
    cap_ch_config.flags.neg_edge = true;
    cap_ch_config.flags.pos_edge = false;
    ESP_ERROR_CHECK(mcpwm_new_capture_channel(cap_timer_aux, &cap_ch_config, &cap_ch[3]));
    ESP_ERROR_CHECK(mcpwm_capture_channel_register_event_callbacks(cap_ch[3], &cbs, (void*)(intptr_t)3));
    ESP_ERROR_CHECK(mcpwm_capture_channel_enable(cap_ch[3]));

    mcpwm_gpio_sync_src_config_t sync_src_aux_cfg = {};
    sync_src_aux_cfg.group_id = 1;
    sync_src_aux_cfg.gpio_num = INDEX_PIN;
    sync_src_aux_cfg.flags.active_neg = true;
    ESP_ERROR_CHECK(mcpwm_new_gpio_sync_src(&sync_src_aux_cfg, &cap_sync_aux));

    mcpwm_capture_timer_sync_phase_config_t sync_phase_aux_cfg = {};
    sync_phase_aux_cfg.sync_src = cap_sync_aux;
    sync_phase_aux_cfg.count_value = 0;
    sync_phase_aux_cfg.direction = MCPWM_TIMER_DIRECTION_UP;
    ESP_ERROR_CHECK(mcpwm_capture_timer_set_phase_on_sync(cap_timer_aux, &sync_phase_aux_cfg));
    ESP_ERROR_CHECK(mcpwm_capture_timer_start(cap_timer_aux));
    ESP_ERROR_CHECK(mcpwm_capture_timer_get_resolution(cap_timer_aux, &cap_resolution_aux_hz));

    {
        char buf[96];
        snprintf(buf, sizeof(buf), "Capture timer group1 resolution: %u Hz", (unsigned)cap_resolution_aux_hz);
        debug_print(buf);
    }
}

// Task to configure GPIO interrupts (core 1)
static void gpio_setup_task(void *pv) {
    (void) pv;
    debug_print("gpio_setup_task started");

    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = 0;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_NEGEDGE;

    // install ISR service once at level 5
    auto isr_res = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_IRAM);
    if (isr_res != ESP_OK) {
        char buf[128];
        snprintf(buf, sizeof(buf), "gpio_install_isr_service failed: %s", esp_err_to_name(isr_res));
        debug_print(buf);
    } else {
        debug_print("GPIO ISR service installed");
    }   

    for (int i = 0; i < NUM_CHANNELS; ++i) {
        io_conf.pin_bit_mask = (1ULL << channel_pins[i]);
        gpio_config(&io_conf);
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "MCPWM capture configured: channel %d -> pin %d", i, (int)channel_pins[i]);
            debug_print(buf);
        }
    }

    // Configure the index increment pin (only for buffer indexing)
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
    io_conf.intr_type = GPIO_INTR_NEGEDGE;

    // Loop: poll the button level periodically to detect changes and show ISR counter
    int prev_level = 1; // pulled-up by default
    while (true) {
        int level = gpio_get_level((gpio_num_t)BUTTON_PIN);
        if (level != prev_level) {
            prev_level = level;
            char buf[96];
            snprintf(buf, sizeof(buf), "Button level changed: %d (button press count=%u)", level, (unsigned)button_press_count);
            debug_print(buf);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void send_buffers_over_usb(uint8_t send_buffer) {
    // Pack sync + all channels into a single buffer and send once (better throughput)
    const size_t payload_len = 2 + (size_t)NUM_CHANNELS * BUFFER_LEN * 3;
    static uint8_t tx_buf[2 + NUM_CHANNELS * BUFFER_LEN * 3];
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
            //char buf[64];
            //snprintf(buf, sizeof(buf), "Buffer wrap detected - sending buffer %d", send_buffer);
            //debug_print(buf);
            send_buffers_over_usb(send_buffer);
        }
        last_index = idx;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void handle_uart_command(const char *line) {
    if (!line || line[0] == '\0') {
        return;
    }

    float tone_hz = 0.0f;
    int duration_ms = 0;
    if (sscanf(line, "TONE %f %d", &tone_hz, &duration_ms) == 2 ||
        sscanf(line, "tone %f %d", &tone_hz, &duration_ms) == 2) {
        request_burst(tone_hz, duration_ms);
        return;
    }

    debug_print("Unknown UART command. Use: TONE <freq_hz> <duration_ms>");
}

extern "C" void app_main(void) {
    init_uart();
    init_pwm_44k1();

    xTaskCreatePinnedToCore(usb_jtag_init_task, "usb_jtag_init", 2048, nullptr, 10, nullptr, 1);
    while (!usb_jtag_ready) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    debug_print("Dual USB Echo + Throughput Example Started");

    // Disable task watchdog entirely for this project
    esp_task_wdt_deinit();

    const char *msg = "USB Serial JTAG ready\r\n";
    usb_serial_jtag_write_bytes(msg, strlen(msg), 2000 / portTICK_PERIOD_MS);

    debug_print("Creating tasks...");

    init_mcpwm_capture();

    TaskHandle_t h_counter = nullptr;
    TaskHandle_t h_gpio = nullptr;
    TaskHandle_t h_bufmon = nullptr;
    
    char buf[256];
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    snprintf(buf, sizeof(buf), "Free heap before tasks: %u bytes", (unsigned)free_heap);
    debug_print(buf);

    BaseType_t r_burst = xTaskCreatePinnedToCore(burst_task, "burst_task", 4096, nullptr, 11, &burstTaskHandle, 1);
    debug_print("burst_task create attempted");
    free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    snprintf(buf, sizeof(buf), "Free heap after burst_task: %u bytes", (unsigned)free_heap);
    debug_print(buf);

    BaseType_t r_button = xTaskCreatePinnedToCore(button_task, "button_task", 2048, nullptr, 10, &buttonTaskHandle, 1);
    debug_print("button_task create attempted");
    free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    snprintf(buf, sizeof(buf), "Free heap after button_task: %u bytes", (unsigned)free_heap);
    debug_print(buf);

    BaseType_t r_gpio = xTaskCreatePinnedToCore(gpio_setup_task, "gpio_setup_task", 4096, nullptr, 10, &h_gpio, 1);
    debug_print("gpio_setup_task create attempted");
    vTaskDelay(pdMS_TO_TICKS(1000));
    free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    snprintf(buf, sizeof(buf), "Free heap after gpio_setup_task: %u bytes", (unsigned)free_heap);
    debug_print(buf);

    BaseType_t r_bufmon = xTaskCreatePinnedToCore(buffer_monitor_task, "buffer_monitor_task", 4096, nullptr, 10, &h_bufmon, 1);
    debug_print("buffer_monitor_task create attempted");
    free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    snprintf(buf, sizeof(buf), "Free heap after buffer_monitor_task: %u bytes", (unsigned)free_heap);
    debug_print(buf);

    BaseType_t r_counter = -1;

    snprintf(buf, sizeof(buf),
             "Task create results: ctr=%d burst=%d btn=%d gpio=%d buf=%d",
             (int)r_counter, (int)r_burst, (int)r_button, (int)r_gpio, (int)r_bufmon);
    debug_print(buf);

    snprintf(buf, sizeof(buf),
             "Tasks created: ctr=%p burst=%p btn=%p gpio=%p buf=%p",
             (void*)h_counter, (void*)burstTaskHandle, (void*)buttonTaskHandle,
             (void*)h_gpio, (void*)h_bufmon);
    debug_print(buf);

    char cmd_buf[128] = {};
    size_t cmd_len = 0;

    while (true) {
        uint8_t data[64];
        int len = uart_read_bytes(UART_NUM_0, data, sizeof(data), 20 / portTICK_PERIOD_MS);
        if (len > 0) {
            for (int i = 0; i < len; ++i) {
                char c = static_cast<char>(data[i]);
                if (c == '\r' || c == '\n') {
                    if (cmd_len > 0) {
                        cmd_buf[cmd_len] = '\0';
                        handle_uart_command(cmd_buf);
                        cmd_len = 0;
                    }
                } else if (cmd_len + 1 < sizeof(cmd_buf)) {
                    cmd_buf[cmd_len++] = c;
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}
