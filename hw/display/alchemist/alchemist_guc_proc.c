/*
 * Intel Arc "Alchemist" (DG2) GPU simulation - satellite GuC coprocessor
 * process launch and QMP control
 *
 * GuC's real firmware runs on its own x86 core (a 486/Pentium-class ISA,
 * confirmed via the CPU models our own build already supports) - actually
 * executing that firmware, rather than simulating its protocol behavior in
 * C, means giving it a real CPU to run on. Embedding a second CPU object in
 * this same QEMU process would force the whole VM onto TCG (QEMU's
 * accelerator choice is per-process, not per-CPU - see current_accel() in
 * accel/accel-system.c), losing KVM for the main guest. Instead this spawns
 * a second, independent qemu-system-x86_64 process - free to run its own
 * "-accel tcg" - and controls it over QMP, the same way tests/qtest/
 * libqtest.c drives a spawned QEMU from C using QEMU's own JSON object
 * model, and the same fork()+exec() convention net/tap.c's
 * net_bridge_run_helper()/launch_script() use for a long-lived helper
 * process.
 *
 * This file only launches the process and proves the QMP control channel
 * works (qmp_capabilities handshake). It does not yet load firmware, run
 * the CPU, or relay register accesses - see docs/alchemist-bringup.md for
 * the phase this belongs to and what's still to come.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include <sys/wait.h>
#include "qemu/sockets.h"
#include "qemu/cutils.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qapi/error.h"
#include "alchemist_internal.h"

#define ALCHEMIST_GUC_PROC_CONNECT_RETRIES 100
#define ALCHEMIST_GUC_PROC_CONNECT_DELAY_US 20000 /* 100 * 20ms = up to 2s */
#define ALCHEMIST_GUC_PROC_QUIT_RETRIES 50
#define ALCHEMIST_GUC_PROC_QUIT_DELAY_US 20000 /* 50 * 20ms = up to 1s */

static void close_fds_after_fork(void)
{
    const int skip_fd[] = { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO };

    qemu_close_all_open_fd(skip_fd, ARRAY_SIZE(skip_fd));
}

/*
 * We're an uninstalled build (running straight out of build/), so the
 * satellite binary is simply our own sibling in the same directory -
 * qemu_init_exec_dir()'s /proc/self/exe technique, done locally since that
 * value isn't exposed to device code. Falls back to $PATH for an installed
 * layout.
 */
static char *find_qemu_binary(void)
{
    char buf[PATH_MAX];
    ssize_t len;
    char *dir, *candidate;

    len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        dir = g_path_get_dirname(buf);
        candidate = g_build_filename(dir, "qemu-system-x86_64", NULL);
        g_free(dir);
        if (access(candidate, X_OK) == 0) {
            return candidate;
        }
        g_free(candidate);
    }

    return g_find_program_in_path("qemu-system-x86_64");
}

static bool guc_proc_qmp_send(AlchemistState *s, QDict *cmd, Error **errp)
{
    g_autoptr(GString) json = qobject_to_json(QOBJECT(cmd));
    ssize_t ret;

    g_string_append_c(json, '\n');
    ret = RETRY_ON_EINTR(write(s->guc_proc.qmp_fd, json->str, json->len));
    if (ret < 0 || (size_t)ret != json->len) {
        error_setg_errno(errp, errno,
                          "alchemist: failed writing to satellite QMP socket");
        return false;
    }
    return true;
}

static QDict *guc_proc_qmp_recv(AlchemistState *s, Error **errp)
{
    GString *line = g_string_new(NULL);
    QObject *obj;
    QDict *dict;

    for (;;) {
        char c;
        ssize_t ret = RETRY_ON_EINTR(read(s->guc_proc.qmp_fd, &c, 1));

        if (ret <= 0) {
            error_setg_errno(errp, errno,
                              "alchemist: failed reading from satellite QMP socket");
            g_string_free(line, true);
            return NULL;
        }
        if (c == '\n') {
            break;
        }
        g_string_append_c(line, c);
    }

    obj = qobject_from_json(line->str, errp);
    g_string_free(line, true);
    if (!obj) {
        return NULL;
    }
    dict = qobject_to(QDict, obj);
    if (!dict) {
        qobject_unref(obj);
        error_setg(errp, "alchemist: satellite QMP reply was not a JSON object");
        return NULL;
    }
    return dict;
}

static bool guc_proc_connect_qmp(AlchemistState *s, Error **errp)
{
    int i;

    for (i = 0; i < ALCHEMIST_GUC_PROC_CONNECT_RETRIES; i++) {
        Error *local_err = NULL;
        int fd = unix_connect(s->guc_proc.qmp_path, &local_err);

        if (fd >= 0) {
            s->guc_proc.qmp_fd = fd;
            error_free(local_err);
            return true;
        }
        error_free(local_err);
        g_usleep(ALCHEMIST_GUC_PROC_CONNECT_DELAY_US);
    }

    error_setg(errp,
               "alchemist: timed out connecting to satellite QMP socket %s",
               s->guc_proc.qmp_path);
    return false;
}

static bool guc_proc_qmp_handshake(AlchemistState *s, Error **errp)
{
    QDict *greeting, *reply, *cmd;

    greeting = guc_proc_qmp_recv(s, errp);
    if (!greeting) {
        return false;
    }
    qobject_unref(greeting);

    cmd = qdict_new();
    qdict_put_str(cmd, "execute", "qmp_capabilities");
    if (!guc_proc_qmp_send(s, cmd, errp)) {
        qobject_unref(cmd);
        return false;
    }
    qobject_unref(cmd);

    reply = guc_proc_qmp_recv(s, errp);
    if (!reply) {
        return false;
    }
    if (!qdict_haskey(reply, "return")) {
        error_setg(errp, "alchemist: satellite qmp_capabilities failed");
        qobject_unref(reply);
        return false;
    }
    qobject_unref(reply);
    return true;
}

bool alchemist_guc_proc_start(AlchemistState *s, Error **errp)
{
    g_autofree char *qemu_bin = find_qemu_binary();
    pid_t pid;

    s->guc_proc.pid = -1;
    s->guc_proc.qmp_fd = -1;

    if (!qemu_bin) {
        error_setg(errp, "alchemist: could not locate qemu-system-x86_64 "
                   "for the satellite GuC coprocessor process");
        return false;
    }

    s->guc_proc.qmp_path = g_strdup_printf("%s/alchemist-guc-qmp-%d-%p.sock",
                                            g_get_tmp_dir(), getpid(),
                                            (void *)s);
    unlink(s->guc_proc.qmp_path);

    pid = fork();
    if (pid < 0) {
        error_setg_errno(errp, errno,
                          "alchemist: could not fork satellite GuC process");
        g_free(s->guc_proc.qmp_path);
        s->guc_proc.qmp_path = NULL;
        return false;
    }

    if (pid == 0) {
        char *qmp_arg = g_strdup_printf("unix:%s,server=on,wait=off",
                                         s->guc_proc.qmp_path);
        char *args[] = {
            qemu_bin,
            (char *)"-machine", (char *)"none",
            (char *)"-accel", (char *)"tcg",
            (char *)"-nodefaults",
            /*
             * -machine none doesn't run the generic x86 possible-CPU-list/
             * APIC-ID machinery real machine types do, so "-cpu 486" alone
             * fails realize ("apic-id property was not initialized
             * properly") - an explicit CPU device with apic-id set is the
             * fix, confirmed against this exact build (see
             * docs/alchemist-bringup.md).
             */
            (char *)"-device", (char *)"486-x86_64-cpu,apic-id=0",
            (char *)"-m", (char *)"16",
            (char *)"-qmp", qmp_arg,
            (char *)"-nographic",
            (char *)"-no-reboot",
            (char *)"-run-with", (char *)"exit-with-parent=on",
            (char *)"-S",
            NULL,
        };

        close_fds_after_fork();
        execv(qemu_bin, args);
        _exit(1);
    }

    s->guc_proc.pid = pid;

    if (!guc_proc_connect_qmp(s, errp)) {
        alchemist_guc_proc_stop(s);
        return false;
    }

    if (!guc_proc_qmp_handshake(s, errp)) {
        alchemist_guc_proc_stop(s);
        return false;
    }

    return true;
}

void alchemist_guc_proc_stop(AlchemistState *s)
{
    int status, i;

    if (s->guc_proc.qmp_fd >= 0) {
        QDict *cmd = qdict_new();

        qdict_put_str(cmd, "execute", "quit");
        guc_proc_qmp_send(s, cmd, NULL);
        qobject_unref(cmd);
        close(s->guc_proc.qmp_fd);
        s->guc_proc.qmp_fd = -1;
    }

    if (s->guc_proc.pid > 0) {
        for (i = 0; i < ALCHEMIST_GUC_PROC_QUIT_RETRIES; i++) {
            if (waitpid(s->guc_proc.pid, &status, WNOHANG) == s->guc_proc.pid) {
                s->guc_proc.pid = -1;
                break;
            }
            g_usleep(ALCHEMIST_GUC_PROC_QUIT_DELAY_US);
        }
        if (s->guc_proc.pid > 0) {
            kill(s->guc_proc.pid, SIGKILL);
            waitpid(s->guc_proc.pid, &status, 0);
            s->guc_proc.pid = -1;
        }
    }

    if (s->guc_proc.qmp_path) {
        unlink(s->guc_proc.qmp_path);
        g_free(s->guc_proc.qmp_path);
        s->guc_proc.qmp_path = NULL;
    }
}
