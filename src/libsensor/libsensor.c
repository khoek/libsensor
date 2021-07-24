#include "libsensor.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <libesp.h>
#include <libiot.h>
#include <string.h>

static const char* TAG = "libsensor";

#define POLL_TASK_PRIORITY 10
#define REPORT_TASK_PRIORITY 5

#define DEFAULT_POLL_TASK_STACK_SIZE 2048
#define DEFAULT_REPORT_TASK_STACK_SIZE 4096

#define DEFAULT_QUEUE_LENGTH 16

typedef struct sensor_info {
    const sensor_type_t* type;
    void* dev;

    QueueHandle_t result_queue;
    char* tag;
    char* mqtt_topic;

    // Used to stop both tasks, since when the `poll_task` stops
    // is signals the `report_task` to stop.
    SemaphoreHandle_t poll_task_should_stop;

    // Should only be given by the `poll_task`, once `dev` has been
    // destroyed and the task promises to no longer access this structure.
    SemaphoreHandle_t report_task_should_stop;
} sensor_info_t;

// DANGER: This function does not free `info->dev`.
static void free_sensor_info(sensor_info_t* info) {
    vQueueDelete(info->result_queue);
    free(info->tag);
    free(info->mqtt_topic);

    vQueueDelete(info->poll_task_should_stop);
    vQueueDelete(info->report_task_should_stop);

    free(info);
}

static void begin_stopping_report_task(const sensor_info_t* info) {
    // At this point we are declaring that we will never access `info` again,
    // since the reporter thread may free it, with the exception that we will
    // send a single null transmission on the result queue in order to wake
    // the reporter task.
    xSemaphoreGive(info->report_task_should_stop);

    uint8_t buff[info->type->queue_item_size];
    memset(buff, 0, sizeof(buff));
    while (xQueueSend(info->result_queue, buff, portMAX_DELAY) != pdTRUE)
        ;
}

static void task_report(void* arg);
static void task_poll(void* arg);

// FIXME check these error paths
// Note that if this function fails than it guarentees that `dev` has been destroyed
// by the time it returns.
esp_err_t libsensor_create(const sensor_type_t* type, const char* tag, void* dev) {
    sensor_info_t* info = malloc(sizeof(sensor_info_t));
    info->type = type;
    info->dev = dev;

    info->result_queue = NULL;
    info->tag = strdup(tag);
    info->mqtt_topic = NULL;

    info->poll_task_should_stop = xSemaphoreCreateBinary();
    info->report_task_should_stop = xSemaphoreCreateBinary();

    if (asprintf(&info->mqtt_topic, "sensor-%s-%s", type->name, tag) == -1) {
        libiot_logf_error(TAG, "string allocation error");
        goto libsensor_sensor_create_fail_before_tasks;
    }

    size_t queue_length = type->queue_length;
    if (!queue_length) {
        queue_length = DEFAULT_QUEUE_LENGTH;
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

    return ESP_OK;

libsensor_sensor_create_fail_before_tasks:
    type->dev_destroy(dev);
    free_sensor_info(info);

    return ESP_FAIL;

libsensor_sensor_create_fail_no_poll:
    // It is to late to just free the `info` struct, since the reporter task can see it.
    // Instead we have to do the poll task's shutdown errands for it.
    type->dev_destroy(dev);
    begin_stopping_report_task(info);

    return ESP_FAIL;
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
        if (uxSemaphoreGetCount(info->report_task_should_stop) && !uxQueueMessagesWaiting(info->result_queue)) {
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

            libiot_mqtt_publish_local(info->mqtt_topic, 2, 0, msg);
            free(msg);
        }
    }

    free_sensor_info((void*) info);

    ESP_ERROR_CHECK(util_stack_overflow_check());
    vTaskDelete(NULL);
}

// This function only returns if the thread has been instructed to stop or if
// an error/timeout occurs. The returned boolean is true if the device may be
// reset and polling retried (in the case of an error/timeout), and is false
// otherwise (if the thread itself has been asked to stop).
static bool poll_loop(const sensor_info_t* info) {
    size_t consecutive_uneventful_iters = 0;
    while (1) {
        // First check whether this thread has been instructed to stop.
        if (xSemaphoreTake(info->poll_task_should_stop, 0) == pdTRUE) {
            return false;
        }

        // If not, poll the sensor once, and process the result.
        poll_result_t result = info->type->poll(info->tag, info->dev, info->result_queue);
        switch (result) {
            case POLL_RESULT_MADE_PROGRESS: {
                consecutive_uneventful_iters = 0;
                break;
            }
            case POLL_RESULT_UNEVENTFUL: {
                consecutive_uneventful_iters++;
                if (consecutive_uneventful_iters > info->type->max_uneventful_iters) {
                    libiot_logf_error(TAG, "poll_loop() gave up due to uneventfulness, caused by: %s-%s", info->type->name, info->tag);
                    return true;
                }
                break;
            }
            case POLL_RESULT_FAIL: {
                libiot_logf_error(TAG, "poll_loop() gave up due to explicit fail, caused by: %s-%s", info->type->name, info->tag);
                return true;
            }
            default: {
                ESP_LOGE(TAG, "unknown poll result!");
                abort();
            }
        }

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
        info->type->dev_start(info->dev);

        should_retry = poll_loop(info);

        info->type->dev_reset(info->dev);
    }

    info->type->dev_destroy(info->dev);

    // At this point we are declaring that we will never access `info` again,
    // since the reporter thread may free it.
    begin_stopping_report_task(info);

    ESP_ERROR_CHECK(util_stack_overflow_check());
    vTaskDelete(NULL);
}
