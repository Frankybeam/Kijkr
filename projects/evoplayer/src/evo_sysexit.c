/*
 * Module: evo_sysexit - see include/evo_sysexit.h.
 */
#include "evo_sysexit.h"
#include "evo_boot_log.h"

#include <stddef.h>

#if defined(EVO_TARGET_PS5)
#include <unistd.h>

int sceKernelDlsym(int moduleHandle, const char *symbol, void **addrOut);
int sceKernelLoadStartModule(const char *name, size_t argc, const void *argv,
                             unsigned int flags, void *opt, int *res);

typedef int (*fn_load_exec)(const char *path, char *const argv[]);
typedef int (*fn_get_app_id)(void);
typedef int (*fn_kill_app)(int app_id, int how, int a, int b);
typedef int (*fn_lnc_kill)(int app_id);

static void *sys_fn(int mod, const char *name)
{
    void *fn = NULL;
    if (mod > 0 && sceKernelDlsym(mod, name, &fn) == 0 && fn) return fn;
    evo_boot_log("quit: %s not there", name);
    return NULL;
}

/*
 * Two more ways, in order, each only if the firmware has it:
 *   1. sceSystemServiceLoadExec("exit") - how a PS4 app closes itself;
 *   2. the system closing this app by its id (KillApp / LncUtil);
 * Called after Application has released everything and asked LoadExec
 * directly; the PS5 goes back to its home screen when the app is gone.
 */
int evo_system_exit(void)
{
    int res = 0;
    int mod = sceKernelLoadStartModule("libSceSystemService.sprx", 0, NULL, 0, NULL, &res);
    if (mod <= 0)
        mod = sceKernelLoadStartModule("/system/common/lib/libSceSystemService.sprx", 0, NULL, 0, NULL, &res);
    evo_boot_log("quit: libSceSystemService handle=%#x", (unsigned)mod);

    fn_load_exec load_exec = (fn_load_exec)sys_fn(mod, "sceSystemServiceLoadExec");
    if (load_exec) {
        evo_boot_log("quit: 1/3 asking the system to close Kijkr (LoadExec)");
        evo_boot_log_flush();
        const int rc = load_exec("exit", NULL);
        evo_boot_log("quit: LoadExec returned %#x", (unsigned)rc);
        usleep(300000);
    }

    fn_get_app_id get_id = (fn_get_app_id)sys_fn(mod, "sceSystemServiceGetAppIdOfRunningBigApp");
    if (!get_id) get_id = (fn_get_app_id)sys_fn(mod, "sceSystemServiceGetAppIdOfBigApp");
    const int app_id = get_id ? get_id() : -1;
    evo_boot_log("quit: own app id %#x", (unsigned)app_id);
    if (app_id > 0) {
        fn_kill_app kill_app = (fn_kill_app)sys_fn(mod, "sceSystemServiceKillApp");
        if (kill_app) {
            evo_boot_log("quit: 2/3 asking the system to close app %#x (KillApp)", (unsigned)app_id);
            evo_boot_log_flush();
            const int rc = kill_app(app_id, -1, 0, 0);
            evo_boot_log("quit: KillApp returned %#x", (unsigned)rc);
            usleep(300000);
        }
        fn_lnc_kill lnc_kill = (fn_lnc_kill)sys_fn(mod, "sceLncUtilKillApp");
        if (lnc_kill) {
            evo_boot_log("quit: 2/3 asking the launcher to close app %#x (LncUtilKillApp)", (unsigned)app_id);
            evo_boot_log_flush();
            const int rc = lnc_kill(app_id);
            evo_boot_log("quit: LncUtilKillApp returned %#x", (unsigned)rc);
            usleep(300000);
        }
    }

    /* Not _exit(): like returning from main, ending a native title from
     * inside crashes it (ProsperoStore notes the same) - parking is safer. */
    evo_boot_log("quit: the system did not close Kijkr - close it from the switcher");
    evo_boot_log_flush();
    return -1;
}
#else
int evo_system_exit(void)
{
    return -1;
}
#endif
