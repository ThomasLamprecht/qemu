#include "qemu/osdep.h"
#include "migration/migration.h"
#include "migration/savevm.h"
#include "migration/snapshot.h"
#include "migration/global_state.h"
#include "migration/ram.h"
#include "migration/qemu-file.h"
#include "sysemu/sysemu.h"
#include "block/block.h"
#include "sysemu/block-backend.h"
#include "qapi/error.h"
#include "qapi/qmp/qerror.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qapi-commands-migration.h"
#include "qapi/qapi-commands-misc.h"
#include "qapi/qapi-commands-block.h"
#include "qemu/cutils.h"

/* #define DEBUG_SAVEVM_STATE */

/* used while emulated sync operation in progress */
#define NOT_DONE -EINPROGRESS

#ifdef DEBUG_SAVEVM_STATE
#define DPRINTF(fmt, ...) \
    do { printf("savevm-async: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

enum {
    SAVE_STATE_DONE,
    SAVE_STATE_ERROR,
    SAVE_STATE_ACTIVE,
    SAVE_STATE_COMPLETED,
    SAVE_STATE_CANCELLED
};


static struct SnapshotState {
    BlockBackend *target;
    size_t bs_pos;
    int state;
    Error *error;
    Error *blocker;
    int saved_vm_running;
    QEMUFile *file;
    int64_t total_time;
    QEMUBH *cleanup_bh;
    QemuThread thread;
} snap_state;

SaveVMInfo *qmp_query_savevm(Error **errp)
{
    SaveVMInfo *info = g_malloc0(sizeof(*info));
    struct SnapshotState *s = &snap_state;

    if (s->state != SAVE_STATE_DONE) {
        info->has_bytes = true;
        info->bytes = s->bs_pos;
        switch (s->state) {
        case SAVE_STATE_ERROR:
            info->has_status = true;
            info->status = g_strdup("failed");
            info->has_total_time = true;
            info->total_time = s->total_time;
            if (s->error) {
                info->has_error = true;
                info->error = g_strdup(error_get_pretty(s->error));
            }
            break;
        case SAVE_STATE_ACTIVE:
            info->has_status = true;
            info->status = g_strdup("active");
            info->has_total_time = true;
            info->total_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME)
                - s->total_time;
            break;
        case SAVE_STATE_COMPLETED:
            info->has_status = true;
            info->status = g_strdup("completed");
            info->has_total_time = true;
            info->total_time = s->total_time;
            break;
        }
    }

    return info;
}

static int save_snapshot_cleanup(void)
{
    int ret = 0;

    DPRINTF("save_snapshot_cleanup\n");

    snap_state.total_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) -
        snap_state.total_time;

    if (snap_state.file) {
        ret = qemu_fclose(snap_state.file);
    }

    if (snap_state.target) {
        /* try to truncate, but ignore errors (will fail on block devices).
         * note: bdrv_read() need whole blocks, so we round up
         */
        size_t size = (snap_state.bs_pos + BDRV_SECTOR_SIZE) & BDRV_SECTOR_MASK;
        blk_truncate(snap_state.target, size, PREALLOC_MODE_OFF, NULL);
        blk_op_unblock_all(snap_state.target, snap_state.blocker);
        error_free(snap_state.blocker);
        snap_state.blocker = NULL;
        blk_unref(snap_state.target);
        snap_state.target = NULL;
    }

    return ret;
}

static void save_snapshot_error(const char *fmt, ...)
{
    va_list ap;
    char *msg;

    va_start(ap, fmt);
    msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);

    DPRINTF("save_snapshot_error: %s\n", msg);

    if (!snap_state.error) {
        error_set(&snap_state.error, ERROR_CLASS_GENERIC_ERROR, "%s", msg);
    }

    g_free (msg);

    snap_state.state = SAVE_STATE_ERROR;
}

static int block_state_close(void *opaque)
{
    snap_state.file = NULL;
    return blk_flush(snap_state.target);
}

typedef struct BlkRwCo {
    int64_t offset;
    QEMUIOVector *qiov;
    ssize_t ret;
} BlkRwCo;

static void coroutine_fn block_state_write_entry(void *opaque) {
    BlkRwCo *rwco = opaque;
    rwco->ret = blk_co_pwritev(snap_state.target, rwco->offset, rwco->qiov->size,
                               rwco->qiov, 0);
    aio_wait_kick();
}

static ssize_t block_state_writev_buffer(void *opaque, struct iovec *iov,
                                         int iovcnt, int64_t pos)
{
    QEMUIOVector qiov;
    BlkRwCo rwco;

    assert(pos == snap_state.bs_pos);
    rwco = (BlkRwCo) {
        .offset = pos,
        .qiov = &qiov,
        .ret = NOT_DONE,
    };

    qemu_iovec_init_external(&qiov, iov, iovcnt);

    if (qemu_in_coroutine()) {
        block_state_write_entry(&rwco);
    } else {
        Coroutine *co = qemu_coroutine_create(&block_state_write_entry, &rwco);
        bdrv_coroutine_enter(blk_bs(snap_state.target), co);
        BDRV_POLL_WHILE(blk_bs(snap_state.target), rwco.ret == NOT_DONE);
    }
    if (rwco.ret < 0) {
        return rwco.ret;
    }

    snap_state.bs_pos += qiov.size;
    return qiov.size;
}

static const QEMUFileOps block_file_ops = {
    .writev_buffer =  block_state_writev_buffer,
    .close =          block_state_close,
};

static void process_savevm_cleanup(void *opaque)
{
    int ret;
    qemu_bh_delete(snap_state.cleanup_bh);
    snap_state.cleanup_bh = NULL;
    qemu_mutex_unlock_iothread();
    qemu_thread_join(&snap_state.thread);
    qemu_mutex_lock_iothread();
    ret = save_snapshot_cleanup();
    if (ret < 0) {
        save_snapshot_error("save_snapshot_cleanup error %d", ret);
    } else if (snap_state.state == SAVE_STATE_ACTIVE) {
        snap_state.state = SAVE_STATE_COMPLETED;
    } else {
        save_snapshot_error("process_savevm_cleanup: invalid state: %d",
                            snap_state.state);
    }
    if (snap_state.saved_vm_running) {
        vm_start();
        snap_state.saved_vm_running = false;
    }
}

static void *process_savevm_thread(void *opaque)
{
    int ret;
    int64_t maxlen;

    rcu_register_thread();

    qemu_savevm_state_header(snap_state.file);
    qemu_savevm_state_setup(snap_state.file);
    ret = qemu_file_get_error(snap_state.file);

    if (ret < 0) {
        save_snapshot_error("qemu_savevm_state_setup failed");
        rcu_unregister_thread();
        return NULL;
    }

    while (snap_state.state == SAVE_STATE_ACTIVE) {
        uint64_t pending_size, pend_precopy, pend_compatible, pend_postcopy;

        qemu_savevm_state_pending(snap_state.file, 0, &pend_precopy, &pend_compatible, &pend_postcopy);
        pending_size = pend_precopy + pend_compatible + pend_postcopy;

        maxlen = blk_getlength(snap_state.target) - 30*1024*1024;

        if (pending_size > 400000 && snap_state.bs_pos + pending_size < maxlen) {
            qemu_mutex_lock_iothread();
            ret = qemu_savevm_state_iterate(snap_state.file, false);
            if (ret < 0) {
                save_snapshot_error("qemu_savevm_state_iterate error %d", ret);
                break;
            }
            qemu_mutex_unlock_iothread();
            DPRINTF("savevm inerate pending size %lu ret %d\n", pending_size, ret);
        } else {
            qemu_mutex_lock_iothread();
            qemu_system_wakeup_request(QEMU_WAKEUP_REASON_OTHER, NULL);
            ret = global_state_store();
            if (ret) {
                save_snapshot_error("global_state_store error %d", ret);
                break;
            }
            ret = vm_stop_force_state(RUN_STATE_FINISH_MIGRATE);
            if (ret < 0) {
                save_snapshot_error("vm_stop_force_state error %d", ret);
                break;
            }
            DPRINTF("savevm inerate finished\n");
            /* upstream made the return value here inconsistent
             * (-1 instead of 'ret' in one case and 0 after flush which can
             * still set a file error...)
             */
            (void)qemu_savevm_state_complete_precopy(snap_state.file, false, false);
            ret = qemu_file_get_error(snap_state.file);
            if (ret < 0) {
                    save_snapshot_error("qemu_savevm_state_iterate error %d", ret);
                    break;
            }
            qemu_savevm_state_cleanup();
            DPRINTF("save complete\n");
            break;
        }
    }

    qemu_bh_schedule(snap_state.cleanup_bh);
    qemu_mutex_unlock_iothread();

    rcu_unregister_thread();
    return NULL;
}

void qmp_savevm_start(bool has_statefile, const char *statefile, Error **errp)
{
    Error *local_err = NULL;

    int bdrv_oflags = BDRV_O_RDWR | BDRV_O_RESIZE | BDRV_O_NO_FLUSH;

    if (snap_state.state != SAVE_STATE_DONE) {
        error_set(errp, ERROR_CLASS_GENERIC_ERROR,
                  "VM snapshot already started\n");
        return;
    }

    /* initialize snapshot info */
    snap_state.saved_vm_running = runstate_is_running();
    snap_state.bs_pos = 0;
    snap_state.total_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    snap_state.blocker = NULL;

    if (snap_state.error) {
        error_free(snap_state.error);
        snap_state.error = NULL;
    }

    if (!has_statefile) {
        vm_stop(RUN_STATE_SAVE_VM);
        snap_state.state = SAVE_STATE_COMPLETED;
        return;
    }

    if (qemu_savevm_state_blocked(errp)) {
        return;
    }

    /* Open the image */
    QDict *options = NULL;
    options = qdict_new();
    qdict_put_str(options, "driver", "raw");
    snap_state.target = blk_new_open(statefile, NULL, options, bdrv_oflags, &local_err);
    if (!snap_state.target) {
        error_set(errp, ERROR_CLASS_GENERIC_ERROR, "failed to open '%s'", statefile);
        goto restart;
    }

    snap_state.file = qemu_fopen_ops(&snap_state, &block_file_ops);

    if (!snap_state.file) {
        error_set(errp, ERROR_CLASS_GENERIC_ERROR, "failed to open '%s'", statefile);
        goto restart;
    }


    error_setg(&snap_state.blocker, "block device is in use by savevm");
    blk_op_block_all(snap_state.target, snap_state.blocker);

    snap_state.state = SAVE_STATE_ACTIVE;
    snap_state.cleanup_bh = qemu_bh_new(process_savevm_cleanup, &snap_state);
    qemu_thread_create(&snap_state.thread, "savevm-async", process_savevm_thread,
                       NULL, QEMU_THREAD_JOINABLE);

    return;

restart:

    save_snapshot_error("setup failed");

    if (snap_state.saved_vm_running) {
        vm_start();
    }
}

void qmp_savevm_end(Error **errp)
{
    if (snap_state.state == SAVE_STATE_DONE) {
        error_set(errp, ERROR_CLASS_GENERIC_ERROR,
                  "VM snapshot not started\n");
        return;
    }

    if (snap_state.state == SAVE_STATE_ACTIVE) {
        snap_state.state = SAVE_STATE_CANCELLED;
        return;
    }

    if (snap_state.saved_vm_running) {
        vm_start();
    }

    snap_state.state = SAVE_STATE_DONE;
}

// FIXME: Deprecated
void qmp_snapshot_drive(const char *device, const char *name, Error **errp)
{
    // Compatibility to older qemu-server.
    qmp_blockdev_snapshot_internal_sync(device, name, errp);
}

// FIXME: Deprecated
void qmp_delete_drive_snapshot(const char *device, const char *name,
                               Error **errp)
{
    // Compatibility to older qemu-server.
    (void)qmp_blockdev_snapshot_delete_internal_sync(device, false, NULL,
                                                     true, name, errp);
}

static ssize_t loadstate_get_buffer(void *opaque, uint8_t *buf, int64_t pos,
                                    size_t size)
{
    BlockBackend *be = opaque;
    int64_t maxlen = blk_getlength(be);
    if (pos > maxlen) {
        return -EIO;
    }
    if ((pos + size) > maxlen) {
        size = maxlen - pos - 1;
    }
    if (size == 0) {
        return 0;
    }
    return blk_pread(be, pos, buf, size);
}

static const QEMUFileOps loadstate_file_ops = {
    .get_buffer = loadstate_get_buffer,
};

int load_snapshot_from_blockdev(const char *filename, Error **errp)
{
    BlockBackend *be;
    Error *local_err = NULL;
    Error *blocker = NULL;

    QEMUFile *f;
    int ret = -EINVAL;

    be = blk_new_open(filename, NULL, NULL, 0, &local_err);

    if (!be) {
        error_setg(errp, "Could not open VM state file");
        goto the_end;
    }

    error_setg(&blocker, "block device is in use by load state");
    blk_op_block_all(be, blocker);

    /* restore the VM state */
    f = qemu_fopen_ops(be, &loadstate_file_ops);
    if (!f) {
        error_setg(errp, "Could not open VM state file");
        goto the_end;
    }

    qemu_system_reset(SHUTDOWN_CAUSE_NONE);
    ret = qemu_loadvm_state(f);

    qemu_fclose(f);
    migration_incoming_state_destroy();
    if (ret < 0) {
        error_setg_errno(errp, -ret, "Error while loading VM state");
        goto the_end;
    }

    ret = 0;

 the_end:
    if (be) {
        blk_op_unblock_all(be, blocker);
        error_free(blocker);
        blk_unref(be);
    }
    return ret;
}
