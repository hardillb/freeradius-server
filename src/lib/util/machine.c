/*
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/** State machine functions
 *
 * @file src/lib/util/machine.c
 *
 * @copyright 2021 Network RADIUS SAS (legal@networkradius.com)
 */
RCSID("$Id$")

#include "machine.h"

typedef struct {
	fr_machine_state_t const *def;		//!< static definition of names, callbacks for this particular state
	fr_dlist_head_t		enter[2];	//!< pre/post enter hooks
	fr_dlist_head_t		process[2];	//!< pre/post process hooks
	fr_dlist_head_t		exit[2];	//!< pre/post exit hooks
} fr_machine_state_inst_t;

/** Hooks
 *
 */
struct fr_machine_s {
	fr_machine_def_t const	*def;		//!< static definition of states, names, callbacks for the state machine
	void			*uctx;		//!< to pass to the various handlers
	fr_machine_state_inst_t	*current;	//!< current state we are in
	void const		*in_handler;	//!< block transitions if we're in a callback

	int			deferred;	//!< deferred transition if we're paused
	int			paused;		//!< are transitions paused?
	bool			dead;

	fr_machine_state_inst_t state[0];	//!< all of the state transitions
};

typedef struct {
	void			*uctx;		//!< to pass back to the function
	fr_machine_hook_func_t	func;		//!< function to call for the hook
	bool			oneshot;	//!< is this a one-shot callback?
	fr_dlist_head_t		*head;		//!< where the hook is stored
	fr_dlist_t		dlist;		//!< linked list of hooks
} fr_machine_hook_t;

#undef PRE
#define PRE (0)
#undef POST
#define POST (1)

/** Call the hook with the current state machine, and the hooks context
 *
 *  Note that in most cases, the hook will have saved it's own state
 *  machine in uctx, and will not need the current state machine.
 */
static inline void call_hook(fr_machine_t *m, fr_dlist_head_t *head, int state1, int state2)
{
	fr_machine_hook_t *hook, *next;

	for (hook = fr_dlist_head(head); hook != NULL; hook = next) {
		next = fr_dlist_next(head, hook);

		hook->func(m, state1, state2, hook->uctx);
		if (hook->oneshot) talloc_free(hook);
	}
}

/** Transition from one state to another.
 *
 *  Including calling pre/post hooks.
 *
 *  None of the functions called from here are allowed to perform a
 *  state transition.
 */
static void state_transition(fr_machine_t *m, int state, void *in_handler)
{
	fr_machine_state_inst_t *current = m->current;
	int old;

	fr_assert(current != NULL);
	fr_assert(m->deferred == 0);

	fr_assert(!m->in_handler);
	m->in_handler = in_handler;

	old = current->def->number;

	/*
	 *	Exit the current state.
	 */
	call_hook(m, &current->exit[PRE], old, state);
	if (current->def->exit) current->def->exit(m, m->uctx);
	call_hook(m, &current->exit[POST], old, state);

	/*
	 *	Reset "current", and enter the new state.
	 */
	current = m->current = &m->state[state];

	call_hook(m, &current->enter[PRE], old, state);
	if (current->def->enter) current->def->enter(m, m->uctx);
	call_hook(m, &current->enter[POST], old, state);

	m->in_handler = NULL;
}


/** Free a state machine
 *
 *  When a state machine is freed, it will first transition to the
 *  "free" state.  That state is presumed to do all appropriate
 *  cleanups.
 */
static int _machine_free(fr_machine_t *m)
{
	fr_assert(m);
	fr_assert(m->def);
	fr_assert(m->def->free);

	fr_assert(m->state[m->def->free].enter != NULL);

	/*
	 *	Exit the current state, and enter the free state.
	 */
	state_transition(m, m->def->free, (void *) _machine_free);

	/*
	 *	Don't call "process" on the free state.  Simply
	 *	entering the free state _should_ clean everything up.
	 */

	return 0;
}

/** Instantiate a state machine
 *
 *  @param ctx	the talloc ctx
 *  @param def	the definition of the state machine
 *  @param uctx	the context passed to the callbacks
 *  @return
 *	- NULL on error
 *	- !NULL state machine which can be used.
 */
fr_machine_t *fr_machine_alloc(TALLOC_CTX *ctx, fr_machine_def_t const *def, void *uctx)
{
	int i, j, next;
	fr_machine_t *m;

	/*
	 *	We always reserve 0 for "invalid state".
	 *
	 *	The "max_state" is the maximum allowed state, which is a valid state number.
	 */
	m = (fr_machine_t *) talloc_zero_array(ctx, uint8_t, sizeof(fr_machine_t) + sizeof(m->state[0]) * (def->max_state + 1));
	if (!m) return NULL;

	talloc_set_type(m, fr_machine_t);

	*m = (fr_machine_t) {
		.uctx = uctx,
		.def = def,
	};

	/*
	 *	Initialize the instance structures for each of the
	 *	states.
	 */
	for (i = 1; i <= def->max_state; i++) {
		fr_machine_state_inst_t *state = &m->state[i];

		state->def = &def->state[i];

		for (j = 0; j < 2; j++) {
			fr_dlist_init(&state->enter[j], fr_machine_hook_t, dlist);
			fr_dlist_init(&state->process[j], fr_machine_hook_t, dlist);
			fr_dlist_init(&state->exit[j], fr_machine_hook_t, dlist);
		}
	}

	/*
	 *	Set the current state to "init".
	 */
	m->current = &m->state[def->init];

	/*
	 *	We don't transition into the "init" state, as there is
	 *	no previous state.  We just run the "process"
	 *	function, which should transition us into a more
	 *	permanent state.
	 *
	 *	We don't run any pre/post hooks, as the state machine
	 *	is new, and no hooks have been added.
	 *
	 *	The result of the initialization routine can be
	 *	another new state, or 0 for "stay in the current
	 *	state".
	 */
	fr_assert(!m->current->def->enter);
	fr_assert(!m->current->def->exit);

	next = m->current->def->process(m, uctx);
	fr_assert(next >= 0);

	if (def->free) talloc_set_destructor(m, _machine_free);

	if (next) fr_machine_transition(m, next);

	return m;
}


/** Process the state machine
 *
 * @param m	The state machine
 * @return
 *	- 0 for "no transition has occured"
 *	- >0 for "we are in a new state".
 *	-<0 for "error, you should tear down the state machine".
 *
 *  This function should be called by the user of the machine.
 *
 *  In general, the caller doesn't really care about the return code
 *  of this function.  The only real utility is >=0 for "continue
 *  calling the state machine as necessary", or <0 for "shut down the
 *  state machine".
 */
int fr_machine_process(fr_machine_t *m)
{
	int state, old;
	fr_machine_state_inst_t *current = m->current;

	fr_assert(current != NULL);
	fr_assert(!m->deferred);
	fr_assert(!m->dead);
	fr_assert(!m->paused);

	m->in_handler = current;
	old = current->def->number;

	call_hook(m, &current->process[PRE], old, old);
	state = current->def->process(m, m->uctx);
	call_hook(m, &current->process[POST], old, old);

	m->in_handler = NULL;

	/*
	 *	No changes.  Tell the caller to wait for something
	 *	else to signal a transition.
	 */
	if (state == 0) return 0;

	/*
	 *	The called function requested that we transition to
	 *	the "free" state.  Don't do that, but instead return
	 *	an error to the caller.  The caller MUST do nothing
	 *	other than free the state machine.
	 */
	if (state == m->def->free) {
		m->dead = true;
		return -1;
	}

	/*
	 *	Transition to the new state.
	 */
	fr_machine_transition(m, state);

	return state;
}

/** Transition to a new state
 *
 * @param m	The state machine
 * @param state	the state to transition to
 * @return
 *	- <0 for error
 *	- 0 for the transition was made (or deferred)
 *
 *  The transition MAY be deferred.  Note that only one transition at
 *  a time can be deferred.
 *
 *  This function MUST NOT be called from any "hook", or from any
 *  enter/exit/process function.  It should ONLY be called from the
 *  "parent" of the state machine, when it decides that the state
 *  machine needs to change.
 *
 *  i.e. from a timer, or an IO callback
 */
int fr_machine_transition(fr_machine_t *m, int state)
{
	fr_machine_state_inst_t *current = m->current;

	if (m->dead) return -1;

	/*
	 *	Bad states are not allowed.
	 */
	if ((state < 0) || (state > m->def->max_state)) return -1;

	/*
	 *	If we are not in a state, we cannot transition to
	 *	anything else.
	 */
	if (!current) return -1;

	/*
	 *	Transition to self is "do nothing".
	 */
	if (current->def->number == state) return 0;

#if 0
	/*
	 *	Asked to do an illegal transition, that's an error.
	 */
	if (!current->out[state]) {
		fr_assert(0);
		return;
	}
#endif

	/*
	 *	We cannot transition from inside of a particular
	 *	state.  Instead, the state MUST return a new state
	 *	number, and fr_machine_process() will do the
	 *	transition.
	 */
	fr_assert(!m->in_handler);

	/*
	 *	The caller may be mucking with bits of the state
	 *	machine and/or the code surrounding the state machine.
	 *	In that case, the caller doesn't want transitions to
	 *	occur until it's done those changes.  Otherwise the
	 *	state machine could disappear in the middle of a
	 *	function, which is bad.
	 *
	 *	However, the rest of the code doesn't know what the
	 *	caller wants.  So the caller "pauses" state
	 *	transitions until it's done.  We check for that here,
	 *	and defer transitions until such time as the
	 *	transitions are resumed.
	 */
	if (m->paused) {
		fr_assert(!m->deferred);
		m->deferred = state;
		return 0;
	}

	/*
	 *	We're allowed to do the transition now, so exit the
	 *	current state, and enter the new one.
	 */
	state_transition(m, state, (void *) fr_machine_transition);

	return 0;
}

/** Get the current state
 *
 * @param m	The state machine
 * @return
 *	The current state, or 0 for "not in any state"
 */
int fr_machine_current(fr_machine_t *m)
{
	fr_assert(!m->dead);

	if (!m->current) return 0;

	return m->current->def->number;
}

/** Get the name of a particular state
 *
 * @param m	The state machine
 * @param state The state to query
 * @return
 *	the name
 */
char const *fr_machine_state_name(fr_machine_t *m, int state)
{
	fr_assert(!m->dead);

	if ((state < 0) || (state > m->def->max_state)) return "???";

	if (!state) {
		if (m->current) {
			state = m->current->def->number;

		} else if (m->deferred) {
			state = m->deferred;

		} else {
			return "???";
		}
	}

	return m->def->state[state].name;
}

static int _machine_hook_free(fr_machine_hook_t *hook)
{
	(void) fr_dlist_remove(hook->head, &hook->dlist);

	return 0;
}


/** Add a hook to a state, with an optional talloc_ctx.
 *
 *  The hook is removed when the talloc ctx is freed.
 *
 *  You can also remove the hook by freeing the returned pointer.
 */
void *fr_machine_hook(fr_machine_t *m, TALLOC_CTX *ctx, int state_to_hook, fr_machine_hook_type_t type, fr_machine_hook_sense_t sense,
		      bool oneshot, fr_machine_hook_func_t func, void *uctx)
{
	fr_machine_hook_t *hook;
	fr_dlist_head_t *head;
	fr_machine_state_inst_t *state = &m->state[state_to_hook];

	fr_assert(!m->dead);

	switch (type) {
	case FR_MACHINE_ENTER:
		head = &state->enter[sense];
		break;

	case FR_MACHINE_PROCESS:
		head = &state->process[sense];
		break;

	case FR_MACHINE_EXIT:
		head = &state->exit[sense];
		break;

	default:
		return NULL;
	}

	hook = talloc_zero(ctx, fr_machine_hook_t);
	if (!hook) return NULL;

	*hook = (fr_machine_hook_t) {
		.func = func,
		.head = head,	/* needed for updating num_elements on remove */
		.uctx = uctx,
		.oneshot = oneshot,
	};

	fr_dlist_insert_tail(head, &hook->dlist);

	talloc_set_destructor(hook, _machine_hook_free);

	return hook;
}

/** Pause any transitions.
 *
 */
void fr_machine_pause(fr_machine_t *m)
{
	fr_assert(!m->dead);

	m->paused++;
}

/** Resume transitions.
 *
 */
void fr_machine_resume(fr_machine_t *m)
{
	int state;

	fr_assert(!m->dead);

	if (m->paused > 0) {
		m->paused--;
		if (m->paused > 0) return;
	}

	if (!m->deferred) return;

	/*
	 *	Clear the deferred transition before making any
	 *	changes, as we're now doing the transition.
	 */
	state = m->deferred;
	m->deferred = 0;

	state_transition(m, state, (void *) fr_machine_resume);
}
