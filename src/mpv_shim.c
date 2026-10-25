/*
 * libmpv-2.dll shim for Nuvio Desktop (mpv config / AnimeJaNai).
 *
 * Why this exists
 * ---------------
 * Nuvio's stock player_bridge.dll starts libmpv with `config=no`, so mpv never
 * reads mpv.conf, input.conf, scripts/ or shaders/. AnimeJaNai needs all four,
 * and a plain custom mpv.conf needs the same hook. Upstream Nuvio has no
 * setting for this. The alternative to this shim is patching Nuvio's C++
 * bridge and rebuilding it for every Nuvio release.
 *
 * This shim is a drop-in libmpv replacement that Nuvio loads through its
 * existing NUVIO_LIBMPV_PATH environment variable. It forwards the libmpv
 * entry points the bridge uses to the real library, plus a few
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
 * The real libmpv is found in this order:
 *
 *   1. NUVIO_MPV_REAL_LIBRARY, when set to a usable library (explicit override
 *      used by the AnimeJaNai installer). A stale value is ignored.
 *   2. The directory of the loaded player_bridge.dll. Nuvio extracts the
 *      bridge and its libmpv into the same content-hash cache directory, so
 *      this follows Nuvio updates without any fixed cache path.
 *   3. <shim>\..\libmpv\libmpv-2.dll (AnimeJaNai install layout).
 *   4. The newest directory under
 *      %LOCALAPPDATA%\Nuvio\Cache\native-player-bridge\windows\.
 *
 * Build (MSVC, x64):
 *     cl /nologo /LD /O2 /W3 mpv_shim.c /link /OUT:libmpv-2.dll user32.lib
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

#define SHIM_MAX_PATH 8192
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

static void shim_clear_exports(void)
{
    p_create = NULL;
    p_initialize = NULL;
    p_terminate_destroy = NULL;
    p_set_option = NULL;
    p_set_option_string = NULL;
    p_set_property = NULL;
    p_set_property_string = NULL;
    p_get_property = NULL;
    p_command = NULL;
    p_error_string = NULL;
    p_free = NULL;
    p_wait_event = NULL;
    p_wakeup = NULL;

    p_get_property_string = NULL;
    p_command_string = NULL;
    p_client_api_version = NULL;
    p_observe_property = NULL;
    p_unobserve_property = NULL;
    p_request_log_messages = NULL;
    p_set_wakeup_callback = NULL;
    p_free_node_contents = NULL;
}

static BOOL shim_join_path(wchar_t *out, size_t capacity, const wchar_t *directory,
                           const wchar_t *leaf)
{
    size_t directory_length = wcslen(directory);
    size_t leaf_length = wcslen(leaf);
    size_t total;

    while (directory_length > 0 &&
           (directory[directory_length - 1] == L'\\' || directory[directory_length - 1] == L'/')) {
        directory_length--;
    }

    total = directory_length + 1 + leaf_length + 1;
    if (total > capacity) {
        return FALSE;
    }

    memcpy(out, directory, directory_length * sizeof(wchar_t));
    out[directory_length] = L'\\';
    memcpy(out + directory_length + 1, leaf, (leaf_length + 1) * sizeof(wchar_t));
    return TRUE;
}

static BOOL shim_module_directory(HMODULE module, wchar_t *out, size_t capacity)
{
    wchar_t path[SHIM_MAX_PATH];
    wchar_t *slash;
    DWORD length = GetModuleFileNameW(module, path, SHIM_MAX_PATH);

    if (length == 0 || length >= SHIM_MAX_PATH) {
        return FALSE;
    }

    slash = wcsrchr(path, L'\\');
    if (!slash) {
        return FALSE;
    }
    *slash = 0;

    if (wcslen(path) + 1 > capacity) {
        return FALSE;
    }
    lstrcpynW(out, path, (int)capacity);
    return TRUE;
}

/* Nuvio extracts the player bridge and its libmpv into the same content-hash
 * directory, e.g. ...\native-player-bridge\windows\<hash>\player_bridge.dll.
 * The bridge's own directory is therefore the most reliable place to find the
 * libmpv that belongs to the Nuvio build currently running. */
static BOOL shim_bridge_candidate(wchar_t *out, size_t capacity)
{
    wchar_t directory[SHIM_MAX_PATH];
    HMODULE bridge = GetModuleHandleW(L"player_bridge.dll");

    if (bridge == NULL || bridge == g_self) {
        return FALSE;
    }
    if (!shim_module_directory(bridge, directory, SHIM_MAX_PATH)) {
        return FALSE;
    }
    return shim_join_path(out, capacity, directory, L"libmpv-2.dll");
}

/* AnimeJaNai layout: <install>\shim\libmpv-2.dll -> <install>\libmpv\libmpv-2.dll */
static BOOL shim_legacy_candidate(wchar_t *out, size_t capacity)
{
    wchar_t directory[SHIM_MAX_PATH];

    if (!shim_module_directory(g_self, directory, SHIM_MAX_PATH)) {
        return FALSE;
    }
    return shim_join_path(out, capacity, directory, L"..\\libmpv\\libmpv-2.dll");
}

/* Last resort when the bridge is not named player_bridge.dll (or the shim is
 * loaded by a test harness): pick the newest extracted native cache directory.
 * Nuvio keeps older content-hash directories around after an update. */
static BOOL shim_cache_candidate(wchar_t *out, size_t capacity)
{
    wchar_t local_app_data[SHIM_MAX_PATH];
    wchar_t root[SHIM_MAX_PATH];
    wchar_t pattern[SHIM_MAX_PATH];
    wchar_t best[SHIM_MAX_PATH];
    WIN32_FIND_DATAW item;
    HANDLE find;
    BOOL found = FALSE;
    ULARGE_INTEGER best_time;
    DWORD length;

    length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, SHIM_MAX_PATH);
    if (length == 0 || length >= SHIM_MAX_PATH) {
        return FALSE;
    }
    if (!shim_join_path(root, SHIM_MAX_PATH, local_app_data,
                        L"Nuvio\\Cache\\native-player-bridge\\windows")) {
        return FALSE;
    }
    if (!shim_join_path(pattern, SHIM_MAX_PATH, root, L"*")) {
        return FALSE;
    }

    find = FindFirstFileW(pattern, &item);
    if (find == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    do {
        wchar_t directory[SHIM_MAX_PATH];
        wchar_t candidate[SHIM_MAX_PATH];
        WIN32_FILE_ATTRIBUTE_DATA attributes;
        ULARGE_INTEGER stamp;

        if ((item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            continue;
        }
        if (wcscmp(item.cFileName, L".") == 0 || wcscmp(item.cFileName, L"..") == 0) {
            continue;
        }
        if (!shim_join_path(directory, SHIM_MAX_PATH, root, item.cFileName)) {
            continue;
        }
        if (!shim_join_path(candidate, SHIM_MAX_PATH, directory, L"libmpv-2.dll")) {
            continue;
        }
        if (!GetFileAttributesExW(candidate, GetFileExInfoStandard, &attributes)) {
            continue;
        }

        stamp.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
        stamp.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
        if (!found || stamp.QuadPart > best_time.QuadPart) {
            lstrcpynW(best, candidate, SHIM_MAX_PATH);
            best_time = stamp;
            found = TRUE;
        }
    } while (FindNextFileW(find, &item));

    FindClose(find);

    if (!found) {
        return FALSE;
    }
    lstrcpynW(out, best, (int)capacity);
    return TRUE;
}

static BOOL shim_bind_exports(void)
{
    BOOL ok = TRUE;

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

    if (!ok) {
        return FALSE;
    }

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

    return TRUE;
}

static BOOL shim_try_library(const wchar_t *path)
{
    wchar_t loaded_path[SHIM_MAX_PATH];
    DWORD length;
    HMODULE module;

    if (path == NULL || path[0] == 0) {
        return FALSE;
    }
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        return FALSE;
    }

    /* LOAD_WITH_ALTERED_SEARCH_PATH makes the real libmpv resolve its own
     * runtime DLLs (msvcp140, vcruntime140, ...) from the cache directory it
     * was extracted into, instead of relying on the process PATH. */
    module = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == NULL) {
        shim_log(L"LoadLibraryExW failed (%lu): %s", GetLastError(), path);
        return FALSE;
    }

    /* Never accept this shim (or another copy of it) as the real library. */
    if (module == g_self || GetProcAddress(module, "nuvio_mpv_shim_version") != NULL) {
        shim_log(L"ignoring shim module as real libmpv: %s", path);
        FreeLibrary(module);
        return FALSE;
    }

    length = GetModuleFileNameW(module, loaded_path, SHIM_MAX_PATH);
    if (length == 0 || length >= SHIM_MAX_PATH) {
        lstrcpynW(loaded_path, path, SHIM_MAX_PATH);
    }

    g_real = module;
    if (!shim_bind_exports()) {
        shim_log(L"%s does not look like libmpv; trying next candidate", loaded_path);
        shim_clear_exports();
        FreeLibrary(g_real);
        g_real = NULL;
        return FALSE;
    }

    lstrcpynW(g_real_path, loaded_path, SHIM_MAX_PATH);
    shim_log(L"using real libmpv: %s", g_real_path);
    return TRUE;
}

static BOOL WINAPI shim_resolve(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    wchar_t candidate[SHIM_MAX_PATH];
    DWORD length;

    (void)once;
    (void)parameter;
    (void)context;

    /* 1. Explicit override, used by the AnimeJaNai installer to point at its
     *    own libmpv build. A stale value falls through to auto-discovery. */
    length = GetEnvironmentVariableW(L"NUVIO_MPV_REAL_LIBRARY", candidate, SHIM_MAX_PATH);
    if (length > 0 && length < SHIM_MAX_PATH) {
        if (shim_try_library(candidate)) {
            return TRUE;
        }
        shim_log(L"NUVIO_MPV_REAL_LIBRARY was not usable; falling back to auto-discovery");
    }

    /* 2. The libmpv that Nuvio extracted next to the running bridge. This
     *    follows content-hash cache changes across Nuvio updates. */
    if (shim_bridge_candidate(candidate, SHIM_MAX_PATH) && shim_try_library(candidate)) {
        return TRUE;
    }

    /* 3. This repository's AnimeJaNai install layout. */
    if (shim_legacy_candidate(candidate, SHIM_MAX_PATH) && shim_try_library(candidate)) {
        return TRUE;
    }

    /* 4. Newest native cache directory, for harnesses and renamed bridges. */
    if (shim_cache_candidate(candidate, SHIM_MAX_PATH) && shim_try_library(candidate)) {
        return TRUE;
    }

    shim_log(L"could not load a usable real libmpv");
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

/* Identifies this DLL as the shim, so auto-discovery never loads a second copy
 * of the shim as if it were the real libmpv. Not part of the libmpv ABI. */
__declspec(dllexport) int nuvio_mpv_shim_version(void)
{
    return 2;
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
