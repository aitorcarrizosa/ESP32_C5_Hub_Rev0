#include <string.h>
#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_eth.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_netif_sntp.h"

#include "led_init.h"
#include "sub_init.h"

// helper (ethernet_init component)
esp_err_t example_eth_init(esp_eth_handle_t **eth_handles_out, uint8_t *eth_port_cnt_out);

void console_start_uart0(void);

static const char *TAG = "hub_main";

static esp_netif_t *s_eth_netif = NULL;
static esp_eth_handle_t s_eth = NULL;

esp_netif_t *hub_eth_get_netif(void)
{
    return s_eth_netif;
}

esp_eth_handle_t hub_eth_get_handle(void)
{
    return s_eth;
}

static EventGroupHandle_t s_netif_eg;
static const int GOT_IP_BIT = BIT0;

static bool s_time_synced = false;
static esp_netif_ip_info_t s_last_ip = {0};

#define HEARTBEAT_URL "https://heartbeat-cl4jo2ojbq-uc.a.run.app"
#define HEARTBEAT_PERIOD_MS (15 * 60 * 1000)   // 15 minutes
#define HTTP_TIMEOUT_MS 8000

static void console_task(void *arg)
{
    (void)arg;
    console_start_uart0();   // no vuelve
    vTaskDelete(NULL);
}

static void log_mac(const char *label, const uint8_t mac[6])
{
    ESP_LOGI(TAG, "%s %02x:%02x:%02x:%02x:%02x:%02x",
             label, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void dhcp_start(void)
{
    if (!s_eth_netif) return;

    esp_err_t err = esp_netif_dhcpc_start(s_eth_netif);
    if (err == ESP_OK || err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGI(TAG, "DHCP running");
    } else {
        ESP_LOGE(TAG, "dhcpc_start failed: %s", esp_err_to_name(err));
    }
}

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_eth_handle_t eth = *(esp_eth_handle_t *)data;

    switch (id) {
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "ETH START");
        break;

    case ETHERNET_EVENT_CONNECTED: {
        uint8_t mac_drv[6] = {0};
        uint8_t mac_netif[6] = {0};

        ESP_LOGI(TAG, "ETH CONNECTED (Link Up)");

        if (esp_eth_ioctl(eth, ETH_CMD_G_MAC_ADDR, mac_drv) == ESP_OK) {
            log_mac("DRV  MAC:", mac_drv);
        }
        if (s_eth_netif && esp_netif_get_mac(s_eth_netif, mac_netif) == ESP_OK) {
            log_mac("NETIF MAC:", mac_netif);
        }

        dhcp_start();
        break;
    }

    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "ETH DISCONNECTED (Link Down)");
        break;

    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "ETH STOP");
        break;

    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;

    if (id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_last_ip = e->ip_info;

        ESP_LOGI(TAG, "GOT IP: " IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "MASK : " IPSTR, IP2STR(&e->ip_info.netmask));
        ESP_LOGI(TAG, "GW   : " IPSTR, IP2STR(&e->ip_info.gw));

        xEventGroupSetBits(s_netif_eg, GOT_IP_BIT);
    } else if (id == IP_EVENT_ETH_LOST_IP) {
        ESP_LOGW(TAG, "LOST IP");
        xEventGroupClearBits(s_netif_eg, GOT_IP_BIT);
        s_time_synced = false; // force re-sync when IP comes back
    }
}

static void obtain_time_sntp(void)
{
    if (s_time_synced) return;

    ESP_LOGI(TAG, "Syncing time with SNTP...");

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.sync_cb = NULL;
    config.smooth_sync = false;

    esp_netif_sntp_init(&config);

    const int max_wait = 15; // seconds
    for (int i = 0; i < max_wait; i++) {
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(1000)) == ESP_OK) {
            time_t now = time(NULL);
            struct tm tm_utc;
            gmtime_r(&now, &tm_utc);

            if (tm_utc.tm_year + 1900 >= 2024) {
                ESP_LOGI(TAG, "Time synced: %04d-%02d-%02d %02d:%02d:%02d UTC",
                         tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                         tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
                s_time_synced = true;
                break;
            }
        }
    }

    esp_netif_sntp_deinit();

    if (!s_time_synced) {
        ESP_LOGW(TAG, "SNTP sync failed or time invalid; HTTPS may fail.");
    }
}

static esp_err_t http_post_json(const char *url, const char *json_body)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;

    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Accept", "application/json");
    esp_http_client_set_header(c, "Connection", "close");

    esp_http_client_set_post_field(c, json_body, (int)strlen(json_body));

    ESP_LOGI("HTTP", "POST %s payload_len=%d", url, (int)strlen(json_body));
    esp_err_t err = esp_http_client_perform(c);

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(c);
        ESP_LOGI("HTTP", "status=%d", status);
    } else {
        ESP_LOGE("HTTP", "perform failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(c);
    return err;
}

static bool delay_abort_on_ip_loss(uint32_t total_ms)
{
    uint32_t remaining = total_ms;
    while (remaining > 0) {
        if (s_netif_eg) {
            EventBits_t bits = xEventGroupGetBits(s_netif_eg);
            if ((bits & GOT_IP_BIT) == 0) {
                return false;
            }
        } else {
            return false;
        }

        uint32_t step = remaining > 1000 ? 1000 : remaining;
        vTaskDelay(pdMS_TO_TICKS(step));
        remaining -= step;
    }
    return true;
}

static void heartbeat_task(void *arg)
{
    (void)arg;

    while (1) {
        xEventGroupWaitBits(s_netif_eg, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        obtain_time_sntp();

        char payload[128];
        snprintf(payload, sizeof(payload), "{\"id\":\"ZAVHCJHKPrL9Am3em5z2\"}");

        http_post_json(HEARTBEAT_URL, payload);

        if (!delay_abort_on_ip_loss(HEARTBEAT_PERIOD_MS)) {
            ESP_LOGW(TAG, "Heartbeat paused (IP lost). Waiting for IP...");
            continue;
        }
    }
}

/**
 * @brief Initialize Ethernet stack only if any Ethernet interface is enabled in menuconfig.
 * @return true if Ethernet started, false if disabled or failed.
 */
static bool try_start_ethernet(void)
{
    // Always init netif/event loop (harmless even if ETH disabled)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_netif_eg = xEventGroupCreate();
    if (!s_netif_eg) {
        ESP_LOGE(TAG, "Failed to create event group");
        return false;
    }

    uint8_t cnt = 0;
    esp_eth_handle_t *handles = NULL;

    esp_err_t err = example_eth_init(&handles, &cnt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "example_eth_init failed: %s (continuing without Ethernet)", esp_err_to_name(err));
        return false;
    }

    // This is the key: cnt==0 means "disabled in menuconfig"
    if (cnt == 0 || handles == NULL) {
        ESP_LOGW(TAG, "Ethernet disabled (no interfaces selected in menuconfig)");
        return false;
    }

    s_eth = handles[0];

    uint8_t mac_drv[6] = {0};
    err = esp_eth_ioctl(s_eth, ETH_CMD_G_MAC_ADDR, mac_drv);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ETH_CMD_G_MAC_ADDR failed: %s", esp_err_to_name(err));
        return false;
    }
    log_mac("Initial DRV MAC:", mac_drv);

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&cfg);
    if (!s_eth_netif) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        return false;
    }

    ESP_ERROR_CHECK(esp_netif_set_mac(s_eth_netif, mac_drv));

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth);
    if (!glue) {
        ESP_LOGE(TAG, "esp_eth_new_netif_glue failed");
        return false;
    }
    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, glue));
    esp_netif_set_default_netif(s_eth_netif);

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL));

    err = esp_eth_start(s_eth);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start failed: %s (continuing without Ethernet)", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Ethernet Started");
    xTaskCreate(heartbeat_task, "heartbeat_task", 16384, NULL, 5, NULL);
    return true;
}

void app_main(void)
{
    // Always start console
    xTaskCreate(console_task, "console", 6144, NULL, 5, NULL);

    // LEDs always available
    led_init();
    status_rgb_set(0, 0, 0); // off by default

    // Sub bridge: only if enabled in menuconfig
#if CONFIG_SUB_UART_ENABLE
    if (!sub_uart_bridge_init()) {
        ESP_LOGW(TAG, "sub_uart_bridge_init failed (continuing)");
    }
#else
    ESP_LOGI(TAG, "Sub UART bridge disabled by menuconfig");
#endif

    // Ethernet/W5500: start only if enabled in menuconfig (cnt>0)
    (void)try_start_ethernet();

    ESP_LOGI(TAG, "Hub main running");
}