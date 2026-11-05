// SPDX-License-Identifier: GPL-3.0-only

#ifndef MOBY_CONFIG_NVS_H
#define MOBY_CONFIG_NVS_H

#include "esp_err.h"
#include "moby_config.h"

// Opens the "moby" NVS namespace and fills store with it. The pair_key is kept
// in plain NVS: flash and NVS encryption are not enabled in this stage.
esp_err_t moby_config_nvs_open(moby_store_t *store);

#endif
