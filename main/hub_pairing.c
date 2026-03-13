#include "hub_pairing.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "led_init.h"
#include "sdkconfig.h"
#include "sub_init.h"

#define PAIRING_NAMESPACE "pairing"
#define PAIRING_BLOB_KEY "nodes"
#define PAIRING_MAGIC 0x50414952u
#define ACK_SUBGIG_CMD "subgig send ACKNOWLEDGE"

typedef struct {
    char line[256];
} uart_line_event_t;

typedef struct {
    uint32_t magic;
    uint16_t count;
    char ids[CONFIG_HUB_PAIR_MAX_NODES][HUB_NODE_ID_LEN + 1];
} pairing_store_t;

typedef struct {
    pairing_store_t store;
    SemaphoreHandle_t lock;
    QueueHandle_t line_queue;
    QueueHandle_t ack_queue;
    TaskHandle_t worker_task;
    TaskHandle_t ack_task;
    bool pairing_active;
    bool long_press_handled;
    TickType_t pairing_deadline;
    TickType_t green_until;
    TickType_t red_until;
    TickType_t button_pressed_since;
    bool capture_active;
    bool capture_has_data;
    char capture_payload[256];
    size_t capture_len;
    size_t capture_expected_len;
} hub_pairing_state_t;

static const char *TAG = "hub_pair";
static hub_pairing_state_t s_state;

static bool is_button_pressed(void)
{
#if CONFIG_HUB_PAIR_BUTTON_GPIO >= 0
    int level = gpio_get_level(CONFIG_HUB_PAIR_BUTTON_GPIO);
#if CONFIG_HUB_PAIR_BUTTON_ACTIVE_LOW
    return level == 0;
#else
    return level != 0;
#endif
#else
    return false;
#endif
}

static void configure_button(void)
{
#if CONFIG_HUB_PAIR_BUTTON_GPIO >= 0
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << CONFIG_HUB_PAIR_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
#if CONFIG_HUB_PAIR_BUTTON_PULLUP
        .pull_up_en = 1,
#else
        .pull_up_en = 0,
#endif
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
#endif
}

static void update_led_locked(TickType_t now)
{
    if (s_state.red_until > now) {
        status_rgb_set(255, 0, 0);
        return;
    }

    if (s_state.green_until > now) {
        status_rgb_set(0, 255, 0);
        return;
    }

    if (s_state.pairing_active) {
        bool blue_on = ((now / pdMS_TO_TICKS(250)) % 2U) == 0U;
        status_rgb_set(0, 0, blue_on ? 255 : 0);
        return;
    }

    status_rgb_off();
}

static void pulse_red_locked(uint32_t duration_ms)
{
    s_state.red_until = xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms);
}

static esp_err_t save_store_locked(void)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(PAIRING_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs_open failed");

    esp_err_t err = nvs_set_blob(nvs, PAIRING_BLOB_KEY, &s_state.store, sizeof(s_state.store));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}

static void normalize_node_id(const char *src, char *dst)
{
    for (size_t i = 0; i < HUB_NODE_ID_LEN; i++) {
        dst[i] = (char)toupper((unsigned char)src[i]);
    }
    dst[HUB_NODE_ID_LEN] = '\0';
}

static int find_node_locked(const char *node_id)
{
    for (uint16_t i = 0; i < s_state.store.count; i++) {
        if (strncmp(s_state.store.ids[i], node_id, HUB_NODE_ID_LEN) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static esp_err_t add_node_locked(const char *node_id, bool *added)
{
    if (added) {
        *added = false;
    }

    if (find_node_locked(node_id) >= 0) {
        return ESP_OK;
    }

    if (s_state.store.count >= CONFIG_HUB_PAIR_MAX_NODES) {
        return ESP_ERR_NO_MEM;
    }

    normalize_node_id(node_id, s_state.store.ids[s_state.store.count]);
    s_state.store.count++;

    ESP_RETURN_ON_ERROR(save_store_locked(), TAG, "failed to save node");

    if (added) {
        *added = true;
    }
    return ESP_OK;
}

static esp_err_t load_store(void)
{
    memset(&s_state.store, 0, sizeof(s_state.store));
    s_state.store.magic = PAIRING_MAGIC;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PAIRING_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t size = sizeof(s_state.store);
    err = nvs_get_blob(nvs, PAIRING_BLOB_KEY, &s_state.store, &size);
    nvs_close(nvs);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (size != sizeof(s_state.store) || s_state.store.magic != PAIRING_MAGIC ||
        s_state.store.count > CONFIG_HUB_PAIR_MAX_NODES) {
        memset(&s_state.store, 0, sizeof(s_state.store));
        s_state.store.magic = PAIRING_MAGIC;
    }

    for (uint16_t i = 0; i < s_state.store.count; i++) {
        normalize_node_id(s_state.store.ids[i], s_state.store.ids[i]);
    }

    return ESP_OK;
}

static bool parse_node_id_from_payload(const char *payload, char *node_id, const char **data_start)
{
    static const char *prefixes[] = { "ID:", "ID=", "UID:", "UID=" };

    for (size_t p = 0; p < (sizeof(prefixes) / sizeof(prefixes[0])); p++) {
        size_t prefix_len = strlen(prefixes[p]);
        if (strncmp(payload, prefixes[p], prefix_len) != 0) {
            continue;
        }

        const char *id = payload + prefix_len;
        for (size_t i = 0; i < HUB_NODE_ID_LEN; i++) {
            if (!isxdigit((unsigned char)id[i])) {
                return false;
            }
            node_id[i] = (char)toupper((unsigned char)id[i]);
        }
        node_id[HUB_NODE_ID_LEN] = '\0';

        const char *next = id + HUB_NODE_ID_LEN;
        while (*next == ' ' || *next == '\t' || *next == '|' || *next == ',' || *next == ';') {
            next++;
        }
        if (data_start) {
            *data_start = next;
        }
        return true;
    }

    return false;
}

static bool append_ascii_dump_chunk(const char *line)
{
    const char *open = strchr(line, '|');
    const char *close = open ? strrchr(open + 1, '|') : NULL;

    if (!open || !close || close <= open + 1) {
        return false;
    }

    size_t chunk_len = (size_t)(close - open - 1);
    if (chunk_len == 0) {
        return false;
    }

    size_t remaining = sizeof(s_state.capture_payload) - 1 - s_state.capture_len;
    if (remaining == 0) {
        return true;
    }
    if (chunk_len > remaining) {
        chunk_len = remaining;
    }

    memcpy(&s_state.capture_payload[s_state.capture_len], open + 1, chunk_len);
    s_state.capture_len += chunk_len;
    s_state.capture_payload[s_state.capture_len] = '\0';
    s_state.capture_has_data = true;
    return true;
}

static void capture_reset(void)
{
    s_state.capture_active = false;
    s_state.capture_has_data = false;
    s_state.capture_len = 0;
    s_state.capture_expected_len = 0;
    s_state.capture_payload[0] = '\0';
}

static void update_capture_expected_len(const char *line)
{
    unsigned int bytes = 0;

    if (sscanf(line, "Receive %u bytes", &bytes) == 1) {
        if (bytes > (sizeof(s_state.capture_payload) - 1)) {
            bytes = (unsigned int)(sizeof(s_state.capture_payload) - 1);
        }
        s_state.capture_expected_len = (size_t)bytes;
    }
}

static esp_err_t queue_ack(void)
{
    uint8_t token = 1;
    return xQueueSend(s_state.ack_queue, &token, 0) == pdTRUE ? ESP_OK : ESP_FAIL;
}

static void process_payload(const char *payload)
{
    char node_id[HUB_NODE_ID_LEN + 1];
    const char *data = NULL;
    bool has_node_id = parse_node_id_from_payload(payload, node_id, &data);
    bool ack_required = false;

    xSemaphoreTake(s_state.lock, portMAX_DELAY);

    if (s_state.pairing_active) {
        if (has_node_id) {
            bool added = false;
            esp_err_t err = add_node_locked(node_id, &added);
            if (err == ESP_OK) {
                pulse_red_locked(200);
                ack_required = true;
                if (added) {
                    ESP_LOGI(TAG, "paired node %s", node_id);
                }
            } else {
                ESP_LOGW(TAG, "failed to store node %s: %s", node_id, esp_err_to_name(err));
            }
        }

        update_led_locked(xTaskGetTickCount());
        xSemaphoreGive(s_state.lock);

        if (ack_required) {
            (void)queue_ack();
        }
        return;
    }

    if (!has_node_id) {
        xSemaphoreGive(s_state.lock);
        ESP_LOGW(TAG, "ignoring payload without node id: %s", payload);
        return;
    }

    if (find_node_locked(node_id) < 0) {
        xSemaphoreGive(s_state.lock);
        ESP_LOGW(TAG, "ignoring payload from unpaired node %s", node_id);
        return;
    }

    pulse_red_locked(200);
    update_led_locked(xTaskGetTickCount());
    xSemaphoreGive(s_state.lock);

    if (data && *data != '\0') {
        ESP_LOGI(TAG, "accepted packet from %s: %s", node_id, data);
    } else {
        ESP_LOGI(TAG, "accepted packet from %s", node_id);
    }
}

static void finalize_capture_if_needed(void)
{
    if (!s_state.capture_active || !s_state.capture_has_data) {
        capture_reset();
        return;
    }

    char payload[sizeof(s_state.capture_payload)];
    memcpy(payload, s_state.capture_payload, sizeof(payload));

    capture_reset();

    process_payload(payload);
}

static void handle_uart_line(const char *line)
{
    if (strcmp(line, "[SUBGIG RX]") == 0) {
        finalize_capture_if_needed();
        s_state.capture_active = true;
        s_state.capture_has_data = false;
        s_state.capture_len = 0;
        s_state.capture_expected_len = 0;
        s_state.capture_payload[0] = '\0';
        return;
    }

    if (!s_state.capture_active) {
        return;
    }

    if (strncmp(line, "  [", 3) == 0 || strncmp(line, "[", 1) == 0) {
        if (append_ascii_dump_chunk(line)) {
            if (s_state.capture_expected_len > 0 &&
                s_state.capture_len >= s_state.capture_expected_len) {
                finalize_capture_if_needed();
            }
            return;
        }
    }

    if (strncmp(line, "Receive ", 8) == 0) {
        update_capture_expected_len(line);
        return;
    }

    if (strchr(line, '>') != NULL) {
        finalize_capture_if_needed();
    }
}

static void sub_line_callback(const char *line, void *ctx)
{
    (void)ctx;

    if (!s_state.line_queue) {
        return;
    }

    uart_line_event_t event = {0};
    strlcpy(event.line, line, sizeof(event.line));
    (void)xQueueSend(s_state.line_queue, &event, 0);
}

static void ack_task(void *arg)
{
    (void)arg;
    uint8_t token = 0;

    while (1) {
        if (xQueueReceive(s_state.ack_queue, &token, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        esp_err_t err = sub_send_line(ACK_SUBGIG_CMD);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to queue ack command: %s", esp_err_to_name(err));
            continue;
        }

        if (CONFIG_HUB_PAIR_ACK_DELAY_MS > 0) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_HUB_PAIR_ACK_DELAY_MS));
        }
    }
}

static void pairing_task(void *arg)
{
    (void)arg;
    uart_line_event_t event;

    while (1) {
        while (xQueueReceive(s_state.line_queue, &event, 0) == pdTRUE) {
            handle_uart_line(event.line);
        }

        TickType_t now = xTaskGetTickCount();

        xSemaphoreTake(s_state.lock, portMAX_DELAY);

        if (s_state.pairing_active && now >= s_state.pairing_deadline) {
            s_state.pairing_active = false;
            s_state.green_until = 0;
            ESP_LOGI(TAG, "pairing mode ended");
        }

#if CONFIG_HUB_PAIR_BUTTON_GPIO >= 0
        bool pressed = is_button_pressed();

        if (pressed) {
            if (s_state.button_pressed_since == 0) {
                s_state.button_pressed_since = now;
            } else if (!s_state.long_press_handled &&
                       (now - s_state.button_pressed_since) >= pdMS_TO_TICKS(CONFIG_HUB_PAIR_HOLD_MS)) {
                s_state.pairing_active = true;
                s_state.long_press_handled = true;
                s_state.pairing_deadline = now + pdMS_TO_TICKS(CONFIG_HUB_PAIR_WINDOW_MS);
                s_state.green_until = now + pdMS_TO_TICKS(500);
                ESP_LOGI(TAG, "pairing mode started for %d ms", CONFIG_HUB_PAIR_WINDOW_MS);
            }
        } else {
            s_state.button_pressed_since = 0;
            s_state.long_press_handled = false;
        }
#endif

        update_led_locked(now);
        xSemaphoreGive(s_state.lock);

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t hub_pairing_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init failed");

    memset(&s_state, 0, sizeof(s_state));
    s_state.lock = xSemaphoreCreateMutex();
    s_state.line_queue = xQueueCreate(16, sizeof(uart_line_event_t));
    s_state.ack_queue = xQueueCreate(8, sizeof(uint8_t));

    if (!s_state.lock || !s_state.line_queue || !s_state.ack_queue) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(load_store(), TAG, "failed to load store");

    configure_button();
    ESP_RETURN_ON_ERROR(sub_set_line_callback(sub_line_callback, NULL), TAG, "failed to set UART callback");

    BaseType_t ok = xTaskCreate(pairing_task, "hub_pair", 6144, NULL, 5, &s_state.worker_task);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }

    ok = xTaskCreate(ack_task, "hub_ack", 3072, NULL, 5, &s_state.ack_task);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "loaded %u paired node(s)", s_state.store.count);
    return ESP_OK;
}

bool hub_pairing_is_active(void)
{
    bool active;
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    active = s_state.pairing_active;
    xSemaphoreGive(s_state.lock);
    return active;
}

esp_err_t hub_pairing_start(void)
{
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    s_state.pairing_active = true;
    s_state.pairing_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CONFIG_HUB_PAIR_WINDOW_MS);
    s_state.green_until = xTaskGetTickCount() + pdMS_TO_TICKS(500);
    xSemaphoreGive(s_state.lock);
    ESP_LOGI(TAG, "pairing mode started");
    return ESP_OK;
}

esp_err_t hub_pairing_clear(void)
{
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    memset(&s_state.store, 0, sizeof(s_state.store));
    s_state.store.magic = PAIRING_MAGIC;
    esp_err_t err = save_store_locked();
    xSemaphoreGive(s_state.lock);
    return err;
}

size_t hub_pairing_get_count(void)
{
    size_t count;
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    count = s_state.store.count;
    xSemaphoreGive(s_state.lock);
    return count;
}

size_t hub_pairing_copy_node_ids(char ids[][HUB_NODE_ID_LEN + 1], size_t max_ids)
{
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    size_t count = s_state.store.count < max_ids ? s_state.store.count : max_ids;
    for (size_t i = 0; i < count; i++) {
        strlcpy(ids[i], s_state.store.ids[i], HUB_NODE_ID_LEN + 1);
    }
    xSemaphoreGive(s_state.lock);
    return count;
}
