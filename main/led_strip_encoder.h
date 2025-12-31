#pragma once

#include <stdint.h>
#include "driver/rmt_encoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Type of led strip encoder configuration
 */
typedef struct {
    uint32_t resolution; /*!< Encoder resolution, in Hz */
} led_strip_encoder_config_t;

// encodes color data into timing symbols the LED can understand
esp_err_t rmt_new_led_strip_encoder(
    const led_strip_encoder_config_t *config, 
    rmt_encoder_handle_t *ret_encoder);

#ifdef __cplusplus
}
#endif
