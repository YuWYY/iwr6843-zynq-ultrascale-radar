#ifndef MOCK_XIL_ASSERT_H
#define MOCK_XIL_ASSERT_H
#include <assert.h>
#define Xil_AssertNonvoid(x) assert(x)
#define Xil_AssertVoid(x) assert(x)
#define Xil_AssertVoidAlways() assert(0)
#define Xil_AssertNonvoidAlways() assert(0)
#endif
