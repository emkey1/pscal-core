#ifndef PSCAL_TOKEN_ARENA_H
#define PSCAL_TOKEN_ARENA_H

#include <stddef.h>

#include "core/utils.h"

/* Rea and Aether build a token for nearly every AST node, and newASTNode copies what it is
 * given, so the parser's own was left behind: most of what a run of either leaked, for as long
 * as the process lives, which on iOS is the whole app. A token here lives until the parse ends
 * (tokenArenaRelease), or the thread's run does. Nothing may keep one past that: an AST node
 * has its own copy. Include this after core/utils.h in a file whose tokens work this way. */
Token *tokenArenaTrack(Token *token);
size_t tokenArenaMark(void);
void tokenArenaRelease(size_t mark);

#define newToken(type, value, line, column) tokenArenaTrack(newToken((type), (value), (line), (column)))
#define freeToken(token) ((void)(token))

#endif /* PSCAL_TOKEN_ARENA_H */
