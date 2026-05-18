/*
 * main.c
 *
 * Application Zephyr : HTTP TCP Client + LED blinky
 * Board : heltec_wifi_lora32_v3/esp32s3/procpu
 * Zephyr : 4.4
 *
 * CORRECTION v4 :
 *   - on_wifi_event et on_dhcp_event : uint32_t → uint64_t
 *     (net_mgmt_event_handler_t utilise uint64_t depuis Zephyr 4.x)
 */

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/socket.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(http_client, LOG_LEVEL_INF);

/* ==================================================================
 * PARAMÈTRES À ADAPTER AVANT COMPILATION
 * ================================================================== */
#define WIFI_SSID    "Galaxy A15 AAFB"
#define WIFI_PSK     "validons"
#define SERVER_IP    "10.65.42.239"   /* IP de ton PC sur le hotspot */
#define SERVER_PORT  8080


/* ==================================================================
 * LED onboard
 * ================================================================== */
#define LED_NODE DT_ALIAS(led0)

#if DT_NODE_HAS_STATUS(LED_NODE, okay)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED_NODE, gpios);
#define HAS_LED 1
#else
#define HAS_LED 0
#endif


/* ==================================================================
 * Sémaphore réseau
 * ================================================================== */
static K_SEM_DEFINE(net_ready, 0, 1);

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback dhcp_cb;


/* ==================================================================
 * Callback Wi-Fi
 *
 * CORRECTION : event est uint64_t dans Zephyr 4.4
 * (net_mgmt_event_handler_t attend uint64_t depuis Zephyr 4.x)
 * ================================================================== */
static void on_wifi_event(struct net_mgmt_event_callback *cb,
                           uint64_t event,          /* ← uint64_t */
                           struct net_if *iface)
{
    if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
        const struct wifi_status *s =
            (const struct wifi_status *)cb->info;

        if (s->conn_status == WIFI_STATUS_CONN_SUCCESS) {
            LOG_INF("Wi-Fi associé : SSID=\"%s\"", WIFI_SSID);
        } else {
            LOG_ERR("Échec connexion Wi-Fi, conn_status=%d",
                    s->conn_status);
        }

    } else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
        LOG_WRN("Wi-Fi déconnecté");
    }
}


/* ==================================================================
 * Callback DHCP
 *
 * CORRECTION : event est uint64_t dans Zephyr 4.4
 * ================================================================== */
static void on_dhcp_event(struct net_mgmt_event_callback *cb,
                           uint64_t event,          /* ← uint64_t */
                           struct net_if *iface)
{
    if (event == NET_EVENT_IPV4_DHCP_BOUND) {
        char ip_str[NET_IPV4_ADDR_LEN];

        net_addr_ntop(AF_INET,
                      &iface->config.dhcpv4.requested_ip,
                      ip_str, sizeof(ip_str));

        LOG_INF("IP DHCP obtenue  : %s", ip_str);
        LOG_INF("Serveur Python   : %s:%d", SERVER_IP, SERVER_PORT);

        k_sem_give(&net_ready);
    }
}


/* ==================================================================
 * wifi_connect()
 * ================================================================== */
static int wifi_connect(void)
{
    struct net_if *iface = net_if_get_default();

    if (!iface) {
        LOG_ERR("Aucune interface réseau disponible");
        return -ENODEV;
    }

    net_mgmt_init_event_callback(
        &wifi_cb, on_wifi_event,
        NET_EVENT_WIFI_CONNECT_RESULT |
        NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&wifi_cb);

    net_mgmt_init_event_callback(
        &dhcp_cb, on_dhcp_event,
        NET_EVENT_IPV4_DHCP_BOUND);
    net_mgmt_add_event_callback(&dhcp_cb);

    struct wifi_connect_req_params params = {
        .ssid        = (const uint8_t *)WIFI_SSID,
        .ssid_length = (uint8_t)strlen(WIFI_SSID),
        .psk         = (const uint8_t *)WIFI_PSK,
        .psk_length  = (uint8_t)strlen(WIFI_PSK),
        .channel     = WIFI_CHANNEL_ANY,
        .security    = WIFI_SECURITY_TYPE_PSK,
        .mfp         = WIFI_MFP_OPTIONAL,
        .band        = WIFI_FREQ_BAND_2_4_GHZ,
        .timeout     = SYS_FOREVER_MS,
    };

    LOG_INF("Connexion Wi-Fi → SSID : \"%s\"", WIFI_SSID);

    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT,
                       iface, &params, sizeof(params));
    if (ret != 0) {
        LOG_ERR("net_mgmt NET_REQUEST_WIFI_CONNECT : %d", ret);
        return ret;
    }

    net_dhcpv4_start(iface);

    LOG_INF("Attente IP DHCP (max 30s)...");
    ret = k_sem_take(&net_ready, K_SECONDS(30));
    if (ret != 0) {
        LOG_ERR("Timeout : pas d'IP DHCP après 30s");
        LOG_ERR("Vérifier hotspot \"%s\" actif en 2,4 GHz", WIFI_SSID);
        return -ETIMEDOUT;
    }

    return 0;
}


/* ==================================================================
 * blink_led()
 * ================================================================== */
static void blink_led(void)
{
#if HAS_LED
    gpio_pin_set_dt(&led, 1);
    k_msleep(150);
    gpio_pin_set_dt(&led, 0);
    k_msleep(150);
#endif
}


/* ==================================================================
 * send_http_post()
 *
 * Flux BSD Sockets Zephyr :
 *   zsock_socket()     → créer socket TCP
 *   zsock_setsockopt() → configurer timeout 5s
 *   zsock_connect()    → three-way handshake TCP
 *   zsock_send()       → envoyer requête HTTP POST
 *   zsock_recv()       → lire réponse ACK
 *   zsock_close()      → fermer connexion TCP
 * ================================================================== */
static int send_http_post(int seq)
{
    char tx_buf[512];
    char rx_buf[512];
    struct sockaddr_in server_addr;
    int sock;
    int ret;

    /* 1. Créer le socket TCP */
    sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        LOG_ERR("[%d] zsock_socket() : errno=%d", seq, errno);
        return -errno;
    }

    /* 2. Timeout réception et émission : 5 secondes */
    struct zsock_timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* 3. Adresse du serveur Python */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(SERVER_PORT);

    ret = zsock_inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);
    if (ret != 1) {
        LOG_ERR("[%d] IP invalide : \"%s\"", seq, SERVER_IP);
        zsock_close(sock);
        return -EINVAL;
    }

    /* 4. Connexion TCP (SYN → SYN-ACK → ACK) */
    LOG_INF("[%d] Connexion TCP → %s:%d ...", seq, SERVER_IP, SERVER_PORT);
    ret = zsock_connect(sock,
                        (struct sockaddr *)&server_addr,
                        sizeof(server_addr));
    if (ret < 0) {
        LOG_ERR("[%d] zsock_connect() : errno=%d", seq, errno);
        zsock_close(sock);
        return -errno;
    }
    LOG_INF("[%d] Connexion TCP établie", seq);

    /* 5. Construire requête HTTP POST */
    char body[128];
    snprintf(body, sizeof(body),
             "{\"device\":\"heltec_v3\","
             "\"seq\":%d,"
             "\"metric\":\"ping\","
             "\"value\":1}",
             seq);

    int body_len = strlen(body);

    int tx_len = snprintf(tx_buf, sizeof(tx_buf),
        "POST /data HTTP/1.0\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        SERVER_IP, SERVER_PORT,
        body_len, body);

    /* 6. Envoyer la requête */
    LOG_INF("[%d] Envoi HTTP POST (%d octets)...", seq, tx_len);
    ret = zsock_send(sock, tx_buf, tx_len, 0);
    if (ret < 0) {
        LOG_ERR("[%d] zsock_send() : errno=%d", seq, errno);
        zsock_close(sock);
        return -errno;
    }
    LOG_INF("[%d] %d octets envoyés", seq, ret);

    /* 7. Lire la réponse ACK du serveur */
    memset(rx_buf, 0, sizeof(rx_buf));
    ret = zsock_recv(sock, rx_buf, sizeof(rx_buf) - 1, 0);
    if (ret < 0) {
        LOG_ERR("[%d] zsock_recv() : errno=%d", seq, errno);
        zsock_close(sock);
        return -errno;
    }
    if (ret == 0) {
        LOG_WRN("[%d] Connexion fermée sans réponse", seq);
        zsock_close(sock);
        return -ECONNRESET;
    }

    rx_buf[ret] = '\0';

    /* Extraire le corps de la réponse (après \r\n\r\n) */
    char *resp_body = strstr(rx_buf, "\r\n\r\n");
    if (resp_body != NULL) {
        resp_body += 4;
        LOG_INF("[%d] *** ACK SERVEUR : %s ***", seq, resp_body);
    } else {
        LOG_INF("[%d] Réponse brute : %s", seq, rx_buf);
    }

    /* 8. Fermer le socket TCP */
    zsock_close(sock);
    LOG_INF("[%d] Socket TCP fermé", seq);

    return 0;
}


/* ==================================================================
 * main()
 * ================================================================== */
int main(void)
{
    LOG_INF("============================================");
    LOG_INF(" Zephyr HTTP TCP Client + Blinky  v4");
    LOG_INF(" Board   : heltec_wifi_lora32_v3");
    LOG_INF(" SSID    : %s", WIFI_SSID);
    LOG_INF(" Serveur : %s:%d", SERVER_IP, SERVER_PORT);
    LOG_INF("============================================");

#if HAS_LED
    if (device_is_ready(led.port)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
        LOG_INF("LED initialisée (pin %d)", led.pin);
    }
#endif

    int ret = wifi_connect();
    if (ret != 0) {
        LOG_ERR("Connexion réseau échouée : %d", ret);
        return ret;
    }

    LOG_INF("Réseau prêt — HTTP POST toutes les 3s vers %s:%d",
            SERVER_IP, SERVER_PORT);

    int seq = 0;

    while (1) {
        seq++;
        blink_led();

        ret = send_http_post(seq);
        if (ret != 0) {
            LOG_WRN("Envoi [%d] échoué (err=%d), retry dans 3s",
                    seq, ret);
        }

        k_sleep(K_SECONDS(3));
    }

    return 0;
}