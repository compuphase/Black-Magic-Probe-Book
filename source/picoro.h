/*
 * picoro - minimal coroutines for C.
 * Written by Tony Finch <dot@dotat.at>
 * http://creativecommons.org/publicdomain/zero/1.0/
 * API modelled after Lua's coroutines
 * http://www.lua.org/manual/5.1/manual.html#2.11
 */

#ifndef PICORO_H
#define PICORO_H

#include <stdbool.h>

typedef struct coro *coro;
typedef void *(*coro_proc)(void *arg);

/**
 * Create a coroutine that will run fun(). The coroutine starts off suspended.
 * When it is first resumed, the argument to resume() is passed to fun(). After
 * the coroutine starts running, it runs until it returns or yields.
 *
 * If fun() returns, its return value is returned by resume() as if the
 * coroutine yielded, except that the coroutine is then no longer resumable
 * and may be discarded.
 *
 * If fun() yields, the argument to yield() is returned as by the resume() call
 * that started the coroutine.
 */
coro coroutine(coro_proc fun);

/**
 * Returns false when the coroutine has run to completion, or when it is blocked
 * inside resume(), meaning that another coroutine is running at that moment.
 */
bool resumable(coro c);

/**
 * Transfer control to another coroutine. The second argument is returned by
 * yield() inside the target coroutine (except for the first time resume() is
 * called). A coroutine that is blocked inside resume() is not resumable.
 */
void *resume(coro c, void *arg);

/**
 * Transfer control back to the coroutine that resumed this one. The argument
 * is returned by resume() in the destination coroutine. A coroutine that is
 * blocked inside yield() may be resumed by any other coroutine.
 */
void *yield(void *arg);

#endif /* PICORO_H */
