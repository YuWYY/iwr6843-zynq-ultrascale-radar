#ifndef MOCK_XIL_IO_H
#define MOCK_XIL_IO_H
#include "xil_types.h"
#include "xil_printf.h"
#define DATA_SYNC __sync_synchronize()
u32 Xil_In32(UINTPTR);void Xil_Out32(UINTPTR,u32);
#endif
