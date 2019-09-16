
#ifndef PARSE_STMT_H
#define PARSE_STMT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "UTIL/ground.h"
#include "AST/ast_expr.h"
#include "PARSE/parse_ctx.h"

// ------------------ defer_scope_t ------------------
// A single scope for containing defer statements
struct defer_scope;
#define BREAKABLE   TRAIT_1
#define CONTINUABLE TRAIT_2

typedef struct defer_scope {
    ast_expr_list_t list;
    struct defer_scope *parent;
    weak_cstr_t label;
    trait_t traits;
} defer_scope_t;

// ------------------ defer_scope_init ------------------
// Initializes a defer scope
void defer_scope_init(defer_scope_t *defer_scope, defer_scope_t *parent, weak_cstr_t label, trait_t traits);

// ------------------ defer_scope_free ------------------
// Frees a defer scope
void defer_scope_free(defer_scope_t *defer_scope);

// ------------------ defer_scope_total ------------------
// Returns the number of deferred statements in this scope and ancestor scopes
length_t defer_scope_total(defer_scope_t *defer_scope);

// ------------------ defer_scope_fulfill ------------------
// Fulfill deferred statements in a single scope, handing over ownership in the process
void defer_scope_fulfill(defer_scope_t *defer_scope, ast_expr_list_t *stmt_list);

// ------------------ parse_stmts ------------------
// Parses one or more statements into two lists.
// 'stmt_list' for standard statements
// 'defer_scope' scope in which deferred statements will use
// NOTE: On failure, stmt_list and defer_scope->list.statements may need to be freed
#define PARSE_STMTS_STANDARD TRAIT_NONE // Standard mode (will parse multiple statements)
#define PARSE_STMTS_SINGLE   TRAIT_1    // Single statement mode (will parse a single statement)
errorcode_t parse_stmts(parse_ctx_t *ctx, ast_expr_list_t *stmt_list, defer_scope_t *defer_scope, trait_t mode);

// ------------------ parse_stmt_call ------------------
// Parses a function call statement
errorcode_t parse_stmt_call(parse_ctx_t *ctx, ast_expr_list_t *expr_list, bool is_tentative);

// ------------------ parse_stmt_declare ------------------
// Parses a variable declaration statement
errorcode_t parse_stmt_declare(parse_ctx_t *ctx, ast_expr_list_t *expr_list);

// ------------------ parse_switch ------------------
// Parses a switch statement
errorcode_t parse_switch(parse_ctx_t *ctx, ast_expr_list_t *stmt_list, defer_scope_t *parent_defer_scope);

#ifdef __cplusplus
}
#endif

#endif // PARSE_STMT_H
