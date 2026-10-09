#ifndef SWO_RX_CLOCK_H
#define SWO_RX_CLOCK_H
#include <stdint.h>
void swo_rx_command(const uint8_t *req,uint8_t *res);
void swo_rx_poll(void);
void swo_rx_usb_reset(void);
int swo_rx_active(void);
uint32_t swo_rx_baud(void);
void swo_rx_uart_dividers(uint32_t *div,uint32_t *osr);
#endif
