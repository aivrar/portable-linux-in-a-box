#include "linux/detect.h"
#include "bridge/bridge.h"
#include "bridge/http_bridge.h"
#include "bridge/service.h"
#include "bridge/provision.h"
#include "bridge/json_escape.h"
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
 *     "deps": ["python", "git"],
 *     "setup": "pip install -r requirements.txt",
 *     "start": "python3 app.py --port 7860",
 *     "port": 7860,
 *     "terminal": true
 * }
 * ====================================================================== */
typedef struct {
    char name[256];
    char repo[1024];
    char deps[1024];   /* comma-separated: "python,node,base" */
    char setup[2048];
    char start[2048];
    char distro[256];
    int  port;
    int  width;
    int  height;
    int  terminal;     /* 1 = show terminal panel for users */
    int  loaded;
} app_config_t;

/* Find a key pattern in JSON only when outside of string values.
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
        while (*p && *p != '"' && i < buf_size - 1) {
            if (*p == '\\' && p[1]) { p++; }
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

static int load_app_json(app_config_t *app) {
    memset(app, 0, sizeof(*app));
    app->port = 7860;
    app->width = 1100;
    app->height = 750;
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

    json_extract(json, "name", app->name, sizeof(app->name));
    json_extract(json, "repo", app->repo, sizeof(app->repo));
    json_extract(json, "deps", app->deps, sizeof(app->deps));
    json_extract(json, "setup", app->setup, sizeof(app->setup));
    json_extract(json, "start", app->start, sizeof(app->start));
    json_extract(json, "distro", app->distro, sizeof(app->distro));

    char tmp[32];
    if (json_extract(json, "port", tmp, sizeof(tmp))) app->port = atoi(tmp);
    if (json_extract(json, "width", tmp, sizeof(tmp))) app->width = atoi(tmp);
    if (json_extract(json, "height", tmp, sizeof(tmp))) app->height = atoi(tmp);
    if (json_extract(json, "terminal", tmp, sizeof(tmp)))
        app->terminal = (strcmp(tmp, "true") == 0 || atoi(tmp) == 1);

    free(json);

    if (!app->name[0]) strncpy(app->name, "Linux App", sizeof(app->name) - 1);
    app->loaded = 1;
    return 1;
}

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
"    S('Starting...','loading');\n"
"    L('Starting application...');\n"
"    const startResult=J(await appStart());\n"
"    if(startResult.started){\n"
"      L('Waiting for server...');\n"
"      for(let i=0;i<90;i++){\n"
"        await new Promise(r=>setTimeout(r,2000));\n"
"        try{const c=J(await appCheck());if(c.ready){break;}}catch(e){}\n"
"        L('.');\n"
"      }\n"
"      const c=J(await appCheck());\n"
"      if(c.ready){\n"
"        const url=J(await appUrl()).url;\n"
"        S('Running','ready');\n"
"        L('Server ready at '+url);\n"
"        frame.src=url;\n"
"        frame.style.display='flex';\n"
"        log.style.display='none';\n"
"        showingLog=false;\n"
"        document.getElementById('toggle-btn').textContent='Show Log';\n"
"      }else{S('Timeout','error');L('Server did not start in time.');}\n"
"    }else{\n"
"      S('Ready','ready');\n"
"      L('Application ready. Use the terminal to run commands.');\n"
"      if(termEnabled){toggleTerm();}\n"
"    }\n"
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
     * single quotes, double quotes, $variables, anything. */
    size_t write_len = strlen(command) + 256;
    char *write_cmd = (char *)malloc(write_len);
    if (!write_cmd) return -1;
    snprintf(write_cmd, write_len,
        "mkdir -p /tmp/linux_template; cat > /tmp/linux_template/_app_script.sh << '__SCRIPT_EOF__'\n"
        "%s\n"
        "__SCRIPT_EOF__\n",
        command);

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
    unsigned long timeout = 1800000; /* 30 minutes max for large installs */

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

    if (!done) push_log(ctx, "(timed out after 30 minutes)");
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
    growbuf_append(&script, "set +e\nexport PYTHONUNBUFFERED=1\n", 31);

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
                    "python3 -m pip --version >/dev/null 2>&1 || "
                    "(curl -fsSL https://bootstrap.pypa.io/get-pip.py | python3 - --user --break-system-packages --progress-bar on) 2>&1; "
                    "export PATH=$HOME/.local/bin:$PATH\n"
                    "echo 'python: done'\n");
            } else if (strcmp(dep, "node") == 0 || strcmp(dep, "nodejs") == 0) {
                n = snprintf(line, sizeof(line),
                    "echo 'Installing: node'\n"
                    "which node || (curl -fsSL https://deb.nodesource.com/setup_lts.x|sudo -E bash - && "
                    "sudo -n apt-get install -y -qq nodejs || sudo apk add nodejs npm) 2>&1\n"
                    "echo 'node: done'\n");
            } else if (strcmp(dep, "git") == 0) {
                n = snprintf(line, sizeof(line),
                    "echo 'Installing: git'\n"
                    "which git || (sudo -n apt-get install -y -qq git || sudo apk add git) 2>&1\n"
                    "echo 'git: done'\n");
            } else if (strcmp(dep, "base") == 0) {
                n = snprintf(line, sizeof(line),
                    "echo 'Installing: base dev tools'\n"
                    "which gcc || (sudo -n apt-get update -qq && sudo -n apt-get install -y -qq build-essential curl wget || "
                    "sudo apk add build-base curl wget) 2>&1\n"
                    "echo 'base: done'\n");
            } else {
                n = snprintf(line, sizeof(line),
                    "echo 'Installing: %s'\n"
                    "sudo -n apt-get install -y -qq %s 2>/dev/null || sudo apk add %s 2>/dev/null\n"
                    "echo '%s: done'\n", dep, dep, dep, dep);
            }
            growbuf_append(&script, line, (size_t)n);
            dep = COMPAT_STRTOK(NULL, ",; ", &saveptr);
        }
    }

    /* Clone repo */
    if (ctx->app->repo[0]) {
        char line[2048];
        int n = snprintf(line, sizeof(line),
            "echo '=== Cloning repository ==='\n"
            "sudo -n mkdir -p /opt/app 2>/dev/null; sudo -n chown $(whoami) /opt/app 2>/dev/null; mkdir -p /opt/app 2>/dev/null\n"
            "if [ -d /opt/app/.git ]; then cd /opt/app && git pull 2>&1; "
            "else git clone --depth 1 '%s' /opt/app 2>&1; fi\n"
            "echo 'Clone: done'\n", ctx->app->repo);
        growbuf_append(&script, line, (size_t)n);
    }

    /* Setup command */
    if (ctx->app->setup[0]) {
        char line[2048];
        int n = snprintf(line, sizeof(line),
            "echo '=== Running setup ==='\n"
            "cd /opt/app 2>/dev/null || true\n"
            "%s 2>&1\n"
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
         * The exported rootfs.tar.gz contains all installed deps + setup state.
         * On a new machine, the backend imports it automatically — no reinstall. */
#ifdef _WIN32
        if (ctx->app->distro[0]) {
            push_log(ctx, "Saving environment snapshot...");
            push_status(ctx, "Saving...", "loading");

            char export_path[MAX_PATH];
            char edir[MAX_PATH];
            GetModuleFileNameA(NULL, edir, MAX_PATH);
            { char *s = strrchr(edir, '\\'); if (s) *s = '\0'; }
            snprintf(export_path, sizeof(export_path),
                     "%s\\linux\\rootfs.tar.gz", edir);

            /* Build distro name same way as main() */
            char safe[128];
            int j = 0;
            for (int i = 0; ctx->app->name[i] && j < (int)sizeof(safe) - 1; i++) {
                char c = ctx->app->name[i];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_')
                    safe[j++] = c;
                else if (c == ' ')
                    safe[j++] = '_';
            }
            safe[j] = '\0';
            if (!safe[0]) strncpy(safe, "app", sizeof(safe));

            char wsl_cmd[MAX_PATH * 2];
            snprintf(wsl_cmd, sizeof(wsl_cmd),
                     "wsl.exe --export linbox-%s \"%s\"", safe, export_path);

            STARTUPINFOA si = {0};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi = {0};
            if (CreateProcessA(NULL, wsl_cmd, NULL, NULL, FALSE,
                               CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, 300000); /* up to 5 min */
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                push_log(ctx, "Environment saved. App folder is now portable.");
            } else {
                push_log(ctx, "Warning: could not export environment snapshot.");
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
        webview_return(ctx->w, task->seq, 0, "{\"started\":true}");
    } else {
        webview_return(ctx->w, task->seq, 0, "{\"started\":false}");
    }
    free(task);
    THREAD_FUNC_RET;
}

static void on_app_start(const char *seq, const char *req, void *arg) {
    (void)req;
    async_task_t *task = (async_task_t *)calloc(1, sizeof(async_task_t));
    task->ctx = (app_json_ctx_t *)arg;
    strncpy(task->seq, seq, sizeof(task->seq) - 1);
    compat_thread_launch(start_thread, task);
}

static THREAD_FUNC_DECL check_thread(THREAD_PARAM param) {
    async_task_t *task = (async_task_t *)param;
    app_json_ctx_t *ctx = task->ctx;
    int idx = svc_find(ctx->svc_mgr, "app");
    if (idx >= 0 && svc_check(ctx->svc_mgr, idx) == SERVICE_RUNNING)
        webview_return(ctx->w, task->seq, 0, "{\"ready\":true}");
    else
        webview_return(ctx->w, task->seq, 0, "{\"ready\":false}");
    free(task);
    THREAD_FUNC_RET;
}

static void on_app_check(const char *seq, const char *req, void *arg) {
    (void)req;
    async_task_t *task = (async_task_t *)calloc(1, sizeof(async_task_t));
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
    char html[16384];
    snprintf(html, sizeof(html), APP_JSON_HTML_TEMPLATE,
             app->name, app->name, app->terminal ? "true" : "false");

    webview_t w = create_webview_or_die();
    if (!w) return 1;

    webview_set_title(w, app->name);
    webview_set_size(w, app->width, app->height, WEBVIEW_HINT_NONE);

    app_json_ctx_t ctx = { w, backend, svc_mgr, app };
    webview_bind(w, "appSetup", on_app_setup, &ctx);
    webview_bind(w, "appStart", on_app_start, &ctx);
    webview_bind(w, "appCheck", on_app_check, &ctx);
    webview_bind(w, "appUrl",   on_app_url,   &ctx);
    webview_bind(w, "appExec",  on_app_exec,  &ctx);

    webview_set_html(w, html);
    webview_run(w);
    webview_destroy(w);
    return 0;
}
#endif /* HAVE_WEBVIEW && (_WIN32 || __APPLE__) */

/* Show a GUI window explaining WSL is needed */
static void show_setup_required(const char *detail) {
#ifdef HAVE_WEBVIEW
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
        "<button class='btn secondary' onclick='window.close()'>Close</button>"
        "</div></div></body></html>";

    size_t html_size = strlen(SETUP_HTML) + strlen(detail) + 64;
    char *html = (char *)malloc(html_size);
    if (html) {
        snprintf(html, html_size, SETUP_HTML, detail);
        webview_t w = webview_create(0, NULL);
        if (w) {
            webview_set_title(w, "Setup Required");
            webview_set_size(w, 580, 620, WEBVIEW_HINT_NONE);
            webview_set_html(w, html);
            free(html);
            webview_run(w);
            webview_destroy(w);
            return;
        }
        free(html);
    }
#endif
    /* Fallback to console */
    fprintf(stderr, "ERROR: %s\n", detail);
    fprintf(stderr, "Install WSL: wsl --install\n");
}

/* ======================================================================
 * CLI mode for app.json apps (used on Linux or when WebView2 is absent)
 *
 * Does the full setup sequence: deps -> clone -> setup -> start,
 * all in the terminal with streamed output.
 * ====================================================================== */
/* Export the per-app WSL distro to rootfs.tar.gz for portability */
static void export_app_distro(const char *app_name) {
#ifdef _WIN32
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

    char exe_dir[MAX_PATH];
    GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    { char *s = strrchr(exe_dir, '\\'); if (s) *s = '\0'; }

    char export_path[MAX_PATH];
    snprintf(export_path, sizeof(export_path),
             "%s\\linux\\rootfs.tar.gz", exe_dir);

    printf("[snapshot] Saving environment to %s ...\n", export_path);

    char wsl_cmd[MAX_PATH * 2];
    snprintf(wsl_cmd, sizeof(wsl_cmd),
             "wsl.exe --export linbox-%s \"%s\"", safe, export_path);

    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};
    if (CreateProcessA(NULL, wsl_cmd, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 300000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        printf("[snapshot] Done. App folder is now portable.\n\n");
    } else {
        printf("[snapshot] Warning: could not save environment snapshot.\n\n");
    }
#else
    (void)app_name;
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
                        "python3 -m pip --version >/dev/null 2>&1 || "
                        "(curl -fsSL https://bootstrap.pypa.io/get-pip.py | python3 - --user --break-system-packages) 2>&1; "
                        "export PATH=$HOME/.local/bin:$PATH");
                } else if (strcmp(dep, "node") == 0 || strcmp(dep, "nodejs") == 0) {
                    snprintf(cmd, sizeof(cmd),
                        "which node || (curl -fsSL https://deb.nodesource.com/setup_lts.x | sudo -E bash - && "
                        "sudo -n apt-get install -y -qq nodejs || sudo apk add nodejs npm) 2>&1");
                } else if (strcmp(dep, "git") == 0) {
                    snprintf(cmd, sizeof(cmd),
                        "which git || (sudo -n apt-get install -y -qq git || sudo apk add git) 2>&1");
                } else if (strcmp(dep, "base") == 0) {
                    snprintf(cmd, sizeof(cmd),
                        "which gcc || (sudo -n apt-get update -qq && sudo -n apt-get install -y -qq build-essential curl wget || "
                        "sudo apk add build-base curl wget) 2>&1");
                } else {
                    snprintf(cmd, sizeof(cmd),
                        "sudo -n apt-get install -y -qq %s 2>/dev/null || sudo apk add %s 2>/dev/null",
                        dep, dep);
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

    /* Clone repo */
    if (app->repo[0]) {
        printf("[clone] %s\n", app->repo);
        bridge_exec_print(backend,
            "sudo -n mkdir -p /opt/app 2>/dev/null; "
            "sudo -n chown $(whoami) /opt/app 2>/dev/null; "
            "mkdir -p /opt/app 2>/dev/null");
        char cmd[2048];
        snprintf(cmd, sizeof(cmd),
            "if [ -d /opt/app/.git ]; then cd /opt/app && git pull 2>&1; "
            "else git clone --depth 1 '%s' /opt/app 2>&1; fi",
            app->repo);
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
        export_app_distro(app->name);

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
                JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
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

        /* Each app gets its own WSL distro for portability.
         * The distro name is derived from the app name so each app
         * is isolated and can be exported/imported independently. */
        char app_distro[256];
        {
            char safe[128];
            int j = 0;
            for (int i = 0; app.name[i] && j < (int)sizeof(safe) - 1; i++) {
                char c = app.name[i];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_')
                    safe[j++] = c;
                else if (c == ' ')
                    safe[j++] = '_';
            }
            safe[j] = '\0';
            if (!safe[0]) strncpy(safe, "app", sizeof(safe));
            snprintf(app_distro, sizeof(app_distro), "linbox-%s", safe);
        }

        /* Check for an exported rootfs.tar.gz in the app's linux/ folder.
         * If present, WSL will import it to create the per-app distro
         * with all deps/setup already installed — no reinstall needed. */
        char rootfs_path[MAX_PATH] = "";
        char exe_dir[MAX_PATH] = "";
#ifdef _WIN32
        GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
        { char *s = strrchr(exe_dir, '\\'); if (s) *s = '\0'; }
        snprintf(rootfs_path, sizeof(rootfs_path),
                 "%s\\linux\\rootfs.tar.gz", exe_dir);
        if (GetFileAttributesA(rootfs_path) == INVALID_FILE_ATTRIBUTES)
            rootfs_path[0] = '\0';  /* not found */
#endif

        linux_config_t config = {0};
        config.distro_name = app_distro;
        config.tar_gz_path = rootfs_path[0] ? rootfs_path : NULL;
        config.timeout_ms = 300000; /* 5 minutes per exec for app installs */

        linux_backend_t *backend = linux_detect_backend(&config);
        char app_error[1024] = "";

        if (!backend) {
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
            /* Show setup-required GUI */
            show_setup_required(app_error);
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
        backend->stop(backend);
        backend->destroy(backend);
        return result;
    }

    /* ---- Normal mode (no app.json) — GUI by default ---- */
    linux_config_t config = {0};
    config.distro_name = "Ubuntu";
    int cli_mode = 0;

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

    if (!backend) {
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
            show_setup_required(error_detail);
        else {
            fprintf(stderr, "ERROR: %s\n", error_detail);
            fprintf(stderr, "Install WSL: wsl --install\n");
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
            backend->destroy(backend);
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
    backend->destroy(backend);
    return 0;
}

#ifdef _WIN32
/* WinMain entry point — no console window */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hInst; (void)hPrev; (void)nShow;
    int result = main(__argc, __argv);
    /* Force exit — kills all threads, WebView2, and WSL child processes */
    ExitProcess((UINT)result);
    return result;  /* unreachable; satisfies compiler */
}
#endif
