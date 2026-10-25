/*
 * libmpv-2.dll shim for Nuvio Desktop + AnimeJaNai.
 *
 * Why this exists
 * ---------------
 * Nuvio's stock player_bridge.dll starts libmpv with `config=no`, so mpv never
 * reads mpv.conf, input.conf, scripts/ or shaders/. AnimeJaNai needs all four.
 * Upstream Nuvio has no setting for this. The alternative to this shim is
 * patching Nuvio's C++ bridge and rebuilding it for every Nuvio release.
 *
 * This shim is a drop-in libmpv replacement that Nuvio loads through its
 * existing NUVIO_LIBMPV_PATH environment variable. It forwards the 13 libmpv
 * entry points the bridge uses to the real (AnimeJaNai) libmpv, plus a few
 * forward-compatibility exports, and rewrites one call on the way through:
 *
 *     mpv_set_option_string(mpv, "config", "no")
 *         -> mpv_set_option_string(mpv, "config-dir", $NUVIO_MPV_CONFIG_DIR)
 *         -> mpv_set_option_string(mpv, "config",     "yes")
 *
 * Unset NUVIO_MPV_CONFIG_DIR and every call is forwarded untouched, so a stock
 * Nuvio install behaves exactly as before. This shim does not depend on any
 * Nuvio internals, so it survives Nuvio updates; only the libmpv ABI matters.
 *
 * Build (MSVC, x64):
 *     cl /nologo /LD /O2 /W3 mpv_shim.c /link /OUT:libmpv-2.dll user32.lib
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <string.h>

#define SHIM_MAX_PATH 32768
#define SHIM_MPV_FORMAT_FLAG 3
#define SHIM_MPV_ERROR_LOADING_FAILED (-14)

typedef struct mpv_handle mpv_handle;

/* Layout matches mpv/client.h; only the pointer is ever handed back. */
typedef struct shim_mpv_event {
    int event_id;
    int error;
    unsigned long long reply_userdata;
    void *data;
} shim_mpv_event;

typedef mpv_handle *(*shim_create_fn)(void);
typedef int (*shim_initialize_fn)(mpv_handle *);
typedef void (*shim_terminate_destroy_fn)(mpv_handle *);
typedef int (*shim_set_option_fn)(mpv_handle *, const char *, int, void *);
typedef int (*shim_set_option_string_fn)(mpv_handle *, const char *, const char *);
typedef int (*shim_set_property_fn)(mpv_handle *, const char *, int, void *);
typedef int (*shim_set_property_string_fn)(mpv_handle *, const char *, const char *);
typedef int (*shim_get_property_fn)(mpv_handle *, const char *, int, void *);
typedef int (*shim_command_fn)(mpv_handle *, const char **);
typedef const char *(*shim_error_string_fn)(int);
typedef void (*shim_free_fn)(void *);
typedef shim_mpv_event *(*shim_wait_event_fn)(mpv_handle *, double);
typedef void (*shim_wakeup_fn)(mpv_handle *);

/* Not used by Nuvio today. Exported so that a future bridge which asks libmpv
 * for one of them still loads; each is resolved optionally and reports a
 * failure instead of taking the process down if the real library lacks it. */
typedef char *(*shim_get_property_string_fn)(mpv_handle *, const char *);
typedef int (*shim_command_string_fn)(mpv_handle *, const char *);
typedef unsigned long (*shim_client_api_version_fn)(void);
typedef int (*shim_observe_property_fn)(mpv_handle *, unsigned long long, const char *, int);
typedef int (*shim_unobserve_property_fn)(mpv_handle *, unsigned long long);
typedef int (*shim_request_log_messages_fn)(mpv_handle *, const char *);
typedef void (*shim_set_wakeup_callback_fn)(mpv_handle *, void (*)(void *), void *);
typedef void (*shim_free_node_contents_fn)(void *);

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static HMODULE g_self;
static HMODULE g_real;
static wchar_t g_real_path[SHIM_MAX_PATH];

static shim_create_fn p_create;
static shim_initialize_fn p_initialize;
static shim_terminate_destroy_fn p_terminate_destroy;
static shim_set_option_fn p_set_option;
static shim_set_option_string_fn p_set_option_string;
static shim_set_property_fn p_set_property;
static shim_set_property_string_fn p_set_property_string;
static shim_get_property_fn p_get_property;
static shim_command_fn p_command;
static shim_error_string_fn p_error_string;
static shim_free_fn p_free;
static shim_wait_event_fn p_wait_event;
static shim_wakeup_fn p_wakeup;

static shim_get_property_string_fn p_get_property_string;
static shim_command_string_fn p_command_string;
static shim_client_api_version_fn p_client_api_version;
static shim_observe_property_fn p_observe_property;
static shim_unobserve_property_fn p_unobserve_property;
static shim_request_log_messages_fn p_request_log_messages;
static shim_set_wakeup_callback_fn p_set_wakeup_callback;
static shim_free_node_contents_fn p_free_node_contents;

/* Nuvio drives one mpv handle at a time (guarded by its own mutex), so the
 * "did this handle already go through the config path" bookkeeping can be a
 * single slot rather than a map. */
static mpv_handle *g_last_handle;
static BOOL g_last_config_handled;

static void shim_log(const wchar_t *fmt, ...)
{
    wchar_t message[1024];
    va_list args;

    va_start(args, fmt);
    wvsprintfW(message, fmt, args);
    va_end(args);

    OutputDebugStringW(L"[NuvioAnimeJaNai] ");
    OutputDebugStringW(message);
    OutputDebugStringW(L"\n");
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = (HMODULE)instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

static BOOL WINAPI shim_resolve(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    wchar_t path[SHIM_MAX_PATH];
    DWORD length;
    BOOL ok = TRUE;

    (void)once;
    (void)parameter;
    (void)context;

    length = GetEnvironmentVariableW(L"NUVIO_MPV_REAL_LIBRARY", path, SHIM_MAX_PATH);
    if (length == 0 || length >= SHIM_MAX_PATH) {
        wchar_t self_path[SHIM_MAX_PATH];
        DWORD self_length = GetModuleFileNameW(g_self, self_path, SHIM_MAX_PATH);
        wchar_t *slash;

        if (self_length == 0 || self_length >= SHIM_MAX_PATH) {
            shim_log(L"cannot determine shim directory (error %lu)", GetLastError());
            return TRUE;
        }
        slash = wcsrchr(self_path, L'\\');
        if (!slash) {
            shim_log(L"cannot split shim path");
            return TRUE;
        }
        *slash = 0;
        /* <install>\shim\libmpv-2.dll  ->  <install>\libmpv\libmpv-2.dll */
        wsprintfW(path, L"%s\\..\\libmpv\\libmpv-2.dll", self_path);
    }

    g_real = LoadLibraryW(path);
    if (!g_real) {
        shim_log(L"LoadLibraryW failed (%lu): %s", GetLastError(), path);
        return TRUE;
    }

    wcscpy_s(g_real_path, SHIM_MAX_PATH, path);

#define SHIM_RESOLVE(field, symbol)                                              \
    do {                                                                         \
        *(FARPROC *)&field = GetProcAddress(g_real, symbol);                      \
        if (!field) {                                                            \
            shim_log(L"missing export: %S", symbol);                              \
            ok = FALSE;                                                          \
        }                                                                        \
    } while (0)

    SHIM_RESOLVE(p_create, "mpv_create");
    SHIM_RESOLVE(p_initialize, "mpv_initialize");
    SHIM_RESOLVE(p_terminate_destroy, "mpv_terminate_destroy");
    SHIM_RESOLVE(p_set_option, "mpv_set_option");
    SHIM_RESOLVE(p_set_option_string, "mpv_set_option_string");
    SHIM_RESOLVE(p_set_property, "mpv_set_property");
    SHIM_RESOLVE(p_set_property_string, "mpv_set_property_string");
    SHIM_RESOLVE(p_get_property, "mpv_get_property");
    SHIM_RESOLVE(p_command, "mpv_command");
    SHIM_RESOLVE(p_error_string, "mpv_error_string");
    SHIM_RESOLVE(p_free, "mpv_free");
    SHIM_RESOLVE(p_wait_event, "mpv_wait_event");
    SHIM_RESOLVE(p_wakeup, "mpv_wakeup");

#undef SHIM_RESOLVE

#define SHIM_RESOLVE_OPTIONAL(field, symbol)                                      \
    do {                                                                         \
        *(FARPROC *)&field = GetProcAddress(g_real, symbol);                      \
        if (!field) {                                                            \
            shim_log(L"optional export missing: %S", symbol);                     \
        }                                                                        \
    } while (0)

    SHIM_RESOLVE_OPTIONAL(p_get_property_string, "mpv_get_property_string");
    SHIM_RESOLVE_OPTIONAL(p_command_string, "mpv_command_string");
    SHIM_RESOLVE_OPTIONAL(p_client_api_version, "mpv_client_api_version");
    SHIM_RESOLVE_OPTIONAL(p_observe_property, "mpv_observe_property");
    SHIM_RESOLVE_OPTIONAL(p_unobserve_property, "mpv_unobserve_property");
    SHIM_RESOLVE_OPTIONAL(p_request_log_messages, "mpv_request_log_messages");
    SHIM_RESOLVE_OPTIONAL(p_set_wakeup_callback, "mpv_set_wakeup_callback");
    SHIM_RESOLVE_OPTIONAL(p_free_node_contents, "mpv_free_node_contents");

#undef SHIM_RESOLVE_OPTIONAL

    if (!ok) {
        shim_log(L"%s does not look like libmpv; refusing to continue", path);
        FreeLibrary(g_real);
        g_real = NULL;
        return TRUE;
    }

    shim_log(L"using real libmpv: %s", path);
    return TRUE;
}

static void shim_ensure_resolved(void)
{
    InitOnceExecuteOnce(&g_once, shim_resolve, NULL, NULL);
}

/*
 * Returns 1 when the config dir was applied, 0 when NUVIO_MPV_CONFIG_DIR is
 * unset (caller should forward the original call), -1 when it is set but
 * unusable (caller should fall back to config=no and log).
 */
static int shim_apply_config(mpv_handle *handle)
{
    wchar_t directory[SHIM_MAX_PATH];
    char utf8[SHIM_MAX_PATH * 3];
    DWORD length;
    DWORD attributes;
    int path_result;
    int config_result;
    int utf8_length;

    length = GetEnvironmentVariableW(L"NUVIO_MPV_CONFIG_DIR", directory, SHIM_MAX_PATH);
    if (length == 0 || length >= SHIM_MAX_PATH) {
        return 0;
    }

    attributes = GetFileAttributesW(directory);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        shim_log(L"NUVIO_MPV_CONFIG_DIR is not a directory: %s", directory);
        return -1;
    }

    utf8_length = WideCharToMultiByte(
        CP_UTF8, 0, directory, (int)length, utf8, (int)sizeof(utf8) - 1, NULL, NULL);
    if (utf8_length <= 0) {
        shim_log(L"NUVIO_MPV_CONFIG_DIR is not valid UTF-8-convertible text");
        return -1;
    }
    utf8[utf8_length] = 0;

    path_result = p_set_option_string(handle, "config-dir", utf8);
    config_result = p_set_option_string(handle, "config", "yes");
    if (path_result < 0 || config_result < 0) {
        shim_log(L"could not enable config (config-dir rc=%d, config rc=%d)",
                 path_result, config_result);
        return -1;
    }

    shim_log(L"mpv config enabled: %s", directory);
    return 1;
}

static int shim_is_disabled_config_value(const char *value)
{
    return value != NULL &&
           (strcmp(value, "no") == 0 || strcmp(value, "false") == 0 || strcmp(value, "0") == 0);
}

__declspec(dllexport) mpv_handle *mpv_create(void)
{
    mpv_handle *handle;

    shim_ensure_resolved();
    if (!p_create) {
        return NULL;
    }

    handle = p_create();
    if (handle) {
        g_last_handle = handle;
        g_last_config_handled = FALSE;
    }
    return handle;
}

__declspec(dllexport) int mpv_initialize(mpv_handle *handle)
{
    shim_ensure_resolved();
    if (!p_initialize) {
        return SHIM_MPV_ERROR_LOADING_FAILED;
    }

    /* Safety net for a future Nuvio that stops setting config=no itself: if
     * the config dir was requested but never applied, apply it now. */
    if (handle != NULL && handle == g_last_handle && !g_last_config_handled) {
        if (shim_apply_config(handle) > 0) {
            g_last_config_handled = TRUE;
        }
    }

    return p_initialize(handle);
}

__declspec(dllexport) void mpv_terminate_destroy(mpv_handle *handle)
{
    shim_ensure_resolved();
    if (!p_terminate_destroy) {
        return;
    }
    if (handle == g_last_handle) {
        g_last_handle = NULL;
        g_last_config_handled = FALSE;
    }
    p_terminate_destroy(handle);
}

__declspec(dllexport) int mpv_set_option(mpv_handle *handle, const char *name,
                                         int format, void *data)
{
    shim_ensure_resolved();
    if (!p_set_option) {
        return SHIM_MPV_ERROR_LOADING_FAILED;
    }

    if (name != NULL && data != NULL && strcmp(name, "config") == 0 &&
        format == SHIM_MPV_FORMAT_FLAG && *(int *)data == 0) {
        int applied = shim_apply_config(handle);
        if (applied > 0) {
            if (handle == g_last_handle) {
                g_last_config_handled = TRUE;
            }
            return 0;
        }
    }

    return p_set_option(handle, name, format, data);
}

__declspec(dllexport) int mpv_set_option_string(mpv_handle *handle, const char *name,
                                                const char *value)
{
    shim_ensure_resolved();
    if (!p_set_option_string) {
        return SHIM_MPV_ERROR_LOADING_FAILED;
    }

    if (name != NULL && strcmp(name, "config") == 0 &&
        shim_is_disabled_config_value(value)) {
        int applied = shim_apply_config(handle);
        if (applied > 0) {
            if (handle == g_last_handle) {
                g_last_config_handled = TRUE;
            }
            return 0;
        }
        /* applied == 0: no config dir requested, keep stock behaviour.
         * applied == -1: unusable config dir, already logged; keep stock too. */
    }

    return p_set_option_string(handle, name, value);
}

__declspec(dllexport) int mpv_set_property(mpv_handle *handle, const char *name,
                                           int format, void *data)
{
    shim_ensure_resolved();
    if (!p_set_property) {
        return SHIM_MPV_ERROR_LOADING_FAILED;
    }
    return p_set_property(handle, name, format, data);
}

__declspec(dllexport) int mpv_set_property_string(mpv_handle *handle, const char *name,
                                                  const char *value)
{
    shim_ensure_resolved();
    if (!p_set_property_string) {
        return SHIM_MPV_ERROR_LOADING_FAILED;
    }
    return p_set_property_string(handle, name, value);
}

__declspec(dllexport) int mpv_get_property(mpv_handle *handle, const char *name,
                                           int format, void *data)
{
    shim_ensure_resolved();
    if (!p_get_property) {
        return SHIM_MPV_ERROR_LOADING_FAILED;
    }
    return p_get_property(handle, name, format, data);
}

__declspec(dllexport) int mpv_command(mpv_handle *handle, const char **args)
{
    shim_ensure_resolved();
    if (!p_command) {
        return SHIM_MPV_ERROR_LOADING_FAILED;
    }
    return p_command(handle, args);
}

__declspec(dllexport) const char *mpv_error_string(int error)
{
    shim_ensure_resolved();
    if (!p_error_string) {
        return "libmpv shim: real library not loaded";
    }
    return p_error_string(error);
}

__declspec(dllexport) void mpv_free(void *data)
{
    shim_ensure_resolved();
    if (p_free) {
        p_free(data);
    }
}

__declspec(dllexport) shim_mpv_event *mpv_wait_event(mpv_handle *handle, double timeout)
{
    shim_ensure_resolved();
    if (!p_wait_event) {
        return NULL;
    }
    return p_wait_event(handle, timeout);
}

__declspec(dllexport) void mpv_wakeup(mpv_handle *handle)
{
    shim_ensure_resolved();
    if (p_wakeup) {
        p_wakeup(handle);
    }
}

/* -------------------------------------------------------------------------
 * Forward-compatibility exports.
 *
 * Nuvio 0.1.27 .. 0.1.32 load exactly the 13 symbols above. These extra ones
 * cover the obvious next candidates so a future bridge that starts using them
 * still finds a working libmpv behind the shim, instead of failing to load.
 * ---------------------------------------------------------------------- */

__declspec(dllexport) char *mpv_get_property_string(mpv_handle *handle, const char *name)
{
    shim_ensure_resolved();
    if (!p_get_property_string) {
        return NULL;
    }
    return p_get_property_string(handle, name);
}

__declspec(dllexport) int mpv_command_string(mpv_handle *handle, const char *args)
{
    shim_ensure_resolved();
    if (!p_command_string) {
        return SHIM_MPV_ERROR_LOADING_FAILED;
    }
    return p_command_string(handle, args);
}

__declspec(dllexport) unsigned long mpv_client_api_version(void)
{
    shim_ensure_resolved();
    if (!p_client_api_version) {
        return 0;
    }
    return p_client_api_version();
}

__declspec(dllexport) int mpv_observe_property(mpv_handle *handle, unsigned long long reply_userdata,
                                               const char *name, int format)
{
    shim_ensure_resolved();
    if (!p_observe_property) {
        return SHIM_MPV_ERROR_LOADING_FAILED;
    }
    return p_observe_property(handle, reply_userdata, name, format);
}

__declspec(dllexport) int mpv_unobserve_property(mpv_handle *handle, unsigned long long reply_userdata)
{
    shim_ensure_resolved();
    if (!p_unobserve_property) {
        return SHIM_MPV_ERROR_LOADING_FAILED;
    }
    return p_unobserve_property(handle, reply_userdata);
}

__declspec(dllexport) int mpv_request_log_messages(mpv_handle *handle, const char *min_level)
{
    shim_ensure_resolved();
    if (!p_request_log_messages) {
        return SHIM_MPV_ERROR_LOADING_FAILED;
    }
    return p_request_log_messages(handle, min_level);
}

__declspec(dllexport) void mpv_set_wakeup_callback(mpv_handle *handle,
                                                   void (*callback)(void *), void *context)
{
    shim_ensure_resolved();
    if (p_set_wakeup_callback) {
        p_set_wakeup_callback(handle, callback, context);
    }
}

__declspec(dllexport) void mpv_free_node_contents(void *node)
{
    shim_ensure_resolved();
    if (p_free_node_contents) {
        p_free_node_contents(node);
    }
}
