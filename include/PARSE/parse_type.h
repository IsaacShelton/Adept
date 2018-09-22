
#ifndef PARSE_TYPE_H
#define PARSE_TYPE_H

#include "AST/ast_type.h"
#include "PARSE/parse_ctx.h"

// ------------------ parse_type ------------------
// Parses a type into an abstract syntax tree form.
int parse_type(parse_ctx_t *ctx, ast_type_t *out_type);

// ------------------ parse_type_func ------------------
// Parses a function type into an abstract syntax tree
// type-element form. Primary called from 'parse_type'.
int parse_type_func(parse_ctx_t *ctx, ast_elem_func_t *out_func_elem);

#endif // PARSE_TYPE_H