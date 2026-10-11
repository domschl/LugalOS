#ifndef DRIVERS_ADC_H
#define DRIVERS_ADC_H

#include <stdint.h>

int adc_read_channel_mv(uint32_t channel);
int adc_read_channel_raw(uint32_t channel);

#endif /* DRIVERS_ADC_H */
