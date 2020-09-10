
#ifndef _ISAAC_IR_LOWERING_H
#define _ISAAC_IR_LOWERING_H

#ifdef __cplusplus
extern "C" {
#endif

/*
    ================================== util.h ==================================
    Module for IR lowering related utilities
    ----------------------------------------------------------------------------
*/

#include "IR/ir.h"

errorcode_t ir_lower_const_cast(ir_pool_t *pool, ir_value_t **inout_value);
errorcode_t ir_lower_const_bitcast(ir_pool_t *pool, ir_value_t **inout_value);
errorcode_t ir_lower_const_trunc(ir_pool_t *pool, ir_value_t **inout_value);
errorcode_t ir_lower_const_sext(ir_pool_t *pool, ir_value_t **inout_value);

#ifdef __cplusplus
}
#endif

#endif // _ISAAC_IR_LOWERING_H
