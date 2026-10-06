#include "drivers/sensor.h"

const char *sensor_chan_name(sensor_chan_t chan) {
    switch (chan) {
        case SENSOR_CHAN_TEMP:     return "temperature";
        case SENSOR_CHAN_PRESSURE: return "pressure";
        case SENSOR_CHAN_HUMIDITY: return "humidity";
        case SENSOR_CHAN_LUX:      return "lux";
        case SENSOR_CHAN_ECO2:     return "eco2";
        case SENSOR_CHAN_TVOC:     return "tvoc";
        case SENSOR_CHAN_GAS_RES:  return "gas_resistance";
        default:                   return "unknown";
    }
}

uint8_t sensor_chan_decimals(sensor_chan_t chan) {
    switch (chan) {
        case SENSOR_CHAN_TEMP:     return 2; /* 0.01 C */
        case SENSOR_CHAN_PRESSURE: return 0; /* 1 Pa */
        case SENSOR_CHAN_HUMIDITY: return 2; /* 0.01 %RH */
        case SENSOR_CHAN_LUX:      return 2; /* 0.01 Lux */
        case SENSOR_CHAN_ECO2:     return 0; /* 1 ppm */
        case SENSOR_CHAN_TVOC:     return 0; /* 1 ppb */
        case SENSOR_CHAN_GAS_RES:  return 0; /* 1 Ohm */
        default:                   return 0;
    }
}
