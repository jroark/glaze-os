/* libui internals: transition buffers (transition.c). Not exported. */
#ifndef LIBUI_TRANSITION_H
#define LIBUI_TRANSITION_H

/* Frees the two off-screen views until the next transition needs them. */
void uitr_release(void);

#endif
