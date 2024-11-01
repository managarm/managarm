/* This file is derived from FreeBSD's e1000_osdep.h and has been adapted for use in managarm */

/******************************************************************************
  SPDX-License-Identifier: BSD-3-Clause

  Copyright (c) 2001-2020, Intel Corporation
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

   1. Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

   3. Neither the name of the Intel Corporation nor the names of its
      contributors may be used to endorse or promote products derived from
      this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE.

******************************************************************************/
/*$FreeBSD$*/

#ifndef _FREEBSD_E1000_OSDEP_H_
#define _FREEBSD_E1000_OSDEP_H_

#include <net/ethernet.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define usec_delay(x) usleep(x)
#define usec_delay_irq(x) usleep(x)
#define msec_delay(x) usleep(x * 1000)
#define msec_delay_irq(x) usleep(x * 1000)

#define DEBUGOUT(format, ...)                                                                      \
	printf("driver/freebsd-e1000: %s %d: " format, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define DEBUGOUT1(...) DEBUGOUT(__VA_ARGS__)
#define DEBUGOUT2(...) DEBUGOUT(__VA_ARGS__)
#define DEBUGOUT3(...) DEBUGOUT(__VA_ARGS__)
#define DEBUGFUNC(F)

#define FALSE 0
#define TRUE 1

#define CMD_MEM_WRT_INVALIDATE 0x0010 /* BIT_4 */

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int64_t s64;
typedef int32_t s32;
typedef int16_t s16;
typedef int8_t s8;
#define __le16 u16
#define __le32 u32
#define __le64 u64

#define ASSERT_CTX_LOCK_HELD(hw)

struct e1000_pci;

struct e1000_osdep {
	struct e1000_pci *pci;
	uintptr_t membase;
	uintptr_t iobase;
	uintptr_t flashbase;
};

#define hw2pci(hw) (((struct e1000_osdep *)(hw)->back)->pci->pci)
#define hw2membase(hw) (((struct e1000_osdep *)(hw)->back)->membase)
#define hw2iobase(hw) (((struct e1000_osdep *)(hw)->back)->iobase)
#define hw2flashbase(hw) (((struct e1000_osdep *)(hw)->back)->flashbase)
#define hw2nic(hw) (frg::container_of(hw, &E1000Nic::_hw))

#define REG64(addr) ((volatile uint64_t *)(uintptr_t)(addr))
#define REG32(addr) ((volatile uint32_t *)(uintptr_t)(addr))
#define REG16(addr) ((volatile uint16_t *)(uintptr_t)(addr))
#define REG8(addr) ((volatile uint8_t *)(uintptr_t)(addr))

#define writel(v, a) (*REG32(a) = (v))
#define readl(a) (*REG32(a))
#define writew(v, a) (*REG16(a) = (v))
#define readw(a) (*REG16(a))

struct e1000_hw;

void e1000_io_write(struct e1000_hw *hw, u16 reg, u32 data);

#define E1000_REGISTER(hw, reg)                                                                    \
	(((hw)->mac.type >= e1000_82543) ? (u32)(reg) : e1000_translate_register_82542(reg))

#define E1000_WRITE_FLUSH(a) E1000_READ_REG(a, E1000_STATUS)

/* Read from an absolute offset in the adapter's memory space */
#define E1000_READ_OFFSET(hw, offset)                                                              \
	readl((const volatile void *)(uintptr_t)(hw2membase(hw) + (offset)))

/* Write to an absolute offset in the adapter's memory space */
#define E1000_WRITE_OFFSET(hw, offset, value)                                                      \
	writel((value), (volatile void *)(uintptr_t)(hw2membase(hw) + (offset)))

/* Register READ/WRITE macros */
#define E1000_READ_REG(hw, reg) E1000_READ_OFFSET((hw), E1000_REGISTER((hw), (reg)))
#define E1000_WRITE_REG(hw, reg, value)                                                            \
	E1000_WRITE_OFFSET((hw), E1000_REGISTER((hw), (reg)), (value))

#define E1000_READ_REG_ARRAY(hw, reg, index)                                                       \
	E1000_READ_OFFSET((hw), E1000_REGISTER((hw), (reg)) + ((index) << 2))
#define E1000_WRITE_REG_ARRAY(hw, reg, index, value)                                               \
	E1000_WRITE_OFFSET((hw), E1000_REGISTER((hw), (reg)) + ((index) << 2), (value))

#define E1000_READ_REG_ARRAY_DWORD E1000_READ_REG_ARRAY
#define E1000_WRITE_REG_ARRAY_DWORD E1000_WRITE_REG_ARRAY

#define E1000_WRITE_REG_IO(hw, reg, value) e1000_io_write(hw, reg, value)

#define E1000_READ_FLASH_REG(hw, reg)                                                              \
	readl((const volatile void *)(uintptr_t)(hw2flashbase(hw) + (reg)))
#define E1000_READ_FLASH_REG16(hw, reg)                                                            \
	readw((const volatile void *)(uintptr_t)(hw2flashbase(hw) + (reg)))
#define E1000_WRITE_FLASH_REG(hw, reg, value)                                                      \
	writel((value), (volatile void *)(uintptr_t)(hw2flashbase(hw) + (reg)))
#define E1000_WRITE_FLASH_REG16(hw, reg, value)                                                    \
	writew((value), (volatile void *)(uintptr_t)(hw2flashbase(hw) + (reg)))

#define ASSERT_NO_LOCKS()

#endif /* _FREEBSD_E1000_OSDEP_H_ */
