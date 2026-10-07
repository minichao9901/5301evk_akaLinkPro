/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SPI_CDC_H
#define SPI_CDC_H
#include <stdint.h>
#include "service_gate.h"
#define SPI_CDC_CMD 0x39U
#define SPI_CDC_MAGIC 0x31435053UL /* SPC1 */
enum { SPI_CDC_STATUS, SPI_CDC_START, SPI_CDC_STOP };
extern service_gate_t spi_cdc_gate;
static inline uint8_t spi_cdc_needs_service(void) { return service_gate_pending(&spi_cdc_gate); }
void spi_cdc_poll(void);
void spi_cdc_hid(uint8_t *request, uint8_t *response);
void spi_cdc_usb_reset(void);
uint8_t spi_cdc_running(void);
#endif
