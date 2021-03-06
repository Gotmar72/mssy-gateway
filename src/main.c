#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "hal.h"
#include "phy.h"
#include "sys.h"
#include "nwk.h"
#include "sysTimer.h"
#include "protocols.h"
#include "uart.h"
#include "database.h"
#include "command_context.h"

static void send_data(uint16_t app_endpoint, void *data, size_t length);

void handle_command(struct command_t *command, uint16_t endpoint);

int uart_putchar(char byte, FILE *stream);

void send_discovery_request(uint16_t endpoint);

static FILE mystdout = FDEV_SETUP_STREAM(&uart_putchar, NULL, _FDEV_SETUP_WRITE);

static NWK_DataReq_t appDataReq;
static uint8_t data_buffer[APP_BUFFER_SIZE];
static uint8_t buffer_position = 0;
uint8_t ready_to_send = 0;

char uart_buffer[UART_BUFFER_LEN];
volatile uint8_t uart_int = 0;

static void data_confirmation(NWK_DataReq_t *req) {
    memset(data_buffer, 0, APP_BUFFER_SIZE);
    ready_to_send = 1;
    (void) req;
}

static void send_data(uint16_t app_addr, void *data, size_t length) {
    if (length == 0 || ready_to_send == 0) {
        return;
    }

    memcpy(data_buffer, data, length);

    appDataReq.dstAddr = app_addr;
    appDataReq.dstEndpoint = APP_ENDPOINT_10;
    appDataReq.srcEndpoint = APP_ENDPOINT_10;
    appDataReq.options = NWK_OPT_ENABLE_SECURITY;
    appDataReq.data = data_buffer;
    appDataReq.size = length;
    appDataReq.confirm = data_confirmation;
    NWK_DataReq(&appDataReq);

    buffer_position = 0;
    ready_to_send = 0;
}

static bool data_received(NWK_DataInd_t *ind) {
    union command_packet_t packet = {
            .bytes = {0}
    };

    memcpy(packet.bytes, ind->data, ind->size);
    handle_command(&packet.command, ind->srcAddr);
    return true;
}

void handle_command(struct command_t *command, uint16_t endpoint) {
    switch (command->header.command_id) {
        case COMMAND_CONNECT:
            if (add_endpoint(endpoint) != SUCCESS) {
                printf("Adding of endpoint %d has failed.\n", endpoint);
            } else {
                printf("New endpoint %d has been added.\n", endpoint);
                send_discovery_request(endpoint);
            }
            break;
        case COMMAND_DISCOVERY_RESPONSE:
            if (index_of(endpoint) != ERR_NOT_FOUND) {
                printf("Got discovery response from %d.\n", endpoint);
                store_devices(endpoint, command->data, command->header.len);
            } else {
                printf("Got discovery response from %d which is not in db!\n", endpoint);
            }
            break;
        case COMMAND_READ_RESPONSE:
            if (index_of(endpoint) != ERR_NOT_FOUND) {
                union device_packet_t packet = {
                        .bytes = {0}
                };
                memcpy(packet.bytes, command->data, command->header.len);
                size_t data_len = packet.device.header.len;
                printf("Read response from %d with len %d.\n", endpoint, data_len);
                for (uint8_t i = 0; i < data_len; i++) {
                    printf("Data on %d = %d\n", i, packet.device.data[i]);
                }
            } else {
                printf("Read response from %d has failed.\n", endpoint);
            }
            break;
        case COMMAND_DESCRIPTION_RESPONSE:
            if (index_of(endpoint) != ERR_NOT_FOUND) {
                union device_packet_t packet = {
                        .bytes = {0}
                };
                memcpy(packet.bytes, command->data, command->header.len);
                size_t data_len = packet.device.header.len;
                printf("Read response from %d with len %d.\n", endpoint, data_len);
                for (uint8_t i = 0; i < data_len; i++) {
                    printf("%c", packet.device.data[i]);
                }
                printf("\n");
            } else {
                printf("Description response from %d has failed.\n", endpoint);
            }
            break;

    }
}

void send_discovery_request(uint16_t endpoint) {
    uint8_t packet_buffer[APP_BUFFER_SIZE];

    uint8_t len = create_command_packet(packet_buffer, COMMAND_DISCOVERY_REQUEST, 0, 0);

    send_data(endpoint, packet_buffer, len);
    printf("Sending discovery to %d.\n", endpoint);
}

static void app_init(void) {
    NWK_SetAddr(APP_ADDR);
    NWK_SetPanId(APP_PANID);
    PHY_SetChannel(APP_CHANNEL);

    PHY_SetRxState(true);

    NWK_OpenEndpoint(APP_ENDPOINT_10, data_received);

    uart_init(38400);
    init_database();
    ready_to_send = 1;
    switch_context(CONTEXT_NORMAL);
}

static void task_handler(void) {
    if (uart_int) {
        memset((void *) uart_buffer, 0, UART_BUFFER_LEN);
        uart_recv_string((void *) uart_buffer);
        decode_command(uart_buffer);
    }
}

int main() {
    stdout = &mystdout;
    SYS_Init();
    app_init();

    while (1) {
        SYS_TaskHandler();
        task_handler();
    }
}

int uart_putchar(char byte, FILE *stream) {
    if (byte == '\n') {
        uart_send('\r');
    }
    uart_send(byte);

    return 0;
}

void menu_print_endpoints() {
    print_endpoints();
}

void menu_print_devices() {
    print_endpoints();
    printf("Enter id of endpoint:\n");
    uint16_t endpoint = read_and_convert();

    if (index_of(endpoint) == ERR_NOT_FOUND) {
        printf("Endpoint %d not found.\n", endpoint);
        return;
    }
    print_devices(endpoint);
}

void menu_read() {
    print_endpoints();
    printf("Enter id of endpoint:\n");
    uint16_t endpoint = read_and_convert();
    int8_t endpoint_index = index_of(endpoint);

    if (endpoint_index == ERR_NOT_FOUND) {
        printf("Endpoint %d not found.\n", endpoint);
        return;
    }

    print_devices(endpoint);
    printf("Enter index of device:\n");
    int8_t device_index = read_and_convert();

    if (has_endpoint_device(endpoint, device_index) == ERR_NOT_FOUND) {
        printf("Device %d not found.\n", endpoint);
        return;
    }

    uint8_t device_packet_buffer[APP_BUFFER_SIZE];
    uint8_t command_packet_buffer[APP_BUFFER_SIZE];
    struct device_header_t device_header = get_devices(endpoint)[device_index];
    uint8_t device_packet_size = create_device_packet(device_packet_buffer, device_header, 0, 0);
    uint8_t command_packet_size = create_command_packet(command_packet_buffer, COMMAND_READ_REQUEST,
                                                        device_packet_buffer, device_packet_size);
    send_data(endpoint, command_packet_buffer, command_packet_size);
}

void menu_description() {
    print_endpoints();
    printf("Enter id of endpoint:\n");
    uint16_t endpoint = read_and_convert();

    if (index_of(endpoint) == ERR_NOT_FOUND) {
        printf("Endpoint %d not found.\n", endpoint);
        return;
    }

    print_devices(endpoint);
    printf("Write device index:\n");
    int8_t device_index = read_and_convert();

    uint8_t device_packet_buffer[APP_BUFFER_SIZE];
    uint8_t command_packet_buffer[APP_BUFFER_SIZE];
    struct device_header_t device_header = get_devices(endpoint)[device_index];
    uint8_t device_packet_size = create_device_packet(device_packet_buffer, device_header, 0, 0);
    uint8_t command_packet_size = create_command_packet(command_packet_buffer, COMMAND_DESCRIPTION_REQUEST,
                                                        device_packet_buffer, device_packet_size);
    send_data(endpoint, command_packet_buffer, command_packet_size);
}

void menu_write() {
    print_endpoints();
    printf("Enter id of endpoint:\n");
    uint16_t endpoint = read_and_convert();

    if (index_of(endpoint) == ERR_NOT_FOUND) {
        printf("Endpoint %d not found.\n", endpoint);
        return;
    }

    print_devices(endpoint);
    printf("Write device index:\n");
    int8_t device_index = read_and_convert();

    printf("Write value:\n");
    int8_t value = read_and_convert();
    int8_t values[1] = {value};

    uint8_t device_packet_buffer[APP_BUFFER_SIZE];
    uint8_t command_packet_buffer[APP_BUFFER_SIZE];
    struct device_header_t device_header = get_devices(endpoint)[device_index];
    uint8_t device_packet_size = create_device_packet(device_packet_buffer, device_header, values, 1);
    uint8_t command_packet_size = create_command_packet(command_packet_buffer, COMMAND_WRITE, device_packet_buffer,
                                                        device_packet_size);
    send_data(endpoint, command_packet_buffer, command_packet_size);
    printf("Write request sent to endpoint %d, device %d\n", endpoint, device_index);
}

void menu_disconnect() {
    print_endpoints();
    printf("Enter id of endpoint to disconnect:\n");
    uint16_t endpoint = read_and_convert();

    if (index_of(endpoint) == ERR_NOT_FOUND) {
        return;
    }
    remove_endpoint(endpoint);
    printf("Endpoint %d has been removed.\n", endpoint);
}
