// Copyright 2015-2018 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "esp_err.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"
// #include "esp_websocket_client.h"

/* using uri parser */
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_parser.h"

typedef struct esp_websocket_client *esp_websocket_client_handle_t;

ESP_EVENT_DECLARE_BASE(WEBSOCKET_EVENTS); // declaration of the task events family

/**
 * @brief Websocket Client events id
 */
typedef enum {
    WEBSOCKET_EVENT_ANY = -1,
    WEBSOCKET_EVENT_ERROR = 0,    /*!< This event occurs when there are any errors during execution */
    WEBSOCKET_EVENT_CONNECTED,    /*!< Once the Websocket has been connected to the server, no data exchange has been performed */
    WEBSOCKET_EVENT_DISCONNECTED, /*!< The connection has been disconnected */
    WEBSOCKET_EVENT_DATA,         /*!< When receiving data from the server, possibly multiple portions of the packet */
    WEBSOCKET_EVENT_MAX
} esp_websocket_event_id_t;

/**
 * @brief Websocket event data
 */
typedef struct {
    const char *data_ptr;                 /*!< Data pointer */
    int data_len;                         /*!< Data length */
    uint8_t op_code;                      /*!< Received opcode */
    esp_websocket_client_handle_t client; /*!< esp_websocket_client_handle_t context */
    void *user_context;                   /*!< user_data context, from esp_websocket_client_config_t user_data */
    int payload_len;                      /*!< Total payload length, payloads exceeding buffer will be posted through multiple events */
    int payload_offset;                   /*!< Actual offset for the data associated with this event */
} esp_websocket_event_data_t;

/**
 * @brief Websocket Client transport
 */
typedef enum {
    WEBSOCKET_TRANSPORT_UNKNOWN = 0x0, /*!< Transport unknown */
    WEBSOCKET_TRANSPORT_OVER_TCP,      /*!< Transport over tcp */
    WEBSOCKET_TRANSPORT_OVER_SSL,      /*!< Transport over ssl */
} esp_websocket_transport_t;

/**
 * @brief Websocket client setup configuration
 */
typedef struct {
    const char *uri;                     /*!< Websocket URI, the information on the URI can be overrides the other fields below, if any */
    const char *host;                    /*!< Domain or IP as string */
    int port;                            /*!< Port to connect, default depend on esp_websocket_transport_t (80 or 443) */
    const char *username;                /*!< Using for Http authentication - Not supported for now */
    const char *password;                /*!< Using for Http authentication - Not supported for now */
    const char *path;                    /*!< HTTP Path, if not set, default is `/` */
    bool disable_auto_reconnect;         /*!< Disable the automatic reconnect function when disconnected */
    void *user_context;                  /*!< HTTP user data context */
    int task_prio;                       /*!< Websocket task priority */
    int task_stack;                      /*!< Websocket task stack */
    int buffer_size;                     /*!< Websocket buffer size */
    const char *cert_pem;                /*!< SSL Certification, PEM format as string, if the client requires to verify server */
    esp_websocket_transport_t transport; /*!< Websocket transport type, see `esp_websocket_transport_t */
    char *subprotocol;                   /*!< Websocket subprotocol */
    char *user_agent;                    /*!< Websocket user-agent */
    char *headers;                       /*!< Websocket additional headers */
    int pingpong_timeout_sec;            /*!< Period before connection is aborted due to no PONGs received */
    bool disable_pingpong_discon;        /*!< Disable auto-disconnect due to no PONG received within pingpong_timeout_sec */

} esp_websocket_client_config_t;

static const char *TAG = "WEBSOCKET_CLIENT";

#define WEBSOCKET_TCP_DEFAULT_PORT (80)
#define WEBSOCKET_SSL_DEFAULT_PORT (443)
#define WEBSOCKET_BUFFER_SIZE_BYTE (1024)
#define WEBSOCKET_RECONNECT_TIMEOUT_MS (10 * 1000)
#define WEBSOCKET_TASK_PRIORITY (5)
#define WEBSOCKET_TASK_STACK (4 * 1024)
#define WEBSOCKET_NETWORK_TIMEOUT_MS (10 * 1000)
#define WEBSOCKET_PING_TIMEOUT_MS (10 * 1000)
#define WEBSOCKET_EVENT_QUEUE_SIZE (1)
#define WEBSOCKET_PINGPONG_TIMEOUT_SEC (120)

#define ESP_WS_CLIENT_MEM_CHECK(TAG, a, action)                                                \
    if (!(a)) {                                                                                \
        ESP_LOGE(TAG, "%s:%d (%s): %s", __FILE__, __LINE__, __FUNCTION__, "Memory exhausted"); \
        action;                                                                                \
    }

#define ESP_WS_CLIENT_STATE_CHECK(TAG, a, action)                                                    \
    if ((a->state) < WEBSOCKET_STATE_INIT) {                                                         \
        ESP_LOGE(TAG, "%s:%d (%s): %s", __FILE__, __LINE__, __FUNCTION__, "Websocket already stop"); \
        action;                                                                                      \
    }

const static int STOPPED_BIT = BIT0;

ESP_EVENT_DEFINE_BASE(WEBSOCKET_EVENTS);

typedef struct {
    int task_stack;
    int task_prio;
    char *uri;
    char *host;
    char *path;
    char *scheme;
    char *username;
    char *password;
    int port;
    bool auto_reconnect;
    void *user_context;
    int network_timeout_ms;
    char *subprotocol;
    char *user_agent;
    char *headers;
    int pingpong_timeout_sec;
} websocket_config_storage_t;

typedef enum {
    WEBSOCKET_STATE_ERROR = -1,
    WEBSOCKET_STATE_UNKNOW = 0,
    WEBSOCKET_STATE_INIT,
    WEBSOCKET_STATE_CONNECTED,
    WEBSOCKET_STATE_WAIT_TIMEOUT,
} websocket_client_state_t;

struct esp_websocket_client {
    esp_event_loop_handle_t event_handle;
    esp_transport_list_handle_t transport_list;
    esp_transport_handle_t transport;
    websocket_config_storage_t *config;
    websocket_client_state_t state;
    uint64_t keepalive_tick_ms;
    uint64_t reconnect_tick_ms;
    uint64_t ping_tick_ms;
    uint64_t pingpong_tick_ms;
    int wait_timeout_ms;
    int auto_reconnect;
    bool run;
    bool wait_for_pong_resp;
    EventGroupHandle_t status_bits;
    xSemaphoreHandle lock;
    char *rx_buffer;
    char *tx_buffer;
    int buffer_size;
    ws_transport_opcodes_t last_opcode;
    int payload_len;
    int payload_offset;
};

static uint64_t _tick_get_ms(void) {
    return esp_timer_get_time() / 1000;
}

/**
 * @brief      Start a Websocket session
 *             This function must be the first function to call,
 *             and it returns a esp_websocket_client_handle_t that you must use as input to other functions in the interface.
 *             This call MUST have a corresponding call to esp_websocket_client_destroy when the operation is complete.
 *
 * @param[in]  config  The configuration
 *
 * @return
 *     - `esp_websocket_client_handle_t`
 *     - NULL if any errors
 */
esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *config);

/**
 * @brief      Set URL for client, when performing this behavior, the options in the URL will replace the old ones
 *             Must stop the WebSocket client before set URI if the client has been connected
 *
 * @param[in]  client  The client
 * @param[in]  uri     The uri
 *
 * @return     esp_err_t
 */
esp_err_t esp_websocket_client_set_uri(esp_websocket_client_handle_t client, const char *uri);

/**
 * @brief      Open the WebSocket connection
 *
 * @param[in]  client  The client
 *
 * @return     esp_err_t
 */
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t client);

/**
 * @brief      Close the WebSocket connection
 *
 * @param[in]  client  The client
 *
 * @return     esp_err_t
 */
esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t client);

/**
 * @brief      Destroy the WebSocket connection and free all resources.
 *             This function must be the last function to call for an session.
 *             It is the opposite of the esp_websocket_client_init function and must be called with the same handle as input that a esp_websocket_client_init call returned.
 *             This might close all connections this handle has used.
 *
 * @param[in]  client  The client
 *
 * @return     esp_err_t
 */
esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t client);

/**
 * @brief      Generic write data to the WebSocket connection; defaults to binary send
 *
 * @param[in]  client  The client
 * @param[in]  data    The data
 * @param[in]  len     The length
 * @param[in]  timeout Write data timeout in RTOS ticks
 *
 * @return
 *     - Number of data was sent
 *     - (-1) if any errors
 */
int esp_websocket_client_send(esp_websocket_client_handle_t client, const char *data, int len, TickType_t timeout);

/**
 * @brief      Write binary data to the WebSocket connection (data send with WS OPCODE=02, i.e. binary)
 *
 * @param[in]  client  The client
 * @param[in]  data    The data
 * @param[in]  len     The length
 * @param[in]  timeout Write data timeout in RTOS ticks
 *
 * @return
 *     - Number of data was sent
 *     - (-1) if any errors
 */
int esp_websocket_client_send_bin(esp_websocket_client_handle_t client, const char *data, int len, TickType_t timeout);

/**
 * @brief      Write textual data to the WebSocket connection (data send with WS OPCODE=01, i.e. text)
 *
 * @param[in]  client  The client
 * @param[in]  data    The data
 * @param[in]  len     The length
 * @param[in]  timeout Write data timeout in RTOS ticks
 *
 * @return
 *     - Number of data was sent
 *     - (-1) if any errors
 */
int esp_websocket_client_send_text(esp_websocket_client_handle_t client, const char *data, int len, TickType_t timeout);

/**
 * @brief      Check the WebSocket client connection state
 *
 * @param[in]  client  The client handle
 *
 * @return
 *     - true
 *     - false
 */
bool esp_websocket_client_is_connected(esp_websocket_client_handle_t client);

/**
 * @brief Register the Websocket Events
 *
 * @param client            The client handle
 * @param event             The event id
 * @param event_handler     The callback function
 * @param event_handler_arg User context
 * @return esp_err_t
 */
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t client,
                                        esp_websocket_event_id_t event,
                                        esp_event_handler_t event_handler,
                                        void *event_handler_arg);

static esp_err_t esp_websocket_client_dispatch_event(esp_websocket_client_handle_t client,
                                                     esp_websocket_event_id_t event,
                                                     const char *data,
                                                     int data_len) {
    esp_err_t err;
    esp_websocket_event_data_t event_data;

    event_data.client = client;
    event_data.user_context = client->config->user_context;
    event_data.data_ptr = data;
    event_data.data_len = data_len;
    event_data.op_code = client->last_opcode;
    event_data.payload_len = client->payload_len;
    event_data.payload_offset = client->payload_offset;

    if ((err = esp_event_post_to(client->event_handle,
                                 WEBSOCKET_EVENTS, event,
                                 &event_data,
                                 sizeof(esp_websocket_event_data_t),
                                 portMAX_DELAY)) != ESP_OK) {
        return err;
    }
    return esp_event_loop_run(client->event_handle, 0);
}

static esp_err_t esp_websocket_client_abort_connection(esp_websocket_client_handle_t client) {
    ESP_WS_CLIENT_STATE_CHECK(TAG, client, return ESP_FAIL);
    esp_transport_close(client->transport);

    if (client->config->auto_reconnect) {
        client->wait_timeout_ms = WEBSOCKET_RECONNECT_TIMEOUT_MS;
        client->reconnect_tick_ms = _tick_get_ms();
        ESP_LOGI(TAG, "Reconnect after %d ms", client->wait_timeout_ms);
    }
    client->state = WEBSOCKET_STATE_WAIT_TIMEOUT;
    esp_websocket_client_dispatch_event(client, WEBSOCKET_EVENT_DISCONNECTED, NULL, 0);
    return ESP_OK;
}

static esp_err_t esp_websocket_client_set_config(esp_websocket_client_handle_t client, const esp_websocket_client_config_t *config) {
    websocket_config_storage_t *cfg = client->config;
    cfg->task_prio = config->task_prio;
    if (cfg->task_prio <= 0) {
        cfg->task_prio = WEBSOCKET_TASK_PRIORITY;
    }

    cfg->task_stack = config->task_stack;
    if (cfg->task_stack == 0) {
        cfg->task_stack = WEBSOCKET_TASK_STACK;
    }

    if (config->host) {
        cfg->host = strdup(config->host);
        ESP_WS_CLIENT_MEM_CHECK(TAG, cfg->host, return ESP_ERR_NO_MEM);
    }

    if (config->port) {
        cfg->port = config->port;
    }

    if (config->username) {
        free(cfg->username);
        cfg->username = strdup(config->username);
        ESP_WS_CLIENT_MEM_CHECK(TAG, cfg->username, return ESP_ERR_NO_MEM);
    }

    if (config->password) {
        free(cfg->password);
        cfg->password = strdup(config->password);
        ESP_WS_CLIENT_MEM_CHECK(TAG, cfg->password, return ESP_ERR_NO_MEM);
    }

    if (config->uri) {
        free(cfg->uri);
        cfg->uri = strdup(config->uri);
        ESP_WS_CLIENT_MEM_CHECK(TAG, cfg->uri, return ESP_ERR_NO_MEM);
    }
    if (config->path) {
        free(cfg->path);
        cfg->path = strdup(config->path);
        ESP_WS_CLIENT_MEM_CHECK(TAG, cfg->path, return ESP_ERR_NO_MEM);
    }
    if (config->subprotocol) {
        free(cfg->subprotocol);
        cfg->subprotocol = strdup(config->subprotocol);
        ESP_WS_CLIENT_MEM_CHECK(TAG, cfg->subprotocol, return ESP_ERR_NO_MEM);
    }
    if (config->user_agent) {
        free(cfg->user_agent);
        cfg->user_agent = strdup(config->user_agent);
        ESP_WS_CLIENT_MEM_CHECK(TAG, cfg->user_agent, return ESP_ERR_NO_MEM);
    }
    if (config->headers) {
        free(cfg->headers);
        cfg->headers = strdup(config->headers);
        ESP_WS_CLIENT_MEM_CHECK(TAG, cfg->headers, return ESP_ERR_NO_MEM);
    }

    cfg->network_timeout_ms = WEBSOCKET_NETWORK_TIMEOUT_MS;
    cfg->user_context = config->user_context;
    cfg->auto_reconnect = true;
    if (config->disable_auto_reconnect) {
        cfg->auto_reconnect = false;
    }

    if (config->disable_pingpong_discon) {
        cfg->pingpong_timeout_sec = 0;
    } else if (config->pingpong_timeout_sec) {
        cfg->pingpong_timeout_sec = config->pingpong_timeout_sec;
    } else {
        cfg->pingpong_timeout_sec = WEBSOCKET_PINGPONG_TIMEOUT_SEC;
    }

    return ESP_OK;
}

static esp_err_t esp_websocket_client_destroy_config(esp_websocket_client_handle_t client) {
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    websocket_config_storage_t *cfg = client->config;
    if (client->config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    free(cfg->host);
    free(cfg->uri);
    free(cfg->path);
    free(cfg->scheme);
    free(cfg->username);
    free(cfg->password);
    free(cfg->subprotocol);
    free(cfg->user_agent);
    free(cfg->headers);
    memset(cfg, 0, sizeof(websocket_config_storage_t));
    free(client->config);
    client->config = NULL;
    return ESP_OK;
}

static void set_websocket_transport_optional_settings(esp_websocket_client_handle_t client, esp_transport_handle_t trans) {
    if (trans && client->config->path) {
        esp_transport_ws_set_path(trans, client->config->path);
    }
    if (trans && client->config->subprotocol) {
        esp_transport_ws_set_subprotocol(trans, client->config->subprotocol);
    }
    if (trans && client->config->user_agent) {
        esp_transport_ws_set_user_agent(trans, client->config->user_agent);
    }
    if (trans && client->config->headers) {
        esp_transport_ws_set_headers(trans, client->config->headers);
    }
}

esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *config) {
    esp_websocket_client_handle_t client = calloc(1, sizeof(struct esp_websocket_client));
    ESP_WS_CLIENT_MEM_CHECK(TAG, client, return NULL);

    esp_event_loop_args_t event_args = {
        .queue_size = WEBSOCKET_EVENT_QUEUE_SIZE,
        .task_name = NULL // no task will be created
    };

    if (esp_event_loop_create(&event_args, &client->event_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Error create event handler for websocket client");
        free(client);
        return NULL;
    }

    client->lock = xSemaphoreCreateRecursiveMutex();
    ESP_WS_CLIENT_MEM_CHECK(TAG, client->lock, goto _websocket_init_fail);

    client->config = calloc(1, sizeof(websocket_config_storage_t));
    ESP_WS_CLIENT_MEM_CHECK(TAG, client->config, goto _websocket_init_fail);

    client->transport_list = esp_transport_list_init();
    ESP_WS_CLIENT_MEM_CHECK(TAG, client->transport_list, goto _websocket_init_fail);

    esp_transport_handle_t tcp = esp_transport_tcp_init();
    ESP_WS_CLIENT_MEM_CHECK(TAG, tcp, goto _websocket_init_fail);

    esp_transport_set_default_port(tcp, WEBSOCKET_TCP_DEFAULT_PORT);
    esp_transport_list_add(client->transport_list, tcp, "_tcp"); // need to save to transport list, for cleanup

    esp_transport_handle_t ws = esp_transport_ws_init(tcp);
    ESP_WS_CLIENT_MEM_CHECK(TAG, ws, goto _websocket_init_fail);

    esp_transport_set_default_port(ws, WEBSOCKET_TCP_DEFAULT_PORT);
    esp_transport_list_add(client->transport_list, ws, "ws");
    if (config->transport == WEBSOCKET_TRANSPORT_OVER_TCP) {
        asprintf(&client->config->scheme, "ws");
        ESP_WS_CLIENT_MEM_CHECK(TAG, client->config->scheme, goto _websocket_init_fail);
    }

    esp_transport_handle_t ssl = esp_transport_ssl_init();
    ESP_WS_CLIENT_MEM_CHECK(TAG, ssl, goto _websocket_init_fail);

    esp_transport_set_default_port(ssl, WEBSOCKET_SSL_DEFAULT_PORT);
    if (config->cert_pem) {
        esp_transport_ssl_set_cert_data(ssl, config->cert_pem, strlen(config->cert_pem));
    }
    esp_transport_list_add(client->transport_list, ssl, "_ssl"); // need to save to transport list, for cleanup

    esp_transport_handle_t wss = esp_transport_ws_init(ssl);
    ESP_WS_CLIENT_MEM_CHECK(TAG, wss, goto _websocket_init_fail);

    esp_transport_set_default_port(wss, WEBSOCKET_SSL_DEFAULT_PORT);

    esp_transport_list_add(client->transport_list, wss, "wss");
    if (config->transport == WEBSOCKET_TRANSPORT_OVER_SSL) {
        asprintf(&client->config->scheme, "wss");
        ESP_WS_CLIENT_MEM_CHECK(TAG, client->config->scheme, goto _websocket_init_fail);
    }

    if (config->uri) {
        if (esp_websocket_client_set_uri(client, config->uri) != ESP_OK) {
            ESP_LOGE(TAG, "Invalid uri");
            goto _websocket_init_fail;
        }
    }

    if (esp_websocket_client_set_config(client, config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set the configuration");
        goto _websocket_init_fail;
    }

    if (client->config->scheme == NULL) {
        asprintf(&client->config->scheme, "ws");
        ESP_WS_CLIENT_MEM_CHECK(TAG, client->config->scheme, goto _websocket_init_fail);
    }

    set_websocket_transport_optional_settings(client, esp_transport_list_get_transport(client->transport_list, "ws"));
    set_websocket_transport_optional_settings(client, esp_transport_list_get_transport(client->transport_list, "wss"));

    client->keepalive_tick_ms = _tick_get_ms();
    client->reconnect_tick_ms = _tick_get_ms();
    client->ping_tick_ms = _tick_get_ms();
    client->wait_for_pong_resp = false;

    int buffer_size = config->buffer_size;
    if (buffer_size <= 0) {
        buffer_size = WEBSOCKET_BUFFER_SIZE_BYTE;
    }
    client->rx_buffer = malloc(buffer_size);
    ESP_WS_CLIENT_MEM_CHECK(TAG, client->rx_buffer, {
        goto _websocket_init_fail;
    });
    client->tx_buffer = malloc(buffer_size);
    ESP_WS_CLIENT_MEM_CHECK(TAG, client->tx_buffer, {
        goto _websocket_init_fail;
    });
    client->status_bits = xEventGroupCreate();
    ESP_WS_CLIENT_MEM_CHECK(TAG, client->status_bits, {
        goto _websocket_init_fail;
    });

    client->buffer_size = buffer_size;
    return client;

_websocket_init_fail:
    esp_websocket_client_destroy(client);
    return NULL;
}

esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t client) {
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (client->run) {
        esp_websocket_client_stop(client);
    }
    if (client->event_handle) {
        esp_event_loop_delete(client->event_handle);
    }
    esp_websocket_client_destroy_config(client);
    esp_transport_list_destroy(client->transport_list);
    vQueueDelete(client->lock);
    free(client->tx_buffer);
    free(client->rx_buffer);
    if (client->status_bits) {
        vEventGroupDelete(client->status_bits);
    }
    free(client);
    client = NULL;
    return ESP_OK;
}

esp_err_t esp_websocket_client_set_uri(esp_websocket_client_handle_t client, const char *uri) {
    if (client == NULL || uri == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    struct http_parser_url puri;
    http_parser_url_init(&puri);
    int parser_status = http_parser_parse_url(uri, strlen(uri), 0, &puri);
    if (parser_status != 0) {
        ESP_LOGE(TAG, "Error parse uri = %s", uri);
        return ESP_FAIL;
    }
    if (puri.field_data[UF_SCHEMA].len) {
        free(client->config->scheme);
        asprintf(&client->config->scheme, "%.*s", puri.field_data[UF_SCHEMA].len, uri + puri.field_data[UF_SCHEMA].off);
        ESP_WS_CLIENT_MEM_CHECK(TAG, client->config->scheme, return ESP_ERR_NO_MEM);
    }

    if (puri.field_data[UF_HOST].len) {
        free(client->config->host);
        asprintf(&client->config->host, "%.*s", puri.field_data[UF_HOST].len, uri + puri.field_data[UF_HOST].off);
        ESP_WS_CLIENT_MEM_CHECK(TAG, client->config->host, return ESP_ERR_NO_MEM);
    }

    if (puri.field_data[UF_PATH].len || puri.field_data[UF_QUERY].len) {
        free(client->config->path);
        if (puri.field_data[UF_QUERY].len == 0) {
            asprintf(&client->config->path, "%.*s", puri.field_data[UF_PATH].len, uri + puri.field_data[UF_PATH].off);
        } else if (puri.field_data[UF_PATH].len == 0) {
            asprintf(&client->config->path, "/?%.*s", puri.field_data[UF_QUERY].len, uri + puri.field_data[UF_QUERY].off);
        } else {
            asprintf(&client->config->path, "%.*s?%.*s", puri.field_data[UF_PATH].len, uri + puri.field_data[UF_PATH].off,
                     puri.field_data[UF_QUERY].len, uri + puri.field_data[UF_QUERY].off);
        }
        ESP_WS_CLIENT_MEM_CHECK(TAG, client->config->path, return ESP_ERR_NO_MEM);
    }
    if (puri.field_data[UF_PORT].off) {
        client->config->port = strtol((const char *)(uri + puri.field_data[UF_PORT].off), NULL, 10);
    }

    if (puri.field_data[UF_USERINFO].len) {
        char *user_info = NULL;
        asprintf(&user_info, "%.*s", puri.field_data[UF_USERINFO].len, uri + puri.field_data[UF_USERINFO].off);
        if (user_info) {
            char *pass = strchr(user_info, ':');
            if (pass) {
                pass[0] = 0; //terminal username
                pass++;
                free(client->config->password);
                client->config->password = strdup(pass);
                ESP_WS_CLIENT_MEM_CHECK(TAG, client->config->password, return ESP_ERR_NO_MEM);
            }
            free(client->config->username);
            client->config->username = strdup(user_info);
            ESP_WS_CLIENT_MEM_CHECK(TAG, client->config->username, return ESP_ERR_NO_MEM);
            free(user_info);
        } else {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static esp_err_t esp_websocket_client_recv(esp_websocket_client_handle_t client) {
    int rlen;
    client->payload_offset = 0;
    do {
        rlen = esp_transport_read(client->transport, client->rx_buffer, client->buffer_size, client->config->network_timeout_ms);
        if (rlen < 0) {
            ESP_LOGE(TAG, "Error read data");
            return ESP_FAIL;
        }
        client->payload_len = esp_transport_ws_get_read_payload_len(client->transport);
        client->last_opcode = esp_transport_ws_get_read_opcode(client->transport);

        esp_websocket_client_dispatch_event(client, WEBSOCKET_EVENT_DATA, client->rx_buffer, rlen);

        client->payload_offset += rlen;
    } while (client->payload_offset < client->payload_len);

    // if a PING message received -> send out the PONG, this will not work for PING messages with payload longer than buffer len
    // if (client->last_opcode == WS_TRANSPORT_OPCODES_PING) {
    //     const char *data = (client->payload_len == 0) ? NULL : client->rx_buffer;
    //     esp_transport_ws_send_raw(client->transport, WS_TRANSPORT_OPCODES_PONG | WS_TRANSPORT_OPCODES_FIN, data, client->payload_len,
    //                               client->config->network_timeout_ms);
    // } else if (client->last_opcode == WS_TRANSPORT_OPCODES_PONG) {
    //     client->wait_for_pong_resp = false;
    // }

    return ESP_OK;
}

static void esp_websocket_client_task(void *pv) {
    const int lock_timeout = portMAX_DELAY;
    esp_websocket_client_handle_t client = (esp_websocket_client_handle_t)pv;
    client->run = true;

    //get transport by scheme
    client->transport = esp_transport_list_get_transport(client->transport_list, client->config->scheme);

    if (client->transport == NULL) {
        ESP_LOGE(TAG, "There are no transports valid, stop websocket client");
        client->run = false;
    }
    //default port
    if (client->config->port == 0) {
        client->config->port = esp_transport_get_default_port(client->transport);
    }

    client->state = WEBSOCKET_STATE_INIT;
    xEventGroupClearBits(client->status_bits, STOPPED_BIT);
    int read_select = 0;
    while (client->run) {
        if (xSemaphoreTakeRecursive(client->lock, lock_timeout) != pdPASS) {
            ESP_LOGE(TAG, "Failed to lock ws-client tasks, exitting the task...");
            break;
        }
        switch ((int)client->state) {
        case WEBSOCKET_STATE_INIT:
            if (client->transport == NULL) {
                ESP_LOGE(TAG, "There are no transport");
                client->run = false;
                break;
            }
            if (esp_transport_connect(client->transport,
                                      client->config->host,
                                      client->config->port,
                                      client->config->network_timeout_ms) < 0) {
                ESP_LOGE(TAG, "Error transport connect");
                esp_websocket_client_abort_connection(client);
                break;
            }
            ESP_LOGD(TAG, "Transport connected to %s://%s:%d", client->config->scheme, client->config->host, client->config->port);

            client->state = WEBSOCKET_STATE_CONNECTED;
            client->wait_for_pong_resp = false;
            esp_websocket_client_dispatch_event(client, WEBSOCKET_EVENT_CONNECTED, NULL, 0);

            break;
        case WEBSOCKET_STATE_CONNECTED:
            // if (_tick_get_ms() - client->ping_tick_ms > WEBSOCKET_PING_TIMEOUT_MS) {
            //     client->ping_tick_ms = _tick_get_ms();
            //     ESP_LOGD(TAG, "Sending PING...");
            //     esp_transport_ws_send_raw(client->transport, WS_TRANSPORT_OPCODES_PING | WS_TRANSPORT_OPCODES_FIN, NULL, 0, client->config->network_timeout_ms);

            //     if (!client->wait_for_pong_resp && client->config->pingpong_timeout_sec) {
            //         client->pingpong_tick_ms = _tick_get_ms();
            //         client->wait_for_pong_resp = true;
            //     }
            // }

            // if (_tick_get_ms() - client->pingpong_tick_ms > client->config->pingpong_timeout_sec * 1000) {
            //     if (client->wait_for_pong_resp) {
            //         ESP_LOGE(TAG, "Error, no PONG received for more than %d seconds after PING", client->config->pingpong_timeout_sec);
            //         esp_websocket_client_abort_connection(client);
            //         break;
            //     }
            // }

            if (read_select == 0) {
                ESP_LOGV(TAG, "Read poll timeout: skipping esp_transport_read()...");
                break;
            }
            client->ping_tick_ms = _tick_get_ms();

            if (esp_websocket_client_recv(client) == ESP_FAIL) {
                ESP_LOGE(TAG, "Error receive data");
                esp_websocket_client_abort_connection(client);
                break;
            }
            break;
        case WEBSOCKET_STATE_WAIT_TIMEOUT:

            if (!client->config->auto_reconnect) {
                client->run = false;
                break;
            }
            if (_tick_get_ms() - client->reconnect_tick_ms > client->wait_timeout_ms) {
                client->state = WEBSOCKET_STATE_INIT;
                client->reconnect_tick_ms = _tick_get_ms();
                ESP_LOGD(TAG, "Reconnecting...");
            }
            break;
        }
        xSemaphoreGiveRecursive(client->lock);
        if (WEBSOCKET_STATE_CONNECTED == client->state) {
            read_select = esp_transport_poll_read(client->transport, 1000); //Poll every 1000ms
            if (read_select < 0) {
                ESP_LOGE(TAG, "Network error: esp_transport_poll_read() returned %d, errno=%d", read_select, errno);
                esp_websocket_client_abort_connection(client);
            }
        } else if (WEBSOCKET_STATE_WAIT_TIMEOUT == client->state) {
            // waiting for reconnecting...
            vTaskDelay(client->wait_timeout_ms / 2 / portTICK_RATE_MS);
        }
    }

    esp_transport_close(client->transport);
    xEventGroupSetBits(client->status_bits, STOPPED_BIT);
    client->state = WEBSOCKET_STATE_UNKNOW;
    vTaskDelete(NULL);
}

esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t client) {
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (client->state >= WEBSOCKET_STATE_INIT) {
        ESP_LOGE(TAG, "The client has started");
        return ESP_FAIL;
    }
    if (xTaskCreate(esp_websocket_client_task, "websocket_task", client->config->task_stack, client, client->config->task_prio, NULL) != pdTRUE) {
        ESP_LOGE(TAG, "Error create websocket task");
        return ESP_FAIL;
    }
    xEventGroupClearBits(client->status_bits, STOPPED_BIT);
    return ESP_OK;
}

esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t client) {
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!client->run) {
        ESP_LOGW(TAG, "Client was not started");
        return ESP_FAIL;
    }
    client->run = false;
    xEventGroupWaitBits(client->status_bits, STOPPED_BIT, false, true, portMAX_DELAY);
    client->state = WEBSOCKET_STATE_UNKNOW;
    return ESP_OK;
}

static int esp_websocket_client_send_with_opcode(esp_websocket_client_handle_t client, ws_transport_opcodes_t opcode, const char *data, int len, TickType_t timeout);

int esp_websocket_client_send_text(esp_websocket_client_handle_t client, const char *data, int len, TickType_t timeout) {
    return esp_websocket_client_send_with_opcode(client, WS_TRANSPORT_OPCODES_TEXT, data, len, timeout);
}

int esp_websocket_client_send(esp_websocket_client_handle_t client, const char *data, int len, TickType_t timeout) {
    return esp_websocket_client_send_with_opcode(client, WS_TRANSPORT_OPCODES_BINARY, data, len, timeout);
}

int esp_websocket_client_send_bin(esp_websocket_client_handle_t client, const char *data, int len, TickType_t timeout) {
    return esp_websocket_client_send_with_opcode(client, WS_TRANSPORT_OPCODES_BINARY, data, len, timeout);
}

static int esp_websocket_client_send_with_opcode(esp_websocket_client_handle_t client, ws_transport_opcodes_t opcode, const char *data, int len, TickType_t timeout) {
    int need_write = len;
    int wlen = 0, widx = 0;
    int ret = ESP_FAIL;

    if (client == NULL || data == NULL || len <= 0) {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_FAIL;
    }

    if (xSemaphoreTakeRecursive(client->lock, timeout) != pdPASS) {
        ESP_LOGE(TAG, "Could not lock ws-client within %d timeout", timeout);
        return ESP_FAIL;
    }

    if (!esp_websocket_client_is_connected(client)) {
        ESP_LOGE(TAG, "Websocket client is not connected");
        goto unlock_and_return;
    }

    if (client->transport == NULL) {
        ESP_LOGE(TAG, "Invalid transport");
        goto unlock_and_return;
    }
    uint32_t current_opcode = opcode;
    while (widx < len) {
        if (need_write > client->buffer_size) {
            need_write = client->buffer_size;
        } else {
            current_opcode |= WS_TRANSPORT_OPCODES_FIN;
        }
        memcpy(client->tx_buffer, data + widx, need_write);
        // send with ws specific way and specific opcode
        wlen = esp_transport_ws_send_raw(client->transport, current_opcode, (char *)client->tx_buffer, need_write,
                                         (timeout == portMAX_DELAY) ? -1 : timeout * portTICK_PERIOD_MS);
        if (wlen <= 0) {
            ret = wlen;
            ESP_LOGE(TAG, "Network error: esp_transport_write() returned %d, errno=%d", ret, errno);
            esp_websocket_client_abort_connection(client);
            goto unlock_and_return;
        }
        current_opcode = 0;
        widx += wlen;
        need_write = len - widx;
    }
    ret = widx;
unlock_and_return:
    xSemaphoreGiveRecursive(client->lock);
    return ret;
}

bool esp_websocket_client_is_connected(esp_websocket_client_handle_t client) {
    if (client == NULL) {
        return false;
    }
    return client->state == WEBSOCKET_STATE_CONNECTED;
}

esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t client,
                                        esp_websocket_event_id_t event,
                                        esp_event_handler_t event_handler,
                                        void *event_handler_arg) {
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_event_handler_register_with(client->event_handle, WEBSOCKET_EVENTS, event, event_handler, event_handler_arg);
}