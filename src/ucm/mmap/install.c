/**
 * Copyright (C) Mellanox Technologies Ltd. 2001-2015.  ALL RIGHTS RESERVED.
 * Copyright (C) ARM Ltd. 2016.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "mmap.h"

#include <ucm/api/ucm.h>
#include <ucm/event/event.h>
#include <ucm/util/log.h>
#include <ucm/util/reloc.h>
#include <ucm/util/replace.h>
#include <ucm/util/sys.h>
#include <ucm/bistro/bistro.h>
#include <ucs/arch/atomic.h>
#include <ucs/sys/sys.h>
#include <ucs/sys/math.h>
#include <ucs/sys/checker.h>
#include <ucs/sys/preprocessor.h>
#include <ucs/arch/bitops.h>
#include <ucs/debug/assert.h>

#include <sys/mman.h>
#include <sys/shm.h>
#include <unistd.h>
#include <pthread.h>


#define UCM_HOOK_STR \
    ((ucm_mmap_hook_mode() == UCM_MMAP_HOOK_RELOC) ?  "reloc" : "bistro")

#define UCM_FIRE_EVENT(_event, _mask, _data, _call)                           \
    do {                                                                      \
        int exp_events = (_event) & (_mask);                                  \
        (_data)->fired_events = 0;                                            \
        _call;                                                                \
        ucm_trace("after %s: got 0x%x/0x%x", UCS_PP_MAKE_STRING(_call),       \
                  (_data)->fired_events, exp_events);                         \
        (_data)->out_events &= ~exp_events | (_data)->fired_events;           \
    } while(0)

#define UCM_MMAP_RELOC_ENTRY(_name) \
    { \
        .symbol     = #_name, \
        .value      = ucm_override_##_name, \
        .prev_value = _name \
    }


typedef struct ucm_mmap_func {
    ucm_reloc_patch_t    patch;
    ucm_event_type_t     event_type;
    ucm_event_type_t     deps;
} ucm_mmap_func_t;

typedef struct ucm_mmap_test_events_data {
    uint32_t             fired_events;
    int                  out_events;
} ucm_mmap_test_events_data_t;

static ucm_mmap_func_t ucm_mmap_funcs[] = {
    { UCM_MMAP_RELOC_ENTRY(mmap),    UCM_EVENT_MMAP,    UCM_EVENT_NONE},
    { UCM_MMAP_RELOC_ENTRY(munmap),  UCM_EVENT_MUNMAP,  UCM_EVENT_NONE},
#if HAVE_MREMAP
    { UCM_MMAP_RELOC_ENTRY(mremap),  UCM_EVENT_MREMAP,  UCM_EVENT_NONE},
#endif
    { UCM_MMAP_RELOC_ENTRY(shmat),   UCM_EVENT_SHMAT,   UCM_EVENT_NONE},
    { UCM_MMAP_RELOC_ENTRY(shmdt),   UCM_EVENT_SHMDT,   UCM_EVENT_SHMAT},
    { UCM_MMAP_RELOC_ENTRY(sbrk),    UCM_EVENT_SBRK,    UCM_EVENT_NONE},
    { UCM_MMAP_RELOC_ENTRY(brk),     UCM_EVENT_BRK,     UCM_EVENT_NONE},
    { UCM_MMAP_RELOC_ENTRY(madvise), UCM_EVENT_MADVISE, UCM_EVENT_NONE},
    { {NULL, NULL, NULL}, UCM_EVENT_NONE}
};

static pthread_mutex_t ucm_mmap_install_mutex = PTHREAD_MUTEX_INITIALIZER;
static int ucm_mmap_installed_events = 0; /* events that were reported as installed */

static void ucm_mmap_event_test_callback(ucm_event_type_t event_type,
                                         ucm_event_t *event, void *arg)
{
    ucm_mmap_test_events_data_t *data = arg;

    /* This callback may be called from multiple threads, which are just calling
     * memory allocations/release, and not testing mmap hooks at the moment.
     * So in order to ensure the thread which tests events sees all fired
     * events, use atomic OR operation.
     */
    ucs_atomic_or32(&data->fired_events, event_type);
}

/* Call brk() and check return value, to avoid compile error of unused result */
static void ucm_brk_checked(void *addr)
{
    int ret = brk(addr);
    if ((ret != 0) && (addr != NULL)) {
        ucm_debug("brk(addr=%p) failed: %m", addr);
    }
}

/* Fire events with pre/post action. The problem is in call sequence: we
 * can't just fire single event - most of the system calls require set of
 * calls to eliminate resource leaks or data corruption, such sequence
 * produces additional events which may affect to event handling. To
 * exclude additional events from processing used pre/post actions where
 * set of handled events is cleared and evaluated for every system call */
static void
ucm_fire_mmap_events_internal(int events, ucm_mmap_test_events_data_t *data,
                              int exclusive)
{
    size_t sbrk_size;
    int shmid;
    void *p;

    if (events & (UCM_EVENT_MMAP|UCM_EVENT_MUNMAP|UCM_EVENT_MREMAP|
                  UCM_EVENT_VM_MAPPED|UCM_EVENT_VM_UNMAPPED)) {
        UCM_FIRE_EVENT(events, UCM_EVENT_MMAP|UCM_EVENT_VM_MAPPED,
                       data, p = mmap(NULL, ucm_get_page_size(), PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
#ifdef HAVE_MREMAP
        /* generate MAP event */
        UCM_FIRE_EVENT(events, UCM_EVENT_MREMAP|UCM_EVENT_VM_MAPPED|UCM_EVENT_VM_UNMAPPED,
                       data, p = mremap(p, ucm_get_page_size(),
                                        ucm_get_page_size() * 2, MREMAP_MAYMOVE));
        /* generate UNMAP event */
        UCM_FIRE_EVENT(events, UCM_EVENT_MREMAP|UCM_EVENT_VM_MAPPED|UCM_EVENT_VM_UNMAPPED,
                       data, p = mremap(p, ucm_get_page_size() * 2, ucm_get_page_size(), 0));
#endif
        /* generate UNMAP event */
        UCM_FIRE_EVENT(events, UCM_EVENT_MMAP|UCM_EVENT_VM_MAPPED,
                       data, p = mmap(p, ucm_get_page_size(), PROT_READ | PROT_WRITE,
                                      MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        UCM_FIRE_EVENT(events, UCM_EVENT_MUNMAP|UCM_EVENT_VM_UNMAPPED,
                       data, munmap(p, ucm_get_page_size()));
    }

    if (events & (UCM_EVENT_SHMAT|UCM_EVENT_SHMDT|UCM_EVENT_VM_MAPPED|UCM_EVENT_VM_UNMAPPED)) {
        shmid = shmget(IPC_PRIVATE, ucm_get_page_size(), IPC_CREAT | SHM_R | SHM_W);
        if (shmid == -1) {
            ucm_debug("shmget failed: %m");
            return;
        }

        UCM_FIRE_EVENT(events, UCM_EVENT_SHMAT|UCM_EVENT_VM_MAPPED,
                       data, p = shmat(shmid, NULL, 0));
        UCM_FIRE_EVENT(events, UCM_EVENT_SHMAT|UCM_EVENT_VM_MAPPED|UCM_EVENT_VM_UNMAPPED,
                       data, p = shmat(shmid, p, SHM_REMAP));
        shmctl(shmid, IPC_RMID, NULL);
        UCM_FIRE_EVENT(events, UCM_EVENT_SHMDT|UCM_EVENT_VM_UNMAPPED,
                       data, shmdt(p));
    }

    if (exclusive && !RUNNING_ON_VALGRIND) {
        sbrk_size = ucm_get_page_size();
        if (events & (UCM_EVENT_BRK|UCM_EVENT_VM_MAPPED|UCM_EVENT_VM_UNMAPPED)) {
            p = ucm_get_current_brk();
            UCM_FIRE_EVENT(events, UCM_EVENT_BRK|UCM_EVENT_VM_MAPPED, data,
                           ucm_brk_checked(UCS_PTR_BYTE_OFFSET(p, sbrk_size)));
            UCM_FIRE_EVENT(events, UCM_EVENT_BRK|UCM_EVENT_VM_UNMAPPED, data,
                           ucm_brk_checked(p));
        }
        if (events & (UCM_EVENT_SBRK|UCM_EVENT_VM_MAPPED|UCM_EVENT_VM_UNMAPPED)) {
            UCM_FIRE_EVENT(events, UCM_EVENT_SBRK|UCM_EVENT_VM_MAPPED,
                           data, (void)sbrk(sbrk_size));
            UCM_FIRE_EVENT(events, UCM_EVENT_SBRK|UCM_EVENT_VM_UNMAPPED,
                           data, (void)sbrk(-sbrk_size));
        }
    } else {
        /* To avoid side effects on other threads and valgrind heap corruption,
         * pass invalid parameters. We assume that if the natives events are
         * delivered, it means VM_MAPPED/UNMAPPED would be delivered as well.
         */
        if (events & UCM_EVENT_BRK) {
            UCM_FIRE_EVENT(events, UCM_EVENT_BRK, data, ucm_brk_checked(NULL));
        }
    }

    if (events & (UCM_EVENT_MADVISE|UCM_EVENT_VM_UNMAPPED)) {
        UCM_FIRE_EVENT(events, UCM_EVENT_MMAP|UCM_EVENT_VM_MAPPED, data,
                       p = mmap(NULL, ucm_get_page_size(), PROT_READ|PROT_WRITE,
                                MAP_PRIVATE|MAP_ANON, -1, 0));
        if (p != MAP_FAILED) {
            UCM_FIRE_EVENT(events, UCM_EVENT_MADVISE, data,
                           madvise(p, ucm_get_page_size(), MADV_DONTNEED));
            UCM_FIRE_EVENT(events, UCM_EVENT_MUNMAP|UCM_EVENT_VM_UNMAPPED, data,
                           munmap(p, ucm_get_page_size()));
        } else {
            ucm_debug("mmap failed: %m");
        }
    }
}

void ucm_fire_mmap_events(int events)
{
    ucm_mmap_test_events_data_t data;

    ucm_fire_mmap_events_internal(events, &data, 0);
}

/* Called with lock held */
static ucs_status_t ucm_mmap_test_events(int events, int exclusive)
{
    ucm_event_handler_t handler;
    ucm_mmap_test_events_data_t data;

    handler.events    = events;
    handler.priority  = -1;
    handler.cb        = ucm_mmap_event_test_callback;
    handler.arg       = &data;
    data.out_events   = events;

    ucm_event_handler_add(&handler);
    ucm_fire_mmap_events_internal(events, &data, exclusive);
    ucm_event_handler_remove(&handler);

    ucm_debug("mmap test: got 0x%x out of 0x%x", data.out_events, events);

    /* Return success if we caught all wanted events */
    if (!ucs_test_all_flags(data.out_events, events)) {
        return UCS_ERR_UNSUPPORTED;
    }

    return UCS_OK;
}

ucs_status_t ucm_mmap_test_installed_events(int events)
{
    ucs_status_t status;

    /*
     * return UCS_OK iff all installed events are actually working
     * we don't check the status of events which were not successfully installed
     */
    pthread_mutex_lock(&ucm_mmap_install_mutex);
    status = ucm_mmap_test_events(events & ucm_mmap_installed_events, 0);
    pthread_mutex_unlock(&ucm_mmap_install_mutex);

    return status;
}

/* Called with lock held */
static ucs_status_t ucs_mmap_install_reloc(int events)
{
    static int installed_events = 0;
    ucm_mmap_func_t *entry;
    ucs_status_t status;
    void *func_ptr;

    if (ucm_mmap_hook_mode() == UCM_MMAP_HOOK_NONE) {
        ucm_debug("installing mmap hooks is disabled by configuration");
        return UCS_ERR_UNSUPPORTED;
    }

    for (entry = ucm_mmap_funcs; entry->patch.symbol != NULL; ++entry) {
        if (!((entry->event_type|entry->deps) & events)) {
            /* Not required */
            continue;
        }

        if (entry->event_type & installed_events) {
            /* Already installed */
            continue;
        }

        ucm_debug("mmap: installing %s hook for %s = %p for event 0x%x",
                  UCM_HOOK_STR, entry->patch.symbol, entry->patch.value,
                  entry->event_type);

        if (ucm_mmap_hook_mode() == UCM_MMAP_HOOK_RELOC) {
            status = ucm_reloc_modify(&entry->patch);
        } else {
            ucm_assert(ucm_mmap_hook_mode() == UCM_MMAP_HOOK_BISTRO);
            func_ptr = ucm_reloc_get_orig(entry->patch.symbol,
                                          entry->patch.value);
            if ((func_ptr == NULL) && !ucs_sys_is_dynamic_lib()) {
                /* prev_value is used to store pointer to libc function,
                 * used in library static build when other ways to
                 * find symbol were not successful */
                func_ptr = entry->patch.prev_value;
            }

            if (func_ptr == NULL) {
                status = UCS_ERR_NO_ELEM;
            } else {
                status = ucm_bistro_patch(func_ptr, entry->patch.value, NULL);
            }
        }
        if (status != UCS_OK) {
            ucm_warn("failed to install %s hook for '%s'", UCM_HOOK_STR,
                     entry->patch.symbol);
            return status;
        }

        installed_events |= entry->event_type;
    }

    return UCS_OK;
}

ucs_status_t ucm_mmap_install(int events, int exclusive)
{
    ucs_status_t status;

    pthread_mutex_lock(&ucm_mmap_install_mutex);

    if (ucs_test_all_flags(ucm_mmap_installed_events, events)) {
        /* if we already installed these events, check that they are still
         * working, and if not - reinstall them.
         */
        status = ucm_mmap_test_events(events, exclusive);
        if (status == UCS_OK) {
            goto out_unlock;
        }
    }

    status = ucs_mmap_install_reloc(events);
    if (status != UCS_OK) {
        ucm_debug("failed to install relocations for mmap");
        goto out_unlock;
    }

    status = ucm_mmap_test_events(events, exclusive);
    if (status != UCS_OK) {
        ucm_debug("failed to install mmap events");
        goto out_unlock;
    }

    /* status == UCS_OK */
    ucm_mmap_installed_events |= events;
    ucm_debug("mmap installed events = 0x%x", ucm_mmap_installed_events);

out_unlock:
    pthread_mutex_unlock(&ucm_mmap_install_mutex);
    return status;
}

void ucm_mmap_init()
{
    ucm_event_type_t native_events;
    ucm_mmap_func_t *entry;

    if (!ucm_global_opts.enable_events ||
        (ucm_mmap_hook_mode() != UCM_MMAP_HOOK_BISTRO)) {
        return;
    }

    /* We must initialize bistro hooks during startup and not later, before
     * other threads could execute the modified functions and fail on invalid
     * instructions
     */
    native_events = 0;
    for (entry = ucm_mmap_funcs; entry->patch.symbol != NULL; ++entry) {
        native_events |= entry->event_type;
    }

    ucm_prevent_dl_unload();
    ucm_mmap_install(native_events, 1);
}
