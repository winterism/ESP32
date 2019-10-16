/*
 *   This file is part of DroneBridge: https://github.com/seeul8er/DroneBridge
 *
 *   Copyright 2018 Wolfgang Christl
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 *
 */

#include <sys/fcntl.h>
#include <sys/param.h>
#include <string.h>
#include <esp_task_wdt.h>
#include <esp_vfs_dev.h>
#include "esp_log.h"
#include "lwip/sockets.h"
#include "driver/uart.h"
#include "globals.h"
#include "msp_ltm_serial.h"
#include "db_protocol.h"
#include "db_esp32_control.h"
#include "tcp_server.h"

#define TAG "DB_CONTROL"

uint16_t app_port_proxy = APP_PORT_PROXY;
uint8_t ltm_frame_buffer[MAX_LTM_FRAMES_IN_BUFFER * LTM_MAX_FRAME_SIZE];
uint ltm_frames_in_buffer = 0;
uint ltm_frames_in_buffer_pnt = 0;

int open_serial_socket() {
    int serial_socket;
    uart_config_t uart_config = {
            .baud_rate = DB_UART_BAUD_RATE,
            .data_bits = UART_DATA_8_BITS,
            .parity    = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_2, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_2, DB_UART_PIN_TX, DB_UART_PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_2, 1024, 0, 0, NULL, 0));
    if ((serial_socket = open("/dev/uart/2", O_RDWR)) == -1) {
        ESP_LOGE(TAG, "Cannot open UART2");
        close(serial_socket);
        uart_driver_delete(UART_NUM_2);
        return ESP_FAIL;
    }
    esp_vfs_dev_uart_use_driver(2);
    return serial_socket;
}

/**
 * @brief Parses & sends complete MSP & LTM messages
 */
void parse_msp_ltm(int tcp_clients[], uint8_t msp_message_buffer[], uint *serial_read_bytes,
                   msp_ltm_port_t *db_msp_ltm_port) {
    uint8_t serial_byte;
    if (uart_read_bytes(UART_NUM_2, &serial_byte, 1, 200 / portTICK_RATE_MS) > 0) {
        (*serial_read_bytes)++;
        if (parse_msp_ltm_byte(db_msp_ltm_port, serial_byte)) {
            msp_message_buffer[(*serial_read_bytes - 1)] = serial_byte;
            if (db_msp_ltm_port->parse_state == MSP_PACKET_RECEIVED) {
                *serial_read_bytes = 0;
                send_to_all_tcp_clients(tcp_clients, msp_message_buffer, *serial_read_bytes);
            } else if (db_msp_ltm_port->parse_state == LTM_PACKET_RECEIVED) {
                memcpy(&ltm_frame_buffer[ltm_frames_in_buffer_pnt], db_msp_ltm_port->ltm_frame_buffer,
                       (db_msp_ltm_port->ltm_payload_cnt + 4));
                ltm_frames_in_buffer_pnt += (db_msp_ltm_port->ltm_payload_cnt + 4);
                ltm_frames_in_buffer++;
                if (ltm_frames_in_buffer == LTM_FRAME_NUM_BUFFER &&
                    (LTM_FRAME_NUM_BUFFER <= MAX_LTM_FRAMES_IN_BUFFER)) {
                    send_to_all_tcp_clients(tcp_clients, ltm_frame_buffer, ltm_frames_in_buffer_pnt);
                    ESP_LOGV(TAG, "Sent %i LTM message(s) to telemetry port!", LTM_FRAME_NUM_BUFFER);
                    ltm_frames_in_buffer = 0;
                    ltm_frames_in_buffer_pnt = 0;
                    *serial_read_bytes = 0;
                }
            }
        }
    }
}


/**
 * Reads one byte from UART and checks if we already got enough bytes to send them out
 *
 * @param tcp_clients Array of connected TCP clients
 * @param serial_read_bytes Number of bytes already read for the current packet
 */
void parse_transparent(int tcp_clients[], uint8_t serial_buffer[], uint *serial_read_bytes) {
    uint8_t serial_byte;
    if (uart_read_bytes(UART_NUM_2, &serial_byte, 1, 200 / portTICK_RATE_MS) > 0) {
        serial_buffer[*serial_read_bytes] = serial_byte;
        (*serial_read_bytes)++;
        if (*serial_read_bytes == TRANSPARENT_BUF_SIZE) {
            send_to_all_tcp_clients(tcp_clients, serial_buffer, *serial_read_bytes);
            *serial_read_bytes = 0;
        }
    }
}

void handle_tcp_master(const int tcp_master_socket, int tcp_clients[]) {
    struct sockaddr_in6 source_addr; // Large enough for both IPv4 or IPv6
    uint addr_len = sizeof(source_addr);
    int new_tcp_client = accept(tcp_master_socket, (struct sockaddr *) &source_addr, &addr_len);
    if (new_tcp_client > 0) {
        for (int i = 0; i < CONFIG_LWIP_MAX_ACTIVE_TCP; i++) {
            if (tcp_clients[i] <= 0) {
                tcp_clients[i] = new_tcp_client;
                fcntl(tcp_clients[i], F_SETFL, O_NONBLOCK);
                char addr_str[128];
                inet_ntoa_r(((struct sockaddr_in *) &source_addr)->sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
                ESP_LOGI(TAG, "New client connected: %s", addr_str);
                return;
            }
        }
        ESP_LOGI(TAG, "Could not accept connection. Too many clients connected.");
    }
}

void control_module_tcp() {
    int tcp_master_socket = open_tcp_server(app_port_proxy);
    int uart_socket = open_serial_socket();
    int tcp_clients[CONFIG_LWIP_MAX_ACTIVE_TCP];
    for (int i = 0; i < CONFIG_LWIP_MAX_ACTIVE_TCP; i++) {
        tcp_clients[i] = -1;
    }
    if (tcp_master_socket == ESP_FAIL || uart_socket == ESP_FAIL) {
        ESP_LOGE(TAG, "Can not start control module");
    }
    fcntl(tcp_master_socket, F_SETFL, O_NONBLOCK);
    uint read_transparent = 0;
    uint read_msp_ltm = 0;
    char tcp_client_buffer[TCP_BUFF_SIZ];
    memset(tcp_client_buffer, 0, TCP_BUFF_SIZ);
    uint8_t msp_message_buffer[UART_BUF_SIZE];
    uint8_t serial_buffer[TRANSPARENT_BUF_SIZE];
    msp_ltm_port_t db_msp_ltm_port;

    ESP_LOGI(TAG, "Started control module");
    while (1) {
        handle_tcp_master(tcp_master_socket, tcp_clients);
        for (int i = 0; i < CONFIG_LWIP_MAX_ACTIVE_TCP; i++) {
            if (tcp_clients[i] > 0) {
                ssize_t recv_length = recv(tcp_clients[i], tcp_client_buffer, TCP_BUFF_SIZ, 0);
                if (recv_length > 0) {
                    ESP_LOGD(TAG, "Received %i bytes", recv_length);
                    int written = uart_write_bytes(UART_NUM_2, tcp_client_buffer, (size_t) recv_length);
                    if (written > 0)
                        ESP_LOGD(TAG, "Wrote %i bytes", written);
                    else
                        ESP_LOGE(TAG, "Error writing to UART %s", esp_err_to_name(errno));
                } else if (recv_length == 0) {
                    shutdown(tcp_clients[i], 0);
                    close(tcp_clients[i]);
                    tcp_clients[i] = -1;
                    ESP_LOGI(TAG, "TCP client disconnected");
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    ESP_LOGE(TAG, "Error receiving from TCP client %i (fd: %i): %d", i, tcp_clients[i], errno);
                    shutdown(tcp_clients[i], 0);
                    close(tcp_clients[i]);
                    tcp_clients[i] = -1;
                }
            }
        }
        switch (SERIAL_PROTOCOL) {
                case 1:
                case 2:
                    parse_msp_ltm(tcp_clients, msp_message_buffer, &read_msp_ltm, &db_msp_ltm_port);
                    break;
                default:
                case 3:
                case 4:
                case 5:
                    parse_transparent(tcp_clients, serial_buffer, &read_transparent);
                    break;
        }
    }
    vTaskDelete(NULL);
}

/**
 * @brief DroneBridge control module implementation for a ESP32 device. Bi-directional link between FC and ground. Can
 * handle MSPv1, MSPv2, LTM and MAVLink.
 * MSP & LTM is parsed and sent packet/frame by frame to ground
 * MAVLink is passed through (fully transparent). Can be used with any protocol.
 */
void control_module() {
    xEventGroupWaitBits(wifi_event_group, BIT2, false, true, portMAX_DELAY);
    xTaskCreate(&control_module_tcp, "control_tcp", 40960, NULL, 5, NULL);
}