#ifndef __LIB_LIBSENSOR_H
#define __LIB_LIBSENSOR_H

#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <stdint.h>

typedef enum poll_result {
    POLL_RESULT_MADE_PROGRESS,
    POLL_RESULT_UNEVENTFUL,
    POLL_RESULT_FAIL,
} poll_result_t;

typedef void (*sensor_dev_fn_t)(void *dev);
typedef poll_result_t (*sensor_poll_fn_t)(const char *tag, void *dev, QueueHandle_t queue);
typedef cJSON *(*sensor_report_fn_t)(const char *tag, void *dev, void *item);

typedef struct sensor_type {
    const char *name;

    size_t queue_item_size;
    uint32_t poll_delay_ms;
    size_t max_uneventful_iters;

    sensor_dev_fn_t dev_start;
    sensor_dev_fn_t dev_reset;
    sensor_dev_fn_t dev_destroy;

    sensor_poll_fn_t poll;
    sensor_report_fn_t report;

    // The following members have reasonable defaults if set to zero.
    size_t queue_length;
    size_t poll_task_stack_size;
    size_t report_task_stack_size;
} sensor_type_t;

esp_err_t libsensor_create(const sensor_type_t *type, const char *tag, void *dev);

// FIXME FIXME FIXME add a destroy function which returns the device handle (and waits for the poller thread to stop)!
// (and thus we won't destroy it in the actual code)

#endif
