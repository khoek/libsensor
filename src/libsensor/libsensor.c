#include "libsensor.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <libesp.h>
#include <libiot.h>
#include <string.h>

static const char* TAG = "libsensor";

#define DISPATCH_TASK_PRIORITY 15
#define DISPATCH_TASK_STACK_SIZE 4096

#define DISPATCH_QUEUE_LENGTH 16

#define POLL_TASK_PRIORITY 10
#define DEFAULT_POLL_TASK_STACK_SIZE 2048

#define REPORT_TASK_PRIORITY 5
#define DEFAULT_REPORT_TASK_STACK_SIZE 4096

#define DEFAULT_REPORT_QUEUE_LENGTH 16

#define MAX_SENSOR_COUNT 16

#define IN_TOPIC_PREFIX_BUFF_LEN 64

#define MQTT_SENSOR_OUT_PREFIX "sensor-out"
#define MQTT_SENSOR_IN_PREFIX "sensor-in"

typedef struct sensor_info {
    const sensor_type_t* type;
    void* dev;
    void* ctx;

    QueueHandle_t result_queue;
    char* tag;
    char* mqtt_topic;

    // Used to stop both the poller and the reporter tasks.
    EventGroupHandle_t events;
#define SENSOR_INFO_EVENT_POLL_TASK_SHOULD_STOP (1ULL << 0)
#define SENSOR_INFO_EVENT_REPORT_TASK_SHOULD_STOP (1ULL << 1)
#define SENSOR_INFO_EVENT_REPORT_TASK_STOPPED (1ULL << 2)
} sensor_info_t;

typedef struct sensor_msg {
    // The path is of the form '<sensor_type>/<sensor_tag>'.
    char* path;
    char* payload;
} sensor_msg_t;

static volatile bool global_initialized = false;

static QueueHandle_t global_dispatch_queue;
static StaticQueue_t global_dispatch_queue_static;
static uint8_t global_dispatch_queue_buff[DISPATCH_QUEUE_LENGTH * sizeof(sensor_msg_t)];

static SemaphoreHandle_t global_state_lock;
static StaticSemaphore_t global_state_lock_static;
static size_t global_sensor_count = 0;
static sensor_info_t* global_sensor_list[MAX_SENSOR_COUNT];

static char global_sensor_in_topic_prefix[IN_TOPIC_PREFIX_BUFF_LEN];
static size_t global_sensor_in_topic_prefix_len;

static void task_dispatch(void* unused);
static void task_report(void* arg);
static void task_poll(void* arg);

// DANGER: This function does not free `info->events`.
static void free_sensor_info(sensor_info_t* info) {
    if (info->type->ctx_destroy) {
        info->type->ctx_destroy(info->ctx);
    }

    vQueueDelete(info->result_queue);
    free(info->tag);
    free(info->mqtt_topic);

    // Note that we cannot delete `info->events` at this point, since
    // the task which caused the stop may still be waiting on it.

    free(info);
}

static void begin_stopping_report_task(const sensor_info_t* info) {
    // At this point we are declaring that we will never access `info` again,
    // since the reporter thread may free it, with the exception that we will
    // send a single null transmission on the result queue in order to wake
    // the reporter task.
    xEventGroupSetBits(info->events, SENSOR_INFO_EVENT_REPORT_TASK_SHOULD_STOP);

    uint8_t buff[info->type->queue_item_size];
    memset(buff, 0, sizeof(buff));
    while (xQueueSend(info->result_queue, buff, portMAX_DELAY) != pdTRUE)
        ;
}

static void begin_stopping_and_wait_for_poll_task(sensor_info_t* info) {
    EventGroupHandle_t events = info->events;

    // DANGER: After this call, `info` may be freed.
    xEventGroupSetBits(events, SENSOR_INFO_EVENT_POLL_TASK_SHOULD_STOP);

    while (!xEventGroupWaitBits(events, SENSOR_INFO_EVENT_REPORT_TASK_STOPPED, pdFALSE, pdFALSE, portMAX_DELAY))
        ;

    vEventGroupDelete(events);
}

void libsensor_init() {
    assert(!global_initialized);

    global_state_lock = xSemaphoreCreateMutexStatic(&global_state_lock_static);
    global_dispatch_queue = xQueueCreateStatic(DISPATCH_QUEUE_LENGTH,
                                               sizeof(sensor_msg_t),
                                               global_dispatch_queue_buff,
                                               &global_dispatch_queue_static);

    assert(xTaskCreate(&task_dispatch, "libsensor_dispatch", DISPATCH_TASK_STACK_SIZE, NULL, DISPATCH_TASK_PRIORITY, NULL) == pdPASS);

    libiot_mqtt_build_local_topic_from_suffix(global_sensor_in_topic_prefix, sizeof(global_sensor_in_topic_prefix), MQTT_SENSOR_IN_PREFIX);
    global_sensor_in_topic_prefix_len = strlen(global_sensor_in_topic_prefix);

    while (xSemaphoreTake(global_state_lock, portMAX_DELAY) != pdTRUE)
        ;

    global_initialized = true;

    xSemaphoreGive(global_state_lock);
}

static void add_sensor(sensor_info_t* info) {
    while (xSemaphoreTake(global_state_lock, portMAX_DELAY) != pdTRUE)
        ;

    sensor_info_t** spot = &global_sensor_list[global_sensor_count];

    global_sensor_count++;
    assert(global_sensor_count < MAX_SENSOR_COUNT);

    *spot = info;

    xSemaphoreGive(global_state_lock);
}

static void remove_sensor(sensor_info_t* info) {
    while (xSemaphoreTake(global_state_lock, portMAX_DELAY) != pdTRUE)
        ;

    size_t i = 0;

    for (; i < global_sensor_count; i++) {
        if (global_sensor_list[i] == info) {
            break;
        }
    }

    assert(i < global_sensor_count);
    i++;

    for (; i < global_sensor_count; i++) {
        global_sensor_list[i - 1] = global_sensor_list[i];
    }

    global_sensor_count--;

    xSemaphoreGive(global_state_lock);
}

// FIXME check these error paths
esp_err_t libsensor_register(const sensor_type_t* type, const char* tag, void* dev, sensor_handle_t* out_handle) {
    assert(global_initialized);

    sensor_info_t* info = malloc(sizeof(sensor_info_t));
    info->type = type;
    info->dev = dev;
    info->ctx = NULL;
    if (type->ctx_init) {
        assert(type->ctx_destroy);
        info->ctx = type->ctx_init();
    }

    info->result_queue = NULL;
    info->tag = strdup(tag);
    info->mqtt_topic = NULL;

    info->events = xEventGroupCreate();

    if (asprintf(&info->mqtt_topic, MQTT_SENSOR_OUT_PREFIX "/%s/%s", type->name, tag) == -1) {
        libiot_logf_error(TAG, "string allocation error");
        goto libsensor_sensor_create_fail_before_tasks;
    }

    size_t queue_length = type->queue_length;
    if (!queue_length) {
        queue_length = DEFAULT_REPORT_QUEUE_LENGTH;
    }

    // Note: it is the job of the reporter task to free the `sensor_info_t` struct, including
    // the result queue.
    info->result_queue = xQueueCreate(queue_length, type->queue_item_size);
    if (!info->result_queue) {
        libiot_logf_error(TAG, "queue allocation error");
        goto libsensor_sensor_create_fail_before_tasks;
    }

    size_t poll_task_stack_size = type->poll_task_stack_size;
    if (!poll_task_stack_size) {
        poll_task_stack_size = DEFAULT_POLL_TASK_STACK_SIZE;
    }

    size_t report_task_stack_size = type->report_task_stack_size;
    if (!report_task_stack_size) {
        report_task_stack_size = DEFAULT_REPORT_TASK_STACK_SIZE;
    }

    char buff[256];
    BaseType_t result;

    snprintf(buff, sizeof(buff), "task_report_%s-%s", type->name, tag);
    result = xTaskCreate(&task_report, buff, report_task_stack_size, (void*) info, REPORT_TASK_PRIORITY, NULL);
    if (result != pdPASS) {
        libiot_logf_error(TAG, "failed to create report task! (0x%X)", result);
        goto libsensor_sensor_create_fail_before_tasks;
    }

    snprintf(buff, sizeof(buff), "task_poll_%s-%s", type->name, tag);
    result = xTaskCreate(&task_poll, buff, poll_task_stack_size, (void*) info, POLL_TASK_PRIORITY, NULL);
    if (result != pdPASS) {
        libiot_logf_error(TAG, "failed to create poll task! (0x%X)", result);
        goto libsensor_sensor_create_fail_no_poll;
    }

    add_sensor(info);

    if (out_handle) {
        *out_handle = info;
    }

    return ESP_OK;

libsensor_sensor_create_fail_before_tasks:
    vEventGroupDelete(info->events);
    free_sensor_info(info);

    return ESP_FAIL;

libsensor_sensor_create_fail_no_poll:
    // It is to late to just free the `info` struct, since the reporter task can see it.
    // Instead we have to do the poll task's shutdown errands for it, and then wait for
    // the reporter task to stop.
    begin_stopping_report_task(info);
    begin_stopping_and_wait_for_poll_task(info);

    return ESP_FAIL;
}

void* libsensor_unregister(sensor_info_t* info) {
    void* dev = info->dev;

    remove_sensor(info);
    // DANGER: After this call, `info` may be freed.
    begin_stopping_and_wait_for_poll_task(info);

    return dev;
}

static void task_report(void* arg) {
    // Note: it is the job of the reporter task to free the `sensor_info_t` struct, including
    // the result queue.
    const sensor_info_t* info = arg;

    while (1) {
        uint8_t buff[info->type->queue_item_size];
        while (xQueueReceive(info->result_queue, (void*) buff, portMAX_DELAY) == pdFALSE)
            ;

        // Has this task been instructed to stop, and have we just recieved the last message? (The latter check
        // is not a race because once the semaphore `info->report_task_should_stop` we promise to never send
        // anything on `info->result_queue`.)
        if ((xEventGroupGetBits(info->events) & SENSOR_INFO_EVENT_REPORT_TASK_SHOULD_STOP) && !uxQueueMessagesWaiting(info->result_queue)) {
            // The last message ever sent to the queue is zeros, ignore it and break from this loop.
            break;
        }

        cJSON* json = info->type->report(info->tag, info->dev, (void*) &buff);
        if (json) {
            char* msg = cJSON_PrintUnformatted(json);
            cJSON_Delete(json);

            if (!msg) {
                ESP_LOGE(TAG, "JSON print fail");
                continue;
            }

            libiot_mqtt_enqueue_local(info->mqtt_topic, 2, 0, msg);
            free(msg);
        }

        ESP_ERROR_CHECK(util_stack_overflow_check());
    }

    xEventGroupSetBits(info->events, SENSOR_INFO_EVENT_REPORT_TASK_STOPPED);
    free_sensor_info((void*) info);

    vTaskDelete(NULL);
}

// This function only returns if the thread has been instructed to stop or if
// an error/timeout occurs. The returned boolean is true if the device may be
// reset and polling retried (in the case of an error/timeout), and is false
// otherwise (if the thread itself has been asked to stop).
static bool poll_loop(const sensor_info_t* info) {
    size_t max_uneventful_iters = info->type->initial_max_uneventful_iters;
    if (!max_uneventful_iters) {
        max_uneventful_iters = info->type->max_uneventful_iters;
    }

    size_t consecutive_uneventful_iters = 0;
    while (1) {
        // First check whether this thread has been instructed to stop.
        if (xEventGroupGetBits(info->events) & SENSOR_INFO_EVENT_POLL_TASK_SHOULD_STOP) {
            return false;
        }

        // If not, poll the sensor once, and process the result.
        sensor_poll_result_t result = info->type->poll(info->tag, info->dev, info->result_queue);
        switch (result) {
            case SENSOR_POLL_RESULT_MADE_PROGRESS: {
                // Change the `max_uneventful_iters` limit from the initial limit
                // (if there was one) to the normal limit.
                max_uneventful_iters = info->type->max_uneventful_iters;

                consecutive_uneventful_iters = 0;
                break;
            }
            case SENSOR_POLL_RESULT_UNEVENTFUL: {
                consecutive_uneventful_iters++;
                if (consecutive_uneventful_iters > max_uneventful_iters) {
                    libiot_logf_error(TAG, "poll_loop() gave up due to uneventfulness, caused by: %s-%s", info->type->name, info->tag);
                    return true;
                }
                break;
            }
            case SENSOR_POLL_RESULT_FAIL: {
                libiot_logf_error(TAG, "poll_loop() gave up due to explicit fail, caused by: %s-%s", info->type->name, info->tag);
                return true;
            }
            default: {
                ESP_LOGE(TAG, "unknown poll result!");
                abort();
            }
        }

        ESP_ERROR_CHECK(util_stack_overflow_check());

        if (info->type->poll_delay_ms) {
            vTaskDelay(1 + (info->type->poll_delay_ms / portTICK_PERIOD_MS));
        }
    }
}

static void task_poll(void* arg) {
    // Note: it is the job of the reporter task to free the `sensor_info_t` struct, including
    // the result queue.
    const sensor_info_t* info = arg;

    bool should_retry = true;
    while (should_retry) {
        info->type->dev_start(info->dev, info->ctx);

        should_retry = poll_loop(info);

        if (info->type->dev_reset) {
            info->type->dev_reset(info->dev);
        }
    }

    // At this point we are declaring that we will never access `info` again,
    // since the reporter thread may free it.
    begin_stopping_report_task(info);

    vTaskDelete(NULL);
}

static void handle_sensor_msg(const sensor_msg_t* msg) {
    const char* path = msg->path;

    cJSON* json = cJSON_Parse(msg->payload);
    if (!json) {
        libiot_logf_error(TAG, "JSON parse error for sensor path: %s", msg->path);
        return;
    }

    while (xSemaphoreTake(global_state_lock, portMAX_DELAY) != pdTRUE)
        ;

    for (size_t i = 0; i < global_sensor_count; i++) {
        sensor_info_t* sensor = global_sensor_list[i];
        size_t name_len = strlen(sensor->type->name);

        // Is the sensor name a prefix of the `path`?
        if (strncmp(sensor->type->name, path, name_len) != 0) {
            continue;
        }

        // If so, again advance `path` past the sensor name.
        path += name_len;
        if (*path != '/') {
            continue;
        }
        path++;

        // Finally, do the tags match?
        if (strcmp(sensor->tag, path) != 0) {
            continue;
        }

        // If so, dispatch the message.
        if (!sensor->type->recv_json) {
            libiot_logf_error(TAG, "dropping message for sensor '%s/%s' with no JSON handler", sensor->type->name, sensor->tag);
            continue;
        }

        sensor->type->recv_json(sensor->tag, sensor->dev, sensor->ctx, json);
    }

    xSemaphoreGive(global_state_lock);

    cJSON_Delete(json);
}

static void task_dispatch(void* unused) {
    while (1) {
        sensor_msg_t msg;
        while (xQueueReceive(global_dispatch_queue, &msg, portMAX_DELAY) != pdTRUE)
            ;

        handle_sensor_msg(&msg);

        free(msg.path);
        free(msg.payload);
        ESP_ERROR_CHECK(util_stack_overflow_check());
    }

    vTaskDelete(NULL);
}

void libsensor_dispatch_mqtt_message(const char* topic, size_t topic_len, const char* payload, size_t payload_len) {
    assert(global_initialized);

    // First check that `topic` has `global_sensor_in_topic_prefix` as a prefix.
    size_t i = 0;
    for (; i < topic_len && i < global_sensor_in_topic_prefix_len; i++) {
        if (topic[i] != global_sensor_in_topic_prefix[i]) {
            return;
        }
    }

    if (i >= topic_len || topic[i] != '/') {
        return;
    }

    // If so, advance `topic` past the prefix so that it is (presumably) of the form "<sensor_name>/<sensor_tag>".
    const char* unterm_path = topic + global_sensor_in_topic_prefix_len + 1;
    size_t unterm_path_len = topic_len - (global_sensor_in_topic_prefix_len + 1);

    sensor_msg_t msg = {
        .path = strndup(unterm_path, unterm_path_len),
        .payload = strndup(payload, payload_len),
    };

    if (xQueueSend(global_dispatch_queue, &msg, 0) != pdTRUE) {
        libiot_logf_error(TAG, "dropping sensor message for sensor path: %s", msg.path);
        goto libsensor_dispatch_mqtt_message_send_failed;
    }

    return;

libsensor_dispatch_mqtt_message_send_failed:
    free(msg.path);
    free(msg.payload);
}
