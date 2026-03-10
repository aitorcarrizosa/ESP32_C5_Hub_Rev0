#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define HUB_NODE_ID_LEN 24

esp_err_t hub_pairing_init(void);
bool hub_pairing_is_active(void);
esp_err_t hub_pairing_start(void);
esp_err_t hub_pairing_clear(void);
size_t hub_pairing_get_count(void);
size_t hub_pairing_copy_node_ids(char ids[][HUB_NODE_ID_LEN + 1], size_t max_ids);
