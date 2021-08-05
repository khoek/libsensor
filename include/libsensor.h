#pragma once

//////// Config options

// Configure the threshold stack high water mark below which will
// trigger the logging of an error message (via libiot).
//
// Set to a number larger than the largest stack size in order to
// print the stack high water marks for each task started by libsensor.
//
// Default is 100.
// #define LIBSENSOR_WARNING_THRESHOLD_STACK_HIGHWATERMARK

////////

#ifndef LIBSENSOR_WARNING_THRESHOLD_STACK_HIGHWATERMARK
#define LIBSENSOR_WARNING_THRESHOLD_STACK_HIGHWATERMARK 100
#endif

#include <cJSON.h>
#include <esp_err.h>
#include <stdint.h>

typedef enum sensor_poll_result {
    SENSOR_POLL_RESULT_MADE_PROGRESS,
    SENSOR_POLL_RESULT_UNEVENTFUL,
    SENSOR_POLL_RESULT_FAIL,
} sensor_poll_result_t;

typedef struct sensor_info sensor_info_t;
typedef const sensor_info_t *sensor_output_handle_t;

typedef void (*sensor_dev_destroy_fn_t)(void *dev);
typedef void (*sensor_dev_start_fn_t)(void *dev, void *ctx);
typedef void (*sensor_dev_reset_fn_t)(void *dev, void *ctx);

typedef sensor_poll_result_t (*sensor_poll_fn_t)(const char *tag, void *dev,
                                                 void *ctx,
                                                 sensor_output_handle_t output);
typedef cJSON *(*sensor_report_fn_t)(const char *tag, void *dev, void *ctx,
                                     void *item);

typedef void *(*sensor_ctx_init_fn_t)(void *dev);
typedef void (*sensor_ctx_destroy_fn_t)(void *ctx);
typedef void (*sensor_recv_json_fn_t)(const char *tag, void *dev, void *ctx,
                                      const cJSON *json);

typedef struct sensor_type {
    ///// Data members, must be set.

    const char *model;

    uint32_t poll_delay_ms;
    size_t max_uneventful_iters;

    ///// Data members, may be left unset (hence initialized to zero) with
    /// reasonable defaults used

    size_t queue_length;
    size_t initial_max_uneventful_iters;

    size_t poll_task_stack_size;
    size_t report_task_stack_size;

    ///// Function members, must be set.

    sensor_dev_destroy_fn_t dev_destroy;
    sensor_dev_start_fn_t dev_start;

    sensor_poll_fn_t poll;
    sensor_report_fn_t report;

    ///// Function members, cannot be null unless specified.

    sensor_dev_reset_fn_t dev_reset;

    sensor_ctx_init_fn_t ctx_init;
    // This function is guarenteed to be called after all other calls which pass
    // the context created by `ctx_init()` have completed.
    sensor_ctx_destroy_fn_t ctx_destroy;

    sensor_recv_json_fn_t recv_json;
} sensor_type_t;

typedef struct sensor_info *sensor_handle_t;

void libsensor_init();

void libsensor_dispatch_mqtt_message(const char *topic, size_t topic_len,
                                     const char *payload, size_t payload_len);

// Capture the `dev` object by creating and registering a sensor with the given
// information.
esp_err_t libsensor_register(const sensor_type_t *type, const char *tag,
                             void *dev, sensor_handle_t *out_handle);

// Unregisters the sensor referred to by the given handle, and destroys the
// `dev` object (using `type->dev_destroy()`) captured by the original call to
// `libsensor_register()`.
void libsensor_unregister(sensor_handle_t handle);

// Unregisters the sensor referred to by the given handle, and returns the `dev`
// object captured by the original call to `libsensor_register()` without
// destroying it.
void *libsensor_unregister_extract_dev(sensor_handle_t handle);

// These functions are to be called inside a `sensor_poll_fn_t`, in order to
// store an item to be processed by the report task.
void libsensor_output_item(sensor_output_handle_t output, void *item);
void libsensor_output_item_with_time(sensor_output_handle_t output, void *item,
                                     uint64_t epoch_time_ms);
