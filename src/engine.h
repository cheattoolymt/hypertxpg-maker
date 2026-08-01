/*
 * engine.h - `htg run` game execution engine (spec section 4).
 *
 * This stage covers exploration and the event/dialogue system:
 *   - Room description, exits (incl. flag-locked exits), item pickup.
 *   - on_enter_event playback, auto/choice event chaining.
 *   - Condition evaluation for event lines: flag:x==bool / var:x op N,
 *     combined with && / || (left-to-right, no parentheses) per spec 2.5.
 *   - Choice actions: set_flag / set_var / join_party / goto.
 *
 * Turn-based battle (spec section 3) is a later stage; encounters and the
 * "戦う" hooks are surfaced but resolved as a placeholder here.
 */
#ifndef HTG_ENGINE_H
#define HTG_ENGINE_H

#include "model.h"

/*
 * Run the loaded project as a game. Takes a borrowed project pointer (the
 * caller still owns/frees it). Returns 0 on a normal quit / game end.
 */
int htg_engine_run(HtgProject *p);

/* ---- exposed for unit-style reuse ---- */

/*
 * Evaluate an event condition string against the project's live flag/var
 * state. Supports single terms (flag:x==true|false, var:x <op> N) joined by
 * && / || (evaluated strictly left to right). A NULL/empty condition is true.
 */
int htg_eval_condition(HtgProject *p, const char *cond);

#endif /* HTG_ENGINE_H */
