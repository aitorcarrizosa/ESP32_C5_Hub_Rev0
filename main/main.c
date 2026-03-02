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

// From your helper (keep the prototype here, no header needed)
esp_err_t example_eth_init(esp_eth_handle_t **eth_handles_out, uint8_t *eth_port_cnt_out);

static const char *TAG = "eth_example";

static esp_netif_t *s_eth_netif = NULL;
static esp_eth_handle_t s_eth = NULL;

static EventGroupHandle_t s_netif_eg;
static const int GOT_IP_BIT = BIT0;

static bool s_time_synced = false;
static esp_netif_ip_info_t s_last_ip = {0};

#define HEARTBEAT_URL "https://heartbeat-cl4jo2ojbq-uc.a.run.app"
#define HEARTBEAT_PERIOD_MS (15 * 60 * 1000)   // 15 minutes
#define HTTP_TIMEOUT_MS 8000

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

/**
 * Cloud Run uses public certs. For TLS verification, the device needs:
 *  - CA bundle (esp_crt_bundle_attach)
 *  - correct time (SNTP), otherwise cert validity checks can fail.
 */
static void obtain_time_sntp(void)
{
    if (s_time_synced) return;

    ESP_LOGI(TAG, "Syncing time with SNTP...");

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.sync_cb = NULL;
    config.smooth_sync = false;

    esp_netif_sntp_init(&config);

    // Wait until time is set (or timeout)
    const int max_wait = 15; // seconds
    for (int i = 0; i < max_wait; i++) {
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(1000)) == ESP_OK) {
            time_t now = time(NULL);
            struct tm tm_utc;
            gmtime_r(&now, &tm_utc);

            // sanity check (year >= 2024)
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
        ESP_LOGW(TAG, "SNTP sync failed or time invalid; HTTPS may fail (cert not valid yet).");
    }
}

static esp_err_t http_post_json(const char *url, const char *json_body)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach, // uses IDF cert bundle
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

        // If you want response body:
        // char buf[256];
        // int n = esp_http_client_read_response(c, buf, sizeof(buf)-1);
        // if (n > 0) { buf[n] = 0; ESP_LOGI("HTTP", "resp: %s", buf); }
    } else {
        ESP_LOGE("HTTP", "perform failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(c);
    return err;
}

static bool delay_abort_on_ip_loss(uint32_t total_ms)
{
    // Sleep in 1s chunks so we can stop quickly if IP is lost
    uint32_t remaining = total_ms;
    while (remaining > 0) {
        EventBits_t bits = xEventGroupGetBits(s_netif_eg);
        if ((bits & GOT_IP_BIT) == 0) {
            return false; // aborted
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

    // wait for IP the first time
    while (1) {
        xEventGroupWaitBits(s_netif_eg, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        // Ensure time is valid for TLS
        obtain_time_sntp();

        // Build a small JSON payload
        uint8_t mac[6] = {0};
        if (s_eth_netif) {
            esp_netif_get_mac(s_eth_netif, mac);
        }

        // time_t now = time(NULL);

        char ip_str[16] = {0};
        esp_ip4addr_ntoa(&s_last_ip.ip, ip_str, sizeof(ip_str));

        char payload[256];
        snprintf(payload, sizeof(payload),
                "{"
                "\"id\":\"ZAVHCJHKPrL9Am3em5z2\""
                "}");
        // POST (even if time sync failed, try — but TLS might reject)
        http_post_json(HEARTBEAT_URL, payload);

        // Wait until next heartbeat, but abort quickly if IP is lost
        if (!delay_abort_on_ip_loss(HEARTBEAT_PERIOD_MS)) {
            ESP_LOGW(TAG, "Heartbeat paused (IP lost). Waiting for IP...");
            continue;
        }
    }
}

void app_main(void)
{
    led_init();
    led_set(LED_GRN, true);   // Turn on green LED
    status_rgb_set(16, 0, 0);   // Turn on LED DevKit status RGB

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_netif_eg = xEventGroupCreate();
    if (!s_netif_eg) {
        ESP_LOGE(TAG, "Failed to create event group");
        return;
    }

    ESP_LOGI(TAG, "Pins: SCLK=%d MOSI=%d MISO=%d CS=%d INT=%d RST=%d SPI_MHz=%d",
             CONFIG_EXAMPLE_ETH_SPI_SCLK_GPIO,
             CONFIG_EXAMPLE_ETH_SPI_MOSI_GPIO,
             CONFIG_EXAMPLE_ETH_SPI_MISO_GPIO,
             CONFIG_EXAMPLE_ETH_SPI_CS0_GPIO,
             CONFIG_EXAMPLE_ETH_SPI_INT0_GPIO,
             CONFIG_EXAMPLE_ETH_SPI_PHY_RST0_GPIO,
             CONFIG_EXAMPLE_ETH_SPI_CLOCK_MHZ);

    // 1) Init Ethernet (helper)
    uint8_t cnt = 0;
    esp_eth_handle_t *handles = NULL;
    ESP_ERROR_CHECK(example_eth_init(&handles, &cnt));
    if (cnt < 1 || !handles) {
        ESP_LOGE(TAG, "No Ethernet ports initialized");
        return;
    }
    s_eth = handles[0];

    // 2) Read MAC from driver, then force netif to match it (prevents DHCP weirdness)
    uint8_t mac_drv[6] = {0};
    ESP_ERROR_CHECK(esp_eth_ioctl(s_eth, ETH_CMD_G_MAC_ADDR, mac_drv));
    log_mac("Initial DRV MAC:", mac_drv);

    // 3) Create netif
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&cfg);
    if (!s_eth_netif) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        return;
    }

    // IMPORTANT: make netif MAC match driver MAC BEFORE attach/DHCP
    ESP_ERROR_CHECK(esp_netif_set_mac(s_eth_netif, mac_drv));

    // 4) Attach glue
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth);
    if (!glue) {
        ESP_LOGE(TAG, "esp_eth_new_netif_glue failed");
        return;
    }
    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, glue));
    esp_netif_set_default_netif(s_eth_netif);

    // 5) Register handlers
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL));

    // 6) Start Ethernet (DHCP will start on LINK UP)
    ESP_ERROR_CHECK(esp_eth_start(s_eth));
    ESP_LOGI(TAG, "Ethernet Started");

    // 7) Start heartbeat task (waits for GOT_IP internally)
    xTaskCreate(heartbeat_task, "heartbeat_task", 16384, NULL, 5, NULL);


}