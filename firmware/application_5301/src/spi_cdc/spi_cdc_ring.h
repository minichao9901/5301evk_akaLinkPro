/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SPI_CDC_RING_H
#define SPI_CDC_RING_H
#include <stdint.h>
static inline uint32_t spi_cdc_position(uint32_t laps, uint32_t pending,
                                       uint32_t position, uint32_t size, uint32_t previous)
{
    /* At DST=end the transfer may be just completing or already reloading.
     * Use the previous monotonic snapshot to distinguish the two epochs. */
    if (position == size) { position = 0U; }
    uint32_t total = (laps + pending) * size + position;
    if ((int32_t)(total - previous) < 0) { total += size; }
    return total;
}
/* Keep half a DMA lap as headroom while copying. Counters wrap modulo 2^32.
 * A full CDC ring never makes us overwrite an in-flight USB buffer. */
static inline uint32_t spi_cdc_trim(uint32_t written, uint32_t *read, uint32_t size)
{
    uint32_t available = written - *read;
    uint32_t keep = size / 2U;
    if (available <= keep) { return 0U; }
    uint32_t dropped = available - keep;
    *read += dropped;
    return dropped;
}
static inline uint32_t spi_cdc_chunk(uint32_t written, uint32_t read, uint32_t size,
                                    uint32_t free, uint32_t budget)
{
    uint32_t n = written - read, contiguous = size - read % size;
    if (n > contiguous) { n = contiguous; }
    if (n > free) { n = free; }
    if (n > budget) { n = budget; }
    if (n > 1024U) { n = 1024U; }
    return n;
}
#endif
