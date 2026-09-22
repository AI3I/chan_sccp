/*
 * Asterisk 20/21/22 compatibility wrapper around ast116.
 * Provides stub macros for APIs removed in Asterisk 21+.
 */

/* Function-like macro stubs for macroexten/macrocontext removed in Asterisk 21+ */
#define ast_channel_macroexten(chan) ("")
#define ast_channel_macroexten_set(chan, val) ((void)0)
#define ast_channel_macrocontext(chan) ("")
#define ast_channel_macrocontext_set(chan, val) ((void)0)

#include "../ast116/ast116.c"
