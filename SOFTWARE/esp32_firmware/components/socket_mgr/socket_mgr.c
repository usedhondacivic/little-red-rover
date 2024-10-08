#include "socket_mgr.h"

#include "freertos/idf_additions.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/projdefs.h"
#include "lwip/sockets.h"
#include "nvs.h"

#include "messages.pb.h"
#include "pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "pb_utils.h"
#include "portmacro.h"
#include "status_led_driver.h"

#define SOCKET_TX_TASK_STACK_SIZE 4096
#define SOCKET_RX_TASK_STACK_SIZE 4096
#define MAX_RX_CALLBACKS 1

static const char *TAG = "SOCKET_MGR";

static char AGENT_IP[16];
#define PORT 8001

esp_err_t get_agent_ip()
{
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    } else {
        size_t l = sizeof(AGENT_IP);
        err = nvs_get_str(my_handle, "uros_ag_ip", AGENT_IP, &l);
        switch (err) {
            case ESP_OK:
                ESP_LOGI(TAG, "Retrieved IP (%s) for agent IP.", AGENT_IP);
                break;
            case ESP_ERR_NVS_NOT_FOUND:
                ESP_LOGI(TAG, "Agent IP has not been set.");
                nvs_close(my_handle);
                return ESP_FAIL;
                break;
            default:
                ESP_LOGI(TAG, "Error (%s) reading!\n", esp_err_to_name(err));
                nvs_close(my_handle);
                return ESP_FAIL;
        }

        nvs_close(my_handle);
    }
    return ESP_OK;
}

static int socket_id;

QueueHandle_t tx_queue = NULL;

static unsigned char tx_buffer[1500];
struct sockaddr_in dest_addr;

static void socket_tx_task(void *arg)
{
    UdpPacket msg;

    while (1) {
        if (xQueueReceive(tx_queue, (void *)&msg, portMAX_DELAY) == pdTRUE) {
            pb_ostream_t stream =
              pb_ostream_from_buffer(tx_buffer, sizeof(tx_buffer));
            bool status = pb_encode(&stream, UdpPacket_fields, &msg);
            if (!status) {
                ESP_LOGE(TAG, "Failed to serialize message.");
            }

            ssize_t sent = sendto(socket_id,
                                  tx_buffer,
                                  stream.bytes_written,
                                  0,
                                  (struct sockaddr *)&dest_addr,
                                  sizeof(dest_addr));

            // This fails whenever a client isn't emptying the network buffer,
            // commented out for now.
            // if (sent != stream.bytes_written) {
            //     ESP_LOGE(TAG,
            //              "Failed to write full packet data. Wrote %ld, "
            //              "expected %zu.",
            //              (long)sent,
            //              stream.bytes_written);
            // }
        }
    }
}

static unsigned char rx_buffer[1500];
static void (*rx_callbacks[MAX_RX_CALLBACKS])(void *);

static void socket_rx_task(void *arg)
{
    struct sockaddr_storage source_addr;
    socklen_t socklen = sizeof(source_addr);
    while (1) {
        int len = recvfrom(socket_id,
                           rx_buffer,
                           sizeof(rx_buffer),
                           0,
                           (struct sockaddr *)&source_addr,
                           &socklen);

        if (len < 0) {
            ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
            return;
        } else {
            pb_istream_t stream = pb_istream_from_buffer(rx_buffer, len);

            // TODO: Get rid of the union stuff, just pass the UdpPacket message
            const pb_msgdesc_t *type = decode_unionmessage_type(&stream);
            bool status = false;

            if (type == TwistCmd_fields) {
                TwistCmd msg = {};
                status =
                  decode_unionmessage_contents(&stream, TwistCmd_fields, &msg);
                (*(rx_callbacks[eTwistCmd]))(&msg);
            }

            if (!status) {
                ESP_LOGE(TAG, "Decode failed: %s\n", PB_GET_ERROR(&stream));
            }
        }
    }
}

void register_callback(void (*callback)(void *), eRxMsgTypes type)
{
    rx_callbacks[type] = callback;
}

void socket_mgr_init()
{
    set_status(eAgentDisconnected);
    while (get_agent_ip() != ESP_OK) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }

    dest_addr.sin_addr.s_addr = inet_addr(AGENT_IP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(PORT);

    struct sockaddr_in src_addr;
    src_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(PORT);

    socket_id = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);

    if (socket_id < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
    }

    int err = bind(socket_id, (struct sockaddr *)&src_addr, sizeof(src_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
    } else {
        set_status(eAgentConnected);
        ESP_LOGI(TAG, "Socket bound, port %d", PORT);
    }

    ESP_LOGI(TAG, "Socket created, communicating with %s:%d", AGENT_IP, PORT);

    tx_queue = xQueueCreate(25, sizeof(UdpPacket));

    xTaskCreatePinnedToCore(socket_tx_task,
                            "socket_tx_task",
                            SOCKET_TX_TASK_STACK_SIZE,
                            NULL,
                            10,
                            NULL,
                            APP_CPU_NUM);

    xTaskCreatePinnedToCore(socket_rx_task,
                            "socket_rx_task",
                            SOCKET_TX_TASK_STACK_SIZE,
                            NULL,
                            10,
                            NULL,
                            APP_CPU_NUM);
}
