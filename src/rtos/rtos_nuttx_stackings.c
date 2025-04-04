/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "rtos.h"
#include "target/armv7m.h"
#include "rtos_nuttx_stackings.h"
#include "rtos_standard_stackings.h"
#include <target/riscv/riscv.h>
#include <helper/bits.h>

/* The cortex_m target uses nuttx_tcbinfo_stack_read which uses a symbol
 * provided by Nuttx to read the registers from memory and place them directly
 * in the order we need. This is because the register offsets change with
 * different versions of Nuttx, FPU vs non-FPU and ARMv7 vs ARMv8.
 * This allows a single function to work with many versions.
 */
static const struct stack_register_offset nuttx_stack_offsets_cortex_m[] = {
	{ ARMV7M_R0,    0, 32 },		/* r0   */
	{ ARMV7M_R1,    4, 32 },		/* r1   */
	{ ARMV7M_R2,    8, 32 },		/* r2   */
	{ ARMV7M_R3,   12, 32 },		/* r3   */
	{ ARMV7M_R4,   16, 32 },		/* r4   */
	{ ARMV7M_R5,   20, 32 },		/* r5   */
	{ ARMV7M_R6,   24, 32 },		/* r6   */
	{ ARMV7M_R7,   28, 32 },		/* r7   */
	{ ARMV7M_R8,   32, 32 },		/* r8   */
	{ ARMV7M_R9,   36, 32 },		/* r9   */
	{ ARMV7M_R10,  40, 32 },		/* r10  */
	{ ARMV7M_R11,  44, 32 },		/* r11  */
	{ ARMV7M_R12,  48, 32 },		/* r12  */
	{ ARMV7M_R13,  52, 32 },		/* sp   */
	{ ARMV7M_R14,  56, 32 },		/* lr   */
	{ ARMV7M_PC,   60, 32 },		/* pc   */
	{ ARMV7M_XPSR, 64, 32 },		/* xPSR */
};

/* The Nuttx stack frame for most architectures has some registers placed
 * by hardware and some by software. The hardware register order and number does not change
 * but the software registers may change with different versions of Nuttx.
 * For example with ARMv7, nuttx-12.3.0 added a new register which changed all
 * the offsets. We can either create separate offset tables for each version of Nuttx
 * which will break again in the future, or read the offsets from the TCB info.
 * Nuttx provides a symbol (g_reg_offs) which holds all the offsets for each stored register.
 * This offset table is stored in GDB org.gnu.gdb.xxx feature order.
 * The same order we need.
 * Please refer:
 * https://sourceware.org/gdb/current/onlinedocs/gdb/ARM-Features.html
 * https://sourceware.org/gdb/current/onlinedocs/gdb/RISC_002dV-Features.html
 */
static int nuttx_cortex_m_tcbinfo_stack_read(struct target *target,
	int64_t stack_ptr, const struct rtos_register_stacking *stacking,
	uint8_t *stack_data)
{
	struct rtos *rtos = target->rtos;
	target_addr_t xcpreg_off = rtos->symbols[NX_SYM_REG_OFFSETS].address;

	for (int i = 0; i < stacking->num_output_registers; ++i) {
		uint16_t stack_reg_offset;
		int ret = target_read_u16(rtos->target, xcpreg_off + 2 * i, &stack_reg_offset);
		if (ret != ERROR_OK) {
			LOG_ERROR("Failed to read stack_reg_offset: ret = %d", ret);
			return ret;
		}
		if (stack_reg_offset != UINT16_MAX && stacking->register_offsets[i].offset >= 0) {
			ret = target_read_buffer(target,
				stack_ptr + stack_reg_offset,
				stacking->register_offsets[i].width_bits / 8,
				&stack_data[stacking->register_offsets[i].offset]);
			if (ret != ERROR_OK) {
				LOG_ERROR("Failed to read register: ret = %d", ret);
				return ret;
			}
		}
	}

	/* Offset match nuttx_stack_offsets_cortex_m */
	const int XPSR_OFFSET = 64;
	const int SP_OFFSET = 52;
	/* Nuttx stack frames (produced in exception_common) store the SP of the ISR minus
	 * the hardware stack frame size. This SP may include an additional 4 byte alignment
	 * depending in xPSR[9]. The Nuttx stack frame stores post alignment since the
	 * hardware will add/remove automatically on both enter/exit.
	 * We need to adjust the SP to get the real SP of the stack.
	 * See Arm Reference manual "Stack alignment on exception entry"
	 */
	uint32_t xpsr = target_buffer_get_u32(target, &stack_data[XPSR_OFFSET]);
	if (xpsr & BIT(9)) {
		uint32_t sp = target_buffer_get_u32(target, &stack_data[SP_OFFSET]);
		target_buffer_set_u32(target, &stack_data[SP_OFFSET], sp - 4 * stacking->stack_growth_direction);
	}

	return ERROR_OK;
}

const struct rtos_register_stacking nuttx_stacking_cortex_m = {
	/* nuttx_tcbinfo_stack_read transforms the stack into just output registers */
	.stack_registers_size = ARRAY_SIZE(nuttx_stack_offsets_cortex_m) * 4,
	.stack_growth_direction = -1,
	.num_output_registers = ARRAY_SIZE(nuttx_stack_offsets_cortex_m),
	.read_stack = nuttx_cortex_m_tcbinfo_stack_read,
	.calculate_process_stack = NULL, /* Stack alignment done in nuttx_cortex_m_tcbinfo_stack_read */
	.register_offsets = nuttx_stack_offsets_cortex_m,
};

static int nuttx_esp_xtensa_stack_read(struct target *target,
	int64_t stack_ptr, const struct rtos_register_stacking *stacking,
	uint8_t *stack_data)
{
	int retval = target_read_buffer(target, stack_ptr, stacking->stack_registers_size, stack_data);
	if (retval != ERROR_OK)
		return retval;

	stack_data[4] &= ~0x10;	/* Clear exception bit in PS */

	return ERROR_OK;
}

static const struct stack_register_offset nuttx_stack_offsets_esp32[] = {
	{ 0, 0x00, 32 },		/* PC */
	{ 1, 0x08, 32 },		/* A0 */
	{ 2, 0x0c, 32 },		/* A1 */
	{ 3, 0x10, 32 },		/* A2 */
	{ 4, 0x14, 32 },		/* A3 */
	{ 5, 0x18, 32 },		/* A4 */
	{ 6, 0x1c, 32 },		/* A5 */
	{ 7, 0x20, 32 },		/* A6 */
	{ 8, 0x24, 32 },		/* A7 */
	{ 9, 0x28, 32 },		/* A8 */
	{ 10, 0x2c, 32 },		/* A9 */
	{ 11, 0x30, 32 },		/* A10 */
	{ 12, 0x34, 32 },		/* A11 */
	{ 13, 0x38, 32 },		/* A12 */
	{ 14, 0x3c, 32 },		/* A13 */
	{ 15, 0x40, 32 },		/* A14 */
	{ 16, 0x44, 32 },		/* A15 */
	/* A16-A63 aren't in the stack frame because they've been flushed to the stack earlier */
	{ 17, -1, 32 },			/* A16 */
	{ 18, -1, 32 },			/* A17 */
	{ 19, -1, 32 },			/* A18 */
	{ 20, -1, 32 },			/* A19 */
	{ 21, -1, 32 },			/* A20 */
	{ 22, -1, 32 },			/* A21 */
	{ 23, -1, 32 },			/* A22 */
	{ 24, -1, 32 },			/* A23 */
	{ 25, -1, 32 },			/* A24 */
	{ 26, -1, 32 },			/* A25 */
	{ 27, -1, 32 },			/* A26 */
	{ 28, -1, 32 },			/* A27 */
	{ 29, -1, 32 },			/* A28 */
	{ 30, -1, 32 },			/* A29 */
	{ 31, -1, 32 },			/* A30 */
	{ 32, -1, 32 },			/* A31 */
	{ 33, -1, 32 },			/* A32 */
	{ 34, -1, 32 },			/* A33 */
	{ 35, -1, 32 },			/* A34 */
	{ 36, -1, 32 },			/* A35 */
	{ 37, -1, 32 },			/* A36 */
	{ 38, -1, 32 },			/* A37 */
	{ 39, -1, 32 },			/* A38 */
	{ 40, -1, 32 },			/* A39 */
	{ 41, -1, 32 },			/* A40 */
	{ 42, -1, 32 },			/* A41 */
	{ 43, -1, 32 },			/* A42 */
	{ 44, -1, 32 },			/* A43 */
	{ 45, -1, 32 },			/* A44 */
	{ 46, -1, 32 },			/* A45 */
	{ 47, -1, 32 },			/* A46 */
	{ 48, -1, 32 },			/* A47 */
	{ 49, -1, 32 },			/* A48 */
	{ 50, -1, 32 },			/* A49 */
	{ 51, -1, 32 },			/* A50 */
	{ 52, -1, 32 },			/* A51 */
	{ 53, -1, 32 },			/* A52 */
	{ 54, -1, 32 },			/* A53 */
	{ 55, -1, 32 },			/* A54 */
	{ 56, -1, 32 },			/* A55 */
	{ 57, -1, 32 },			/* A56 */
	{ 58, -1, 32 },			/* A57 */
	{ 59, -1, 32 },			/* A58 */
	{ 60, -1, 32 },			/* A59 */
	{ 61, -1, 32 },			/* A60 */
	{ 62, -1, 32 },			/* A61 */
	{ 63, -1, 32 },			/* A62 */
	{ 64, -1, 32 },			/* A63 */
	{ 65, 0x58, 32 },		/* lbeg */
	{ 66, 0x5c, 32 },		/* lend */
	{ 67, 0x60, 32 },		/* lcount */
	{ 68, 0x48, 32 },		/* SAR */
	{ 69, -1, 32 },			/* windowbase */
	{ 70, -1, 32 },			/* windowstart */
	{ 71, -1, 32 },			/* configid0 */
	{ 72, -1, 32 },			/* configid1 */
	{ 73, 0x04, 32 },		/* PS */
	{ 74, -1, 32 },			/* threadptr */
	{ 75, -1, 32 },			/* br */
	{ 76, 0x54, 32 },		/* scompare1 */
	{ 77, -1, 32 },			/* acclo */
	{ 78, -1, 32 },			/* acchi */
	{ 79, -1, 32 },			/* m0 */
	{ 80, -1, 32 },			/* m1 */
	{ 81, -1, 32 },			/* m2 */
	{ 82, -1, 32 },			/* m3 */
	{ 83, -1, 32 },			/* expstate */
	{ 84, -1, 32 },			/* f64r_lo */
	{ 85, -1, 32 },			/* f64r_hi */
	{ 86, -1, 32 },			/* f64s */
	{ 87, -1, 32 },			/* f0 */
	{ 88, -1, 32 },			/* f1 */
	{ 89, -1, 32 },			/* f2 */
	{ 90, -1, 32 },			/* f3 */
	{ 91, -1, 32 },			/* f4 */
	{ 92, -1, 32 },			/* f5 */
	{ 93, -1, 32 },			/* f6 */
	{ 94, -1, 32 },			/* f7 */
	{ 95, -1, 32 },			/* f8 */
	{ 96, -1, 32 },			/* f9 */
	{ 97, -1, 32 },			/* f10 */
	{ 98, -1, 32 },			/* f11 */
	{ 99, -1, 32 },			/* f12 */
	{ 100, -1, 32 },		/* f13 */
	{ 101, -1, 32 },		/* f14 */
	{ 102, -1, 32 },		/* f15 */
	{ 103, -1, 32 },		/* fcr */
	{ 104, -1, 32 },		/* fsr */
};

const struct rtos_register_stacking nuttx_esp32_stacking = {
	.stack_registers_size = 26 * 4,
	.stack_growth_direction = -1,
	.num_output_registers = ARRAY_SIZE(nuttx_stack_offsets_esp32),
	.calculate_process_stack = rtos_generic_stack_align8,
	.register_offsets = nuttx_stack_offsets_esp32,
	.read_stack = nuttx_esp_xtensa_stack_read,
};

static const struct stack_register_offset nuttx_stack_offsets_esp32s2[] = {
	{ 0, 0x00, 32 },		/* PC */
	{ 1, 0x08, 32 },		/* A0 */
	{ 2, 0x0c, 32 },		/* A1 */
	{ 3, 0x10, 32 },		/* A2 */
	{ 4, 0x14, 32 },		/* A3 */
	{ 5, 0x18, 32 },		/* A4 */
	{ 6, 0x1c, 32 },		/* A5 */
	{ 7, 0x20, 32 },		/* A6 */
	{ 8, 0x24, 32 },		/* A7 */
	{ 9, 0x28, 32 },		/* A8 */
	{ 10, 0x2c, 32 },		/* A9 */
	{ 11, 0x30, 32 },		/* A10 */
	{ 12, 0x34, 32 },		/* A11 */
	{ 13, 0x38, 32 },		/* A12 */
	{ 14, 0x3c, 32 },		/* A13 */
	{ 15, 0x40, 32 },		/* A14 */
	{ 16, 0x44, 32 },		/* A15 */
	/* A16-A63 aren't in the stack frame because they've been flushed to the stack earlier */
	{ 17, -1, 32 },			/* A16 */
	{ 18, -1, 32 },			/* A17 */
	{ 19, -1, 32 },			/* A18 */
	{ 20, -1, 32 },			/* A19 */
	{ 21, -1, 32 },			/* A20 */
	{ 22, -1, 32 },			/* A21 */
	{ 23, -1, 32 },			/* A22 */
	{ 24, -1, 32 },			/* A23 */
	{ 25, -1, 32 },			/* A24 */
	{ 26, -1, 32 },			/* A25 */
	{ 27, -1, 32 },			/* A26 */
	{ 28, -1, 32 },			/* A27 */
	{ 29, -1, 32 },			/* A28 */
	{ 30, -1, 32 },			/* A29 */
	{ 31, -1, 32 },			/* A30 */
	{ 32, -1, 32 },			/* A31 */
	{ 33, -1, 32 },			/* A32 */
	{ 34, -1, 32 },			/* A33 */
	{ 35, -1, 32 },			/* A34 */
	{ 36, -1, 32 },			/* A35 */
	{ 37, -1, 32 },			/* A36 */
	{ 38, -1, 32 },			/* A37 */
	{ 39, -1, 32 },			/* A38 */
	{ 40, -1, 32 },			/* A39 */
	{ 41, -1, 32 },			/* A40 */
	{ 42, -1, 32 },			/* A41 */
	{ 43, -1, 32 },			/* A42 */
	{ 44, -1, 32 },			/* A43 */
	{ 45, -1, 32 },			/* A44 */
	{ 46, -1, 32 },			/* A45 */
	{ 47, -1, 32 },			/* A46 */
	{ 48, -1, 32 },			/* A47 */
	{ 49, -1, 32 },			/* A48 */
	{ 50, -1, 32 },			/* A49 */
	{ 51, -1, 32 },			/* A50 */
	{ 52, -1, 32 },			/* A51 */
	{ 53, -1, 32 },			/* A52 */
	{ 54, -1, 32 },			/* A53 */
	{ 55, -1, 32 },			/* A54 */
	{ 56, -1, 32 },			/* A55 */
	{ 57, -1, 32 },			/* A56 */
	{ 58, -1, 32 },			/* A57 */
	{ 59, -1, 32 },			/* A58 */
	{ 60, -1, 32 },			/* A59 */
	{ 61, -1, 32 },			/* A60 */
	{ 62, -1, 32 },			/* A61 */
	{ 63, -1, 32 },			/* A62 */
	{ 64, -1, 32 },			/* A63 */
	{ 65, 0x48, 32 },		/* SAR */
	{ 66, -1, 32 },			/* windowbase */
	{ 67, -1, 32 },			/* windowstart */
	{ 68, -1, 32 },			/* configid0 */
	{ 69, -1, 32 },			/* configid1 */
	{ 70, 0x04, 32 },		/* PS */
	{ 71, -1, 32 },			/* threadptr */
	{ 72, -1, 32 },			/* gpio_out */
};

const struct rtos_register_stacking nuttx_esp32s2_stacking = {
	.stack_registers_size = 25 * 4,
	.stack_growth_direction = -1,
	.num_output_registers = ARRAY_SIZE(nuttx_stack_offsets_esp32s2),
	.calculate_process_stack = rtos_generic_stack_align8,
	.register_offsets = nuttx_stack_offsets_esp32s2,
	.read_stack = nuttx_esp_xtensa_stack_read,
};

static const struct stack_register_offset nuttx_stack_offsets_esp32s3[] = {
	{ 0, 0x00, 32 },		/* PC */
	{ 1, 0x08, 32 },		/* A0 */
	{ 2, 0x0c, 32 },		/* A1 */
	{ 3, 0x10, 32 },		/* A2 */
	{ 4, 0x14, 32 },		/* A3 */
	{ 5, 0x18, 32 },		/* A4 */
	{ 6, 0x1c, 32 },		/* A5 */
	{ 7, 0x20, 32 },		/* A6 */
	{ 8, 0x24, 32 },		/* A7 */
	{ 9, 0x28, 32 },		/* A8 */
	{ 10, 0x2c, 32 },		/* A9 */
	{ 11, 0x30, 32 },		/* A10 */
	{ 12, 0x34, 32 },		/* A11 */
	{ 13, 0x38, 32 },		/* A12 */
	{ 14, 0x3c, 32 },		/* A13 */
	{ 15, 0x40, 32 },		/* A14 */
	{ 16, 0x44, 32 },		/* A15 */
	/* A16-A63 aren't in the stack frame because they've been flushed to the stack earlier */
	{ 17, -1, 32 },			/* A16 */
	{ 18, -1, 32 },			/* A17 */
	{ 19, -1, 32 },			/* A18 */
	{ 20, -1, 32 },			/* A19 */
	{ 21, -1, 32 },			/* A20 */
	{ 22, -1, 32 },			/* A21 */
	{ 23, -1, 32 },			/* A22 */
	{ 24, -1, 32 },			/* A23 */
	{ 25, -1, 32 },			/* A24 */
	{ 26, -1, 32 },			/* A25 */
	{ 27, -1, 32 },			/* A26 */
	{ 28, -1, 32 },			/* A27 */
	{ 29, -1, 32 },			/* A28 */
	{ 30, -1, 32 },			/* A29 */
	{ 31, -1, 32 },			/* A30 */
	{ 32, -1, 32 },			/* A31 */
	{ 33, -1, 32 },			/* A32 */
	{ 34, -1, 32 },			/* A33 */
	{ 35, -1, 32 },			/* A34 */
	{ 36, -1, 32 },			/* A35 */
	{ 37, -1, 32 },			/* A36 */
	{ 38, -1, 32 },			/* A37 */
	{ 39, -1, 32 },			/* A38 */
	{ 40, -1, 32 },			/* A39 */
	{ 41, -1, 32 },			/* A40 */
	{ 42, -1, 32 },			/* A41 */
	{ 43, -1, 32 },			/* A42 */
	{ 44, -1, 32 },			/* A43 */
	{ 45, -1, 32 },			/* A44 */
	{ 46, -1, 32 },			/* A45 */
	{ 47, -1, 32 },			/* A46 */
	{ 48, -1, 32 },			/* A47 */
	{ 49, -1, 32 },			/* A48 */
	{ 50, -1, 32 },			/* A49 */
	{ 51, -1, 32 },			/* A50 */
	{ 52, -1, 32 },			/* A51 */
	{ 53, -1, 32 },			/* A52 */
	{ 54, -1, 32 },			/* A53 */
	{ 55, -1, 32 },			/* A54 */
	{ 56, -1, 32 },			/* A55 */
	{ 57, -1, 32 },			/* A56 */
	{ 58, -1, 32 },			/* A57 */
	{ 59, -1, 32 },			/* A58 */
	{ 60, -1, 32 },			/* A59 */
	{ 61, -1, 32 },			/* A60 */
	{ 62, -1, 32 },			/* A61 */
	{ 63, -1, 32 },			/* A62 */
	{ 64, -1, 32 },			/* A63 */
	{ 65, 0x58, 32 },		/* lbeg */
	{ 66, 0x5c, 32 },		/* lend */
	{ 67, 0x60, 32 },		/* lcount */
	{ 68, 0x48, 32 },		/* SAR */
	{ 69, -1, 32 },			/* windowbase */
	{ 70, -1, 32 },			/* windowstart */
	{ 71, -1, 32 },			/* configid0 */
	{ 72, -1, 32 },			/* configid1 */
	{ 73, 0x04, 32 },		/* PS */
	{ 74, -1, 32 },			/* threadptr */
	{ 75, -1, 32 },			/* br */
	{ 76, 0x54, 32 },		/* scompare1 */
	{ 77, -1, 32 },			/* acclo */
	{ 78, -1, 32 },			/* acchi */
	{ 79, -1, 32 },			/* m0 */
	{ 80, -1, 32 },			/* m1 */
	{ 81, -1, 32 },			/* m2 */
	{ 82, -1, 32 },			/* m3 */
	{ 83, -1, 32 },			/* gpio_out */
	{ 84, -1, 32 },			/* f0 */
	{ 85, -1, 32 },			/* f1 */
	{ 86, -1, 32 },			/* f2 */
	{ 87, -1, 32 },			/* f3 */
	{ 88, -1, 32 },			/* f4 */
	{ 89, -1, 32 },			/* f5 */
	{ 90, -1, 32 },			/* f6 */
	{ 91, -1, 32 },			/* f7 */
	{ 92, -1, 32 },			/* f8 */
	{ 93, -1, 32 },			/* f9 */
	{ 94, -1, 32 },			/* f10 */
	{ 95, -1, 32 },			/* f11 */
	{ 96, -1, 32 },			/* f12 */
	{ 97, -1, 32 },			/* f13 */
	{ 98, -1, 32 },			/* f14 */
	{ 99, -1, 32 },			/* f15 */
	{ 100, -1, 32 },		/* fcr */
	{ 101, -1, 32 },		/* fsr */
	{ 102, -1, 32 },		/* accx_0 */
	{ 103, -1, 32 },		/* accx_1 */
	{ 104, -1, 32 },		/* qacc_h_0 */
	{ 105, -1, 32 },		/* qacc_h_1 */
	{ 106, -1, 32 },		/* qacc_h_2 */
	{ 107, -1, 32 },		/* qacc_h_3 */
	{ 108, -1, 32 },		/* qacc_h_4 */
	{ 109, -1, 32 },		/* qacc_l_0 */
	{ 110, -1, 32 },		/* qacc_l_1 */
	{ 111, -1, 32 },		/* qacc_l_2 */
	{ 112, -1, 32 },		/* qacc_l_3 */
	{ 113, -1, 32 },		/* qacc_l_4 */
	{ 114, -1, 32 },		/* sar_byte */
	{ 115, -1, 32 },		/* fft_bit_width */
	{ 116, -1, 32 },		/* ua_state_0 */
	{ 117, -1, 32 },		/* ua_state_1 */
	{ 118, -1, 32 },		/* ua_state_2 */
	{ 119, -1, 32 },		/* ua_state_3 */
	{ 120, -1, 32 },		/* q0 */
	{ 121, -1, 32 },		/* q1 */
	{ 122, -1, 32 },		/* q2 */
	{ 123, -1, 32 },		/* q3 */
	{ 124, -1, 32 },		/* q4 */
	{ 125, -1, 32 },		/* q5 */
	{ 126, -1, 32 },		/* q6 */
	{ 127, -1, 32 },		/* q7 */
};

const struct rtos_register_stacking nuttx_esp32s3_stacking = {
	.stack_registers_size = 26 * 4,
	.stack_growth_direction = -1,
	.num_output_registers = ARRAY_SIZE(nuttx_stack_offsets_esp32s3),
	.calculate_process_stack = rtos_generic_stack_align8,
	.register_offsets = nuttx_stack_offsets_esp32s3,
	.read_stack = nuttx_esp_xtensa_stack_read,
};
