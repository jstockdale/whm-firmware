#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "mon_config.h"
#include "mon_wifi.h"
#include "mon_parse.h"
#include "mon_ws.h"
static const char *TAG = "mon_net";
static void wifi_evt(void *a, esp_event_base_t base, int32_t id, void *d)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
        esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();              /* quiet retry */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)d;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        static bool sntp_up;
        if (!sntp_up) {
            sntp_up = true;
            esp_sntp_config_t sc =
                ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&sc);
        }
    }
}
static void rx_task(void *arg)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(MON_UDP_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    bind(s, (struct sockaddr *)&a, sizeof(a));
    ESP_LOGI(TAG, "listening on UDP :%d (passive)", MON_UDP_PORT);
    static uint8_t buf[160];
    struct sockaddr_in src; socklen_t sl;
    for (;;) {
        sl = sizeof(src);
        int n = recvfrom(s, buf, sizeof(buf), 0,
                         (struct sockaddr *)&src, &sl);
        if (n > 0) {
            mon_parse_pkt(buf, n);
            mon_ws_feed(buf, n, src.sin_addr.s_addr);
        }
    }
}
static void status_task(void *arg)
{
    uint32_t pk = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t k = g_mon.n_kf, dk = k - pk; pk = k;
        printf("[M] step=%lu st=%s own=%u x=%.1f y=%.1f cam=%.1f "
               "kf/s=%lu rung@%lu drop=%lu from=%s\n",
               (unsigned long)g_mon.step, mon_st_name(g_mon.st),
               g_mon.owner, g_mon.x, g_mon.y, g_mon.cam,
               (unsigned long)dk, (unsigned long)g_mon.rung_step,
               (unsigned long)g_mon.n_drop, g_mon.from);
    }
}
void mon_net_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_evt, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_evt, NULL, NULL);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    mon_wifi_boot();                     /* saved creds, if any */
    xTaskCreate(rx_task, "mon_rx", 4096, NULL, 10, NULL);
    xTaskCreate(status_task, "mon_stat", 4096, NULL, 5, NULL);
}
