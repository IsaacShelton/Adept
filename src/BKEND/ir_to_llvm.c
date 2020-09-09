
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <ctype.h>

#include "IR/ir.h"
#include "UTIL/util.h"
#include "UTIL/color.h"
#include "UTIL/filename.h"
#include "BKEND/ir_to_llvm.h"
#include "DRVR/debug.h"
#include "DRVR/object.h"

LLVMTypeRef ir_to_llvm_type(ir_type_t *ir_type){
    // Converts an ir type to an llvm type
    LLVMTypeRef type_ref_tmp;

    switch(ir_type->kind){
    case TYPE_KIND_POINTER:
        type_ref_tmp = ir_to_llvm_type((ir_type_t*) ir_type->extra);
        if(type_ref_tmp == NULL) return NULL;
        return LLVMPointerType(type_ref_tmp, 0);
    case TYPE_KIND_S8:      return LLVMInt8Type();
    case TYPE_KIND_S16:     return LLVMInt16Type();
    case TYPE_KIND_S32:     return LLVMInt32Type();
    case TYPE_KIND_S64:     return LLVMInt64Type();
    case TYPE_KIND_U8:      return LLVMInt8Type();
    case TYPE_KIND_U16:     return LLVMInt16Type();
    case TYPE_KIND_U32:     return LLVMInt32Type();
    case TYPE_KIND_U64:     return LLVMInt64Type();
    case TYPE_KIND_HALF:    return LLVMHalfType();
    case TYPE_KIND_FLOAT:   return LLVMFloatType();
    case TYPE_KIND_DOUBLE:  return LLVMDoubleType();
    case TYPE_KIND_BOOLEAN: return LLVMInt1Type();
    case TYPE_KIND_UNION:
        printf("INTERNAL ERROR: TYPE_KIND_UNION not implemented yet inside ir_to_llvm_type\n");
        return NULL;
    case TYPE_KIND_STRUCTURE: {
            // TODO: Should probably cache struct types so they don't have to be
            //           remade into LLVM types every time we use them.

            ir_type_extra_composite_t *composite = (ir_type_extra_composite_t*) ir_type->extra;
            LLVMTypeRef fields[composite->subtypes_length];

            for(length_t i = 0; i != composite->subtypes_length; i++){
                fields[i] = ir_to_llvm_type(composite->subtypes[i]);
                if(fields[i] == NULL) return NULL;
            }

            LLVMTypeRef type_ref_tmp = LLVMStructType(fields, composite->subtypes_length,
                composite->traits & TYPE_KIND_COMPOSITE_PACKED);

            return type_ref_tmp;
        }
    case TYPE_KIND_VOID: return LLVMVoidType();
    case TYPE_KIND_FUNCPTR: {
            ir_type_extra_function_t *function = (ir_type_extra_function_t*) ir_type->extra;
            LLVMTypeRef args[function->arity];

            for(length_t i = 0; i != function->arity; i++){
                args[i] = ir_to_llvm_type(function->arg_types[i]);
                if(args[i] == NULL) return NULL;
            }

            LLVMTypeRef type_ref_tmp = LLVMFunctionType(ir_to_llvm_type(function->return_type),
                args, function->arity, function->traits & TYPE_KIND_FUNC_VARARG);
            type_ref_tmp = LLVMPointerType(type_ref_tmp, 0);

            return type_ref_tmp;
        }
        break;
    case TYPE_KIND_FIXED_ARRAY:{
            ir_type_extra_fixed_array_t *fixed_array = (ir_type_extra_fixed_array_t*) ir_type->extra;
            type_ref_tmp = ir_to_llvm_type(fixed_array->subtype);
            if(type_ref_tmp == NULL) return NULL;
            return LLVMArrayType(type_ref_tmp, fixed_array->length);
        }
        break;
    default: return NULL; // No suitable llvm type
    }
}

LLVMValueRef ir_to_llvm_value(llvm_context_t *llvm, ir_value_t *value){
    // Retrieves a literal or previously computed value

    if(value == NULL){
        redprintf("INTERNAL ERROR: ir_to_llvm_value() got NULL pointer\n");
        return NULL;
    }

    switch(value->value_type){
    case VALUE_TYPE_LITERAL: {
            switch(value->type->kind){
            case TYPE_KIND_S8: return LLVMConstInt(LLVMInt8Type(), *((adept_byte*) value->extra), true);
            case TYPE_KIND_U8: return LLVMConstInt(LLVMInt8Type(), *((adept_ubyte*) value->extra), false);
            case TYPE_KIND_S16: return LLVMConstInt(LLVMInt16Type(), *((adept_short*) value->extra), true);
            case TYPE_KIND_U16: return LLVMConstInt(LLVMInt16Type(), *((adept_ushort*) value->extra), false);
            case TYPE_KIND_S32: return LLVMConstInt(LLVMInt32Type(), *((adept_int*) value->extra), true);
            case TYPE_KIND_U32: return LLVMConstInt(LLVMInt32Type(), *((adept_uint*) value->extra), false);
            case TYPE_KIND_S64: return LLVMConstInt(LLVMInt64Type(), *((adept_long*) value->extra), true);
            case TYPE_KIND_U64: return LLVMConstInt(LLVMInt64Type(), *((adept_ulong*) value->extra), false);
            case TYPE_KIND_FLOAT: return LLVMConstReal(LLVMFloatType(), *((adept_float*) value->extra));
            case TYPE_KIND_DOUBLE: return LLVMConstReal(LLVMDoubleType(), *((adept_double*) value->extra));
            case TYPE_KIND_BOOLEAN: return LLVMConstInt(LLVMInt1Type(), *((adept_bool*) value->extra), false);
            default:
                redprintf("INTERNAL ERROR: Unknown type kind literal in ir_to_llvm_value\n");
                return NULL;
            }
        }
    case VALUE_TYPE_RESULT: {
            ir_value_result_t *extra = (ir_value_result_t*) value->extra;
            return llvm->catalog->blocks[extra->block_id].value_references[extra->instruction_id];
        }
    case VALUE_TYPE_NULLPTR:
        return LLVMConstNull(LLVMPointerType(LLVMInt8Type(), 0));
    case VALUE_TYPE_NULLPTR_OF_TYPE:
        return LLVMConstNull(ir_to_llvm_type(value->type));
    case VALUE_TYPE_ARRAY_LITERAL: {
            ir_value_array_literal_t *array_literal = value->extra;

            // Assume that value->type is a pointer to array element type
            LLVMTypeRef type = ir_to_llvm_type((ir_type_t*) value->type->extra);
            
            LLVMValueRef values[array_literal->length];

            for(length_t i = 0; i != array_literal->length; i++){
                // Assumes ir_value_t values are constants (should have been checked earlier)
                values[i] = ir_to_llvm_value(llvm, array_literal->values[i]);
            }

            LLVMValueRef static_array = LLVMConstArray(type, values, array_literal->length);
            LLVMValueRef global_data = LLVMAddGlobal(llvm->module, LLVMArrayType(type, array_literal->length), "");
            LLVMSetLinkage(global_data, LLVMInternalLinkage);
            LLVMSetGlobalConstant(global_data, true);
            LLVMSetInitializer(global_data, static_array);

            LLVMValueRef indices[2];
            indices[0] = LLVMConstInt(LLVMInt32Type(), 0, true);
            indices[1] = LLVMConstInt(LLVMInt32Type(), 0, true);

            return LLVMConstGEP(global_data, indices, 2);
        }
    case VALUE_TYPE_STRUCT_LITERAL: {
            ir_value_struct_literal_t *struct_literal = value->extra;

            // Assume that value->type is a pointer to a struct
            LLVMTypeRef type = ir_to_llvm_type(value->type);
            
            LLVMValueRef values[struct_literal->length];

            for(length_t i = 0; i != struct_literal->length; i++){
                // Assumes ir_value_t values are constants (should have been checked earlier)
                values[i] = ir_to_llvm_value(llvm, struct_literal->values[i]);
            }
            
            return LLVMConstNamedStruct(type, values, struct_literal->length);
        }
    case VALUE_TYPE_ANON_GLOBAL: case VALUE_TYPE_CONST_ANON_GLOBAL: {
            ir_value_anon_global_t *extra = (ir_value_anon_global_t*) value->extra;
            return llvm->anon_global_variables[extra->anon_global_id];
        }
    case VALUE_TYPE_CSTR_OF_LEN: {
            ir_value_cstr_of_len_t *cstr_of_len = value->extra;
            LLVMValueRef global_data = LLVMAddGlobal(llvm->module, LLVMArrayType(LLVMInt8Type(), cstr_of_len->length), ".str");
            LLVMSetLinkage(global_data, LLVMInternalLinkage);
            LLVMSetGlobalConstant(global_data, true);
            LLVMSetInitializer(global_data, LLVMConstString(cstr_of_len->array, cstr_of_len->length, true));
            LLVMValueRef indices[2];
            indices[0] = LLVMConstInt(LLVMInt32Type(), 0, true);
            indices[1] = LLVMConstInt(LLVMInt32Type(), 0, true);
            LLVMValueRef literal = LLVMBuildGEP(llvm->builder, global_data, indices, 2, "");
            return literal;
        }
    case VALUE_TYPE_CONST_BITCAST:
            return LLVMConstBitCast(ir_to_llvm_value(llvm, value->extra), ir_to_llvm_type(value->type));
    case VALUE_TYPE_CONST_ZEXT:
            return LLVMConstZExt(ir_to_llvm_value(llvm, value->extra), ir_to_llvm_type(value->type));
    case VALUE_TYPE_CONST_SEXT:
            return LLVMConstSExt(ir_to_llvm_value(llvm, value->extra), ir_to_llvm_type(value->type));
    case VALUE_TYPE_CONST_FEXT:
            return LLVMConstFPExt(ir_to_llvm_value(llvm, value->extra), ir_to_llvm_type(value->type));
    case VALUE_TYPE_CONST_TRUNC:
            return LLVMConstTrunc(ir_to_llvm_value(llvm, value->extra), ir_to_llvm_type(value->type));
    case VALUE_TYPE_CONST_FTRUNC:
            return LLVMConstFPTrunc(ir_to_llvm_value(llvm, value->extra), ir_to_llvm_type(value->type));
    case VALUE_TYPE_CONST_INTTOPTR:
            return LLVMConstIntToPtr(ir_to_llvm_value(llvm, value->extra), ir_to_llvm_type(value->type));
    case VALUE_TYPE_CONST_PTRTOINT:
            return LLVMConstPtrToInt(ir_to_llvm_value(llvm, value->extra), ir_to_llvm_type(value->type));
    case VALUE_TYPE_CONST_FPTOUI:
            return LLVMConstFPToUI(ir_to_llvm_value(llvm, value->extra), ir_to_llvm_type(value->type));
    case VALUE_TYPE_CONST_FPTOSI:
            return LLVMConstFPToSI(ir_to_llvm_value(llvm, value->extra), ir_to_llvm_type(value->type));
    case VALUE_TYPE_CONST_UITOFP:
            return LLVMConstUIToFP(ir_to_llvm_value(llvm, value->extra), ir_to_llvm_type(value->type));
    case VALUE_TYPE_CONST_SITOFP:
            return LLVMConstSIToFP(ir_to_llvm_value(llvm, value->extra), ir_to_llvm_type(value->type));
    case VALUE_TYPE_CONST_REINTERPRET:
            return ir_to_llvm_value(llvm, value->extra);
    case VALUE_TYPE_STRUCT_CONSTRUCTION: {
            ir_value_struct_construction_t *construction = (ir_value_struct_construction_t*) value->extra;

            LLVMValueRef constructed = LLVMGetUndef(ir_to_llvm_type(value->type));

            for(length_t i = 0; i != construction->length; i++){
                constructed = LLVMBuildInsertValue(llvm->builder, constructed, ir_to_llvm_value(llvm, construction->values[i]), i, "");
            }

            return constructed;
        }
    case VALUE_TYPE_OFFSETOF: {
            ir_value_offsetof_t *offsetof = (ir_value_offsetof_t*) value->extra;
            unsigned long long offset = LLVMOffsetOfElement(llvm->data_layout, ir_to_llvm_type(offsetof->type), offsetof->index);
            return LLVMConstInt(LLVMInt64Type(), offset, false);
        }
    case VALUE_TYPE_CONST_SIZEOF: {
            ir_value_const_sizeof_t *const_sizeof = (ir_value_const_sizeof_t*) value->extra;
            length_t type_size = const_sizeof->type->kind == TYPE_KIND_VOID ? 0 : LLVMABISizeOfType(llvm->data_layout, ir_to_llvm_type(const_sizeof->type));
            return LLVMConstInt(LLVMInt64Type(), type_size, false);
        }
    case VALUE_TYPE_CONST_ADD: {
            ir_value_const_math_t *const_add = (ir_value_const_math_t*) value->extra;
            return LLVMConstAdd(ir_to_llvm_value(llvm, const_add->a), ir_to_llvm_value(llvm, const_add->b));
        }
    default:
        redprintf("INTERNAL ERROR: Unknown value type %d of value in ir_to_llvm_value\n", value->value_type);
        return NULL;
    }

    return NULL;
}

errorcode_t ir_to_llvm_functions(llvm_context_t *llvm, object_t *object){
    // Generates llvm function skeletons from ir function data

    LLVMModuleRef llvm_module = llvm->module;
    ir_func_t *module_funcs = object->ir_module.funcs;
    length_t module_funcs_length = object->ir_module.funcs_length;
    LLVMValueRef *func_skeletons = llvm->func_skeletons;

    LLVMAttributeRef nounwind = LLVMCreateEnumAttribute(LLVMGetGlobalContext(), LLVMGetEnumAttributeKindForName("nounwind", 8), 0);

    for(length_t ir_func_id = 0; ir_func_id != module_funcs_length; ir_func_id++){
        ir_func_t *ir_func = &module_funcs[ir_func_id];
        LLVMTypeRef parameters[ir_func->arity];

        for(length_t a = 0; a != ir_func->arity; a++){
            parameters[a] = ir_to_llvm_type(ir_func->argument_types[a]);
            if(parameters[a] == NULL) return FAILURE;
        }

        LLVMTypeRef return_type = ir_to_llvm_type(ir_func->return_type);
        LLVMTypeRef llvm_func_type = LLVMFunctionType(return_type, parameters, ir_func->arity, ir_func->traits & IR_FUNC_VARARG);

        LLVMValueRef *skeleton = &func_skeletons[ir_func_id];

        if(ir_func->traits & IR_FUNC_FOREIGN || ir_func->traits & IR_FUNC_MAIN){
            *skeleton = LLVMAddFunction(llvm_module, ir_func->name, llvm_func_type);
        } else {
            char adept_implementation_name[256];
            ir_implementation(ir_func_id, 'a', adept_implementation_name);

            *skeleton = LLVMAddFunction(llvm_module, adept_implementation_name, llvm_func_type);
            LLVMSetLinkage(*skeleton, LLVMPrivateLinkage);
        }

        LLVMCallConv call_conv = ir_func->traits & IR_FUNC_STDCALL ? LLVMX86StdcallCallConv : LLVMCCallConv;
        LLVMSetFunctionCallConv(*skeleton, call_conv);
        
        // Add nounwind to everything that isn't foreign
        if(!(ir_func->traits & IR_FUNC_FOREIGN))
            LLVMAddAttributeAtIndex(*skeleton, LLVMAttributeFunctionIndex, nounwind);
    }

    return SUCCESS;
}

errorcode_t ir_to_llvm_function_bodies(llvm_context_t *llvm, object_t *object){
    // Generates llvm function bodies from ir function data
    // NOTE: Expects function skeltons to already be present

    LLVMModuleRef llvm_module = llvm->module;
    ir_func_t *module_funcs = object->ir_module.funcs;
    length_t module_funcs_length = object->ir_module.funcs_length;
    LLVMValueRef *func_skeletons = llvm->func_skeletons;

    llvm_phi2_relocation_t *unrelocated_phis = NULL;
    length_t unrelocated_phis_length = 0;
    length_t unrelocated_phis_capacity = 0;

    for(length_t f = 0; f != module_funcs_length; f++){
        LLVMBuilderRef builder = LLVMCreateBuilder();
        ir_basicblock_t *basicblocks = module_funcs[f].basicblocks;
        length_t basicblocks_length = module_funcs[f].basicblocks_length;

        value_catalog_t catalog;
        catalog.blocks = malloc(sizeof(value_catalog_block_t) * basicblocks_length);
        catalog.blocks_length = basicblocks_length;
        for(length_t c = 0; c != basicblocks_length; c++) catalog.blocks[c].value_references = malloc(sizeof(LLVMValueRef) * basicblocks[c].instructions_length);

        varstack_t stack;
        stack.values = malloc(sizeof(LLVMValueRef) * module_funcs[f].variable_count);
        stack.types = malloc(sizeof(LLVMTypeRef) * module_funcs[f].variable_count);
        stack.length = module_funcs[f].variable_count;

        llvm->module = llvm_module;
        llvm->builder = builder;
        llvm->catalog = &catalog;
        llvm->stack = &stack;

        LLVMBasicBlockRef *llvm_blocks = malloc(sizeof(LLVMBasicBlockRef) * basicblocks_length);
        ir_instr_t *instr;
        LLVMValueRef llvm_result;

        // If the true exit point of a block changed, its real value will be in here
        // (Used for PHI instructions to have the proper end point)
        LLVMBasicBlockRef *llvm_exit_blocks = malloc(sizeof(LLVMBasicBlockRef) * basicblocks_length);

        // Information about PHI instructions that need to have their incoming blocks delayed
        unrelocated_phis_length = 0;

        // Create basicblocks
        for(length_t b = 0; b != basicblocks_length; b++){
            llvm_blocks[b] = LLVMAppendBasicBlock(func_skeletons[f], "");
            llvm_exit_blocks[b] = llvm_blocks[b];
        }

        // Drop references to any old PHIs
        llvm->line_phi = NULL;
        llvm->column_phi = NULL;

        if(llvm->compiler->checks & COMPILER_NULL_CHECKS && basicblocks_length != 0){
            // Create local variables to hold line number and column number for
            // when we call pseudo-function to handle null check failures
            // Create pseudo-function
            llvm->null_check_on_fail_block = LLVMAppendBasicBlock(func_skeletons[f], "");
            LLVMPositionBuilderAtEnd(builder, llvm->null_check_on_fail_block);

            // Establish dependencies and define them if necessary
            LLVMValueRef printf_fn = LLVMGetNamedFunction(llvm->module, "printf");
            LLVMValueRef exit_fn = LLVMGetNamedFunction(llvm->module, "exit");

            if(exit_fn == NULL){
                LLVMTypeRef int32 = LLVMInt32Type();
                LLVMTypeRef exit_fn_type = LLVMFunctionType(int32, &int32, 1, false);
                exit_fn = LLVMAddFunction(llvm->module, "exit", exit_fn_type);
            }

            if(printf_fn == NULL){
                LLVMTypeRef int32 = LLVMInt32Type();
                LLVMTypeRef charptr = LLVMPointerType(LLVMInt8Type(), 0);
                LLVMTypeRef printf_fn_type = LLVMFunctionType(int32, &charptr, 1, true);
                printf_fn = LLVMAddFunction(llvm->module, "printf", printf_fn_type);
            }

            LLVMValueRef global_data;

            // Create template error message
            if(!llvm->has_null_check_failure_message_bytes){
                const char *error_msg = "===== RUNTIME ERROR: NULL POINTER DEREFERENCE, MEMBER-ACCESS, OR ELEMENT-ACCESS! =====\nIn file:\t%s\nIn function:\t%s\nLine:\t%d\nColumn:\t%d\n";
                length_t error_msg_length = strlen(error_msg) + 1;
                global_data = LLVMAddGlobal(llvm->module, LLVMArrayType(LLVMInt8Type(), error_msg_length), ".str");
                LLVMSetLinkage(global_data, LLVMInternalLinkage);
                LLVMSetGlobalConstant(global_data, true);
                LLVMSetInitializer(global_data, LLVMConstString(error_msg, error_msg_length, true));
                llvm->null_check_failure_message_bytes = global_data;
                llvm->has_null_check_failure_message_bytes = true;
            }
            
            LLVMValueRef indices[2];
            indices[0] = LLVMConstInt(LLVMInt32Type(), 0, true);
            indices[1] = LLVMConstInt(LLVMInt32Type(), 0, true);
            LLVMValueRef arg = LLVMBuildGEP(llvm->builder, llvm->null_check_failure_message_bytes, indices, 2, "");

            // Decide on filename to use for error message
            const char *filename = module_funcs[f].maybe_filename;
            if(filename == NULL) filename = "<unknown file>";

            // Decide on function definition string to use for error function
            const char *func_name = module_funcs[f].maybe_definition_string;
            if(func_name == NULL) func_name = module_funcs[f].name;

            // Define filename string
            length_t filename_len = strlen(filename) + 1;
            global_data = LLVMAddGlobal(llvm->module, LLVMArrayType(LLVMInt8Type(), filename_len), ".str");
            LLVMSetLinkage(global_data, LLVMInternalLinkage);
            LLVMSetGlobalConstant(global_data, true);
            LLVMSetInitializer(global_data, LLVMConstString(filename, filename_len, true));
            indices[0] = LLVMConstInt(LLVMInt32Type(), 0, true);
            indices[1] = LLVMConstInt(LLVMInt32Type(), 0, true);
            LLVMValueRef filename_str = LLVMBuildGEP(llvm->builder, global_data, indices, 2, "");

            // Define function definition string
            length_t func_name_len = strlen(func_name) + 1;
            global_data = LLVMAddGlobal(llvm->module, LLVMArrayType(LLVMInt8Type(), func_name_len), ".str");
            LLVMSetLinkage(global_data, LLVMInternalLinkage);
            LLVMSetGlobalConstant(global_data, true);
            LLVMSetInitializer(global_data, LLVMConstString(func_name, func_name_len, true));
            indices[0] = LLVMConstInt(LLVMInt32Type(), 0, true);
            indices[1] = LLVMConstInt(LLVMInt32Type(), 0, true);
            LLVMValueRef func_name_str = LLVMBuildGEP(llvm->builder, global_data, indices, 2, "");

            llvm->line_phi = LLVMBuildPhi(llvm->builder, LLVMInt32Type(), "");
            llvm->column_phi = LLVMBuildPhi(llvm->builder, LLVMInt32Type(), "");

            // Create argument list
            LLVMValueRef args[] = {arg, filename_str, func_name_str, llvm->line_phi, llvm->column_phi};

            // Print the error message
            LLVMBuildCall(builder, printf_fn, args, 5, "");

            // Exit the program
            arg = LLVMConstInt(LLVMInt32Type(), 1, true);
            LLVMBuildCall(builder, exit_fn, &arg, 1, "");
            LLVMBuildUnreachable(builder);
        }

        for(length_t b = 0; b != basicblocks_length; b++){
            LLVMPositionBuilderAtEnd(builder, llvm_blocks[b]);
            ir_basicblock_t *basicblock = &basicblocks[b];

            if(b == 0){ // Do any function entry instructions needed
                // Allocate stack variables
                for(length_t s = 0; s != stack.length; s++){
                    bridge_var_t *var = bridge_scope_find_var_by_id(module_funcs[f].scope, s);

                    if(var == NULL){
                        redprintf("INTERNAL ERROR: VAR IN EXPORT STAGE COULD NOT BE FOUND (id: %d)\n", (int) s);
                        for(length_t c = 0; c != catalog.blocks_length; c++) free(catalog.blocks[c].value_references);
                        free(catalog.blocks);
                        free(stack.values);
                        free(stack.types);
                        free(llvm_blocks);
                        free(llvm_exit_blocks);
                        free(unrelocated_phis);
                        LLVMDisposeBuilder(builder);
                        return FAILURE;
                    }

                    LLVMTypeRef alloca_type = ir_to_llvm_type(var->ir_type);

                    if(alloca_type == NULL){
                        for(length_t c = 0; c != catalog.blocks_length; c++) free(catalog.blocks[c].value_references);
                        free(catalog.blocks);
                        free(stack.values);
                        free(stack.types);
                        free(llvm_blocks);
                        free(llvm_exit_blocks);
                        free(unrelocated_phis);
                        LLVMDisposeBuilder(builder);
                        return FAILURE;
                    }

                    stack.values[s] = LLVMBuildAlloca(builder, alloca_type, "");
                    stack.types[s] = alloca_type;

                    if(s < module_funcs[f].arity){
                        // Function argument that needs passed argument value
                        LLVMBuildStore(builder, LLVMGetParam(func_skeletons[f], s), stack.values[s]);
                    }
                }
            }

            for(length_t i = 0; i != basicblock->instructions_length; i++){
                switch(basicblock->instructions[i]->id){
                case INSTRUCTION_RET:
                    instr = basicblock->instructions[i];
                    LLVMBuildRet(builder, ((ir_instr_ret_t*) instr)->value == NULL ? NULL : ir_to_llvm_value(llvm, ((ir_instr_ret_t*) instr)->value));
                    break;
                case INSTRUCTION_ADD:
                    instr = basicblock->instructions[i];

                    // Do excess instructions for adding pointers to get llvm to shut up
                    if(((ir_instr_math_t*) instr)->a->type->kind == TYPE_KIND_POINTER){
                        LLVMValueRef val_a = ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a);
                        LLVMValueRef val_b = ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b);
                        val_a = LLVMBuildPtrToInt(builder, val_a, LLVMInt64Type(), "");
                        val_b = LLVMBuildPtrToInt(builder, val_b, LLVMInt64Type(), "");
                        llvm_result = LLVMBuildAdd(builder, val_a, val_b, "");
                        llvm_result = LLVMBuildIntToPtr(builder, llvm_result, ir_to_llvm_type(((ir_instr_math_t*) instr)->a->type), "");
                        catalog.blocks[b].value_references[i] = llvm_result;
                    } else {
                        llvm_result = LLVMBuildAdd(builder, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                        catalog.blocks[b].value_references[i] = llvm_result;    
                    }
                    break;
                case INSTRUCTION_FADD:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildFAdd(builder, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_SUBTRACT:
                    instr = basicblock->instructions[i];

                    // Do excess instructions for subtracting pointers to get llvm to shut up
                    if(((ir_instr_math_t*) instr)->a->type->kind == TYPE_KIND_POINTER){
                        LLVMValueRef val_a = ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a);
                        LLVMValueRef val_b = ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b);
                        val_a = LLVMBuildPtrToInt(builder, val_a, LLVMInt64Type(), "");
                        val_b = LLVMBuildPtrToInt(builder, val_b, LLVMInt64Type(), "");
                        llvm_result = LLVMBuildSub(builder, val_a, val_b, "");
                        llvm_result = LLVMBuildIntToPtr(builder, llvm_result, ir_to_llvm_type(((ir_instr_math_t*) instr)->a->type), "");
                        catalog.blocks[b].value_references[i] = llvm_result;
                    } else {
                        llvm_result = LLVMBuildSub(builder, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                        catalog.blocks[b].value_references[i] = llvm_result;    
                    }
                    break;
                case INSTRUCTION_FSUBTRACT:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildFSub(builder, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_MULTIPLY:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildMul(builder, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_FMULTIPLY:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildFMul(builder, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_UDIVIDE:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildUDiv(builder, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_SDIVIDE:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildSDiv(builder, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_FDIVIDE:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildFDiv(builder, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_UMODULUS:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildURem(builder, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_SMODULUS:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildSRem(builder, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_FMODULUS:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildFRem(builder, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_CALL: {
                        instr = basicblocks[b].instructions[i];
                        LLVMValueRef arguments[((ir_instr_call_t*) instr)->values_length];

                        for(length_t v = 0; v != ((ir_instr_call_t*) instr)->values_length; v++){
                            arguments[v] = ir_to_llvm_value(llvm, ((ir_instr_call_t*) instr)->values[v]);
                        }

                        const char *implementation_name;
                        char adept_implementation_name[256];
                        ir_func_t *target_ir_func = &object->ir_module.funcs[((ir_instr_call_t*) instr)->ir_func_id];

                        if(target_ir_func->traits & IR_FUNC_FOREIGN || target_ir_func->traits & IR_FUNC_MAIN){
                            implementation_name = target_ir_func->name;
                        } else {
                            ir_implementation(((ir_instr_call_t*) instr)->ir_func_id, 'a', adept_implementation_name);
                            implementation_name = adept_implementation_name;
                        }

                        LLVMValueRef named_func = LLVMGetNamedFunction(llvm_module, implementation_name);
                        assert(named_func != NULL);

                        llvm_result = LLVMBuildCall(builder, named_func, arguments, ((ir_instr_call_t*) instr)->values_length, "");
                        catalog.blocks[b].value_references[i] = llvm_result;
                    }
                    break;
                case INSTRUCTION_CALL_ADDRESS: {
                        instr = basicblocks[b].instructions[i];
                        LLVMValueRef arguments[((ir_instr_call_address_t*) instr)->values_length];

                        for(length_t v = 0; v != ((ir_instr_call_address_t*) instr)->values_length; v++){
                            arguments[v] = ir_to_llvm_value(llvm, ((ir_instr_call_address_t*) instr)->values[v]);
                        }

                        LLVMValueRef target_func = ir_to_llvm_value(llvm, ((ir_instr_call_address_t*) instr)->address);

                        llvm_result = LLVMBuildCall(builder, target_func, arguments, ((ir_instr_call_address_t*) instr)->values_length, "");
                        catalog.blocks[b].value_references[i] = llvm_result;
                    }
                    break;
                case INSTRUCTION_STORE: {
                        instr = basicblock->instructions[i];

                        ir_instr_store_t *store_instr = (ir_instr_store_t*) instr;
                        LLVMValueRef destination = ir_to_llvm_value(llvm, store_instr->destination);
                        ir_to_llvm_null_check(llvm, f, destination, store_instr->maybe_line_number, store_instr->maybe_column_number, &llvm_exit_blocks[b]);

                        llvm_result = LLVMBuildStore(builder, ir_to_llvm_value(llvm, ((ir_instr_store_t*) instr)->value), destination);
                        catalog.blocks[b].value_references[i] = llvm_result;
                    }
                    break;
                case INSTRUCTION_LOAD: {
                        instr = basicblock->instructions[i];

                        ir_instr_load_t *load_instr = (ir_instr_load_t*) instr;
                        LLVMValueRef pointer = ir_to_llvm_value(llvm, load_instr->value);
                        ir_to_llvm_null_check(llvm, f, pointer, load_instr->maybe_line_number, load_instr->maybe_column_number, &llvm_exit_blocks[b]);

                        llvm_result = LLVMBuildLoad(builder, pointer, "");
                        catalog.blocks[b].value_references[i] = llvm_result;
                    }
                    break;
                case INSTRUCTION_VARPTR:
                    catalog.blocks[b].value_references[i] = llvm->stack->values[((ir_instr_varptr_t*) basicblock->instructions[i])->index];
                    break;
                case INSTRUCTION_GLOBALVARPTR:
                    catalog.blocks[b].value_references[i] = llvm->global_variables[((ir_instr_varptr_t*) basicblock->instructions[i])->index];
                    break;
                case INSTRUCTION_BREAK:
                    LLVMBuildBr(builder, llvm_blocks[((ir_instr_break_t*) basicblock->instructions[i])->block_id]);
                    break;
                case INSTRUCTION_CONDBREAK:
                    instr = basicblock->instructions[i];
                    LLVMBuildCondBr(builder, ir_to_llvm_value(llvm, ((ir_instr_cond_break_t*) instr)->value), llvm_blocks[((ir_instr_cond_break_t*) instr)->true_block_id],
                    llvm_blocks[((ir_instr_cond_break_t*) instr)->false_block_id]);
                    break;
                case INSTRUCTION_EQUALS:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildICmp(builder, LLVMIntEQ, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_FEQUALS:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildFCmp(builder, LLVMRealOEQ, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_NOTEQUALS:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildICmp(builder, LLVMIntNE, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_FNOTEQUALS:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildFCmp(builder, LLVMRealONE, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_UGREATER:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildICmp(builder, LLVMIntUGT, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_SGREATER:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildICmp(builder, LLVMIntSGT, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_FGREATER:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildFCmp(builder, LLVMRealOGT, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_ULESSER:
                    instr = basicblock->instructions[i];
                    catalog.blocks[b].value_references[i] = LLVMBuildICmp(builder, LLVMIntULT, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    break;
                case INSTRUCTION_SLESSER:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildICmp(builder, LLVMIntSLT, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_FLESSER:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildFCmp(builder, LLVMRealOLT, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_UGREATEREQ:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildICmp(builder, LLVMIntUGE, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_SGREATEREQ:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildICmp(builder, LLVMIntSGE, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_FGREATEREQ:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildFCmp(builder, LLVMRealOGE, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_ULESSEREQ:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildICmp(builder, LLVMIntULE, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_SLESSEREQ:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildICmp(builder, LLVMIntSLE, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_FLESSEREQ:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildFCmp(builder, LLVMRealOLE, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_MEMBER: {
                        instr = basicblock->instructions[i];

                        ir_instr_member_t *member_instr = (ir_instr_member_t*) instr;
                        LLVMValueRef foundation = ir_to_llvm_value(llvm, member_instr->value);

                        ir_to_llvm_null_check(llvm, f, foundation, member_instr->maybe_line_number, member_instr->maybe_column_number, &llvm_exit_blocks[b]);

                        LLVMValueRef gep_indices[2];
                        gep_indices[0] = LLVMConstInt(LLVMInt32Type(), 0, true);
                        gep_indices[1] = LLVMConstInt(LLVMInt32Type(), ((ir_instr_member_t*) instr)->member, true);
                        catalog.blocks[b].value_references[i] = LLVMBuildGEP(builder, foundation, gep_indices, 2, "");
                    }
                    break;
                case INSTRUCTION_ARRAY_ACCESS: {
                        instr = basicblock->instructions[i];

                        ir_instr_array_access_t *array_access_instr = (ir_instr_array_access_t*) instr;
                        LLVMValueRef foundation = ir_to_llvm_value(llvm, array_access_instr->value);

                        ir_to_llvm_null_check(llvm, f, foundation, array_access_instr->maybe_line_number, array_access_instr->maybe_column_number, &llvm_exit_blocks[b]);

                        LLVMValueRef gep_index = ir_to_llvm_value(llvm, ((ir_instr_array_access_t*) instr)->index);
                        llvm_result = LLVMBuildGEP(builder, ir_to_llvm_value(llvm, ((ir_instr_array_access_t*) instr)->value), &gep_index, 1, "");
                        catalog.blocks[b].value_references[i] = llvm_result;
                    }
                    break;
                case INSTRUCTION_FUNC_ADDRESS:
                    instr = basicblock->instructions[i];

                    if(((ir_instr_func_address_t*) instr)->name == NULL){
                        // Not a foreign function, so resolve via id
                        char implementation_name[256];
                        ir_implementation(((ir_instr_func_address_t*) instr)->ir_func_id, 'a', implementation_name);
                        llvm_result = LLVMGetNamedFunction(llvm_module, implementation_name);
                    } else {
                        // Is a foreign function, so get by name
                        llvm_result = LLVMGetNamedFunction(llvm_module, ((ir_instr_func_address_t*) instr)->name);
                    }

                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_BITCAST:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildBitCast(builder, ir_to_llvm_value(llvm, ((ir_instr_cast_t*) instr)->value), ir_to_llvm_type(((ir_instr_cast_t*) instr)->result_type), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_ZEXT:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildZExt(builder, ir_to_llvm_value(llvm, ((ir_instr_cast_t*) instr)->value), ir_to_llvm_type(((ir_instr_cast_t*) instr)->result_type), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_SEXT:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildSExt(builder, ir_to_llvm_value(llvm, ((ir_instr_cast_t*) instr)->value), ir_to_llvm_type(((ir_instr_cast_t*) instr)->result_type), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_FEXT:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildFPExt(builder, ir_to_llvm_value(llvm, ((ir_instr_cast_t*) instr)->value), ir_to_llvm_type(((ir_instr_cast_t*) instr)->result_type), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_TRUNC:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildTrunc(builder, ir_to_llvm_value(llvm, ((ir_instr_cast_t*) instr)->value), ir_to_llvm_type(((ir_instr_cast_t*) instr)->result_type), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_FTRUNC:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildFPTrunc(builder, ir_to_llvm_value(llvm, ((ir_instr_cast_t*) instr)->value), ir_to_llvm_type(((ir_instr_cast_t*) instr)->result_type), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_INTTOPTR:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildIntToPtr(builder, ir_to_llvm_value(llvm, ((ir_instr_cast_t*) instr)->value), ir_to_llvm_type(((ir_instr_cast_t*) instr)->result_type), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_PTRTOINT:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildPtrToInt(builder, ir_to_llvm_value(llvm, ((ir_instr_cast_t*) instr)->value), ir_to_llvm_type(((ir_instr_cast_t*) instr)->result_type), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_FPTOUI:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildFPToUI(builder, ir_to_llvm_value(llvm, ((ir_instr_cast_t*) instr)->value), ir_to_llvm_type(((ir_instr_cast_t*) instr)->result_type), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_FPTOSI:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildFPToSI(builder, ir_to_llvm_value(llvm, ((ir_instr_cast_t*) instr)->value), ir_to_llvm_type(((ir_instr_cast_t*) instr)->result_type), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_UITOFP:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildUIToFP(builder, ir_to_llvm_value(llvm, ((ir_instr_cast_t*) instr)->value), ir_to_llvm_type(((ir_instr_cast_t*) instr)->result_type), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_SITOFP:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildSIToFP(builder, ir_to_llvm_value(llvm, ((ir_instr_cast_t*) instr)->value), ir_to_llvm_type(((ir_instr_cast_t*) instr)->result_type), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_ISZERO: case INSTRUCTION_ISNTZERO: {
                        instr = basicblock->instructions[i];

                        unsigned int type_kind = ((ir_instr_cast_t*) instr)->value->type->kind;
                        bool type_kind_is_float = (type_kind == TYPE_KIND_FLOAT || type_kind == TYPE_KIND_DOUBLE);
                        LLVMValueRef zero;

                        switch(type_kind){
                        case TYPE_KIND_S8: zero = LLVMConstInt(LLVMInt8Type(), 0, true); break;
                        case TYPE_KIND_U8: zero = LLVMConstInt(LLVMInt8Type(), 0, false); break;
                        case TYPE_KIND_S16: zero = LLVMConstInt(LLVMInt16Type(), 0, true); break;
                        case TYPE_KIND_U16: zero = LLVMConstInt(LLVMInt16Type(), 0, false); break;
                        case TYPE_KIND_S32: zero = LLVMConstInt(LLVMInt32Type(), 0, true); break;
                        case TYPE_KIND_U32: zero = LLVMConstInt(LLVMInt32Type(), 0, false); break;
                        case TYPE_KIND_S64: zero = LLVMConstInt(LLVMInt64Type(), 0, true); break;
                        case TYPE_KIND_U64: zero = LLVMConstInt(LLVMInt64Type(), 0, false); break;
                        case TYPE_KIND_FLOAT: zero = LLVMConstReal(LLVMFloatType(), 0); break;
                        case TYPE_KIND_DOUBLE: zero = LLVMConstReal(LLVMDoubleType(), 0); break;
                        case TYPE_KIND_BOOLEAN: zero = LLVMConstInt(LLVMInt1Type(), 0, false); break;
                        case TYPE_KIND_FUNCPTR:
                        case TYPE_KIND_POINTER: zero = LLVMConstNull(ir_to_llvm_type(((ir_instr_cast_t*) instr)->value->type)); break;
                        default:
                            redprintf("INTERNAL ERROR: INSTRUCTION_ISxxZERO received unknown type kind\n");
                            for(length_t c = 0; c != catalog.blocks_length; c++) free(catalog.blocks[c].value_references);
                            free(catalog.blocks);
                            free(stack.values);
                            free(stack.types);
                            free(llvm_blocks);
                            free(llvm_exit_blocks);
                            free(unrelocated_phis);
                            LLVMDisposeBuilder(builder);
                            return FAILURE;
                        }

                        bool isz = (basicblock->instructions[i]->id == INSTRUCTION_ISZERO);
                        llvm_result = ir_to_llvm_value(llvm, ((ir_instr_cast_t*) instr)->value);

                        if(type_kind_is_float){
                            catalog.blocks[b].value_references[i] = LLVMBuildFCmp(builder, isz ? LLVMRealOEQ : LLVMRealONE, llvm_result, zero, "");
                        } else {
                            catalog.blocks[b].value_references[i] = LLVMBuildICmp(builder, isz ? LLVMIntEQ : LLVMIntNE, llvm_result, zero, "");
                        }
                    }
                    break;
                case INSTRUCTION_REINTERPRET:
                    // Reinterprets a signed vs unsigned integer in higher level IR
                    // LLVM Can ignore this instruction
                    instr = basicblock->instructions[i];
                    catalog.blocks[b].value_references[i] = ir_to_llvm_value(llvm, ((ir_instr_cast_t*) instr)->value);
                    break;
                case INSTRUCTION_AND:
                case INSTRUCTION_BIT_AND:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildAnd(builder, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_OR:
                case INSTRUCTION_BIT_OR:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildOr(builder, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_SIZEOF: {
                        instr = basicblock->instructions[i];
                        length_t type_size = LLVMABISizeOfType(llvm->data_layout, ir_to_llvm_type(((ir_instr_sizeof_t*) instr)->type));
                        catalog.blocks[b].value_references[i] = LLVMConstInt(LLVMInt64Type(), type_size, false);
                    }
                    break;
                case INSTRUCTION_OFFSETOF: {
                        instr = basicblock->instructions[i];
                        unsigned long long offset = LLVMOffsetOfElement(llvm->data_layout, ir_to_llvm_type(((ir_instr_offsetof_t*) instr)->type), ((ir_instr_offsetof_t*) instr)->index);
                        catalog.blocks[b].value_references[i] = LLVMConstInt(LLVMInt64Type(), offset, false);
                    }
                    break;
                case INSTRUCTION_VARZEROINIT: {
                        instr = basicblock->instructions[i];
                        LLVMValueRef var_to_init = llvm->stack->values[((ir_instr_varzeroinit_t*) instr)->index];
                        LLVMTypeRef var_type = llvm->stack->types[((ir_instr_varzeroinit_t*) instr)->index];
                        LLVMBuildStore(builder, LLVMConstNull(var_type), var_to_init);
                    }
                    break;
                case INSTRUCTION_MALLOC: {
                        instr = basicblock->instructions[i];
                        LLVMTypeRef ty = ir_to_llvm_type(((ir_instr_malloc_t*) instr)->type);
                        LLVMValueRef allocated;

                        if( ((ir_instr_malloc_t*) instr)->amount == NULL ){    
                            allocated = LLVMBuildMalloc(builder, ty, "");
                            catalog.blocks[b].value_references[i] = allocated;
                            
                            if(!(((ir_instr_malloc_t*) instr)->is_undef || llvm->compiler->traits & COMPILER_UNSAFE_NEW)){
                                LLVMBuildStore(builder, LLVMConstNull(ty), LLVMBuildBitCast(builder, allocated, LLVMPointerType(ty, 0), ""));
                            }
                        } else {
                            LLVMValueRef count = ir_to_llvm_value(llvm, ((ir_instr_malloc_t*) instr)->amount);
                            allocated = LLVMBuildArrayMalloc(builder, ty, count, "");
                            catalog.blocks[b].value_references[i] = allocated;

                            if(!(((ir_instr_malloc_t*) instr)->is_undef || llvm->compiler->traits & COMPILER_UNSAFE_NEW)){
                                LLVMValueRef *memset_intrinsic = &llvm->memset_intrinsic;

                                if(*memset_intrinsic == NULL){
                                    LLVMTypeRef arg_types[4];
                                    arg_types[0] = LLVMPointerType(LLVMInt8Type(), 0);
                                    arg_types[1] = LLVMInt8Type();
                                    arg_types[2] = LLVMInt64Type();
                                    arg_types[3] = LLVMInt1Type();

                                    LLVMTypeRef memset_intrinsic_type = LLVMFunctionType(LLVMVoidType(), arg_types, 4, 0);
                                    *memset_intrinsic = LLVMAddFunction(llvm->module, "llvm.memset.p0i8.i64", memset_intrinsic_type);
                                }

                                LLVMValueRef per_item_size = LLVMConstInt(LLVMInt64Type(), LLVMABISizeOfType(llvm->data_layout, ty), false);
                                count = LLVMBuildZExt(llvm->builder, count, LLVMInt64Type(), "");

                                LLVMValueRef args[4];
                                args[0] = LLVMBuildBitCast(llvm->builder, allocated, LLVMPointerType(LLVMInt8Type(), 0), "");
                                args[1] = LLVMConstInt(LLVMInt8Type(), 0, false);
                                args[2] = LLVMBuildMul(llvm->builder, per_item_size, count, "");
                                args[3] = LLVMConstInt(LLVMInt1Type(), 0, false);

                                LLVMBuildCall(builder, *memset_intrinsic, args, 4, "");
                            }
                        }
                    }
                    break;
                case INSTRUCTION_FREE: {
                        instr = basicblock->instructions[i];
                        catalog.blocks[b].value_references[i] = LLVMBuildFree(builder, ir_to_llvm_value(llvm, ((ir_instr_free_t*) instr)->value));
                    }
                    break;
                case INSTRUCTION_MEMCPY: {
                        instr = basicblock->instructions[i];

                        LLVMValueRef *memcpy_intrinsic = &llvm->memcpy_intrinsic;

                        if(*memcpy_intrinsic == NULL){
                            LLVMTypeRef arg_types[4];
                            arg_types[0] = LLVMPointerType(LLVMInt8Type(), 0);
                            arg_types[1] = LLVMPointerType(LLVMInt8Type(), 0);
                            arg_types[2] = LLVMInt64Type();
                            arg_types[3] = LLVMInt1Type();

                            LLVMTypeRef memcpy_intrinsic_type = LLVMFunctionType(LLVMVoidType(), arg_types, 4, 0);
                            *memcpy_intrinsic = LLVMAddFunction(llvm->module, "llvm.memcpy.p0i8.p0i8.i64", memcpy_intrinsic_type);
                        }

                        LLVMValueRef args[4];
                        args[0] = ir_to_llvm_value(llvm, ((ir_instr_memcpy_t*) instr)->destination);
                        args[1] = ir_to_llvm_value(llvm, ((ir_instr_memcpy_t*) instr)->value);
                        args[2] = ir_to_llvm_value(llvm, ((ir_instr_memcpy_t*) instr)->bytes);
                        args[3] = LLVMConstInt(LLVMInt1Type(), ((ir_instr_memcpy_t*) instr)->is_volatile, false);

                        LLVMBuildCall(builder, *memcpy_intrinsic, args, 4, "");
                        catalog.blocks[b].value_references[i] = NULL;
                    }
                    break;
                case INSTRUCTION_BIT_XOR:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildXor(builder, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_BIT_LSHIFT:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildShl(builder, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_BIT_RSHIFT:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildAShr(builder, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_BIT_LGC_RSHIFT:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildLShr(builder, ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->a), ir_to_llvm_value(llvm, ((ir_instr_math_t*) instr)->b), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_BIT_COMPLEMENT: {
                        instr = basicblock->instructions[i];

                        unsigned int type_kind = ((ir_instr_unary_t*) instr)->value->type->kind;
                        LLVMValueRef base = ir_to_llvm_value(llvm, ((ir_instr_unary_t*) instr)->value);
                        
                        unsigned int bits = global_type_kind_sizes_64[type_kind];
                        LLVMValueRef transform = LLVMConstInt(LLVMIntType(bits), ~0, global_type_kind_signs[type_kind]);

                        llvm_result = LLVMBuildXor(builder, base, transform, "");
                        catalog.blocks[b].value_references[i] = llvm_result;
                    }
                    break;
                case INSTRUCTION_NEGATE: {
                        instr = basicblock->instructions[i];
                        LLVMValueRef base = ir_to_llvm_value(llvm, ((ir_instr_unary_t*) instr)->value);
                        LLVMValueRef zero = LLVMConstNull(LLVMTypeOf(base));
                        instr = basicblock->instructions[i];
                        llvm_result = LLVMBuildSub(builder, zero, base, "");
                        catalog.blocks[b].value_references[i] = llvm_result;
                    }
                    break;
                case INSTRUCTION_FNEGATE:
                    instr = basicblock->instructions[i];
                    llvm_result = LLVMBuildFNeg(builder, ir_to_llvm_value(llvm, ((ir_instr_unary_t*) instr)->value), "");
                    catalog.blocks[b].value_references[i] = llvm_result;
                    break;
                case INSTRUCTION_SELECT:
                    instr = basicblock->instructions[i];
                    catalog.blocks[b].value_references[i] = LLVMBuildSelect(builder,
                        ir_to_llvm_value(llvm, ((ir_instr_select_t*) instr)->condition),
                        ir_to_llvm_value(llvm, ((ir_instr_select_t*) instr)->if_true),
                        ir_to_llvm_value(llvm, ((ir_instr_select_t*) instr)->if_false), "");
                    break;
                case INSTRUCTION_PHI2: {
                        instr = basicblock->instructions[i];
                        LLVMValueRef phi = LLVMBuildPhi(llvm->builder, ir_to_llvm_type(instr->result_type), "");

                        LLVMValueRef when_a = ir_to_llvm_value(llvm, ((ir_instr_phi2_t*) instr)->a);
                        LLVMValueRef when_b = ir_to_llvm_value(llvm, ((ir_instr_phi2_t*) instr)->b);

                        expand((void**) &unrelocated_phis, sizeof(llvm_phi2_relocation_t), unrelocated_phis_length, &unrelocated_phis_capacity, 1, 4);
                        llvm_phi2_relocation_t *delayed = &unrelocated_phis[unrelocated_phis_length++];
                        delayed->phi = phi;
                        delayed->a = when_a;
                        delayed->b = when_b;
                        delayed->basicblock_a = ((ir_instr_phi2_t*) instr)->block_id_a;
                        delayed->basicblock_b = ((ir_instr_phi2_t*) instr)->block_id_b;

                        catalog.blocks[b].value_references[i] = phi;
                    }
                    break;
                case INSTRUCTION_SWITCH: {
                        instr = basicblock->instructions[i];

                        LLVMValueRef value = ir_to_llvm_value(llvm, ((ir_instr_switch_t*) instr)->condition);
                        LLVMValueRef switch_val = LLVMBuildSwitch(builder, value, llvm_blocks[((ir_instr_switch_t*) instr)->default_block_id], ((ir_instr_switch_t*) instr)->cases_length);
                        
                        for(length_t i = 0; i != ((ir_instr_switch_t*) instr)->cases_length; i++){
                            LLVMValueRef case_value = ir_to_llvm_value(llvm, ((ir_instr_switch_t*) instr)->case_values[i]);
                            LLVMAddCase(switch_val, case_value, llvm_blocks[((ir_instr_switch_t*) instr)->case_block_ids[i]]);
                        }

                        catalog.blocks[b].value_references[i] = NULL;
                    }
                    break;
                case INSTRUCTION_ALLOC: {
                        instr = basicblock->instructions[i];

                        ir_instr_alloc_t *alloc = (ir_instr_alloc_t*) instr;
                        ir_type_t *target_result_type = alloc->result_type;

                        if(target_result_type->kind != TYPE_KIND_POINTER){
                            redprintf("INTERNAL ERROR: INSTRUCTION_ALLOC got non-pointer result type when exporting ir to llvm\n");
                            catalog.blocks[b].value_references[i] = LLVMConstPointerNull(ir_to_llvm_type(target_result_type));
                            break;
                        }

                        catalog.blocks[b].value_references[i] = (alloc->count)
                            ? LLVMBuildArrayAlloca(llvm->builder, ir_to_llvm_type(target_result_type->extra), ir_to_llvm_value(llvm, alloc->count), "")
                            : LLVMBuildAlloca(llvm->builder, ir_to_llvm_type(target_result_type->extra), "");

                        if(alloc->alignment != 0)
                            LLVMSetAlignment(catalog.blocks[b].value_references[i], alloc->alignment);
                    }
                    break;
                case INSTRUCTION_STACK_SAVE: {
                        LLVMValueRef *stacksave_intrinsic = &llvm->stacksave_intrinsic;

                        if(*stacksave_intrinsic == NULL){
                            LLVMTypeRef stacksave_intrinsic_type = LLVMFunctionType(LLVMPointerType(LLVMInt8Type(), 0), NULL, 0, 0);
                            *stacksave_intrinsic = LLVMAddFunction(llvm->module, "llvm.stacksave", stacksave_intrinsic_type);
                        }

                        catalog.blocks[b].value_references[i] = LLVMBuildCall(builder, *stacksave_intrinsic, NULL, 0, "");
                    }
                    break;
                case INSTRUCTION_STACK_RESTORE: {
                        instr = basicblock->instructions[i];

                        LLVMValueRef *stackrestore_intrinsic = &llvm->stackrestore_intrinsic;

                        if(*stackrestore_intrinsic == NULL){
                            LLVMTypeRef arg_types[1];
                            arg_types[0] = LLVMPointerType(LLVMInt8Type(), 0);

                            LLVMTypeRef stackrestore_intrinsic_type = LLVMFunctionType(LLVMVoidType(), arg_types, 1, 0);
                            *stackrestore_intrinsic = LLVMAddFunction(llvm->module, "llvm.stackrestore", stackrestore_intrinsic_type);
                        }

                        LLVMValueRef args[1];
                        args[0] = ir_to_llvm_value(llvm, ((ir_instr_stack_restore_t*) instr)->stack_pointer);

                        LLVMBuildCall(builder, *stackrestore_intrinsic, args, 1, "");
                        catalog.blocks[b].value_references[i] = NULL;
                    }
                    break;
                case INSTRUCTION_VA_START: case INSTRUCTION_VA_END: {
                        instr = basicblock->instructions[i];

                        bool is_start = instr->id == INSTRUCTION_VA_START;
                        LLVMValueRef *va_intrinsic = is_start ? &llvm->va_start_intrinsic : &llvm->va_end_intrinsic;

                        if(*va_intrinsic == NULL){
                            LLVMTypeRef params[] = {LLVMPointerType(LLVMInt8Type(), 0)};
                            LLVMTypeRef va_intrinsic_type = LLVMFunctionType(LLVMVoidType(), params, 1, 0);
                            *va_intrinsic = LLVMAddFunction(llvm->module, is_start ? "llvm.va_start" : "llvm.va_end", va_intrinsic_type);
                        }

                        LLVMValueRef args[1];
                        args[0] = ir_to_llvm_value(llvm, ((ir_instr_unary_t*) instr)->value);
                        
                        LLVMBuildCall(builder, *va_intrinsic, args, 1, "");
                        catalog.blocks[b].value_references[i] = NULL;
                    }
                    break;
                case INSTRUCTION_VA_ARG: {
                        instr = basicblock->instructions[i];

                        LLVMValueRef list = ir_to_llvm_value(llvm, ((ir_instr_va_arg_t*) instr)->va_list);
                        LLVMTypeRef arg_type = ir_to_llvm_type(((ir_instr_va_arg_t*) instr)->result_type);

                        catalog.blocks[b].value_references[i] = LLVMBuildVAArg(builder, list, arg_type, "");
                    }
                    break;
                case INSTRUCTION_VA_COPY: {
                        instr = basicblock->instructions[i];

                        LLVMValueRef *va_copy_intrinsic = &llvm->va_copy_intrinsic;

                        if(*va_copy_intrinsic == NULL){
                            LLVMTypeRef llvm_ptr_type = LLVMPointerType(LLVMInt8Type(), 0);
                            LLVMTypeRef params[] = {llvm_ptr_type, llvm_ptr_type};
                            LLVMTypeRef va_copy_intrinsic_type = LLVMFunctionType(LLVMVoidType(), params, 2, 0);
                            *va_copy_intrinsic = LLVMAddFunction(llvm->module, "llvm.va_copy", va_copy_intrinsic_type);
                        }

                        LLVMValueRef args[2];
                        args[0] = ir_to_llvm_value(llvm, ((ir_instr_va_copy_t*) instr)->dest_value);
                        args[1] = ir_to_llvm_value(llvm, ((ir_instr_va_copy_t*) instr)->src_value);
                        
                        LLVMBuildCall(builder, *va_copy_intrinsic, args, 2, "");
                        catalog.blocks[b].value_references[i] = NULL;
                    }
                    break;
                default:
                    redprintf("INTERNAL ERROR: Unexpected instruction '%d' when exporting ir to llvm\n", basicblocks[b].instructions[i]->id);
                    for(length_t c = 0; c != catalog.blocks_length; c++) free(catalog.blocks[c].value_references);
                    free(catalog.blocks);
                    free(stack.values);
                    free(stack.types);
                    free(llvm_blocks);
                    free(llvm_exit_blocks);
                    free(unrelocated_phis);
                    LLVMDisposeBuilder(builder);
                    return FAILURE;
                }
            }
        }

        // Relocate incoming references for basicblocks
        for(length_t i = 0; i != unrelocated_phis_length; i++){
            llvm_phi2_relocation_t *relocation = &unrelocated_phis[i];

            // Now that we have information about the correct exit blocks, we can fill in
            // the incoming blocks
            LLVMAddIncoming(relocation->phi, &relocation->a, &llvm_exit_blocks[relocation->basicblock_a], 1);
            LLVMAddIncoming(relocation->phi, &relocation->b, &llvm_exit_blocks[relocation->basicblock_b], 1);
        }

        if(llvm->compiler->checks & COMPILER_NULL_CHECKS) {
            // Remove basicblock and PHI nodes for null check failure pseudo-function if not used
            // NOTE: Assumes (LLVMCountIncoming(llvm->line_phi) == LLVMCountIncoming(llvm->column_phi))
            if(llvm->line_phi && LLVMCountIncoming(llvm->line_phi) == 0 && llvm->null_check_on_fail_block){
                LLVMDeleteBasicBlock(llvm->null_check_on_fail_block);
            }
        }

        for(length_t c = 0; c != catalog.blocks_length; c++) free(catalog.blocks[c].value_references);
        free(catalog.blocks);
        free(stack.values);
        free(stack.types);
        free(llvm_blocks);
        free(llvm_exit_blocks);
        LLVMDisposeBuilder(builder);
    }

    free(unrelocated_phis);
    return SUCCESS;
}

errorcode_t ir_to_llvm_globals(llvm_context_t *llvm, object_t *object){
    ir_global_t *globals = object->ir_module.globals;
    length_t globals_length = object->ir_module.globals_length;
    ir_anon_global_t *anon_globals = object->ir_module.anon_globals;
    length_t anon_globals_length = object->ir_module.anon_globals_length;

    LLVMModuleRef module = llvm->module;
    char global_implementation_name[256];

    for(length_t i = 0; i != anon_globals_length; i++){
        LLVMTypeRef anon_global_llvm_type = ir_to_llvm_type(anon_globals[i].type);
        llvm->anon_global_variables[i] = LLVMAddGlobal(module, anon_global_llvm_type, "");
        LLVMSetLinkage(llvm->anon_global_variables[i], LLVMPrivateLinkage);
        LLVMSetGlobalConstant(llvm->anon_global_variables[i], anon_globals[i].traits & IR_ANON_GLOBAL_CONSTANT);
    }

    for(length_t i = 0; i != anon_globals_length; i++){
        if(anon_globals[i].initializer == NULL) continue;
        LLVMSetInitializer(llvm->anon_global_variables[i], ir_to_llvm_value(llvm, anon_globals[i].initializer));
    }

    for(length_t i = 0; i != globals_length; i++){
        bool is_external = globals[i].traits & IR_GLOBAL_EXTERNAL;
        LLVMTypeRef global_llvm_type = ir_to_llvm_type(globals[i].type);

        if(is_external)
            ir_implementation(i, 'g', global_implementation_name);

        llvm->global_variables[i] = LLVMAddGlobal(module, global_llvm_type, is_external ? globals[i].name : global_implementation_name);
        LLVMSetLinkage(llvm->global_variables[i], is_external ? LLVMExternalLinkage : LLVMPrivateLinkage);

        if(globals[i].traits & IR_GLOBAL_THREAD_LOCAL)
            LLVMSetThreadLocal(llvm->global_variables[i], true);
        
        if(globals[i].trusted_static_initializer){
            // Non-user static value initializer
            // (Used for __types__ and __types_length__)
            LLVMSetInitializer(llvm->global_variables[i], ir_to_llvm_value(llvm, globals[i].trusted_static_initializer));
        } else if(!is_external){
            LLVMSetInitializer(llvm->global_variables[i], LLVMGetUndef(global_llvm_type));
        }
    }

    return SUCCESS;
}

errorcode_t ir_to_llvm(compiler_t *compiler, object_t *object){
    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmParsers();
    LLVMInitializeAllAsmPrinters();

    ir_module_t *module = &object->ir_module;
    llvm_context_t llvm;

    llvm.module = LLVMModuleCreateWithName(filename_name_const(object->filename));
    llvm.memcpy_intrinsic = NULL;
    llvm.memset_intrinsic = NULL;
    llvm.stacksave_intrinsic = NULL;
    llvm.stackrestore_intrinsic = NULL;
    llvm.va_start_intrinsic = NULL;
    llvm.va_end_intrinsic = NULL;
    llvm.va_copy_intrinsic = NULL;
    llvm.compiler = compiler;

    bool disposeTriple = false;

	#ifdef _WIN32
    char *triple = "x86_64-pc-windows-gnu";
	#else
    char *triple;

    switch(compiler->cross_compile_for){
    case CROSS_COMPILE_WINDOWS:
        triple = "x86_64-pc-windows-gnu";
        break;
    case CROSS_COMPILE_MACOS:
        triple = "x86_64-apple-darwin19.6.0";
        break;
    default:
        triple = LLVMGetDefaultTargetTriple();
        disposeTriple = true;
    }
	#endif

    LLVMSetTarget(llvm.module, triple);

    char *error_message; LLVMTargetRef target;
    if(LLVMGetTargetFromTriple(triple, &target, &error_message)){
        redprintf("INTERNAL ERROR: LLVMGetTargetFromTriple failed: %s\n", error_message);
        LLVMDisposeMessage(error_message);
        return FAILURE;
    }

    if(compiler->output_filename == NULL){
        // May need to change based on operating system etc.
        compiler->output_filename = filename_without_ext(object->filename);
    }
	
	// Automatically add proper extension if missing
    filename_auto_ext(&compiler->output_filename, compiler->cross_compile_for, FILENAME_AUTO_EXECUTABLE);

    char* object_filename = filename_ext(compiler->output_filename, "o");

    char *cpu = "generic";
    char *features = "";
    LLVMCodeGenOptLevel level = ir_to_llvm_config_optlvl(compiler);
    LLVMRelocMode reloc = compiler->use_pic ? LLVMRelocPIC : LLVMRelocDefault;
    LLVMCodeModel code_model = LLVMCodeModelDefault;
    LLVMTargetMachineRef target_machine = LLVMCreateTargetMachine(target, triple, cpu, features, level, reloc, code_model);

    LLVMTargetDataRef data_layout = LLVMCreateTargetDataLayout(target_machine);
    LLVMSetModuleDataLayout(llvm.module, data_layout);
    llvm.data_layout = data_layout;

    llvm.func_skeletons = malloc(sizeof(LLVMValueRef) * module->funcs_length);
    llvm.global_variables = malloc(sizeof(LLVMValueRef) * module->globals_length);
    llvm.anon_global_variables = malloc(sizeof(LLVMValueRef) * module->anon_globals_length);
    llvm.has_null_check_failure_message_bytes = false;

    if(ir_to_llvm_globals(&llvm, object) || ir_to_llvm_functions(&llvm, object)
    || ir_to_llvm_function_bodies(&llvm, object)){
        free(object_filename);
        free(llvm.func_skeletons);
        free(llvm.global_variables);
        free(llvm.anon_global_variables);
        LLVMDisposeTargetData(data_layout);
        LLVMDisposeTargetMachine(target_machine);
        return FAILURE;
    }

    #ifdef ENABLE_DEBUG_FEATURES
    if(compiler->debug_traits & COMPILER_DEBUG_LLVMIR) LLVMDumpModule(llvm.module);
    #endif // ENABLE_DEBUG_FEATURES

    // Free reference arrays
    free(llvm.func_skeletons);
    free(llvm.global_variables);
    free(llvm.anon_global_variables);

	char *link_command; // Will be determined based on system

    char *linker_additional = "";
    length_t linker_additional_length = 0;
    length_t linker_additional_index = 0;

    // TODO: Clean up this messy code
    // We need like a string builder or something
    for(length_t i = 0; i != object->ast.libraries_length; i++){
        linker_additional_length += strlen(object->ast.libraries[i]) + 3;

        switch(object->ast.library_kinds[i]){
        case LIBRARY_KIND_LIBRARY:
             // Already have enough space for "- l"
            break;
        case LIBRARY_KIND_FRAMEWORK: // " -framework"
            linker_additional_length += 11;
            break;
        }
    }

    // TODO: Clean up this messy code
    // We need like a string builder or something
    if(linker_additional_length != 0){
        linker_additional = malloc(linker_additional_length + 1);
        for(length_t i = 0; i != object->ast.libraries_length; i++){
            switch(object->ast.library_kinds[i]){
            case LIBRARY_KIND_LIBRARY:
                // Sanitize
                for(length_t s = 0; object->ast.libraries[i][s] != 0x00; s++){
                    if(!isalnum(object->ast.libraries[i][s])){
                        memmove(&object->ast.libraries[i][s], &object->ast.libraries[i][s + 1], strlen(&object->ast.libraries[i][s + 1]) + 1);
                        s--;
                    }
                }
                memcpy(&linker_additional[linker_additional_index], " -l", 3);
                linker_additional_index += 3;
                length_t lib_length = strlen(object->ast.libraries[i]);
                memcpy(&linker_additional[linker_additional_index], object->ast.libraries[i], lib_length);
                linker_additional_index += lib_length;
                continue;
            case LIBRARY_KIND_FRAMEWORK:
                memcpy(&linker_additional[linker_additional_index], " -framework", 11);
                linker_additional_index += 11;
                // fallthrough
            }

            linker_additional[linker_additional_index++] = ' ';
            linker_additional[linker_additional_index++] = '\"';
            length_t lib_length = strlen(object->ast.libraries[i]);
            memcpy(&linker_additional[linker_additional_index], object->ast.libraries[i], lib_length);
            linker_additional_index += lib_length;
            linker_additional[linker_additional_index++] = '\"';
        }
        linker_additional[linker_additional_index] = '\0';
    } else {
        linker_additional = malloc(1);
        *linker_additional = '\0';
    }
    
	#ifdef _WIN32
    if(compiler->cross_compile_for == CROSS_COMPILE_MACOS){
        const char *linker = "gcc"; // May need to change depending on system etc.
        const char *linker_libm = compiler->use_libm ? " -lm" : "";
        link_command = mallocandsprintf("%s \"%s\"%s%s -o \"%s\"", linker, object_filename, linker_additional, linker_libm, compiler->output_filename);
    } else {
        // Windows Linking
        const char *linker = "ld.exe"; // May need to change depending on system etc.
        const char *linker_options = "--start-group";
        const char *root = compiler->root;
        link_command = mallocandsprintf("\"\"%s%s\" -static \"%scrt2.o\" \"%scrtbegin.o\" %s%s \"%s\" \"%slibdep.a\" C:/Windows/System32/msvcrt.dll -o \"%s\"\"", root, linker, root, root, linker_options, linker_additional, object_filename, root, compiler->output_filename);
    }
	#else
	// UNIX Linking
	
    if(compiler->cross_compile_for == CROSS_COMPILE_WINDOWS){
        const char *linker = "cross-compile-windows/x86_64-w64-mingw32-ld";
        const char *linker_options = "";
        const char *root = compiler->root;

        strong_cstr_t cross_linker = mallocandsprintf("%s%s", compiler->root, linker);
        if(!file_exists(cross_linker)){
            printf("\n");
            redprintf("Cross compiling for Windows requires the 'cross-compile-windows' extension!\n");
            redprintf("You need to first download and install it from here:\n");
            printf("    https://github.com/IsaacShelton/AdeptCrossCompilation/releases\n");
            free(cross_linker);
            free(linker_additional);
            return FAILURE;
        }

        free(cross_linker);
        link_command = mallocandsprintf("\"%s%s\" --start-group -static \"%scross-compile-windows/crt2.o\" \"%scross-compile-windows/crtbegin.o\" %s%s \"%s\" \"%scross-compile-windows/libdep.a\" --end-group %scross-compile-windows/libmsvcrt.a -o \"%s\"", root, linker, root, root, linker_options, linker_additional, object_filename, root, root, compiler->output_filename);
    } else {
        const char *linker = "gcc"; // May need to change depending on system etc.
        const char *linker_libm = compiler->use_libm ? " -lm" : "";
        link_command = mallocandsprintf("%s \"%s\"%s%s -o \"%s\"", linker, object_filename, linker_additional, linker_libm, compiler->output_filename);
    }
	#endif

    free(linker_additional);

    #ifdef ENABLE_DEBUG_FEATURES
    if(!(llvm.compiler->debug_traits & COMPILER_DEBUG_NO_VERIFICATION) && LLVMVerifyModule(llvm.module, LLVMPrintMessageAction, NULL) == 1){
        yellowprintf("\n========== LLVM Verification Failed! ==========\n");
    }
    #endif

    debug_signal(compiler, DEBUG_SIGNAL_AT_OUT, NULL);
    LLVMPassManagerRef pass_manager = LLVMCreatePassManager();
    LLVMCodeGenFileType codegen = LLVMObjectFile;

    if(LLVMTargetMachineEmitToFile(target_machine, llvm.module, object_filename, codegen, &error_message)){
        redprintf("INTERNAL ERROR: LLVMTargetMachineEmitToFile failed: %s\n", error_message);
        LLVMDisposeTargetData(data_layout);
        LLVMRunPassManager(pass_manager, llvm.module);
        LLVMDisposeTargetMachine(target_machine);
        LLVMDisposePassManager(pass_manager);
        if(disposeTriple) LLVMDisposeMessage(triple);
        
        LLVMDisposeMessage(error_message);
        free(object_filename);
        return FAILURE;
    }

    LLVMDisposeTargetData(data_layout);
    LLVMRunPassManager(pass_manager, llvm.module);
    LLVMDisposeTargetMachine(target_machine);
    LLVMDisposePassManager(pass_manager);
    if(disposeTriple) LLVMDisposeMessage(triple);

    if(compiler->cross_compile_for == CROSS_COMPILE_MACOS){
        // Don't support linking output Mach-O object files
        printf("Mach-O Object File Generated (Requires Manual Linking)\n");
        printf("\nLink Command: '%s'\n", link_command);
        free(object_filename);
        free(link_command);
        return SUCCESS;
    }
    
    debug_signal(compiler, DEBUG_SIGNAL_AT_LINKING, NULL);
    
    // TODO: SECURITY: Stop using system(3) call to invoke linker
    if(system(link_command) != 0){
        redprintf("EXTERNAL ERROR: link command failed\n%s\n", link_command);
        free(object_filename);
        free(link_command);
        return FAILURE;
    }

    if(compiler->traits & COMPILER_EXECUTE_RESULT){
        #ifdef _WIN32
        /* For windows, make sure we change all '/' to '\' before invoking */
        char *execute_filename = strclone(compiler->output_filename);
        length_t execute_filename_length = strlen(execute_filename);

        for(length_t i = 0; i != execute_filename_length; i++)
            if(execute_filename[i] == '/') execute_filename[i] = '\\';

        system(execute_filename);
        free(execute_filename);
        #else
		char *executable = strclone(compiler->output_filename);
		filename_prepend_dotslash_if_needed(&executable);
        system(executable);
		free(executable);
        #endif
    }

    if(!(compiler->traits & COMPILER_NO_REMOVE_OBJECT)) remove(object_filename);
    free(object_filename);
    free(link_command);
    return SUCCESS;
}

void ir_to_llvm_null_check(llvm_context_t *llvm, length_t func_skeleton_index, LLVMValueRef pointer, int line, int column, LLVMBasicBlockRef *out_landing_basicblock){
    if(!(llvm->compiler->checks & COMPILER_NULL_CHECKS)) return;

    LLVMBasicBlockRef not_null_block = LLVMAppendBasicBlock(llvm->func_skeletons[func_skeleton_index], "");

    LLVMBasicBlockRef current_block = LLVMGetInsertBlock(llvm->builder);
    LLVMValueRef line_value = LLVMConstInt(LLVMInt32Type(), line, true);
    LLVMValueRef column_value = LLVMConstInt(LLVMInt32Type(), column, true);

    LLVMAddIncoming(llvm->line_phi, &line_value, &current_block, 1);
    LLVMAddIncoming(llvm->column_phi, &column_value, &current_block, 1);

    LLVMValueRef if_null = LLVMBuildIsNull(llvm->builder, pointer, "");
    LLVMBuildCondBr(llvm->builder, if_null, llvm->null_check_on_fail_block, not_null_block);
    LLVMPositionBuilderAtEnd(llvm->builder, not_null_block);

    // Set landing basicblock output to be the not-null case block
    if(out_landing_basicblock) *out_landing_basicblock = not_null_block;
}

LLVMCodeGenOptLevel ir_to_llvm_config_optlvl(compiler_t *compiler){
    switch(compiler->optimization){
    case OPTIMIZATION_NONE:       return LLVMCodeGenLevelNone;
    case OPTIMIZATION_LESS:       return LLVMCodeGenLevelLess;
    case OPTIMIZATION_DEFAULT:    return LLVMCodeGenLevelDefault;
    case OPTIMIZATION_AGGRESSIVE: return LLVMCodeGenLevelAggressive;
    default: return LLVMCodeGenLevelDefault;
    }
}
