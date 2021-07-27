#ifndef __LIB_LIBSENSOR_H
#define __LIB_LIBSENSOR_H

#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <stdint.h>

typedef enum sensor_poll_result {
    SENSOR_POLL_RESULT_MADE_PROGRESS,
    SENSOR_POLL_RESULT_UNEVENTFUL,
    SENSOR_POLL_RESULT_FAIL,
} sensor_poll_result_t;

typedef void (*sensor_dev_start_fn_t)(void *dev, void *ctx);
typedef void (*sensor_dev_reset_fn_t)(void *dev);

typedef sensor_poll_result_t (*sensor_poll_fn_t)(const char *tag, void *dev, QueueHandle_t queue);
typedef cJSON *(*sensor_report_fn_t)(const char *tag, void *dev, void *item);

typedef void *(*sensor_ctx_init_fn_t)();
typedef void (*sensor_ctx_destroy_fn_t)(void *ctx);
typedef void (*sensor_recv_json_fn_t)(const char *tag, void *dev, void *ctx, const cJSON *json);

typedef struct sensor_type {
    ///// Data members, must be set.

    const char *name;

    size_t queue_item_size;
    uint32_t poll_delay_ms;
    size_t max_uneventful_iters;

    ///// Data members, may be left unset (hence initialized to zero) with reasonable defaults used

    size_t queue_length;
    size_t initial_max_uneventful_iters;

    size_t poll_task_stack_size;
    size_t report_task_stack_size;

    ///// Function members, cannot be null unless specified.

    sensor_dev_start_fn_t dev_start;
    // May be NULL.
    sensor_dev_reset_fn_t dev_reset;

    sensor_poll_fn_t poll;
    sensor_report_fn_t report;

    // May be NULL.
    sensor_ctx_init_fn_t ctx_init;
    // May be NULL. It is guarenteed to be called after all other calls which pass
    // the context have completed.
    sensor_ctx_destroy_fn_t ctx_destroy;
    // May be NULL.
    sensor_recv_json_fn_t recv_json;
} sensor_type_t;

typedef struct sensor_info *sensor_handle_t;

void libsensor_init();

esp_err_t libsensor_register(const sensor_type_t *type, const char *tag, void *dev, sensor_handle_t *out_handle);

// Returns the `dev` object captured by the original call to `libsensor_register`.
void *libsensor_unregister(sensor_handle_t handle);

void libsensor_dispatch_mqtt_message(const char *topic, size_t topic_len, const char *payload, size_t payload_len);

#endif
