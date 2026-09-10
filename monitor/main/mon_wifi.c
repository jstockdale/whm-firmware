#include <string.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include <stdio.h>
#include "nvs.h"
#include "mon_wifi.h"
static const char *TAG = "mon_wifi";
#define NS "mon"
static esp_err_t load(char *ssid, size_t sn, char *pass, size_t pn)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK)
        return ESP_ERR_NOT_FOUND;
    esp_err_t r = nvs_get_str(h, "wifi_ssid", ssid, &sn);
    if (r == ESP_OK) {
        if (nvs_get_str(h, "wifi_pass", pass, &pn) != ESP_OK)
            pass[0] = 0;
    }
    nvs_close(h);
    return r;
}
static void apply(const char *ssid, const char *pass)
{
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_disconnect();
    esp_wifi_connect();
}
esp_err_t mon_wifi_join(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t r = nvs_open(NS, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    nvs_set_str(h, "wifi_ssid", ssid);
    nvs_set_str(h, "wifi_pass", pass ? pass : "");
    nvs_commit(h); nvs_close(h);
    apply(ssid, pass ? pass : "");
    return ESP_OK;
}
void mon_wifi_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "wifi_ssid");
        nvs_erase_key(h, "wifi_pass");
        nvs_commit(h); nvs_close(h);
    }
    esp_wifi_disconnect();
    printf("wifi creds cleared\n");
}
void mon_wifi_status(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        esp_netif_ip_info_t ip;
        esp_netif_t *nif =
            esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_get_ip_info(nif, &ip);
        printf("connected \"%s\"  rssi %d  ip " IPSTR "\n",
               (char *)ap.ssid, ap.rssi, IP2STR(&ip.ip));
    } else {
        printf("not connected - 'wifi join <ssid> [pw]'\n");
    }
}
int mon_wifi_info(int *rssi, char *ip, int ipn)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return -1;
    *rssi = ap.rssi;
    esp_netif_ip_info_t inf;
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_get_ip_info(nif, &inf);
    snprintf(ip, ipn, IPSTR, IP2STR(&inf.ip));
    return 0;
}
void mon_wifi_boot(void)
{
    char ssid[33], pass[65];
    if (load(ssid, sizeof ssid, pass, sizeof pass) == ESP_OK) {
        ESP_LOGI(TAG, "STA rejoin via saved wifi creds (\"%s\")",
                 ssid);
        apply(ssid, pass);
    } else {
        printf("no wifi creds saved - 'wifi join <ssid> [pw]'\n");
    }
}
