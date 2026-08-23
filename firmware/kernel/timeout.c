
/****************************************************************************
 * Tick-based interval timers/one-shots - be mindful this is not really
 * intended for continuous timers but for events that need to run for a short
 * time and be cancelled without further software intervention.
 ****************************************************************************/

#include "config.h"
#include "system.h" /* TIME_AFTER */
#include "kernel.h"
#include "timeout.h"
#include "general.h"

/* list of active timeout events */
static struct timeout *tmo_list[MAX_NUM_TIMEOUTS+1];
static long next_tmo_check;

/* Recalculate after registration/cancellation, which are infrequent compared
 * with the 100 Hz tick. Keeping a deadline avoids walking every active timer
 * on ticks where none can expire. IRQs must be disabled by the caller. */
static void timeout_recalc_next(void)
{
    long next = current_tick + 60*HZ;
    struct timeout **p = tmo_list;
    struct timeout *curr;

    while ((curr = *p++) != NULL)
    {
        if (TIME_BEFORE(curr->expires, next))
            next = curr->expires;
    }

    next_tmo_check = next;
}

/* timeout tick task - calls event handlers when they expire
 * Event handlers may alter expiration, callback and data during operation.
 */
static void timeout_tick(void)
{
    unsigned long tick = current_tick;
    struct timeout **p = tmo_list;
    struct timeout *curr;

    if (TIME_BEFORE(tick, next_tmo_check))
        return;

    /* Callbacks may register or cancel timers and update this deadline. */
    next_tmo_check = tick + 60*HZ;

    while ((curr = *p) != NULL)
    {
        int ticks;

        if(TIME_BEFORE(tick, curr->expires))
        {
            if (TIME_BEFORE(curr->expires, next_tmo_check))
                next_tmo_check = curr->expires;

            p++;
            continue;
        }

        /* this event has expired - call callback */
        ticks = curr->callback(curr);
        if(ticks > 0)
        {
            curr->expires = tick + ticks; /* reload */
            if (TIME_BEFORE(curr->expires, next_tmo_check))
                next_tmo_check = curr->expires;

            if (*p == curr)
                p++;
        }
        else
        {
            timeout_cancel(curr); /* cancel */
            if (*p == curr)
                p++;
        }
    }
}

/* Cancels a timeout callback - can be called from the ISR */
void timeout_cancel(struct timeout *tmo)
{
    int oldlevel = disable_irq_save();
    int rc = remove_array_ptr((void **)tmo_list, tmo);

    if(rc >= 0 && *tmo_list == NULL)
    {
        tick_remove_task(timeout_tick); /* Last one - remove task */
    }
    else if(rc >= 0)
    {
        timeout_recalc_next();
    }

    restore_irq(oldlevel);
}

/* Adds a timeout callback - calling with an active timeout resets the
   interval - can be called from the ISR */
void timeout_register(struct timeout *tmo, timeout_cb_type callback,
                      int ticks, intptr_t data)
{
    int oldlevel;
    void **arr, **p;

    if(tmo == NULL)
        return;

    oldlevel = disable_irq_save();

    /* See if this one is already registered */
    arr = (void **)tmo_list;
    p = find_array_ptr(arr, tmo);

    if(p - arr < MAX_NUM_TIMEOUTS)
    {
        /* Vacancy */
        if(*p == NULL)
        {
            /* Not present */
            if(*tmo_list == NULL)
            {
                tick_add_task(timeout_tick); /* First one - add task */
            }

            *p = tmo;
        }

        tmo->callback = callback;
        tmo->data = data;
        tmo->expires = current_tick + ticks;
        timeout_recalc_next();
    }

    restore_irq(oldlevel);
}
