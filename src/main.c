/* Enable rand_s() on MSVC — must be defined before any CRT include. Backed
 * by RtlGenRandom; gives cryptographic-quality randomness for the per-launch
 * bridge auth token. */
#define _CRT_RAND_S

#include "linux/detect.h"
#include "bridge/bridge.h"
#include "bridge/http_bridge.h"
#include "bridge/service.h"
#include "bridge/provision.h"
#include "bridge/json_escape.h"
#include "bridge/shell_escape.h"
#include "compat.h"
#ifdef HAVE_WEBVIEW
#include "gui/gui.h"
#include "webview.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(_WIN32) && defined(HAVE_WEBVIEW)
/* DWM dark-title-bar + window icon helper.
 * Declared inline so we don't have to pull in dwmapi.h (version-dependent);
 * we link dwmapi.lib via CMakeLists.txt. DWMWA_USE_IMMERSIVE_DARK_MODE is
 * attribute 20 on Windows 10 1903+ / 11, and 19 on Windows 10 1809-1903 —
 * we try both and ignore failures, which is a no-op on unsupported versions. */
WINBASEAPI HRESULT WINAPI DwmSetWindowAttribute(HWND, DWORD, LPCVOID, DWORD);

static void apply_window_style(webview_t w, const char *exe_dir, const char *app_name) {
    if (!w) return;
    HWND hwnd = (HWND)webview_get_window(w);
    if (!hwnd) return;

    /* Dark title bar (Windows 10 1809+) */
    BOOL use_dark = TRUE;
    (void)DwmSetWindowAttribute(hwnd, 20, &use_dark, sizeof(use_dark));
    (void)DwmSetWindowAttribute(hwnd, 19, &use_dark, sizeof(use_dark));

    /* Icon lookup: search <exe_dir>\ for app.ico, then <safe_name>.ico
     * (both original and lowercased), so a per-app Brave.ico/brave.ico lands
     * in the top-left corner and taskbar entry automatically. */
    if (!exe_dir || !exe_dir[0]) return;

    char ico_path[MAX_PATH];
    snprintf(ico_path, sizeof(ico_path), "%s\\app.ico", exe_dir);
    BOOL found = (GetFileAttributesA(ico_path) != INVALID_FILE_ATTRIBUTES);

    if (!found && app_name && app_name[0]) {
        char safe[128];
        size_t j = 0;
        for (size_t i = 0; app_name[i] && j < sizeof(safe) - 1; i++) {
            char c = app_name[i];
            if (c == ' ' || c == '/' || c == '\\') c = '_';
            safe[j++] = c;
        }
        safe[j] = '\0';

        snprintf(ico_path, sizeof(ico_path), "%s\\%s.ico", exe_dir, safe);
        found = (GetFileAttributesA(ico_path) != INVALID_FILE_ATTRIBUTES);

        if (!found) {
            for (size_t i = 0; safe[i]; i++)
                if (safe[i] >= 'A' && safe[i] <= 'Z') safe[i] = (char)(safe[i] + 32);
            snprintf(ico_path, sizeof(ico_path), "%s\\%s.ico", exe_dir, safe);
            found = (GetFileAttributesA(ico_path) != INVALID_FILE_ATTRIBUTES);
        }
    }

    if (!found) return;

    wchar_t ico_w[MAX_PATH];
    if (MultiByteToWideChar(CP_UTF8, 0, ico_path, -1, ico_w, MAX_PATH) <= 0)
        return;

    HICON ibig = (HICON)LoadImageW(NULL, ico_w, IMAGE_ICON,
                                   GetSystemMetrics(SM_CXICON),
                                   GetSystemMetrics(SM_CYICON),
                                   LR_LOADFROMFILE | LR_DEFAULTCOLOR);
    HICON ismall = (HICON)LoadImageW(NULL, ico_w, IMAGE_ICON,
                                     GetSystemMetrics(SM_CXSMICON),
                                     GetSystemMetrics(SM_CYSMICON),
                                     LR_LOADFROMFILE | LR_DEFAULTCOLOR);
    if (ibig)   SendMessageW(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)ibig);
    if (ismall) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)ismall);
}
#endif /* _WIN32 && HAVE_WEBVIEW */

/* ======================================================================
 * app.json support — zero-code app configuration
 *
 * If an app.json file exists next to the executable, it is loaded and
 * used to configure the application automatically. No C code needed.
 *
 * Example app.json:
 * {
 *     "name": "My AI App",
 *     "repo": "https://github.com/user/project",
 *     "deps": "python, git",
 *     "setup": "pip install -r requirements.txt",
 *     "start": "python3 app.py --port 7860",
 *     "port": 7860,
 *     "terminal": true
 * }
 * ====================================================================== */
typedef struct {
    char name[256];    /* short identifier — used to derive the WSL distro name */
    char title[256];   /* optional display title for the window/header; falls
                          back to `name` if empty. Keeps display separate from
                          distro identity so renaming the window doesn't
                          rename the WSL distro. */
    char repo[1024];
    char deps[1024];   /* comma-separated: "python,node,base" */
    char setup[2048];
    char start[2048];
    char distro[256];
    int  port;
    int  bridge_port;  /* 0 = auto (port + 1000) */
    int  width;
    int  height;
    int  terminal;     /* 1 = show terminal panel for users */
    int  snapshot;     /* 1 = export rootfs snapshot after setup */
    int  loaded;
} app_config_t;

/* Minimal flat-key JSON parser. Limitations:
 * - Only extracts top-level keys (no nested object traversal).
 * - Does not handle JSON arrays as values.
 * - String values must not contain escaped quotes adjacent to the key.
 * - Intended only for the simple app.json schema; not a general-purpose parser.
 *
 * Find a key pattern in JSON only when outside of string values.
 * Prevents matching "port" inside "description": "set port here". */
static const char *find_key_outside_strings(const char *json, const char *pattern,
                                            size_t plen) {
    int in_string = 0;
    for (const char *p = json; *p; p++) {
        if (in_string) {
            if (*p == '\\' && p[1]) { p++; continue; }  /* skip escaped char */
            if (*p == '"') in_string = 0;
            continue;
        }
        /* Outside any string */
        if (*p == '"' && strncmp(p, pattern, plen) == 0)
            return p;  /* found key at top level */
        if (*p == '"') in_string = 1;  /* entering a value string */
    }
    return NULL;
}

static int json_extract(const char *json, const char *key,
                        char *buf, int buf_size) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    size_t plen = strlen(pattern);
    const char *p = find_key_outside_strings(json, pattern, plen);
    if (!p) return 0;
    p += plen;
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && i < buf_size - 1) {
            if (*p == '\\' && p[1]) { p++; buf[i++] = *p++; continue; }
            if (*p == '"') break;
            buf[i++] = *p++;
        }
        buf[i] = '\0';
        return 1;
    }
    /* Number */
    int i = 0;
    while (*p && *p != ',' && *p != '}' && *p != ' ' && i < buf_size - 1)
        buf[i++] = *p++;
    buf[i] = '\0';
    return 1;
}

/* Forward declaration — used by invalidate_stale_snapshot below */
static void make_distro_name(const char *app_name, char *out, size_t out_size);

/* ── Snapshot Invalidation ─────────────────────────────────────
 * Compute a hash of the setup+deps+repo fields from app.json.
 * If the hash doesn't match the stored hash next to rootfs.tar.gz,
 * delete the stale snapshot so the app rebuilds from scratch.
 * This handles cases like switching git repos or changing cmake flags. */
static unsigned long _hash_djb2(const char *str) {
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*str++))
        h = ((h << 5) + h) + c;
    return h;
}

static void invalidate_stale_snapshot(const app_config_t *app, const char *exe_dir) {
#ifdef _WIN32
    if (!app->snapshot) {
        return;
    }

    /* Build a hash from the fields that affect the built environment */
    char hashable[8192];
    snprintf(hashable, sizeof(hashable), "%s|%s|%s", app->deps, app->setup, app->repo);
    unsigned long hash = _hash_djb2(hashable);

    char hash_path[MAX_PATH];
    snprintf(hash_path, sizeof(hash_path), "%s\\linux\\rootfs.setup_hash", exe_dir);

    char rootfs_path[MAX_PATH];
    snprintf(rootfs_path, sizeof(rootfs_path), "%s\\linux\\rootfs.tar.gz", exe_dir);

    /* If no snapshot exists, nothing to invalidate */
    if (GetFileAttributesA(rootfs_path) == INVALID_FILE_ATTRIBUTES)
        return;

    /* Read existing hash */
    FILE *hf = fopen(hash_path, "r");
    if (hf) {
        char stored[32] = "";
        if (fgets(stored, sizeof(stored), hf)) {
            unsigned long old_hash = strtoul(stored, NULL, 16);
            fclose(hf);
            if (old_hash == hash)
                return;  /* hash matches — snapshot is current */
        } else {
            fclose(hf);
        }
    }

    /* Hash mismatch or no hash file — snapshot is stale.
     * SAFE APPROACH: only delete the snapshot tarball so setup re-runs,
     * but NEVER unregister the distro. Unregistering destroys the entire
     * ext4 filesystem including user data (models, configs, etc.).
     * The setup script is designed to be incremental — it will detect
     * what is already installed and only rebuild what changed. */
    printf("[snapshot] app.json setup changed — will re-run setup on next start.\n");
    printf("[snapshot] (Distro preserved — only snapshot invalidated.)\n");
    DeleteFileA(rootfs_path);

    /* Update the hash file so we don't re-trigger on every start.
     * Setup will run because the distro was imported without deps,
     * or setup.sh handles incremental rebuilds. */
    {
        char hash_str[32];
        snprintf(hash_str, sizeof(hash_str), "%lx", hash);
        FILE *wf = fopen(hash_path, "w");
        if (wf) {
            fputs(hash_str, wf);
            fclose(wf);
        }
    }
#else
    (void)app; (void)exe_dir;
#endif
}

static void save_setup_hash(const app_config_t *app, const char *exe_dir) {
#ifdef _WIN32
    char hashable[8192];
    snprintf(hashable, sizeof(hashable), "%s|%s|%s", app->deps, app->setup, app->repo);
    unsigned long hash = _hash_djb2(hashable);

    char hash_path[MAX_PATH];
    snprintf(hash_path, sizeof(hash_path), "%s\\linux\\rootfs.setup_hash", exe_dir);

    FILE *hf = fopen(hash_path, "w");
    if (hf) {
        fprintf(hf, "%lx", hash);
        fclose(hf);
    }
#else
    (void)app; (void)exe_dir;
#endif
}

static int load_app_json(app_config_t *app) {
    memset(app, 0, sizeof(*app));
    app->port = 7860;
    app->width = 1100;
    app->height = 750;
    app->snapshot = 1;
    strncpy(app->distro, "linux-template", sizeof(app->distro) - 1);

    /* Find app.json next to our executable */
    char path[MAX_PATH];
#ifdef _WIN32
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *s = strrchr(path, '\\');
    if (s) *s = '\0';
    strncat(path, "\\app.json", MAX_PATH - strlen(path) - 1);
#else
    snprintf(path, sizeof(path), "app.json");
#endif

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 65536) { fclose(f); return 0; }

    char *json = (char *)malloc((size_t)sz + 1);
    if (!json) { fclose(f); return 0; }
    size_t nread = fread(json, 1, (size_t)sz, f);
    json[nread] = '\0';
    fclose(f);

    json_extract(json, "name",  app->name,  sizeof(app->name));
    json_extract(json, "title", app->title, sizeof(app->title));
    json_extract(json, "repo",  app->repo,  sizeof(app->repo));
    json_extract(json, "deps", app->deps, sizeof(app->deps));
    json_extract(json, "setup", app->setup, sizeof(app->setup));
    json_extract(json, "start", app->start, sizeof(app->start));
    json_extract(json, "distro", app->distro, sizeof(app->distro));

    char tmp[32];
    if (json_extract(json, "port", tmp, sizeof(tmp))) app->port = atoi(tmp);
    if (json_extract(json, "bridge_port", tmp, sizeof(tmp))) app->bridge_port = atoi(tmp);
    if (json_extract(json, "width", tmp, sizeof(tmp))) app->width = atoi(tmp);
    if (json_extract(json, "height", tmp, sizeof(tmp))) app->height = atoi(tmp);
    if (json_extract(json, "terminal", tmp, sizeof(tmp)))
        app->terminal = (strcmp(tmp, "true") == 0 || atoi(tmp) == 1);
    if (json_extract(json, "snapshot", tmp, sizeof(tmp)))
        app->snapshot = !(strcmp(tmp, "false") == 0 || atoi(tmp) == 0);

    free(json);

    if (!app->name[0]) strncpy(app->name, "Linux App", sizeof(app->name) - 1);
    app->loaded = 1;
    return 1;
}

/* Generate a per-app WSL distro name from the app name.
 * Returns "linbox-<sanitized_name>" in the provided buffer. */
static void make_distro_name(const char *app_name, char *out, size_t out_size) {
    char safe[128];
    int j = 0;
    for (int i = 0; app_name[i] && j < (int)sizeof(safe) - 1; i++) {
        char c = app_name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_')
            safe[j++] = c;
        else if (c == ' ')
            safe[j++] = '_';
    }
    safe[j] = '\0';
    if (!safe[0]) strncpy(safe, "app", sizeof(safe));
    snprintf(out, out_size, "linbox-%s", safe);
}

/* Forward declaration — defined below, used in both app.json GUI and setup dialog */
#ifdef HAVE_WEBVIEW
static void on_close_app(const char *seq, const char *req, void *arg);
#endif

#if defined(HAVE_WEBVIEW) && (defined(_WIN32) || defined(__APPLE__))
/* ======================================================================
 * app.json WebView GUI — auto-generated UI for JSON-configured apps
 * Uses compat.h macros for cross-platform threading/timing.
 * ====================================================================== */
static const char *APP_JSON_HTML_TEMPLATE =
"<!DOCTYPE html><html><head><meta charset='utf-8'>\n"
"<title>%s</title>\n"
"<style>\n"
"*{margin:0;padding:0;box-sizing:border-box}\n"
"body{font-family:'Segoe UI',system-ui,sans-serif;background:#0d1117;color:#c9d1d9;display:flex;flex-direction:column;height:100vh}\n"
"header{background:#161b22;border-bottom:1px solid #30363d;padding:10px 20px;display:flex;align-items:center;justify-content:space-between}\n"
"header h1{font-size:15px;font-weight:600;color:#f0f6fc}\n"
".badge{font-size:11px;padding:2px 8px;border-radius:12px;font-weight:500}\n"
".badge.loading{background:#9e6a03;color:#fff}\n"
".badge.ready{background:#238636;color:#fff}\n"
".badge.error{background:#da3633;color:#fff}\n"
".btn{padding:5px 12px;border:1px solid #30363d;border-radius:6px;background:#21262d;color:#c9d1d9;cursor:pointer;font-size:12px}\n"
".btn:hover{background:#30363d}\n"
"#log{flex:1;overflow-y:auto;padding:16px;font-family:'Cascadia Code','Consolas',monospace;font-size:12px;line-height:1.6;white-space:pre-wrap;color:#8b949e}\n"
"#app-frame{flex:1;border:none;display:none}\n"
/* Terminal drawer */
"#term-drawer{display:none;border-top:1px solid #30363d;max-height:35vh;flex-shrink:0}\n"
"#term-output{height:calc(100%% - 36px);overflow-y:auto;padding:8px 12px;font-family:'Cascadia Code','Consolas',monospace;font-size:12px;line-height:1.5;white-space:pre-wrap;color:#8b949e}\n"
"#term-bar{display:flex;gap:6px;padding:4px 8px;background:#161b22;border-top:1px solid #30363d}\n"
"#term-bar input{flex:1;background:#0d1117;border:1px solid #30363d;border-radius:4px;color:#f0f6fc;padding:3px 8px;font-family:monospace;font-size:12px;outline:none}\n"
"#term-bar input:focus{border-color:#58a6ff}\n"
"</style></head>\n"
"<body>\n"
"<header>\n"
"  <h1>%s</h1>\n"
"  <div style='display:flex;gap:8px;align-items:center'>\n"
"    <span class='badge loading' id='status'>Starting...</span>\n"
"    <button class='btn' id='toggle-btn' onclick='toggleView()'>Show Log</button>\n"
"    <button class='btn' id='term-btn' style='display:none' onclick='toggleTerm()'>Terminal</button>\n"
"  </div>\n"
"</header>\n"
"<div id='log'></div>\n"
"<iframe id='app-frame'></iframe>\n"
"<div id='term-drawer'>\n"
"  <div id='term-output'></div>\n"
"  <div id='term-bar'>\n"
"    <span style='color:#7ee787;font-family:monospace;line-height:24px'>$</span>\n"
"    <input id='term-input' type='text' placeholder='Run a Linux command...' spellcheck='false'>\n"
"    <button class='btn' onclick='termRun()' style='padding:3px 8px'>Run</button>\n"
"  </div>\n"
"</div>\n"
"<script>\n"
"const log=document.getElementById('log'),frame=document.getElementById('app-frame');\n"
"const termOut=document.getElementById('term-output'),termIn=document.getElementById('term-input');\n"
"let showingLog=true,termOpen=false,termEnabled=%s;\n"
"function L(t){log.textContent+=t+'\\n';log.scrollTop=log.scrollHeight;}\n"
"function S(t,c){const s=document.getElementById('status');s.textContent=t;s.className='badge '+c;}\n"
"function J(v){return typeof v==='string'?JSON.parse(v):v;}\n"
"function toggleView(){showingLog=!showingLog;log.style.display=showingLog?'block':'none';frame.style.display=showingLog?'none':'flex';document.getElementById('toggle-btn').textContent=showingLog?'Show App':'Show Log';}\n"
"function toggleTerm(){termOpen=!termOpen;document.getElementById('term-drawer').style.display=termOpen?'flex':'none';if(termOpen)termIn.focus();}\n"
"async function termRun(){\n"
"  const cmd=termIn.value.trim();if(!cmd)return;termIn.value='';\n"
"  termOut.textContent+='$ '+cmd+'\\n';\n"
"  try{const r=J(await appExec(cmd));\n"
"    if(r.stdout)termOut.textContent+=r.stdout;\n"
"    if(r.stderr)termOut.textContent+=r.stderr;\n"
"    if(r.exit_code!==0)termOut.textContent+='exit: '+r.exit_code+'\\n';\n"
"  }catch(e){termOut.textContent+='Error: '+e+'\\n';}\n"
"  termOut.scrollTop=termOut.scrollHeight;\n"
"}\n"
"termIn.onkeydown=e=>{if(e.key==='Enter')termRun();};\n"
"if(termEnabled)document.getElementById('term-btn').style.display='';\n"
"(async()=>{\n"
"  try{\n"
"    S('Setting up...','loading');\n"
"    L('Setting up application...');\n"
"    const setup=J(await appSetup());\n"
"    L(setup.log||'');\n"
"    if(setup.error){S('Setup failed','error');L('ERROR: '+setup.error);return;}\n"
"    L('');\n"
"    S('Starting UI...','loading');\n"
"    var _br=J(await appStartBridge());\n"
"    if(!_br.ok){S('Error','error');L('Bridge failed: '+(_br.error||'unknown'));return;}\n"
"    if(_br.mode==='direct'){\n"
"      L('No bridge.py found — starting the configured app server directly...');\n"
"      var _started=J(await appStart());\n"
"      if(!_started.started){S('Ready','ready');L('No start command configured.');return;}\n"
"      var _appReady=false;\n"
"      for(var _i=0;_i<120;_i++){\n"
"        await new Promise(function(r){setTimeout(r,1000)});\n"
"        var _check=J(await appCheck());if(_check.ready){_appReady=true;break;}\n"
"        if(_i%5===4)L('.');\n"
"      }\n"
"      if(_appReady){var _appUrl=J(await appUrl()).url;S('Running','ready');window.location.href=_appUrl;}\n"
"      else{S('Error','error');L('App server did not start after 2 minutes. Check the start command and app port.');}\n"
"      return;\n"
"    }\n"
"    L('Starting bridge server...');\n"
"    var _burl='http://'+_br.host+':'+_br.port;\n"
"    var _target=_burl;\n"
"    if(_br.token){var _tok=J(await appGetToken()).token;_target+='/?tq_t='+encodeURIComponent(_tok);}\n"
"    L('Bridge at '+_burl+' — waiting for UI...');\n"
"    var _uiReady=false;\n"
"    for(var _i=0;_i<120;_i++){\n"
"      await new Promise(function(r){setTimeout(r,1000)});\n"
"      try{await fetch(_burl+'/api/live',{mode:'no-cors'});_uiReady=true;break;}catch(_e){}\n"
"      if(_i%5===4)L('.');\n"
"    }\n"
"    if(_uiReady){\n"
"      S('Running','ready');\n"
"      window.location.href=_target;\n"
"    }else{S('Error','error');L('UI server did not start after 2 minutes. Check that Python is installed.');}\n"
"  }catch(e){S('Error','error');L('Fatal: '+e);}\n"
"})();\n"
"</script></body></html>\n";

typedef struct {
    webview_t        w;
    linux_backend_t *backend;
    service_manager_t *svc_mgr;
    app_config_t    *app;
} app_json_ctx_t;

/* app_json_escape replaced by shared json_escape() from bridge/json_escape.h */
#define app_json_escape json_escape

/* Background thread data for async callbacks */
typedef struct {
    app_json_ctx_t *ctx;
    char seq[64];
} async_task_t;

/* Dispatch helper — runs webview_eval on the main thread */
static void dispatch_eval(webview_t w, void *arg) {
    char *js = (char *)arg;
    webview_eval(w, js);
    free(js);
}

/* Push a line of text to the log in real-time from any thread */
static void push_log(app_json_ctx_t *ctx, const char *text) {
    if (!text || !text[0]) return;
    char *esc = app_json_escape(text);
    size_t sz = strlen(esc) + 64;
    char *js = (char *)malloc(sz);
    if (js) {
        snprintf(js, sz, "L(\"%s\");", esc);
        webview_dispatch(ctx->w, dispatch_eval, js);
        /* js is freed by dispatch_eval after it runs */
    }
    free(esc);
}

/* Push a status badge update */
static void push_status(app_json_ctx_t *ctx, const char *text, const char *cls) {
    size_t sz = strlen(text) + strlen(cls) + 32;
    char *js = (char *)malloc(sz);
    if (js) {
        snprintf(js, sz, "S('%s','%s');", text, cls);
        webview_dispatch(ctx->w, dispatch_eval, js);
    }
}

/* Run a command/script and stream output to the log in real-time.
 *
 * Writes the script to a temp file, executes it in the background,
 * and polls the output log for new lines. This avoids shell quoting
 * issues that occur when embedding complex scripts in sh -c '...'. */
static int exec_with_log(app_json_ctx_t *ctx, const char *command) {
    /* Step 1: Write the script to a temp file via heredoc.
     * This avoids all quoting issues — the script can contain
     * single quotes, double quotes, $variables, anything.
     * Sanitize: if the command contains the delimiter, replace it to
     * prevent premature heredoc termination. */
    char *sanitized = NULL;
    const char *safe_cmd = command;
    if (strstr(command, "__SCRIPT_EOF__")) {
        size_t cmd_len_s = strlen(command);
        sanitized = (char *)malloc(cmd_len_s + 1);
        if (!sanitized) return -1;
        memcpy(sanitized, command, cmd_len_s + 1);
        /* Replace all occurrences of the delimiter in the command */
        char *pos = sanitized;
        while ((pos = strstr(pos, "__SCRIPT_EOF__")) != NULL) {
            memcpy(pos, "__SCRIPT_E0F__", 14);
            pos += 14;
        }
        safe_cmd = sanitized;
    }
    size_t write_len = strlen(safe_cmd) + 256;
    char *write_cmd = (char *)malloc(write_len);
    if (!write_cmd) { free(sanitized); return -1; }
    snprintf(write_cmd, write_len,
        "mkdir -p /tmp/linux_template; cat > /tmp/linux_template/_app_script.sh << '__SCRIPT_EOF__'\n"
        "%s\n"
        "__SCRIPT_EOF__\n",
        safe_cmd);
    free(sanitized);

    int exit_code = -1;
    linux_error_t exec_rc = ctx->backend->exec(ctx->backend, write_cmd,
                                                NULL, NULL, &exit_code);
    free(write_cmd);
    if (exec_rc != LINUX_OK || exit_code != 0) return -1;

    /* Step 2: Run the script in the background, output to log file */
    ctx->backend->exec(ctx->backend,
        "rm -f /tmp/linux_template/_app_log.txt; "
        "sh -c 'bash /tmp/linux_template/_app_script.sh "
            "> /tmp/linux_template/_app_log.txt 2>&1; "
            "echo __DONE:$?__ >> /tmp/linux_template/_app_log.txt' &",
        NULL, NULL, NULL);

    /* Step 3: Poll the log file for new output */
    int lines_seen = 0;
    int rc = -1;
    int done = 0;
    unsigned long start_time = COMPAT_TICK_MS();
    unsigned long timeout = 7200000; /* 2 hours max for large CUDA builds + vLLM install */

    while (!done && (COMPAT_TICK_MS() - start_time) < timeout) {
        COMPAT_SLEEP_MS(1500);

        char tail_cmd[128];
        snprintf(tail_cmd, sizeof(tail_cmd),
            "tail -n +%d /tmp/linux_template/_app_log.txt 2>/dev/null",
            lines_seen + 1);

        char *out = NULL;
        ctx->backend->exec(ctx->backend, tail_cmd, &out, NULL, NULL);

        if (out && out[0]) {
            char *marker = strstr(out, "__DONE:");
            if (marker) {
                rc = atoi(marker + 7);
                *marker = '\0';
                done = 1;
            }

            /* Push each new line to the GUI */
            char *saveptr = NULL;
            char *line = COMPAT_STRTOK(out, "\n", &saveptr);
            while (line) {
                if (line[0]) {
                    push_log(ctx, line);
                    lines_seen++;
                }
                line = COMPAT_STRTOK(NULL, "\n", &saveptr);
            }
        }
        free(out);
    }

    /* Cleanup */
    ctx->backend->exec(ctx->backend,
        "rm -f /tmp/linux_template/_app_script.sh", NULL, NULL, NULL);

    if (!done) push_log(ctx, "(timed out after 2 hours)");
    return done ? rc : -1;
}

static THREAD_FUNC_DECL setup_thread(THREAD_PARAM param) {
    async_task_t *task = (async_task_t *)param;
    app_json_ctx_t *ctx = task->ctx;

    /* Build one big setup script that does everything:
     * deps → clone → setup, all in one command streamed to a log file.
     * This avoids multiple blocking exec() calls. */
    growbuf_t script;
    growbuf_init(&script, 4096);
    /* Use set +e so one failed step doesn't abort everything.
     * Use unbuffered output so pip/curl progress shows in real-time. */
    {
        const char *header = "set +e\nexport PYTHONUNBUFFERED=1\n";
        growbuf_append(&script, header, strlen(header));
    }

    /* Dependencies */
    if (ctx->app->deps[0]) {
        growbuf_append(&script, "echo '=== Installing dependencies ==='\n",
                       (size_t)strlen("echo '=== Installing dependencies ==='\n"));
        char deps_copy[1024];
        strncpy(deps_copy, ctx->app->deps, sizeof(deps_copy) - 1);
        deps_copy[sizeof(deps_copy) - 1] = '\0';

        char *saveptr = NULL;
        char *dep = COMPAT_STRTOK(deps_copy, ",; ", &saveptr);
        while (dep) {
            while (*dep == ' ') dep++;
            char line[512];
            int n;
            if (strcmp(dep, "python") == 0 || strcmp(dep, "python3") == 0) {
                n = snprintf(line, sizeof(line),
                    "echo 'Installing: python'\n"
                    "which python3 >/dev/null 2>&1 || (apt-get update -qq && apt-get install -y -qq python3 python3-pip curl) 2>&1\n"
                    "python3 -m pip --version >/dev/null 2>&1 || "
                    "(curl -fsSL https://bootstrap.pypa.io/get-pip.py | python3 - --user --break-system-packages) 2>&1\n"
                    "export PATH=$HOME/.local/bin:$PATH\n"
                    "echo 'python: done'\n");
            } else if (strcmp(dep, "node") == 0 || strcmp(dep, "nodejs") == 0) {
                n = snprintf(line, sizeof(line),
                    "echo 'Installing: node'\n"
                    "which node >/dev/null 2>&1 || (apt-get update -qq && apt-get install -y -qq curl ca-certificates && "
                    "curl -fsSL https://deb.nodesource.com/setup_lts.x | bash - && "
                    "apt-get install -y -qq nodejs || apk add nodejs npm) 2>&1\n"
                    "echo 'node: done'\n");
            } else if (strcmp(dep, "git") == 0) {
                n = snprintf(line, sizeof(line),
                    "echo 'Installing: git'\n"
                    "which git >/dev/null 2>&1 || (apt-get update -qq && apt-get install -y -qq git || apk add git) 2>&1\n"
                    "echo 'git: done'\n");
            } else if (strcmp(dep, "base") == 0) {
                n = snprintf(line, sizeof(line),
                    "echo 'Installing: base dev tools'\n"
                    "which gcc >/dev/null 2>&1 || (apt-get update -qq && apt-get install -y -qq build-essential curl wget git || "
                    "apk add build-base curl wget) 2>&1\n"
                    "echo 'base: done'\n");
            } else {
                char *esc_dep = shell_escape(dep);
                if (esc_dep) {
                    n = snprintf(line, sizeof(line),
                        "echo 'Installing: '%s\n"
                        "sudo -n apt-get install -y -qq %s 2>/dev/null || sudo apk add %s 2>/dev/null\n"
                        "echo %s': done'\n", esc_dep, esc_dep, esc_dep, esc_dep);
                    free(esc_dep);
                } else {
                    n = 0;
                }
            }
            growbuf_append(&script, line, (size_t)n);
            dep = COMPAT_STRTOK(NULL, ",; ", &saveptr);
        }
    }

    /* Ensure /opt/app exists — symlink to the Windows mount path of the exe dir.
     * This is needed even without a repo so setup commands can reference app files. */
    {
        char edir[MAX_PATH];
        GetModuleFileNameA(NULL, edir, MAX_PATH);
        { char *s = strrchr(edir, '\\'); if (s) *s = '\0'; }
        char wpath[MAX_PATH];
        snprintf(wpath, sizeof(wpath), "/mnt/%c%s", edir[0] | 0x20, edir + 2);
        for (char *p = wpath; *p; p++) if (*p == '\\') *p = '/';

        char line[MAX_PATH + 256];
        int n = snprintf(line, sizeof(line),
            "if [ ! -e /opt/app ]; then\n"
            "  sudo -n mkdir -p /opt 2>/dev/null\n"
            "  ln -sf '%s' /opt/app 2>/dev/null || "
            "  (sudo -n mkdir -p /opt/app 2>/dev/null && sudo -n cp -a '%s/'* /opt/app/ 2>/dev/null)\n"
            "fi\n", wpath, wpath);
        growbuf_append(&script, line, (size_t)n);
    }

    /* Clone repo */
    if (ctx->app->repo[0]) {
        char *esc_repo = shell_escape(ctx->app->repo);
        char line[2048];
        int n = snprintf(line, sizeof(line),
            "echo '=== Cloning repository ==='\n"
            "sudo -n mkdir -p /opt/app 2>/dev/null; sudo -n chown $(whoami) /opt/app 2>/dev/null; mkdir -p /opt/app 2>/dev/null\n"
            "if [ -d /opt/app/.git ]; then cd /opt/app && git pull 2>&1; "
            "else git clone --depth 1 %s /opt/app 2>&1; fi\n"
            "echo 'Clone: done'\n", esc_repo ? esc_repo : "''");
        free(esc_repo);
        growbuf_append(&script, line, (size_t)n);
    }

    /* Setup command */
    if (ctx->app->setup[0]) {
        char line[2048];
        int n = snprintf(line, sizeof(line),
            "echo '=== Running setup ==='\n"
            "cd /opt/app 2>/dev/null || true\n"
            "%s 2>&1\n"
            "setup_rc=$?\n"
            "if [ \"$setup_rc\" -ne 0 ]; then\n"
            "  echo \"Setup: failed ($setup_rc)\"\n"
            "  exit \"$setup_rc\"\n"
            "fi\n"
            "echo 'Setup: done'\n", ctx->app->setup);
        growbuf_append(&script, line, (size_t)n);
    }

    char *full_script = growbuf_finish(&script);

    /* Run the entire script through exec_with_log for real-time streaming */
    push_status(ctx, "Installing...", "loading");
    push_log(ctx, "Starting setup...");
    int rc = exec_with_log(ctx, full_script ? full_script : "echo 'Nothing to do'");
    free(full_script);

    if (rc != 0) {
        push_log(ctx, "Setup failed!");
        push_status(ctx, "Failed", "error");
        webview_return(ctx->w, task->seq, 0,
                       "{\"log\":\"\",\"error\":\"Setup failed\"}");
    } else {
        push_log(ctx, "Setup complete!");

        /* Export the per-app WSL distro so the app folder is truly portable.
         * Only export once — if rootfs.tar.gz already exists, skip. */
#ifdef _WIN32
        {
            char edir[MAX_PATH];
            GetModuleFileNameA(NULL, edir, MAX_PATH);
            { char *s = strrchr(edir, '\\'); if (s) *s = '\0'; }

            char export_path[MAX_PATH];
            snprintf(export_path, sizeof(export_path),
                     "%s\\linux\\rootfs.tar.gz", edir);

            if (!ctx->app->snapshot) {
                push_log(ctx, "Environment snapshot export skipped by app.json.");
            } else if (GetFileAttributesA(export_path) == INVALID_FILE_ATTRIBUTES) {
                push_log(ctx, "Saving environment snapshot...");
                push_status(ctx, "Saving...", "loading");

                char distro[256];
                make_distro_name(ctx->app->name, distro, sizeof(distro));

                char wsl_cmd[MAX_PATH * 2];
                snprintf(wsl_cmd, sizeof(wsl_cmd),
                         "wsl.exe --export %s \"%s\"", distro, export_path);

                STARTUPINFOA si = {0};
                si.cb = sizeof(si);
                si.dwFlags = STARTF_USESHOWWINDOW;
                si.wShowWindow = SW_HIDE;
                PROCESS_INFORMATION pi = {0};
                if (CreateProcessA(NULL, wsl_cmd, NULL, NULL, FALSE,
                                   CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                    /* 4 hours — large distros (27+ GB with CUDA + vLLM + llama.cpp)
                     * can take 1-3 hours to export on HDDs or slow SSDs. */
                    DWORD wait_rc = WaitForSingleObject(pi.hProcess, 14400000);
                    DWORD exit_code = 1;
                    if (wait_rc == WAIT_OBJECT_0) {
                        GetExitCodeProcess(pi.hProcess, &exit_code);
                    } else {
                        /* Timed out — kill the export process */
                        TerminateProcess(pi.hProcess, 1);
                        WaitForSingleObject(pi.hProcess, 5000);
                    }
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);

                    /* Verify: export succeeded (exit 0) AND file is non-trivial size.
                     * A corrupt/partial export would mark setup "done" but break next launch. */
                    BOOL export_ok = (exit_code == 0);
                    if (export_ok) {
                        HANDLE hf = CreateFileA(export_path, GENERIC_READ, FILE_SHARE_READ,
                                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                        if (hf != INVALID_HANDLE_VALUE) {
                            LARGE_INTEGER fsz; fsz.QuadPart = 0;
                            GetFileSizeEx(hf, &fsz);
                            CloseHandle(hf);
                            /* Sanity: a valid rootfs.tar.gz is at least 10 MB.
                             * Truncated exports are usually much smaller. */
                            if (fsz.QuadPart < 10 * 1024 * 1024) export_ok = FALSE;
                        } else {
                            export_ok = FALSE;
                        }
                    }

                    if (export_ok) {
                        push_log(ctx, "Environment saved. App folder is now portable.");
                        /* Only save hash on clean export — otherwise next launch
                         * will detect the stale/missing snapshot and re-run setup. */
                        save_setup_hash(ctx->app, edir);
                    } else {
                        push_log(ctx, "Warning: snapshot export failed or was truncated -- deleting partial file.");
                        DeleteFileA(export_path);
                    }
                } else {
                    push_log(ctx, "Warning: could not export environment snapshot.");
                }
            }
        }
#endif

        push_status(ctx, "Ready", "ready");
        webview_return(ctx->w, task->seq, 0,
                       "{\"log\":\"\",\"error\":null}");
    }

    free(task);
    THREAD_FUNC_RET;
}

static void on_app_setup(const char *seq, const char *req, void *arg) {
    (void)req;
    async_task_t *task = (async_task_t *)calloc(1, sizeof(async_task_t));
    if (!task) return;
    task->ctx = (app_json_ctx_t *)arg;
    strncpy(task->seq, seq, sizeof(task->seq) - 1);
    compat_thread_launch(setup_thread, task);
}

static THREAD_FUNC_DECL start_thread(THREAD_PARAM param) {
    async_task_t *task = (async_task_t *)param;
    app_json_ctx_t *ctx = task->ctx;
    if (ctx->app->start[0]) {
        char health[256] = "";
        if (ctx->app->port > 0)
            snprintf(health, sizeof(health), "http://localhost:%d/", ctx->app->port);
        int idx = svc_register(ctx->svc_mgr, "app",
                               ctx->app->start, health[0] ? health : NULL,
                               ctx->app->port);
        if (idx >= 0) svc_start(ctx->svc_mgr, idx);
        char resp[128];
        snprintf(resp, sizeof(resp),
                 "{\"started\":true,\"port\":%d}", ctx->app->port);
        webview_return(ctx->w, task->seq, 0, resp);
    } else {
        webview_return(ctx->w, task->seq, 0, "{\"started\":false}");
    }
    free(task);
    THREAD_FUNC_RET;
}

static void on_app_start(const char *seq, const char *req, void *arg) {
    (void)req;
    async_task_t *task = (async_task_t *)calloc(1, sizeof(async_task_t));
    if (!task) return;
    task->ctx = (app_json_ctx_t *)arg;
    strncpy(task->seq, seq, sizeof(task->seq) - 1);
    compat_thread_launch(start_thread, task);
}

static THREAD_FUNC_DECL check_thread(THREAD_PARAM param) {
    async_task_t *task = (async_task_t *)param;
    app_json_ctx_t *ctx = task->ctx;
    int idx = svc_find(ctx->svc_mgr, "app");
    if (idx >= 0 && svc_check(ctx->svc_mgr, idx) == SVC_RUNNING)
        webview_return(ctx->w, task->seq, 0, "{\"ready\":true}");
    else
        webview_return(ctx->w, task->seq, 0, "{\"ready\":false}");
    free(task);
    THREAD_FUNC_RET;
}

static void on_app_check(const char *seq, const char *req, void *arg) {
    (void)req;
    async_task_t *task = (async_task_t *)calloc(1, sizeof(async_task_t));
    if (!task) return;
    task->ctx = (app_json_ctx_t *)arg;
    strncpy(task->seq, seq, sizeof(task->seq) - 1);
    compat_thread_launch(check_thread, task);
}

static void on_app_url(const char *seq, const char *req, void *arg) {
    app_json_ctx_t *ctx = (app_json_ctx_t *)arg;
    (void)req;
    char resp[256];
    snprintf(resp, sizeof(resp), "{\"url\":\"http://localhost:%d\"}", ctx->app->port);
    webview_return(ctx->w, seq, 0, resp);
}

/* Launch bridge.py inside WSL using the backend exec.
 * Discovers the WSL2 VM IP so the WebView can reach the bridge directly,
 * avoiding unreliable WSL2 localhost forwarding. */
static int bridge_pid = 0;
static int bridge_port = 9091;  /* per-app: derived from app.port + 1000 */
static char bridge_host[64] = "localhost";  /* WSL2 VM IP or localhost */
static int bridge_token_required = 0;

/* Per-launch random bearer token passed to bridge.py via TQ_AUTH_TOKEN and
 * handed to the WebView via appGetToken(). The WebView navigates to
 * http://<host>:<port>/?tq_t=<token> exactly once, where the bridge
 * validates and sets an HttpOnly same-origin cookie. Keeps the bridge's
 * deny-by-default auth on without forcing TQ_AUTH_DISABLED=1. */
static char bridge_token[65] = {0};  /* 32 random bytes -> 64 hex + NUL */

static void gen_bridge_token(void) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        unsigned int rnd = 0;
        /* rand_s is backed by RtlGenRandom; cryptographic-quality on Windows. */
        if (rand_s(&rnd) != 0) {
            /* rand_s never fails on modern Windows (RtlGenRandom is always
             * available). If it somehow does, fold time + iteration into
             * rnd so we never emit an all-zero token — but we cannot
             * claim cryptographic quality from this fallback. */
            rnd = (unsigned int)(GetTickCount() ^ (unsigned int)(i * 0x9e3779b1u));
        }
        unsigned char b = (unsigned char)(rnd & 0xff);
        bridge_token[i * 2]     = hex[b >> 4];
        bridge_token[i * 2 + 1] = hex[b & 0x0f];
    }
    bridge_token[64] = '\0';
}

static void set_bridge_start_error(char *err, size_t err_len,
                                   const char *msg) {
    if (!err || err_len == 0) return;
    snprintf(err, err_len, "%s", msg && msg[0] ? msg : "Failed to start bridge.py");
}

/* Returns 0 when a bridge process was launched, 1 when this is a regular
 * app.json child with no bridge.py (the caller should start app.start), and
 * -1 on a genuine bridge launch failure. */
static int start_bridge(app_json_ctx_t *ctx, char *err, size_t err_len) {
    char exe_dir[MAX_PATH];
    GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    char *s = strrchr(exe_dir, '\\');
    if (s) *s = '\0';

    /* bridge.py is optional. Generic children created by the workbench expose
     * their configured app port directly; custom bridge-based children place
     * bridge.py beside the exe. */
    char bridge_path[MAX_PATH];
    snprintf(bridge_path, sizeof(bridge_path), "%s\\bridge.py", exe_dir);
    if (GetFileAttributesA(bridge_path) == INVALID_FILE_ATTRIBUTES) {
        bridge_token_required = 0;
        return 1;
    }
    char watchdog_path[MAX_PATH];
    snprintf(watchdog_path, sizeof(watchdog_path), "%s\\bridge_watchdog.py", exe_dir);
    int has_watchdog =
        GetFileAttributesA(watchdog_path) != INVALID_FILE_ATTRIBUTES;
    bridge_token_required = has_watchdog;

    /* Convert Windows path to WSL /mnt/ path */
    char wsl_path[MAX_PATH];
    snprintf(wsl_path, sizeof(wsl_path), "/mnt/%c%s",
             exe_dir[0] | 0x20, exe_dir + 2);
    for (char *p = wsl_path; *p; p++)
        if (*p == '\\') *p = '/';

    /* The app bridges bind loopback by default as part of the desktop trust
     * boundary. Windows reaches WSL loopback bridges through localhost
     * forwarding, so handing the WebView a WSL VM IP makes loopback-bound
     * bridges look dead even when http://127.0.0.1:<port>/ is healthy.
     *
     * Operators who deliberately widen a bridge bind can opt into WSL-IP
     * navigation by setting APP_BRIDGE_USE_WSL_IP=1 in the launcher env. */
    snprintf(bridge_host, sizeof(bridge_host), "%s", "localhost");
    const char *use_wsl_ip = getenv("APP_BRIDGE_USE_WSL_IP");
    if (use_wsl_ip && strcmp(use_wsl_ip, "1") == 0)
    {
        char *ip_out = NULL;
        ctx->backend->exec(ctx->backend, "hostname -I | awk '{print $1}'",
                           &ip_out, NULL, NULL);
        if (ip_out && ip_out[0] && ip_out[0] != '\n') {
            /* Trim whitespace */
            char *end = ip_out + strlen(ip_out) - 1;
            while (end > ip_out && (*end == '\n' || *end == '\r' || *end == ' '))
                *end-- = '\0';
            snprintf(bridge_host, sizeof(bridge_host), "%s", ip_out);
        }
        free(ip_out);
    }

    /* Mint one bearer token per GUI process. ``appStartBridge`` is also
     * used as a self-heal hook when the already-loaded UI detects that the
     * bridge/watchdog disappeared. Reusing the in-memory token keeps that
     * path idempotent: if an existing watchdog is merely slow or already
     * restarting the bridge, a second appStartBridge call cannot rotate the
     * UI onto a token the live bridge does not know about. */
    if (bridge_token[0] == '\0')
        gen_bridge_token();

    /* Launch bridge_watchdog.py as a background process, passing port + token via env.
     * TQ_AUTH_TOKEN satisfies the bridge's deny-by-default auth gate (added
     * in Phase 5.1); the WebView retrieves the same token via appGetToken()
     * and navigates with ?tq_t=<token> to exchange it for a same-origin
     * HttpOnly cookie, after which ui.html's fetch() calls authenticate
     * automatically. The watchdog then owns bridge.py restarts and uses
     * TQ_APP_DIR to relaunch the bridge from this app directory. */
    char *esc_app_dir = shell_escape(wsl_path);
    char entry_wsl[MAX_PATH * 2];
    snprintf(entry_wsl, sizeof(entry_wsl), "%s/%s", wsl_path,
             has_watchdog ? "bridge_watchdog.py" : "bridge.py");
    char *esc_entry = shell_escape(entry_wsl);
    if (!esc_app_dir || !esc_entry) {
        free(esc_app_dir);
        free(esc_entry);
        set_bridge_start_error(err, err_len, "Could not escape bridge launch paths");
        return -1;
    }
    char cmd[8192];
    if (has_watchdog) {
        snprintf(cmd, sizeof(cmd),
                 "mkdir -p /tmp/linux_template; "
                 "BRIDGE_PORT=%d TQ_AUTH_TOKEN=%s TQ_APP_DIR=%s "
                 "TQ_BRIDGE_LOG_DIR=/tmp/linux_template "
                 "PYTHONUNBUFFERED=1 PYTHONFAULTHANDLER=1 "
                 "setsid nohup python3 -u %s "
                 "> /tmp/linux_template/watchdog-stderr.log 2>&1 < /dev/null & echo $!",
                 bridge_port, bridge_token, esc_app_dir, esc_entry);
    } else {
        /* Backward-compatible custom bridges predate the watchdog. Keep them
         * working while still passing the per-launch token to bridges that
         * understand the newer authentication contract. */
        snprintf(cmd, sizeof(cmd),
                 "mkdir -p /tmp/linux_template; rm -f /tmp/linux_template/watchdog.pid; "
                 "BRIDGE_PORT=%d TQ_AUTH_TOKEN=%s TQ_APP_DIR=%s "
                 "TQ_BRIDGE_LOG_DIR=/tmp/linux_template "
                 "PYTHONUNBUFFERED=1 PYTHONFAULTHANDLER=1 "
                 "setsid nohup python3 -u %s "
                 "> /tmp/linux_template/bridge.log 2>&1 < /dev/null & "
                 "pid=$!; echo $pid > /tmp/linux_template/bridge.pid; echo $pid",
                 bridge_port, bridge_token, esc_app_dir, esc_entry);
    }
    free(esc_app_dir);
    free(esc_entry);

    char *out = NULL;
    char *stderr_out = NULL;
    int exit_code = -1;
    linux_error_t rc = ctx->backend->exec(ctx->backend, cmd, &out, &stderr_out, &exit_code);
    if (rc != LINUX_OK || exit_code != 0) {
        const char *backend_name = (ctx->backend && ctx->backend->name)
            ? ctx->backend->name : "backend";
        const char *backend_err = (ctx->backend && ctx->backend->last_error)
            ? ctx->backend->last_error(ctx->backend) : "";
        snprintf(err, err_len,
                 "Failed to start %s via %s (rc=%s, exit=%d)%s%s%s%.400s%s%.300s",
                 has_watchdog ? "bridge_watchdog.py" : "bridge.py",
                 backend_name, linux_error_string(rc), exit_code,
                 backend_err && backend_err[0] ? ": " : "",
                 backend_err && backend_err[0] ? backend_err : "",
                 out && out[0] ? " output: " : "",
                 out && out[0] ? out : "",
                 stderr_out && stderr_out[0] ? " stderr: " : "",
                 stderr_out && stderr_out[0] ? stderr_out : "");
        free(out);
        free(stderr_out);
        return -1;
    }
    bridge_pid = (out && out[0]) ? atoi(out) : 0;
    free(out);
    free(stderr_out);
    return 0;
}

static void stop_bridge_with_backend(linux_backend_t *backend) {
    if (bridge_pid > 1 && backend) {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
                 "wp=$(cat /tmp/linux_template/watchdog.pid 2>/dev/null); "
                 "[ -n \"$wp\" ] || wp=%d; "
                 "if [ -n \"$wp\" ]; then "
                 "kill -TERM -- -$wp 2>/dev/null || kill -TERM $wp 2>/dev/null; "
                 "for i in $(seq 1 25); do kill -0 $wp 2>/dev/null || break; sleep 1; done; "
                 "kill -0 $wp 2>/dev/null && (kill -KILL -- -$wp 2>/dev/null; kill -KILL $wp 2>/dev/null); "
                 "fi; "
                 "bp=$(cat /tmp/linux_template/bridge.pid 2>/dev/null); "
                 "if [ -n \"$bp\" ] && kill -0 \"$bp\" 2>/dev/null; then "
                 "kill -TERM \"$bp\" 2>/dev/null; "
                 "for i in $(seq 1 15); do kill -0 \"$bp\" 2>/dev/null || break; sleep 1; done; "
                 "kill -0 \"$bp\" 2>/dev/null && kill -KILL \"$bp\" 2>/dev/null; "
                 "fi",
                 bridge_pid);
        backend->exec(backend, cmd, NULL, NULL, NULL);
        bridge_pid = 0;
    }
}

static void on_app_start_bridge(const char *seq, const char *req, void *arg) {
    app_json_ctx_t *ctx = (app_json_ctx_t *)arg;
    (void)req;
    char err[1024] = "";
    int rc = start_bridge(ctx, err, sizeof(err));
    if (rc == 1) {
        webview_return(ctx->w, seq, 0,
                       "{\"ok\":true,\"mode\":\"direct\"}");
    } else if (rc == 0) {
        char resp[256];
        snprintf(resp, sizeof(resp),
                 "{\"ok\":true,\"mode\":\"bridge\",\"host\":\"%s\",\"port\":%d,\"token\":%s}",
                 bridge_host, bridge_port,
                 bridge_token_required ? "true" : "false");
        webview_return(ctx->w, seq, 0, resp);
    } else {
        if (!err[0])
            set_bridge_start_error(err, sizeof(err), "Failed to start bridge.py");
        char *esc = json_escape(err);
        size_t need = strlen(esc ? esc : "") + 64;
        char *resp = (char *)malloc(need);
        if (resp) {
            snprintf(resp, need, "{\"ok\":false,\"error\":\"%s\"}", esc ? esc : "");
            webview_return(ctx->w, seq, 0, resp);
            free(resp);
        } else {
            webview_return(ctx->w, seq, 0,
                           "{\"ok\":false,\"error\":\"Failed to start bridge.py\"}");
        }
        free(esc);
    }
}

/* Return the per-launch bridge token wrapped in a JSON object, matching
 * the shape of every other appXxx() bind so the JS helper J() can treat
 * the value uniformly. The WebView uses the token once, in the
 * navigation URL (?tq_t=<token>), to exchange it for an HttpOnly
 * same-origin cookie; after that the token stays only in the C
 * process's heap. */
static void on_app_get_token(const char *seq, const char *req, void *arg) {
    app_json_ctx_t *ctx = (app_json_ctx_t *)arg;
    (void)req;
    char resp[96];
    /* bridge_token is [0-9a-f]{64}, so no JSON escaping needed. */
    snprintf(resp, sizeof(resp), "{\"token\":\"%s\"}", bridge_token);
    webview_return(ctx->w, seq, 0, resp);
}

static void on_app_navigate(const char *seq, const char *req, void *arg) {
    app_json_ctx_t *ctx = (app_json_ctx_t *)arg;
    /* req is JSON array: ["http://localhost:8080"] */
    char url[512] = "";
    if (req && req[0] == '[' && req[1] == '"') {
        const char *s = req + 2;
        const char *e = strchr(s, '"');
        if (e && (size_t)(e - s) < sizeof(url)) {
            memcpy(url, s, (size_t)(e - s));
            url[e - s] = '\0';
        }
    }
    if (url[0]) webview_navigate(ctx->w, url);
    webview_return(ctx->w, seq, 0, "{}");
}

/* appExec — terminal command execution for app.json apps */
typedef struct {
    app_json_ctx_t *ctx;
    char seq[64];
    char *req;
} exec_task_t;

static THREAD_FUNC_DECL app_exec_thread(THREAD_PARAM param) {
    exec_task_t *task = (exec_task_t *)param;
    app_json_ctx_t *ctx = task->ctx;

    /* Extract command string from ["..."] */
    const char *req = task->req;
    const char *s = req ? strchr(req, '"') : NULL;
    const char *e = s ? strrchr(req, '"') : NULL;
    if (!s || !e || s == e) {
        webview_return(ctx->w, task->seq, 1, "\"Invalid command\"");
        free(task->req); free(task);
        return 0;
    }
    s++;
    size_t len = (size_t)(e - s);
    char *cmd = (char *)malloc(len + 1);
    if (!cmd) {
        webview_return(ctx->w, task->seq, 1, "\"Out of memory\"");
        free(task->req); free(task);
        return 0;
    }
    memcpy(cmd, s, len);
    cmd[len] = '\0';

    char *out = NULL, *err = NULL;
    int code = -1;
    ctx->backend->exec(ctx->backend, cmd, &out, &err, &code);
    free(cmd);

    char *out_esc = app_json_escape(out ? out : "");
    char *err_esc = app_json_escape(err ? err : "");
    size_t sz = strlen(out_esc) + strlen(err_esc) + 128;
    char *resp = (char *)malloc(sz);
    snprintf(resp, sz, "{\"stdout\":\"%s\",\"stderr\":\"%s\",\"exit_code\":%d}",
             out_esc, err_esc, code);
    webview_return(ctx->w, task->seq, 0, resp);
    free(out); free(err); free(out_esc); free(err_esc); free(resp);
    free(task->req); free(task);
    THREAD_FUNC_RET;
}

static void on_app_exec(const char *seq, const char *req, void *arg) {
    exec_task_t *task = (exec_task_t *)calloc(1, sizeof(exec_task_t));
    if (!task) return;
    task->ctx = (app_json_ctx_t *)arg;
    strncpy(task->seq, seq, sizeof(task->seq) - 1);
    task->req = req ? strdup(req) : NULL;
    compat_thread_launch(app_exec_thread, task);
}

static webview_t create_webview_or_die(void) {
    webview_t w = webview_create(0, NULL);
    if (!w) {
#ifdef _WIN32
        MessageBoxA(NULL,
            "Could not create the application window.\n\n"
            "This usually means the WebView2 runtime is not installed.\n"
            "It comes with Microsoft Edge — make sure Edge is installed\n"
            "and up to date, or download the WebView2 runtime from:\n\n"
            "https://developer.microsoft.com/en-us/microsoft-edge/webview2/",
            "WebView2 Required",
            MB_OK | MB_ICONERROR);
#else
        fprintf(stderr, "Failed to create webview. Install WebView2 runtime.\n");
#endif
    }
    return w;
}

static int run_app_json(linux_backend_t *backend, service_manager_t *svc_mgr,
                        app_config_t *app) {
    /* Display name for the window title + setup-panel header, independent
     * of app->name which identifies the WSL distro. An app.json can set
     * "title": "..." to show a long/pretty name in the window chrome while
     * keeping a short "name" so the distro stays as linbox-<name>. */
    const char *display = app->title[0] ? app->title : app->name;

    char html[16384];
    snprintf(html, sizeof(html), APP_JSON_HTML_TEMPLATE,
             display, display, app->terminal ? "true" : "false");

    webview_t w = create_webview_or_die();
    if (!w) return 1;

    webview_set_title(w, display);
    webview_set_size(w, app->width, app->height, WEBVIEW_HINT_NONE);

#ifdef _WIN32
    /* Dark title bar + per-app icon (look for <exe_dir>\app.ico or
     * <exe_dir>\<app-name>.ico). Does nothing on Windows versions without
     * DWM immersive dark mode support. */
    {
        char edir[MAX_PATH];
        GetModuleFileNameA(NULL, edir, MAX_PATH);
        char *s = strrchr(edir, '\\'); if (s) *s = '\0';
        apply_window_style(w, edir, app->name);
    }
#endif

    /* Use explicit bridge_port from app.json, or default to port + 1000 */
    bridge_port = app->bridge_port > 0 ? app->bridge_port : app->port + 1000;
    if (bridge_port < 1 || bridge_port > 65535) bridge_port = 9091;

    app_json_ctx_t ctx = { w, backend, svc_mgr, app };
    webview_bind(w, "appSetup", on_app_setup, &ctx);
    webview_bind(w, "appStart", on_app_start, &ctx);
    webview_bind(w, "appCheck", on_app_check, &ctx);
    webview_bind(w, "appUrl",   on_app_url,   &ctx);
    webview_bind(w, "appNavigate", on_app_navigate, &ctx);
    webview_bind(w, "appStartBridge", on_app_start_bridge, &ctx);
    webview_bind(w, "appGetToken", on_app_get_token, &ctx);
    webview_bind(w, "appExec",  on_app_exec,  &ctx);
    webview_bind(w, "closeApp", on_close_app, w);

    webview_set_html(w, html);
    webview_run(w);
    webview_destroy(w);
    return 0;
}
#endif /* HAVE_WEBVIEW && (_WIN32 || __APPLE__) */

/* Close/shutdown callback — terminates the WebView window */
#ifdef HAVE_WEBVIEW
static void on_close_app(const char *seq, const char *req, void *arg) {
    (void)seq; (void)req;
    webview_t w = (webview_t)arg;
    webview_terminate(w);
}
#endif

static char *html_escape_text(const char *text) {
    if (!text) text = "";
    size_t capacity = strlen(text) * 6 + 1;
    char *out = (char *)malloc(capacity);
    if (!out) return NULL;
    char *dst = out;
    for (const char *src = text; *src; src++) {
        const char *replacement = NULL;
        switch (*src) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '"': replacement = "&quot;"; break;
        case '\'': replacement = "&#39;"; break;
        default: *dst++ = *src; break;
        }
        if (replacement) {
            size_t n = strlen(replacement);
            memcpy(dst, replacement, n);
            dst += n;
        }
    }
    *dst = '\0';
    return out;
}

/* Show either genuine install instructions or an actionable startup error.
 * A detected backend failure must never be mislabeled as “WSL not installed.” */
static void show_setup_required(const char *detail, int install_required) {
#ifdef HAVE_WEBVIEW
    if (!install_required) {
        static const char *START_ERROR_HTML =
            "<!DOCTYPE html><html><head><meta charset='utf-8'><style>"
            "*{margin:0;padding:0;box-sizing:border-box}"
            "body{font-family:'Segoe UI',system-ui,sans-serif;background:#0d1117;"
            "color:#c9d1d9;display:flex;align-items:center;justify-content:center;"
            "height:100vh;padding:20px}"
            ".card{background:#161b22;border:1px solid #30363d;border-radius:12px;"
            "padding:32px;max-width:600px}"
            "h1{color:#f0f6fc;font-size:20px;margin-bottom:16px;text-align:center}"
            "h2{color:#f0f6fc;font-size:15px;margin:18px 0 8px}"
            "p,li{font-size:13px;line-height:1.6;color:#c9d1d9}"
            ".detail{background:#0d1117;border:1px solid #30363d;border-radius:6px;"
            "padding:10px;font-family:monospace;font-size:11px;margin-bottom:16px;"
            "color:#f85149;word-break:break-all;white-space:pre-wrap}"
            ".info{background:#0d1117;border:1px solid #30363d;border-radius:6px;"
            "padding:14px;margin-bottom:16px}"
            "ol{margin:10px 0 18px 20px}li{margin-bottom:8px}"
            "code{background:#30363d;padding:2px 6px;border-radius:3px;color:#fff}"
            ".buttons{display:flex;gap:10px;justify-content:center}"
            ".btn{padding:10px 20px;border:1px solid #30363d;border-radius:6px;"
            "background:#21262d;color:#c9d1d9;cursor:pointer;text-decoration:none}"
            ".btn.primary{background:#238636;border-color:#238636;color:#fff}"
            "</style></head><body><div class='card'>"
            "<h1>Linux Environment Could Not Start</h1>"
            "<div class='detail'>%s</div>"
            "<div class='info'><p><b>WSL was detected.</b> Do not reinstall it "
            "based on this message. The Linux session did not become ready, "
            "and the app already retried a quick startup failure once.</p></div>"
            "<h2>If it happens again</h2><ol>"
            "<li>Close this window and start the app once more.</li>"
            "<li>If it repeats, run <code>wsl --shutdown</code> in PowerShell, "
            "then reopen the app.</li>"
            "<li>Run <code>wsl --update</code> if Windows reports that an update "
            "is required.</li><li>Keep the diagnostic detail above for a bug "
            "report if the failure persists.</li></ol>"
            "<div class='buttons'><a class='btn primary' target='_blank' "
            "href='https://learn.microsoft.com/en-us/windows/wsl/troubleshooting'>"
            "WSL Troubleshooting</a>"
            "<button class='btn' onclick='closeApp()'>Close</button></div>"
            "</div></body></html>";
        char *escaped = html_escape_text(detail);
        if (escaped) {
            size_t size = strlen(START_ERROR_HTML) + strlen(escaped) + 32;
            char *html = (char *)malloc(size);
            if (html) {
                snprintf(html, size, START_ERROR_HTML, escaped);
                webview_t w = webview_create(0, NULL);
                if (w) {
                    webview_set_title(w, "Linux Environment Could Not Start");
                    webview_set_size(w, 620, 600, WEBVIEW_HINT_NONE);
                    webview_bind(w, "closeApp", on_close_app, w);
                    webview_set_html(w, html);
                    free(html);
                    free(escaped);
                    webview_run(w);
                    webview_destroy(w);
                    return;
                }
                free(html);
            }
            free(escaped);
        }
    }
    if (!install_required) goto console_fallback;

    static const char *SETUP_HTML =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<style>"
        "*{margin:0;padding:0;box-sizing:border-box}"
        "body{font-family:'Segoe UI',system-ui,sans-serif;background:#0d1117;"
        "color:#c9d1d9;display:flex;align-items:center;justify-content:center;"
        "height:100vh;padding:20px}"
        ".card{background:#161b22;border:1px solid #30363d;border-radius:12px;"
        "padding:36px;max-width:560px}"
        "h1{color:#f0f6fc;font-size:20px;margin-bottom:16px;text-align:center}"
        "h2{color:#f0f6fc;font-size:15px;margin:20px 0 8px}"
        "p{font-size:13px;line-height:1.7;margin-bottom:12px;color:#8b949e}"
        ".detail{background:#0d1117;border:1px solid #30363d;border-radius:6px;"
        "padding:10px;font-family:monospace;font-size:11px;"
        "margin-bottom:16px;color:#f85149;word-break:break-all}"
        ".info{background:#0d1117;border:1px solid #30363d;border-radius:6px;"
        "padding:14px;margin-bottom:16px}"
        ".info p{margin-bottom:6px;color:#c9d1d9;font-size:13px}"
        ".steps{margin:12px 0 16px 20px}"
        ".steps li{margin-bottom:8px;font-size:13px;color:#c9d1d9}"
        "code{background:#30363d;padding:2px 6px;border-radius:3px;"
        "font-size:12px;color:#f0f6fc}"
        ".buttons{display:flex;gap:10px;justify-content:center;margin-top:20px}"
        ".btn{padding:10px 20px;border:1px solid #30363d;border-radius:6px;"
        "font-size:13px;cursor:pointer;text-decoration:none;text-align:center}"
        ".btn.primary{background:#238636;border-color:#238636;color:#fff}"
        ".btn.primary:hover{background:#2ea043}"
        ".btn.secondary{background:#21262d;color:#c9d1d9}"
        ".btn.secondary:hover{background:#30363d}"
        "</style></head><body>"
        "<div class='card'>"
        "<h1>One-Time Setup Required</h1>"
        "<div class='detail'>%s</div>"
        "<h2>What is WSL?</h2>"
        "<div class='info'>"
        "<p><b>WSL (Windows Subsystem for Linux)</b> is a free, built-in "
        "Windows feature by Microsoft. It lets this app run a lightweight "
        "Linux environment in the background for AI, development tools, "
        "and other tasks that require Linux.</p>"
        "<p>It installs as part of Windows (like Bluetooth or printing) — "
        "it is not a separate program. It uses about 1 GB of disk space "
        "and runs only when this app is open.</p>"
        "</div>"
        "<h2>How to install</h2>"
        "<ol class='steps'>"
        "<li>Open <b>PowerShell as Administrator</b><br>"
        "<span style='color:#8b949e;font-size:12px'>"
        "Right-click the Start button, choose Terminal (Admin)</span></li>"
        "<li>Type this command and press Enter:<br>"
        "<code>wsl --install</code></li>"
        "<li>Wait for the download to finish (~1 GB)</li>"
        "<li><b>Restart your computer</b> when prompted</li>"
        "<li>Open this app again — it will work automatically</li>"
        "</ol>"
        "<div class='buttons'>"
        "<a class='btn primary' "
        "href='https://learn.microsoft.com/en-us/windows/wsl/install' "
        "target='_blank'>Microsoft WSL Guide</a>"
        "<button class='btn secondary' onclick='closeApp()'>Close</button>"
        "</div></body></html>";

    char *install_detail = html_escape_text(detail);
    if (!install_detail) goto console_fallback;
    size_t html_size = strlen(SETUP_HTML) + strlen(install_detail) + 64;
    char *html = (char *)malloc(html_size);
    if (html) {
        snprintf(html, html_size, SETUP_HTML, install_detail);
        webview_t w = webview_create(0, NULL);
        if (w) {
            webview_set_title(w, "Setup Required");
            webview_set_size(w, 580, 620, WEBVIEW_HINT_NONE);
            {
                char edir[MAX_PATH];
                GetModuleFileNameA(NULL, edir, MAX_PATH);
                char *s = strrchr(edir, '\\'); if (s) *s = '\0';
                apply_window_style(w, edir, "Setup");
            }
            webview_bind(w, "closeApp", on_close_app, w);
            webview_set_html(w, html);
            free(html);
            free(install_detail);
            webview_run(w);
            webview_destroy(w);
            return;
        }
        free(html);
    }
    free(install_detail);
#endif
console_fallback:
    /* Fallback to console */
    fprintf(stderr, "ERROR: %s\n", detail);
    if (install_required)
        fprintf(stderr, "Install WSL: wsl --install\n");
    else
        fprintf(stderr, "WSL was detected. Try: wsl --shutdown, then reopen the app.\n");
}

/* ======================================================================
 * CLI mode for app.json apps (used on Linux or when WebView2 is absent)
 *
 * Does the full setup sequence: deps -> clone -> setup -> start,
 * all in the terminal with streamed output.
 * ====================================================================== */
/* Export the per-app WSL distro to rootfs.tar.gz for portability.
 * Only exports if rootfs.tar.gz doesn't already exist. */
static void export_app_distro(const app_config_t *app) {
#ifdef _WIN32
    if (!app->snapshot) {
        printf("[snapshot] Export skipped by app.json.\n");
        return;
    }

    char exe_dir[MAX_PATH];
    GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    { char *s = strrchr(exe_dir, '\\'); if (s) *s = '\0'; }

    /* Auto-invalidate stale snapshot if app.json setup changed */
    invalidate_stale_snapshot(app, exe_dir);

    char export_path[MAX_PATH];
    snprintf(export_path, sizeof(export_path),
             "%s\\linux\\rootfs.tar.gz", exe_dir);

    /* Only export once */
    if (GetFileAttributesA(export_path) != INVALID_FILE_ATTRIBUTES) return;

    char distro[256];
    make_distro_name(app->name, distro, sizeof(distro));

    printf("[snapshot] Saving environment to %s ...\n", export_path);

    char wsl_cmd[MAX_PATH * 2];
    snprintf(wsl_cmd, sizeof(wsl_cmd),
             "wsl.exe --export %s \"%s\"", distro, export_path);

    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};
    if (CreateProcessA(NULL, wsl_cmd, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        /* 4 hours — large distros (27+ GB) can take 1-3 hours on slow disks */
        DWORD wait_rc = WaitForSingleObject(pi.hProcess, 14400000);
        DWORD exit_code = 1;
        if (wait_rc == WAIT_OBJECT_0) {
            GetExitCodeProcess(pi.hProcess, &exit_code);
        } else {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        /* Verify export succeeded and file is non-trivial size */
        BOOL export_ok = (exit_code == 0);
        if (export_ok) {
            HANDLE hf = CreateFileA(export_path, GENERIC_READ, FILE_SHARE_READ,
                                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER fsz; fsz.QuadPart = 0;
                GetFileSizeEx(hf, &fsz);
                CloseHandle(hf);
                if (fsz.QuadPart < 10 * 1024 * 1024) export_ok = FALSE;
            } else {
                export_ok = FALSE;
            }
        }

        if (export_ok) {
            printf("[snapshot] Done. App folder is now portable.\n\n");
            save_setup_hash(app, exe_dir);
        } else {
            printf("[snapshot] Warning: export failed or was truncated -- deleting partial file.\n\n");
            DeleteFileA(export_path);
        }
    } else {
        printf("[snapshot] Warning: could not save environment snapshot.\n\n");
    }
#else
    (void)app;
#endif
}

static int run_app_json_cli(linux_backend_t *backend, app_config_t *app) {
    /* Install dependencies */
    if (app->deps[0]) {
        printf("[deps] Installing: %s\n", app->deps);
        char deps_copy[1024];
        strncpy(deps_copy, app->deps, sizeof(deps_copy) - 1);
        deps_copy[sizeof(deps_copy) - 1] = '\0';

        /* Split on comma/semicolon/space and install each */
        char *saveptr = NULL;
#ifdef _WIN32
        char *dep = COMPAT_STRTOK(deps_copy, ",; ", &saveptr);
#else
        char *dep = strtok_r(deps_copy, ",; ", &saveptr);
#endif
        while (dep) {
            while (*dep == ' ') dep++;
            if (dep[0]) {
                char cmd[512];
                if (strcmp(dep, "python") == 0 || strcmp(dep, "python3") == 0) {
                    snprintf(cmd, sizeof(cmd),
                        "which python3 >/dev/null 2>&1 || (apt-get update -qq && apt-get install -y -qq python3 python3-pip curl) 2>&1; "
                        "python3 -m pip --version >/dev/null 2>&1 || "
                        "(curl -fsSL https://bootstrap.pypa.io/get-pip.py | python3 - --user --break-system-packages) 2>&1; "
                        "export PATH=$HOME/.local/bin:$PATH");
                } else if (strcmp(dep, "node") == 0 || strcmp(dep, "nodejs") == 0) {
                    snprintf(cmd, sizeof(cmd),
                        "which node >/dev/null 2>&1 || (apt-get update -qq && apt-get install -y -qq curl ca-certificates && "
                        "curl -fsSL https://deb.nodesource.com/setup_lts.x | bash - && "
                        "apt-get install -y -qq nodejs || apk add nodejs npm) 2>&1");
                } else if (strcmp(dep, "git") == 0) {
                    snprintf(cmd, sizeof(cmd),
                        "which git >/dev/null 2>&1 || (apt-get update -qq && apt-get install -y -qq git || apk add git) 2>&1");
                } else if (strcmp(dep, "base") == 0) {
                    snprintf(cmd, sizeof(cmd),
                        "which gcc >/dev/null 2>&1 || (apt-get update -qq && apt-get install -y -qq build-essential curl wget git || "
                        "apk add build-base curl wget) 2>&1");
                } else {
                    char *esc_dep = shell_escape(dep);
                    if (esc_dep) {
                        snprintf(cmd, sizeof(cmd),
                            "sudo -n apt-get install -y -qq %s 2>/dev/null || sudo apk add %s 2>/dev/null",
                            esc_dep, esc_dep);
                        free(esc_dep);
                    } else {
                        cmd[0] = '\0';
                    }
                }
                printf("[deps] %s...\n", dep);
                bridge_exec_print(backend, cmd);
            }
#ifdef _WIN32
            dep = COMPAT_STRTOK(NULL, ",; ", &saveptr);
#else
            dep = strtok_r(NULL, ",; ", &saveptr);
#endif
        }
        printf("[deps] Done.\n\n");
    }

    /* Ensure /opt/app exists — symlink to Windows mount path */
    {
        char edir[MAX_PATH];
        GetModuleFileNameA(NULL, edir, MAX_PATH);
        { char *s = strrchr(edir, '\\'); if (s) *s = '\0'; }
        char wpath[MAX_PATH];
        snprintf(wpath, sizeof(wpath), "/mnt/%c%s", edir[0] | 0x20, edir + 2);
        for (char *p = wpath; *p; p++) if (*p == '\\') *p = '/';
        char cmd[MAX_PATH + 256];
        snprintf(cmd, sizeof(cmd),
            "if [ ! -e /opt/app ]; then "
            "sudo -n mkdir -p /opt 2>/dev/null; "
            "ln -sf '%s' /opt/app 2>/dev/null || "
            "(sudo -n mkdir -p /opt/app 2>/dev/null && sudo -n cp -a '%s/'* /opt/app/ 2>/dev/null); "
            "fi", wpath, wpath);
        bridge_exec_print(backend, cmd);
    }

    /* Clone repo */
    if (app->repo[0]) {
        printf("[clone] %s\n", app->repo);
        bridge_exec_print(backend,
            "sudo -n mkdir -p /opt/app 2>/dev/null; "
            "sudo -n chown $(whoami) /opt/app 2>/dev/null; "
            "mkdir -p /opt/app 2>/dev/null");
        char *esc_repo = shell_escape(app->repo);
        char cmd[2048];
        snprintf(cmd, sizeof(cmd),
            "if [ -d /opt/app/.git ]; then cd /opt/app && git pull 2>&1; "
            "else git clone --depth 1 %s /opt/app 2>&1; fi",
            esc_repo ? esc_repo : "''");
        free(esc_repo);
        bridge_exec_print(backend, cmd);
        printf("\n");
    }

    /* Setup */
    if (app->setup[0]) {
        printf("[setup] %s\n", app->setup);
        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "cd /opt/app 2>/dev/null; %s 2>&1", app->setup);
        bridge_exec_print(backend, cmd);
        printf("\n");
    }

    /* Export distro snapshot after deps+setup for portability */
    if (app->deps[0] || app->setup[0] || app->repo[0])
        export_app_distro(app);

    /* Start (runs in foreground — Ctrl+C to stop) */
    if (app->start[0]) {
        printf("[start] %s\n", app->start);
        if (app->port > 0)
            printf("[start] Server will listen on port %d\n", app->port);
        printf("[start] Press Ctrl+C to stop.\n\n");
        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "cd /opt/app 2>/dev/null; %s 2>&1", app->start);
        bridge_exec_print(backend, cmd);
    } else {
        printf("No start command. Dropping into shell.\n\n");
        run_cli_repl(backend);
    }

    return 0;
}

/* ======================================================================
 * Interactive CLI REPL (used with --cli or as GUI fallback)
 * ====================================================================== */
static int run_cli_repl(linux_backend_t *backend) {
    printf("Interactive Linux shell. Type commands, 'exit' to quit.\n\n");

    char line[4096];
    while (1) {
        printf("$ ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin))
            break;

        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (len == 0) continue;
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0)
            break;

        bridge_exec_print(backend, line);
    }
    return 0;
}

static void print_usage(const char *prog) {
    printf("linux-template — Windows apps with Linux inside\n\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("  No arguments    Launch the GUI (or run app.json if present)\n");
    printf("  --cli           Run in terminal mode instead of GUI\n");
    printf("  --distro NAME   WSL distribution name (default: Ubuntu)\n");
    printf("  -v, --verbose   Enable verbose logging\n");
    printf("  -h, --help      Show this help\n");
}

static const char *backend_type_name(linux_backend_type_t t) {
    switch (t) {
    case LINUX_BACKEND_NONE:    return "None";
    case LINUX_BACKEND_NATIVE:  return "Native (Linux host)";
    case LINUX_BACKEND_WSL2:    return "WSL2";
    case LINUX_BACKEND_WHPX:    return "WHPX (Hyper-V)";
    case LINUX_BACKEND_QEMU:    return "QEMU (auto-accelerated)";
    case LINUX_BACKEND_TINYEMU: return "TinyEMU (embedded emulation)";
    default:                    return "Unknown";
    }
}

int main(int argc, char *argv[]) {
#ifdef _WIN32
    /* Prevent multiple instances — WebView2 takes an exclusive lock on its
     * cache directory, so a second instance hangs forever.  Use a named mutex
     * derived from the exe path so each app gets its own lock. */
    {
        char mutex_name[MAX_PATH + 32];
        char exe_path[MAX_PATH];
        GetModuleFileNameA(NULL, exe_path, MAX_PATH);
        /* Turn backslashes into underscores for a valid mutex name */
        for (char *p = exe_path; *p; p++)
            if (*p == '\\' || *p == ':') *p = '_';
        snprintf(mutex_name, sizeof(mutex_name), "Global\\linbox_%s", exe_path);
        HANDLE hmutex = CreateMutexA(NULL, TRUE, mutex_name);
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            MessageBoxA(NULL, "This app is already running.",
                        "Already Running", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        /* hmutex intentionally not closed — stays alive with process */
    }

    /* Redirect WebView2 cache to a 'cache' subdirectory next to our exe */
    {
        char cache_dir[MAX_PATH];
        GetModuleFileNameA(NULL, cache_dir, MAX_PATH);
        char *s = strrchr(cache_dir, '\\');
        if (s) *s = '\0';
        strncat(cache_dir, "\\cache", MAX_PATH - strlen(cache_dir) - 1);
        CreateDirectoryA(cache_dir, NULL);
        SetEnvironmentVariableA("WEBVIEW2_USER_DATA_FOLDER", cache_dir);
    }

    /* Create a Job Object so ALL child processes (including WebView2)
     * are automatically killed when this exe exits. This prevents
     * zombie msedgewebview2.exe processes. */
    {
        HANDLE job = CreateJobObject(NULL, NULL);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {0};
            jeli.BasicLimitInformation.LimitFlags =
                JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                JOB_OBJECT_LIMIT_BREAKAWAY_OK;  /* allow explicitly detached helpers */
            SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                    &jeli, sizeof(jeli));
            AssignProcessToJobObject(job, GetCurrentProcess());
            /* Job handle intentionally not closed — stays alive with process */
        }
    }
#endif

    /* ---- Check for app.json first ---- */
    app_config_t app;
    if (load_app_json(&app)) {
        printf("=== %s ===\n", app.name);
        printf("Loaded app.json configuration.\n\n");

        /* Each app gets its own WSL distro for portability. */
        char app_distro[256];
        make_distro_name(app.name, app_distro, sizeof(app_distro));

        /* Check for an exported rootfs.tar.gz in the app's linux/ folder.
         * If present, WSL will import it to create the per-app distro
         * with all deps/setup already installed — no reinstall needed. */
        char rootfs_path[MAX_PATH] = "";
        char exe_dir[MAX_PATH] = "";
#ifdef _WIN32
        GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
        { char *s = strrchr(exe_dir, '\\'); if (s) *s = '\0'; }

        /* Auto-invalidate stale snapshot if app.json setup changed */
        invalidate_stale_snapshot(&app, exe_dir);
        snprintf(rootfs_path, sizeof(rootfs_path),
                 "%s\\linux\\rootfs.tar.gz", exe_dir);
        if (GetFileAttributesA(rootfs_path) == INVALID_FILE_ATTRIBUTES)
            rootfs_path[0] = '\0';  /* not found */
#endif

        linux_config_t config = {0};
        config.distro_name = app_distro;
        config.tar_gz_path = rootfs_path[0] ? rootfs_path : NULL;
        config.timeout_ms = 7200000; /* 2 hours per exec — CUDA builds + vLLM install */

        linux_backend_t *backend = linux_detect_backend(&config);
        char app_error[1024] = "";
        int install_required = 0;

        if (!backend) {
            install_required = 1;
            snprintf(app_error, sizeof(app_error),
                     "No Linux backend found. WSL is not installed.");
        } else {
            linux_error_t rc = backend->start(backend, &config);
            if (rc != LINUX_OK) {
                snprintf(app_error, sizeof(app_error),
                         "Failed to start %s: %s%s%s",
                         backend->name, linux_error_string(rc),
                         backend->last_error ? " — " : "",
                         backend->last_error ? backend->last_error(backend) : "");
                backend->destroy(backend);
                backend = NULL;
            }
        }

        if (!backend) {
            show_setup_required(app_error, install_required);
            return 1;
        }

        service_manager_t svc_mgr;
        svc_init(&svc_mgr, backend);

        /* Check if --cli was passed (quick scan — full parsing is below for non-app.json) */
        int app_cli = 0;
        for (int i = 1; i < argc; i++)
            if (strcmp(argv[i], "--cli") == 0) { app_cli = 1; break; }
        /* Also check verbose flag for app.json path */
        for (int i = 1; i < argc; i++)
            if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0)
                config.verbose = 1;

        int result = 0;
        if (app_cli) {
            result = run_app_json_cli(backend, &app);
#if defined(HAVE_WEBVIEW) && (defined(_WIN32) || defined(__APPLE__))
        } else {
            result = run_app_json(backend, &svc_mgr, &app);
#else
        } else {
            /* No GUI available on this platform */
            printf("No GUI available. Running in CLI mode.\n\n");
            result = run_app_json_cli(backend, &app);
#endif
        }
        svc_stop_all(&svc_mgr);
        stop_bridge_with_backend(backend);
        backend->stop(backend);
#ifdef _WIN32
        /* Belt-and-suspenders: terminate the per-app distro so the WSL VM
         * doesn't idle in memory after the app window closes. The python
         * server inside the distro cannot terminate its own distro (WSL2
         * forbids self-termination), so we make the call here from the
         * Windows host where it works. Silent on errors — if WSL is gone
         * already, or never installed, a popup would be worse than
         * nothing. CREATE_NO_WINDOW keeps the wsl.exe console flash hidden. */
        {
            char wsl_cmd[512];
            snprintf(wsl_cmd, sizeof(wsl_cmd),
                     "wsl.exe --terminate %s", app_distro);
            STARTUPINFOA wsi = { sizeof(wsi) };
            PROCESS_INFORMATION wpi = { 0 };
            if (CreateProcessA(NULL, wsl_cmd, NULL, NULL, FALSE,
                               CREATE_NO_WINDOW, NULL, NULL, &wsi, &wpi)) {
                WaitForSingleObject(wpi.hProcess, 5000);
                CloseHandle(wpi.hProcess);
                CloseHandle(wpi.hThread);
            }
        }
#else
        /* On Windows, skip destroy — ExitProcess() in WinMain kills all
         * threads before memory is freed, avoiding a use-after-free race
         * with detached WebView2/exec threads that reference the backend. */
        backend->destroy(backend);
#endif
        return result;
    }

    /* ---- Normal mode (no app.json) — GUI by default ---- */
    linux_config_t config = {0};
    int cli_mode = 0;

    /* Template gets its own portable distro too */
    char template_distro[256] = "linbox-template";
    char template_rootfs[MAX_PATH] = "";
#ifdef _WIN32
    {
        char edir[MAX_PATH];
        GetModuleFileNameA(NULL, edir, MAX_PATH);
        { char *s = strrchr(edir, '\\'); if (s) *s = '\0'; }

        /* Check for exported snapshot first, then minimal base */
        snprintf(template_rootfs, sizeof(template_rootfs),
                 "%s\\linux\\rootfs.tar.gz", edir);
        if (GetFileAttributesA(template_rootfs) == INVALID_FILE_ATTRIBUTES) {
            snprintf(template_rootfs, sizeof(template_rootfs),
                     "%s\\linux\\ubuntu-base.tar.gz", edir);
            if (GetFileAttributesA(template_rootfs) == INVALID_FILE_ATTRIBUTES)
                template_rootfs[0] = '\0';
        }
    }
#endif
    config.distro_name = template_distro;
    config.tar_gz_path = template_rootfs[0] ? template_rootfs : NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cli") == 0) {
            cli_mode = 1;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            config.verbose = 1;
        } else if (strcmp(argv[i], "--distro") == 0 && i + 1 < argc) {
            config.distro_name = argv[++i];
        } else if (strcmp(argv[i], "--gui") == 0) {
            /* accepted for backwards compat, GUI is already default */
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Banner */
    printf("=== linux-template ===\n\n");

    /* Platform */
#ifdef _WIN32
    printf("Platform: Windows\n");
#elif defined(__linux__)
    printf("Platform: Linux\n");
#else
    printf("Platform: Unknown\n");
#endif

    /* Detect best backend */
    linux_backend_type_t best = linux_detect_best_type();
    printf("Best backend: %s\n\n", backend_type_name(best));

    /* Create backend */
    linux_backend_t *backend = linux_detect_backend(&config);
    char error_detail[1024] = "";
    int install_required = 0;

    if (!backend) {
        install_required = 1;
        snprintf(error_detail, sizeof(error_detail),
                 "No Linux backend found. WSL is not installed.");
    } else {
        printf("Selected backend: %s\n", backend->name);
        linux_error_t rc = backend->start(backend, &config);
        if (rc != LINUX_OK) {
            snprintf(error_detail, sizeof(error_detail),
                     "Failed to start %s: %s%s%s",
                     backend->name, linux_error_string(rc),
                     backend->last_error ? " — " : "",
                     backend->last_error ? backend->last_error(backend) : "");
            backend->destroy(backend);
            backend = NULL;
        }
    }

    if (!backend) {
        if (!cli_mode)
            show_setup_required(error_detail, install_required);
        else {
            fprintf(stderr, "ERROR: %s\n", error_detail);
            if (install_required)
                fprintf(stderr, "Install WSL: wsl --install\n");
            else
                fprintf(stderr, "WSL was detected. Try: wsl --shutdown, then retry.\n");
        }
        return 1;
    }

    printf("Backend started successfully.\n\n");

    /* Initialize service manager */
    service_manager_t svc_mgr;
    svc_init(&svc_mgr, backend);

    /* Launch GUI or CLI */
    if (!cli_mode) {
#ifdef HAVE_WEBVIEW
        printf("Launching GUI...\n");
        int gui_result = gui_run_webview(backend, &config, &svc_mgr);
        if (gui_result >= 0) {
            svc_stop_all(&svc_mgr);
            backend->stop(backend);
#ifndef _WIN32
            backend->destroy(backend);
#endif
            return gui_result;
        }
        /* GUI failed (no WebView2) — fall through to CLI */
        printf("GUI unavailable, falling back to CLI.\n\n");
#else
        printf("No GUI available on this platform. Running in CLI mode.\n\n");
#endif
    }

    /* CLI mode — interactive Linux shell */
    bridge_exec_print(backend, "echo \"$(uname -s) $(uname -r) — $(cat /etc/os-release 2>/dev/null | grep PRETTY_NAME | cut -d= -f2 | tr -d '\"' || echo unknown)\"");
    printf("\n");
    run_cli_repl(backend);

    svc_stop_all(&svc_mgr);
    backend->stop(backend);
#ifndef _WIN32
    backend->destroy(backend);
#endif
    return 0;
}

#ifdef _WIN32
/* WinMain entry point — no console window.
 * ExitProcess() is called BEFORE main() returns to kill all threads
 * instantly. This prevents a use-after-free window where detached
 * threads (WebView2 callbacks, exec threads) could access the backend
 * after backend->destroy() frees it. The OS reclaims all memory on
 * process exit, so skipping destroy is safe and eliminates the race. */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hInst; (void)hPrev; (void)nShow;
    int result = main(__argc, __argv);
    ExitProcess((UINT)result);
    return result;  /* unreachable; satisfies compiler */
}
#endif
