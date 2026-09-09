#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "mon_config.h"
#include "mon_parse.h"
static const char *TAG = "mon_net";
static void wifi_evt(void *a, esp_event_base_t base, int32_t id, void *d)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
        esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "STA disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)d;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&e->ip_info.ip));
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
    for (;;) {
        int n = recvfrom(s, buf, sizeof(buf), 0, NULL, NULL);
        if (n > 0) mon_parse_pkt(buf, n);
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
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, MON_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, MON_WIFI_PASS,
            sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    xTaskCreate(rx_task, "mon_rx", 4096, NULL, 10, NULL);
    xTaskCreate(status_task, "mon_stat", 4096, NULL, 5, NULL);
}
