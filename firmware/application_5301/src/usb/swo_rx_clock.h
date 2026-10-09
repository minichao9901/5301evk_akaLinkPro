#ifndef SWO_RX_CLOCK_H
#define SWO_RX_CLOCK_H
#include <stdint.h>
void swo_rx_command(const uint8_t *req,uint8_t *res);
/* Authoritative ISR/main state, read inline without fetching the idle XIP
 * poll function. A live token keeps lease expiry and rollback serviced. */
typedef struct {
  volatile uint32_t pending, token;
} swo_rx_work_t;
extern swo_rx_work_t swo_rx_work;
static inline int swo_rx_needs_service(void) {
  return swo_rx_work.pending || swo_rx_work.token;
}
void swo_rx_poll(void);
void swo_rx_usb_reset(void);
int swo_rx_active(void);
uint32_t swo_rx_baud(void);
void swo_rx_uart_dividers(uint32_t *div,uint32_t *osr);
#endif
