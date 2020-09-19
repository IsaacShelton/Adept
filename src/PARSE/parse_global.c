
#include "UTIL/util.h"
#include "PARSE/parse.h"
#include "PARSE/parse_expr.h"
#include "PARSE/parse_type.h"
#include "PARSE/parse_global.h"

errorcode_t parse_global(parse_ctx_t *ctx){
    length_t *i = ctx->i;
    token_t *tokens = ctx->tokenlist->tokens;
    source_t source = ctx->tokenlist->sources[*i];
    ast_t *ast = ctx->ast;
    trait_t traits = TRAIT_NONE;

    if(ctx->struct_association != NULL){
        compiler_panicf(ctx->compiler, source, "Cannot declare global variable within struct domain");
        return FAILURE;
    }

    while(true){
        if(tokens[*i].id == TOKEN_EXTERNAL){
            traits |= AST_GLOBAL_EXTERNAL;
            (*i)++;
            continue;
        }

        if(tokens[*i].id == TOKEN_THREAD_LOCAL){
            traits |= AST_GLOBAL_THREAD_LOCAL;
            (*i)++;
            continue;
        }    

        break;
    }
    
    weak_cstr_t name = parse_eat_word(ctx, "INTERNAL ERROR: Expected word");
    if(name == NULL) return FAILURE;

    if(tokens[*i].id == TOKEN_EQUALS){
        return parse_old_style_constant_global(ctx, name, source);
    }

    if(tokens[*i].id == TOKEN_POD){
        traits |= AST_GLOBAL_POD;
        (*i)++;
    }
    
    ast_expr_t *initial_value = NULL;

    ast_type_t type;
    if(parse_type(ctx, &type)) return FAILURE;

    if(tokens[*i].id == TOKEN_ASSIGN){
        (*i)++;
        if(tokens[*i].id == TOKEN_UNDEF){
             // 'undef' does nothing for globals, so pretend like this is a plain definition
            (*i)++;
        } else if(parse_expr(ctx, &initial_value)){
            ast_type_free(&type);
            return FAILURE;
        }
    }

    ast_add_global(ast, name, type, initial_value, traits, source);
    return SUCCESS;
}

errorcode_t parse_constant_declaration(parse_ctx_t *ctx, ast_constant_t *out_constant){
    // const NAME = value
    //   ^

    // NOTE: Assumes first token is 'const' keyword
    source_t source = ctx->tokenlist->sources[*ctx->i];

    // Get the name of the constant value
    weak_cstr_t name = parse_grab_word(ctx, "Expected name for constant value after 'const' keyword");
    if(name == NULL) return FAILURE;

    // Move to the next token after the name we got
    (*ctx->i)++;

    // Eat '='
    if(parse_eat(ctx, TOKEN_ASSIGN, "Expected '=' after name of constant")) return FAILURE;

    // Parse the expression of the constant
    ast_expr_t *value;
    if(parse_expr(ctx, &value)) return FAILURE;

    // Construct the new constant value
    out_constant->name = name;
    out_constant->expression = value;
    out_constant->traits = TRAIT_NONE;
    out_constant->source = source;
    return SUCCESS;
}

errorcode_t parse_global_constant_declaration(parse_ctx_t *ctx){
    ast_t *ast = ctx->ast;

    ast_constant_t new_constant;
    if(parse_constant_declaration(ctx, &new_constant)) return FAILURE;

    // Make room for another constant
    expand((void**) &ast->constants, sizeof(ast_constant_t), ast->constants_length, &ast->constants_capacity, 1, 8);
    ast->constants[ast->constants_length++] = new_constant;
    return SUCCESS;
}

errorcode_t parse_old_style_constant_global(parse_ctx_t *ctx, weak_cstr_t name, source_t source){
    // SOME_CONSTANT == value
    //               ^

    ast_t *ast = ctx->ast;
    *(ctx->i) += 1;

    ast_expr_t *value;
    if(parse_expr(ctx, &value)) return FAILURE;

    expand((void**) &ast->constants, sizeof(ast_constant_t), ast->constants_length, &ast->constants_capacity, 1, 8);

    ast_constant_t *constant = &ast->constants[ast->constants_length++];
    constant->name = name;
    constant->expression = value;
    constant->traits = TRAIT_NONE;
    constant->source = source;
    return SUCCESS;
}
