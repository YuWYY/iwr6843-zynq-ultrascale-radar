/* SPDX-License-Identifier: MIT */
#ifndef RADAR_CRC32_H
#define RADAR_CRC32_H
#include <stddef.h>
#include <stdint.h>
/* CRC-32/ISO-HDLC: start 0xffffffff, finish xor 0xffffffff. */
uint32_t radar_crc32_update(uint32_t state,const void *data,size_t bytes);
#endif
