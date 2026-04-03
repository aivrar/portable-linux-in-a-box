#include "gui.h"
#include "../bridge/http_bridge.h"
#include "../bridge/service.h"
#include "../bridge/provision.h"
#include "../bridge/json_escape.h"
#include "../compat.h"
#include "webview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

/* --------------------------------------------------------------------------
 * Embedded HTML
 * -------------------------------------------------------------------------- */
static const char *GUI_HTML =
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"<meta charset='utf-8'>\n"
"<title>linux-template</title>\n"
"<style>\n"
"*{margin:0;padding:0;box-sizing:border-box}\n"
"body{font-family:'Segoe UI',system-ui,sans-serif;background:#0d1117;color:#c9d1d9;display:flex;flex-direction:column;height:100vh}\n"
"header{background:#161b22;border-bottom:1px solid #30363d;padding:12px 20px;display:flex;align-items:center;gap:12px}\n"
"header h1{font-size:16px;font-weight:600;color:#f0f6fc}\n"
".badge{font-size:11px;padding:2px 8px;border-radius:12px;background:#238636;color:#fff;font-weight:500}\n"
".tabs{background:#161b22;display:flex;border-bottom:1px solid #30363d}\n"
".tab{padding:8px 16px;cursor:pointer;font-size:13px;color:#8b949e;border-bottom:2px solid transparent;user-select:none}\n"
".tab:hover{color:#c9d1d9}.tab.active{color:#f0f6fc;border-bottom-color:#f78166}\n"
".panel{flex:1;display:none;flex-direction:column;overflow:hidden}.panel.active{display:flex}\n"
"#terminal{flex:1;overflow-y:auto;padding:16px;font-family:'Cascadia Code','Consolas',monospace;font-size:13px;line-height:1.6}\n"
".line{white-space:pre-wrap;word-break:break-all}\n"
".line.cmd{color:#79c0ff}.line.cmd::before{content:'$ ';color:#7ee787}\n"
".line.out{color:#c9d1d9}.line.err{color:#f85149}.line.sys{color:#8b949e;font-style:italic}\n"
".input-bar{background:#161b22;border-top:1px solid #30363d;padding:10px 16px;display:flex;gap:8px}\n"
".prompt{color:#7ee787;font-family:monospace;line-height:32px}\n"
"input[type=text],textarea{background:#0d1117;border:1px solid #30363d;border-radius:6px;color:#f0f6fc;padding:6px 12px;font-family:'Cascadia Code','Consolas',monospace;font-size:13px;outline:none}\n"
"input[type=text]:focus,textarea:focus{border-color:#58a6ff}\n"
".btn{padding:6px 14px;border:1px solid #30363d;border-radius:6px;background:#21262d;color:#c9d1d9;cursor:pointer;font-size:13px}\n"
".btn:hover{background:#30363d;border-color:#8b949e}\n"
".btn.primary{background:#238636;border-color:#238636;color:#fff}.btn.primary:hover{background:#2ea043}\n"
".btn.danger{background:#da3633;border-color:#da3633;color:#fff}.btn.danger:hover{background:#f85149}\n"
".btn:disabled{opacity:.5;cursor:default}\n"
"/* Services */\n"
".svc-list{flex:1;overflow-y:auto;padding:16px;display:flex;flex-direction:column;gap:10px}\n"
".svc-card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:14px 18px}\n"
".svc-header{display:flex;align-items:center;justify-content:space-between;margin-bottom:8px}\n"
".svc-name{font-weight:600;font-size:14px;color:#f0f6fc}\n"
".svc-state{font-size:11px;padding:2px 8px;border-radius:12px;font-weight:500}\n"
".svc-state.stopped{background:#30363d;color:#8b949e}\n"
".svc-state.starting{background:#9e6a03;color:#fff}\n"
".svc-state.running{background:#238636;color:#fff}\n"
".svc-state.failed{background:#da3633;color:#fff}\n"
".svc-details{font-size:12px;color:#8b949e;margin-bottom:10px;font-family:monospace}\n"
".svc-actions{display:flex;gap:8px}\n"
".svc-log{margin-top:8px;background:#0d1117;border:1px solid #30363d;border-radius:6px;padding:8px 12px;font-family:monospace;font-size:12px;max-height:120px;overflow-y:auto;color:#8b949e;white-space:pre-wrap}\n"
"/* HTTP */\n"
".http-form{padding:16px;display:flex;flex-direction:column;gap:10px}\n"
".http-form label{font-size:13px;color:#8b949e}\n"
"textarea{min-height:80px;resize:vertical}\n"
"#http-output{flex:1;overflow-y:auto;margin:0 16px 16px;padding:12px;background:#161b22;border-radius:6px;font-family:monospace;font-size:13px;white-space:pre-wrap;color:#c9d1d9;border:1px solid #30363d}\n"
"/* Files */\n"
".file-panel{padding:16px;display:flex;flex-direction:column;gap:10px}\n"
".file-panel .row{display:flex;gap:8px;align-items:end}\n"
".file-panel .row .field{flex:1;display:flex;flex-direction:column;gap:4px}\n"
".file-panel .row .field label{font-size:12px;color:#8b949e}\n"
"#file-content{flex:1;margin:0 16px 16px;padding:12px;background:#161b22;border:1px solid #30363d;border-radius:6px;font-family:monospace;font-size:13px;color:#c9d1d9;overflow-y:auto;white-space:pre-wrap}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<header>\n"
"  <h1>linux-template</h1>\n"
"  <span class='badge' id='backend-badge'>starting...</span>\n"
"  <select id='distro-select' style='background:#21262d;color:#c9d1d9;border:1px solid #30363d;border-radius:4px;padding:4px 10px;font-size:12px;outline:none;min-width:120px'></select>\n"
"</header>\n"
"<div class='tabs' id='tabs'>\n"
"  <div class='tab active' data-tab='terminal'>Terminal</div>\n"
"  <div class='tab' data-tab='services'>Services</div>\n"
"  <div class='tab' data-tab='files'>Files</div>\n"
"  <div class='tab' data-tab='http'>HTTP</div>\n"
"  <div class='tab' data-tab='setup'>Setup</div>\n"
"  <div class='tab' data-tab='create' style='margin-left:auto;margin-right:8px;background:#238636;color:#fff;border-radius:6px;padding:6px 16px;font-weight:600'>Create App</div>\n"
"</div>\n"
"\n"
"<!-- Terminal -->\n"
"<div class='panel active' id='panel-terminal'>\n"
"  <div id='terminal'></div>\n"
"  <div class='input-bar'>\n"
"    <span class='prompt'>$</span>\n"
"    <input id='cmd-input' type='text' placeholder='Type a Linux command...' autofocus spellcheck='false' style='flex:1'>\n"
"    <button class='btn primary' onclick='runCmd()'>Run</button>\n"
"  </div>\n"
"</div>\n"
"\n"
"<!-- Services -->\n"
"<div class='panel' id='panel-services'>\n"
"  <div style='padding:12px 16px;border-bottom:1px solid #30363d;color:#8b949e;font-size:12px'>\n"
"    <b style='color:#f0f6fc'>Background Services</b> — Start and manage long-running Linux processes like web servers, AI models, or databases. Apps you create with Create App manage their own services automatically. This tab lets you control them manually.\n"
"  </div>\n"
"  <div class='svc-list' id='svc-list'>\n"
"    <div style='color:#8b949e;text-align:center;padding:40px'>No services running.<br>Register one below, or create an app from the Create App tab.</div>\n"
"  </div>\n"
"  <div class='input-bar'>\n"
"    <input id='svc-name' type='text' placeholder='Service name' style='width:120px'>\n"
"    <input id='svc-cmd' type='text' placeholder='Command (e.g. python3 -m http.server 8080)' style='flex:1'>\n"
"    <input id='svc-port' type='text' placeholder='Port' style='width:70px'>\n"
"    <button class='btn primary' onclick='registerService()'>Register</button>\n"
"  </div>\n"
"</div>\n"
"\n"
"<!-- Files -->\n"
"<div class='panel' id='panel-files'>\n"
"  <div style='padding:12px 16px;border-bottom:1px solid #30363d;color:#8b949e;font-size:12px'>\n"
"    <b style='color:#f0f6fc'>Linux Files</b> — Read, write, and check files inside the Linux environment. Useful for editing config files, checking logs, or verifying that setup worked correctly.\n"
"  </div>\n"
"  <div class='file-panel'>\n"
"    <div class='row'>\n"
"      <div class='field'><label>Linux Path</label>\n"
"        <input id='file-path' type='text' placeholder='/etc/os-release'></div>\n"
"      <button class='btn primary' onclick='readFile()'>Read</button>\n"
"      <button class='btn' onclick='checkExists()'>Exists?</button>\n"
"    </div>\n"
"    <div class='row'>\n"
"      <div class='field'><label>Write content to file</label>\n"
"        <textarea id='file-write' placeholder='File content to write...'></textarea></div>\n"
"      <button class='btn' onclick='writeFile()' style='align-self:end'>Write</button>\n"
"    </div>\n"
"  </div>\n"
"  <div id='file-content'>File content will appear here...</div>\n"
"</div>\n"
"\n"
"<!-- Setup / Provisioning -->\n"
"<div class='panel' id='panel-setup'>\n"
"  <div class='file-panel'>\n"
"    <h2 style='color:#f0f6fc;font-size:15px;margin-bottom:4px'>Install Tools (for this machine)</h2>\n"
"    <p style='font-size:13px;color:#8b949e;margin-bottom:12px'>These buttons install tools into your local Linux environment so you can use them from the Terminal tab. This does not create apps — use the <span style='color:#7ee787;cursor:pointer' onclick='document.querySelector(\"[data-tab=create]\").click()'>Create App</span> tab for that.</p>\n"
"    <div style='display:flex;flex-wrap:wrap;gap:8px'>\n"
"      <button class='btn primary' onclick='runProvision(\"base\")'>Base Dev Tools</button>\n"
"      <button class='btn primary' onclick='runProvision(\"python\")'>Python 3 + pip</button>\n"
"      <button class='btn primary' onclick='runProvision(\"node\")'>Node.js + npm</button>\n"
"      <button class='btn primary' onclick='runProvision(\"vllm\")'>vLLM (AI)</button>\n"
"      <button class='btn primary' onclick='runProvision(\"llamacpp\")'>llama.cpp</button>\n"
"      <button class='btn' onclick='runProvision(\"update\")' style='margin-left:8px'>Update All</button>\n"
"    </div>\n"
"    <div style='margin-top:12px'>\n"
"      <label style='font-size:12px;color:#8b949e'>Or run a custom install command:</label>\n"
"      <div style='display:flex;gap:8px;margin-top:4px'>\n"
"        <input id='custom-install' type='text' style='flex:1' placeholder='sudo apt-get install -y ...'>\n"
"        <button class='btn' onclick='runCustomInstall()'>Install</button>\n"
"      </div>\n"
"    </div>\n"
"  </div>\n"
"  <div id='setup-log' style='flex:1;overflow-y:auto;margin:0 16px 16px;padding:12px;background:#161b22;border:1px solid #30363d;border-radius:6px;font-family:monospace;font-size:12px;color:#8b949e;white-space:pre-wrap'>Setup log will appear here...</div>\n"
"</div>\n"
"\n"
"<!-- HTTP -->\n"
"<div class='panel' id='panel-http'>\n"
"  <div style='padding:12px 16px;border-bottom:1px solid #30363d;color:#8b949e;font-size:12px'>\n"
"    <b style='color:#f0f6fc'>HTTP Tester</b> — Send requests to services running in Linux. Useful for checking if an AI server or web app is responding correctly after you start it.\n"
"  </div>\n"
"  <div class='http-form'>\n"
"    <label>URL</label>\n"
"    <input id='http-url' type='text' value='http://localhost:8000/v1/models'>\n"
"    <div style='display:flex;gap:8px'>\n"
"      <button class='btn primary' onclick='httpGet()'>GET</button>\n"
"      <button class='btn' onclick='httpPost()'>POST</button>\n"
"    </div>\n"
"    <label>POST Body (JSON)</label>\n"
"    <textarea id='http-body' placeholder='{\"prompt\": \"Hello\"}'></textarea>\n"
"  </div>\n"
"  <div id='http-output'>Response will appear here...</div>\n"
"</div>\n"
"\n"
"<!-- Create App -->\n"
"<div class='panel' id='panel-create' style='overflow-y:auto'>\n"
"  <div class='file-panel' style='max-width:600px;margin:0 auto;padding-bottom:20px'>\n"
"    <h2 style='color:#f0f6fc;font-size:16px;margin-bottom:4px'>Create a New App (for distribution)</h2>\n"
"    <p style='font-size:13px;color:#8b949e;margin-bottom:8px'>Package an app that other people can download and run. Pick a template or fill in manually:</p>\n"
"    <div style='display:flex;gap:8px;margin-bottom:16px;align-items:center'>\n"
"      <select id='sample-select' onchange='loadSample(this.value)' style='flex:1;background:#21262d;color:#c9d1d9;border:1px solid #30363d;border-radius:6px;padding:8px 12px;font-size:13px;outline:none;cursor:pointer'>\n"
"        <option value=''>-- Choose a template --</option>\n"
"        <optgroup label='AI / Machine Learning'>\n"
"          <option value='comfyui'>ComfyUI — AI image generation</option>\n"
"          <option value='llama'>Llama Chat — local AI chatbot (llama.cpp)</option>\n"
"          <option value='vllm'>vLLM Server — GPU-accelerated LLM inference</option>\n"
"          <option value='ollama'>Ollama — easy local AI models</option>\n"
"          <option value='stableDiffusion'>Stable Diffusion WebUI — AI image generation</option>\n"
"          <option value='textgen'>Text Generation WebUI — chat with AI models</option>\n"
"        </optgroup>\n"
"        <optgroup label='Web Development'>\n"
"          <option value='devserver'>Python Dev Server — simple HTTP server</option>\n"
"          <option value='flask'>Flask App — Python web framework</option>\n"
"          <option value='express'>Express.js — Node.js web server</option>\n"
"          <option value='nextjs'>Next.js — React web framework</option>\n"
"        </optgroup>\n"
"        <optgroup label='Databases'>\n"
"          <option value='postgres'>PostgreSQL — relational database</option>\n"
"          <option value='redis'>Redis — in-memory data store</option>\n"
"        </optgroup>\n"
"        <optgroup label='Tools'>\n"
"          <option value='jupyter'>Jupyter Notebook — interactive Python</option>\n"
"          <option value='codeserver'>Code Server — VS Code in browser</option>\n"
"        </optgroup>\n"
"      </select>\n"
"      <button class='btn' onclick='clearForm()' style='font-size:12px;color:#8b949e;white-space:nowrap'>Clear</button>\n"
"    </div>\n"
"    <div style='display:flex;flex-direction:column;gap:12px'>\n"
"      <div><label style='font-size:12px;color:#8b949e;display:block;margin-bottom:4px'>App Name</label>\n"
"        <input id='ca-name' type='text' placeholder='My AI App' style='width:100%'></div>\n"
"      <div><label style='font-size:12px;color:#8b949e;display:block;margin-bottom:4px'>Git Repository URL (optional)</label>\n"
"        <input id='ca-repo' type='text' placeholder='https://github.com/user/project' style='width:100%'></div>\n"
"      <div><label style='font-size:12px;color:#8b949e;display:block;margin-bottom:4px'>Setup Command (runs once on first launch)</label>\n"
"        <input id='ca-setup' type='text' placeholder='pip install -r requirements.txt' style='width:100%'></div>\n"
"      <div><label style='font-size:12px;color:#8b949e;display:block;margin-bottom:4px'>Start Command (launches the server)</label>\n"
"        <input id='ca-start' type='text' placeholder='python3 app.py --port 7860' style='width:100%'></div>\n"
"      <div style='display:flex;gap:12px'>\n"
"        <div style='flex:1'><label style='font-size:12px;color:#8b949e;display:block;margin-bottom:4px'>Port</label>\n"
"          <input id='ca-port' type='text' value='7860' style='width:100%'></div>\n"
"        <div style='flex:1'><label style='font-size:12px;color:#8b949e;display:block;margin-bottom:4px'>WSL Distro</label>\n"
"          <input id='ca-distro' type='text' value='Ubuntu' style='width:100%'></div>\n"
"      </div>\n"
"      <div><label style='font-size:12px;color:#8b949e;display:block;margin-bottom:4px'>Dependencies (auto-installed before setup)</label>\n"
"        <input id='ca-deps' type='text' placeholder='python, git, node, base, rust' style='width:100%'></div>\n"
"      <div style='display:flex;gap:12px'>\n"
"        <div style='flex:1'><label style='font-size:12px;color:#8b949e;display:block;margin-bottom:4px'>Window Width</label>\n"
"          <input id='ca-width' type='text' value='1100' style='width:100%'></div>\n"
"        <div style='flex:1'><label style='font-size:12px;color:#8b949e;display:block;margin-bottom:4px'>Window Height</label>\n"
"          <input id='ca-height' type='text' value='750' style='width:100%'></div>\n"
"      </div>\n"
"      <label style='font-size:12px;color:#8b949e;display:flex;align-items:center;gap:6px;cursor:pointer'>\n"
"        <input id='ca-terminal' type='checkbox'> Include terminal for users\n"
"      </label>\n"
"      <button class='btn primary' onclick='createApp()' style='padding:10px;font-size:14px;margin-top:8px'>Create &amp; Package App</button>\n"
"    </div>\n"
"  </div>\n"
"  <div id='action-area' style='margin:0 16px 16px'>\n"
"    <div style='display:flex;gap:8px;align-items:center;margin-bottom:8px' id='action-buttons' style='display:none'>\n"
"      <button class='btn primary' id='test-btn' onclick='testApp()' style='display:none'>Install &amp; Preview</button>\n"
"      <button class='btn' id='folder-btn' onclick='openAppFolder()' style='display:none'>Open App Folder</button>\n"
"      <span id='test-status' style='font-size:12px;color:#8b949e'></span>\n"
"    </div>\n"
"    <div id='app-log' style='min-height:60px;max-height:350px;overflow-y:auto;padding:12px;background:#0d1117;border:1px solid #30363d;border-radius:6px;font-family:monospace;font-size:12px;color:#c9d1d9;white-space:pre-wrap'></div>\n"
"  </div>\n"
"</div>\n"
"\n"
"<script>\n"
"document.addEventListener('contextmenu', e => e.preventDefault());\n"
"const term = document.getElementById('terminal');\n"
"const cmdInput = document.getElementById('cmd-input');\n"
"let busy = false;\n"
"let history = [], histIdx = -1;\n"
"function J(v){return typeof v==='string'?JSON.parse(v):v;}\n"
"\n"
"// Sample presets for Create App\n"
"const SAMPLES={\n"
"  comfyui:{name:'ComfyUI',repo:'https://github.com/comfyanonymous/ComfyUI',deps:'python, git',setup:'pip install -r requirements.txt',start:'python3 main.py --port 8188',port:'8188',terminal:true},\n"
"  llama:{name:'Llama Chat',repo:'',deps:'base',setup:'cd ~ && git clone --depth 1 https://github.com/ggerganov/llama.cpp && cd llama.cpp && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc) && mkdir -p ~/models && curl -fsSL -o ~/models/tinyllama.gguf https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_0.gguf',start:'~/llama.cpp/build/bin/llama-server -m ~/models/tinyllama.gguf --port 8080',port:'8080',terminal:false},\n"
"  vllm:{name:'vLLM Server',repo:'',deps:'python',setup:'export PATH=$HOME/.local/bin:$PATH; python3 -m pip install --user --break-system-packages vllm',start:'export PATH=$HOME/.local/bin:$PATH; python3 -m vllm.entrypoints.openai.api_server --model facebook/opt-125m --port 8000',port:'8000',terminal:true},\n"
"  ollama:{name:'Ollama',repo:'',deps:'base',setup:'curl -fsSL https://ollama.ai/install.sh | sh',start:'ollama serve',port:'11434',terminal:true},\n"
"  stableDiffusion:{name:'Stable Diffusion WebUI',repo:'https://github.com/AUTOMATIC1111/stable-diffusion-webui',deps:'python, git',setup:'bash webui.sh --skip-torch-cuda-test --exit',start:'bash webui.sh --listen --port 7860 --skip-torch-cuda-test',port:'7860',terminal:true},\n"
"  textgen:{name:'Text Generation WebUI',repo:'https://github.com/oobabooga/text-generation-webui',deps:'python, git',setup:'pip install -r requirements.txt',start:'python3 server.py --listen --listen-port 7861',port:'7861',terminal:true},\n"
"  devserver:{name:'Python Dev Server',repo:'',deps:'python',setup:'mkdir -p /opt/app && echo \"<h1>Hello from Linux</h1>\" > /opt/app/index.html',start:'cd /opt/app && python3 -m http.server 8000',port:'8000',terminal:true},\n"
"  flask:{name:'Flask App',repo:'',deps:'python',setup:'pip install flask && mkdir -p /opt/app && echo \"from flask import Flask\\napp = Flask(__name__)\\n@app.route(\\'/\\')\\ndef hello(): return \\'<h1>Hello from Flask</h1>\\'\" > /opt/app/app.py',start:'cd /opt/app && flask run --host=0.0.0.0 --port 5000',port:'5000',terminal:true},\n"
"  express:{name:'Express.js',repo:'',deps:'node',setup:'mkdir -p /opt/app && cd /opt/app && npm init -y && npm install express && echo \"const express=require(\\'express\\');const app=express();app.get(\\'/\\',(r,s)=>s.send(\\'<h1>Hello from Express</h1>\\'));app.listen(3000)\" > index.js',start:'cd /opt/app && node index.js',port:'3000',terminal:true},\n"
"  nextjs:{name:'Next.js',repo:'',deps:'node',setup:'npx --yes create-next-app@latest /opt/app --use-npm --yes',start:'cd /opt/app && npm run dev -- -p 3000',port:'3000',terminal:true},\n"
"  postgres:{name:'PostgreSQL',repo:'',deps:'',setup:'sudo apt-get update -qq && sudo apt-get install -y -qq postgresql postgresql-client',start:'sudo service postgresql start && echo \"PostgreSQL running on port 5432\" && tail -f /var/log/postgresql/postgresql-*-main.log',port:'5432',terminal:true},\n"
"  redis:{name:'Redis',repo:'',deps:'',setup:'sudo apt-get update -qq && sudo apt-get install -y -qq redis-server',start:'redis-server --port 6379 --protected-mode no',port:'6379',terminal:true},\n"
"  jupyter:{name:'Jupyter Notebook',repo:'',deps:'python',setup:'pip install jupyter',start:'jupyter notebook --ip=0.0.0.0 --port=8888 --no-browser --NotebookApp.token=\\'\\'',port:'8888',terminal:true},\n"
"  codeserver:{name:'Code Server',repo:'',deps:'base',setup:'curl -fsSL https://code-server.dev/install.sh | sh',start:'code-server --bind-addr 0.0.0.0:8080 --auth none',port:'8080',terminal:true}\n"
"};\n"
"function loadSample(id){\n"
"  const s=SAMPLES[id];if(!s)return;\n"
"  document.getElementById('ca-name').value=s.name;\n"
"  document.getElementById('ca-repo').value=s.repo;\n"
"  document.getElementById('ca-deps').value=s.deps;\n"
"  document.getElementById('ca-setup').value=s.setup;\n"
"  document.getElementById('ca-start').value=s.start;\n"
"  document.getElementById('ca-port').value=s.port;\n"
"  document.getElementById('ca-terminal').checked=s.terminal;\n"
"  document.getElementById('app-log').textContent='Loaded sample: '+s.name+'\\nReview the fields and click Create & Package App.';\n"
"}\n"
"function clearForm(){\n"
"  ['ca-name','ca-repo','ca-deps','ca-setup','ca-start'].forEach(id=>document.getElementById(id).value='');\n"
"  document.getElementById('ca-port').value='7860';\n"
"  document.getElementById('ca-terminal').checked=false;\n"
"  document.getElementById('app-log').textContent='';\n"
"  document.getElementById('sample-select').value='';\n"
"}\n"
"\n"
"// Sync distro picker → Create App form\n"
"document.getElementById('distro-select').addEventListener('change', e => {\n"
"  document.getElementById('ca-distro').value = e.target.value;\n"
"});\n"
"\n"
"// Tab switching\n"
"document.getElementById('tabs').addEventListener('click', e => {\n"
"  if (!e.target.dataset.tab) return;\n"
"  document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));\n"
"  document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));\n"
"  e.target.classList.add('active');\n"
"  document.getElementById('panel-' + e.target.dataset.tab).classList.add('active');\n"
"  if (e.target.dataset.tab === 'services') refreshServices();\n"
"});\n"
"\n"
"function addLine(text, cls) {\n"
"  const el = document.createElement('div');\n"
"  el.className = 'line ' + cls;\n"
"  el.textContent = text;\n"
"  term.appendChild(el);\n"
"  term.scrollTop = term.scrollHeight;\n"
"}\n"
"\n"
"// Terminal\n"
"async function runCmd() {\n"
"  if (busy) return;\n"
"  const cmd = cmdInput.value.trim();\n"
"  if (!cmd) return;\n"
"  history.push(cmd); histIdx = history.length;\n"
"  cmdInput.value = '';\n"
"  addLine(cmd, 'cmd');\n"
"  busy = true;\n"
"  try {\n"
"    const r = J(await linuxExec(cmd));\n"
"    if (r.stdout) addLine(r.stdout, 'out');\n"
"    if (r.stderr) addLine(r.stderr, 'err');\n"
"    if (r.exit_code !== 0) addLine('exit code: ' + r.exit_code, 'err');\n"
"  } catch(e) { addLine('Error: ' + e, 'err'); }\n"
"  busy = false;\n"
"  cmdInput.focus();\n"
"}\n"
"cmdInput.addEventListener('keydown', e => {\n"
"  if (e.key === 'Enter') runCmd();\n"
"  if (e.key === 'ArrowUp' && history.length) { histIdx = Math.max(0, histIdx-1); cmdInput.value = history[histIdx]; }\n"
"  if (e.key === 'ArrowDown' && history.length) { histIdx = Math.min(history.length, histIdx+1); cmdInput.value = history[histIdx] || ''; }\n"
"});\n"
"\n"
"// Services\n"
"async function refreshServices() {\n"
"  try {\n"
"    const svcs = J(await svcStatus());\n"
"    const list = document.getElementById('svc-list');\n"
"    if (!svcs.length) { list.innerHTML = '<div style=\"color:#8b949e;text-align:center;padding:40px\">No services registered.<br>Register one below.</div>'; return; }\n"
"    list.innerHTML = svcs.map((s,i) => `\n"
"      <div class='svc-card'>\n"
"        <div class='svc-header'>\n"
"          <span class='svc-name'>${s.name}</span>\n"
"          <span class='svc-state ${s.state}'>${s.state}</span>\n"
"        </div>\n"
"        <div class='svc-details'>Port: ${s.port || 'N/A'}${s.error ? ' | Error: '+s.error : ''}</div>\n"
"        <div class='svc-actions'>\n"
"          <button class='btn primary' onclick='startSvc(\"${s.name}\")' ${s.state==='running'?'disabled':''}>Start</button>\n"
"          <button class='btn danger' onclick='stopSvc(\"${s.name}\")' ${s.state!=='running'?'disabled':''}>Stop</button>\n"
"          <button class='btn' onclick='checkSvc(\"${s.name}\")'>Check</button>\n"
"          <button class='btn' onclick='viewLog(\"${s.name}\")'>Log</button>\n"
"        </div>\n"
"        <div class='svc-log' id='log-${s.name}' style='display:none'></div>\n"
"      </div>`).join('');\n"
"  } catch(e) { console.error(e); }\n"
"}\n"
"async function registerService() {\n"
"  const name = document.getElementById('svc-name').value.trim();\n"
"  const cmd = document.getElementById('svc-cmd').value.trim();\n"
"  const port = document.getElementById('svc-port').value.trim();\n"
"  if (!name || !cmd) return;\n"
"  try {\n"
"    await svcRegister(JSON.stringify({name, command:cmd, port:parseInt(port)||0}));\n"
"    document.getElementById('svc-name').value = '';\n"
"    document.getElementById('svc-cmd').value = '';\n"
"    document.getElementById('svc-port').value = '';\n"
"    refreshServices();\n"
"  } catch(e) { alert(e); }\n"
"}\n"
"async function startSvc(name) { try { await svcAction(JSON.stringify({action:'start',name})); setTimeout(refreshServices,1000); } catch(e) { alert(e); }}\n"
"async function stopSvc(name) { try { await svcAction(JSON.stringify({action:'stop',name})); refreshServices(); } catch(e) { alert(e); }}\n"
"async function checkSvc(name) { try { await svcAction(JSON.stringify({action:'check',name})); refreshServices(); } catch(e) { alert(e); }}\n"
"async function viewLog(name) {\n"
"  const el = document.getElementById('log-'+name);\n"
"  el.style.display = el.style.display==='none' ? 'block' : 'none';\n"
"  if (el.style.display === 'block') {\n"
"    try {\n"
"      const r = J(await linuxExec('tail -30 /tmp/linux_template/svc_'+name+'.log 2>/dev/null || echo No log yet'));\n"
"      el.textContent = r.stdout || r.stderr || 'Empty';\n"
"    } catch(e) { el.textContent = 'Error: '+e; }\n"
"  }\n"
"}\n"
"\n"
"// Files\n"
"async function readFile() {\n"
"  const path = document.getElementById('file-path').value.trim();\n"
"  if (!path) return;\n"
"  const out = document.getElementById('file-content');\n"
"  out.textContent = 'Reading...';\n"
"  try {\n"
"    const r = J(await linuxExec('cat ' + JSON.stringify(path)));\n"
"    out.textContent = r.stdout || r.stderr || '(empty file)';\n"
"  } catch(e) { out.textContent = 'Error: '+e; }\n"
"}\n"
"async function checkExists() {\n"
"  const path = document.getElementById('file-path').value.trim();\n"
"  if (!path) return;\n"
"  try {\n"
"    const r = J(await linuxExec('test -e '+JSON.stringify(path)+' && echo EXISTS || echo NOT_FOUND'));\n"
"    document.getElementById('file-content').textContent = r.stdout.trim();\n"
"  } catch(e) { document.getElementById('file-content').textContent = 'Error: '+e; }\n"
"}\n"
"async function writeFile() {\n"
"  const path = document.getElementById('file-path').value.trim();\n"
"  const content = document.getElementById('file-write').value;\n"
"  if (!path) return;\n"
"  try {\n"
"    const r = J(await linuxExec('cat > '+JSON.stringify(path)+\" << 'LTEOF'\\n\"+content+\"\\nLTEOF\"));\n"
"    document.getElementById('file-content').textContent = r.exit_code === 0 ? 'Written successfully.' : 'Error: '+(r.stderr||'unknown');\n"
"  } catch(e) { document.getElementById('file-content').textContent = 'Error: '+e; }\n"
"}\n"
"\n"
"// HTTP\n"
"async function httpGet() {\n"
"  const url = document.getElementById('http-url').value;\n"
"  document.getElementById('http-output').textContent = 'Requesting...';\n"
"  try {\n"
"    const r = J(await httpRequest(JSON.stringify({method:'GET',url})));\n"
"    document.getElementById('http-output').textContent = 'HTTP '+r.status+'\\n\\n'+r.body;\n"
"  } catch(e) { document.getElementById('http-output').textContent = 'Error: '+e; }\n"
"}\n"
"async function httpPost() {\n"
"  const url = document.getElementById('http-url').value;\n"
"  const body = document.getElementById('http-body').value;\n"
"  document.getElementById('http-output').textContent = 'Requesting...';\n"
"  try {\n"
"    const r = J(await httpRequest(JSON.stringify({method:'POST',url,body})));\n"
"    document.getElementById('http-output').textContent = 'HTTP '+r.status+'\\n\\n'+r.body;\n"
"  } catch(e) { document.getElementById('http-output').textContent = 'Error: '+e; }\n"
"}\n"
"\n"
"// Setup / Provisioning\n"
"async function runProvision(recipe) {\n"
"  const log = document.getElementById('setup-log');\n"
"  log.textContent = 'Running ' + recipe + ' setup...\\n';\n"
"  try {\n"
"    const r = J(await provisionRun(recipe));\n"
"    log.textContent += r.log || 'Done.';\n"
"    if (r.error) log.textContent += '\\nERROR: ' + r.error;\n"
"  } catch(e) { log.textContent += '\\nError: ' + e; }\n"
"}\n"
"async function runCustomInstall() {\n"
"  const cmd = document.getElementById('custom-install').value.trim();\n"
"  if (!cmd) return;\n"
"  const log = document.getElementById('setup-log');\n"
"  log.textContent = '$ ' + cmd + '\\n';\n"
"  try {\n"
"    const r = J(await linuxExec(cmd));\n"
"    if (r.stdout) log.textContent += r.stdout;\n"
"    if (r.stderr) log.textContent += r.stderr;\n"
"    log.textContent += '\\n' + (r.exit_code === 0 ? 'Done.' : 'Exit code: ' + r.exit_code);\n"
"  } catch(e) { log.textContent += '\\nError: ' + e; }\n"
"}\n"
"\n"
"// Create App\n"
"let lastAppPath = '';\n"
"const appLog = () => document.getElementById('app-log');\n"
"function logMsg(t) { const l=appLog(); l.textContent+=t+'\\n'; l.scrollTop=l.scrollHeight; }\n"
"\n"
"async function createApp() {\n"
"  const name = document.getElementById('ca-name').value.trim();\n"
"  if (!name) { alert('App name is required.'); return; }\n"
"  const config = {\n"
"    name: name,\n"
"    repo: document.getElementById('ca-repo').value.trim(),\n"
"    deps: document.getElementById('ca-deps').value.trim(),\n"
"    setup: document.getElementById('ca-setup').value.trim(),\n"
"    start: document.getElementById('ca-start').value.trim(),\n"
"    distro: document.getElementById('ca-distro').value.trim() || 'Ubuntu',\n"
"    port: String(parseInt(document.getElementById('ca-port').value) || 7860),\n"
"    width: String(parseInt(document.getElementById('ca-width').value) || 1100),\n"
"    height: String(parseInt(document.getElementById('ca-height').value) || 750),\n"
"    terminal: document.getElementById('ca-terminal').checked ? 'true' : 'false'\n"
"  };\n"
"  appLog().textContent = '';\n"
"  document.getElementById('test-btn').style.display = 'none';\n"
"  document.getElementById('folder-btn').style.display = 'none';\n"
"  document.getElementById('test-status').textContent = '';\n"
"  logMsg('Creating app: ' + name + '...');\n"
"  try {\n"
"    const r = J(await createAppPackage(JSON.stringify(config)));\n"
"    if (r.log) logMsg(r.log);\n"
"    if (r.error) {\n"
"      logMsg('ERROR: ' + r.error);\n"
"    } else {\n"
"      logMsg('App created successfully!');\n"
"      logMsg('');\n"
"      lastAppPath = r.path || '';\n"
"      document.getElementById('test-btn').style.display = '';\n"
"      document.getElementById('folder-btn').style.display = '';\n"
"      document.getElementById('test-status').textContent = 'Ready';\n"
"    }\n"
"  } catch(e) { logMsg('Error: ' + e); }\n"
"}\n"
"\n"
"async function testApp() {\n"
"  if (!lastAppPath) return;\n"
"  document.getElementById('test-status').textContent = 'Installing...';\n"
"  document.getElementById('test-btn').disabled = true;\n"
"  logMsg('--- Install & Preview ---');\n"
"\n"
"  const deps = document.getElementById('ca-deps').value.trim();\n"
"  const repo = document.getElementById('ca-repo').value.trim();\n"
"  const setup = document.getElementById('ca-setup').value.trim();\n"
"  const start = document.getElementById('ca-start').value.trim();\n"
"  const port = document.getElementById('ca-port').value.trim();\n"
"\n"
"  // Build one big script — use ; instead of newlines\n"
"  let script = '';\n"
"  if (deps) {\n"
"    script += 'echo \"=== Installing dependencies ===\"; ';\n"
"    deps.split(/[,;]/).map(d=>d.trim()).filter(Boolean).forEach(dep => {\n"
"      script += 'echo \"Installing: ' + dep + '\"; ';\n"
"      if (dep==='python'||dep==='python3') script += 'python3 -m pip --version >/dev/null 2>&1 || (curl -fsSL https://bootstrap.pypa.io/get-pip.py | python3 - --user --break-system-packages) 2>&1; export PATH=$HOME/.local/bin:$PATH; ';\n"
"      else if (dep==='node'||dep==='nodejs') script += 'which node || (curl -fsSL https://deb.nodesource.com/setup_lts.x|sudo -n -E bash - && sudo -n apt-get install -y nodejs || echo \"Need sudo — run: sudo apt-get install -y nodejs\") 2>&1; ';\n"
"      else if (dep==='git') script += 'which git || (sudo -n apt-get install -y git || echo \"Need sudo — run: sudo apt-get install -y git\") 2>&1; ';\n"
"      else if (dep==='base') script += 'which gcc || (sudo -n apt-get update && sudo -n apt-get install -y build-essential curl wget || echo \"Need sudo — run: sudo apt-get install -y build-essential\") 2>&1; ';\n"
"      else script += 'sudo -n apt-get install -y ' + dep + ' 2>&1 || echo \"Need sudo for ' + dep + '\"; ';\n"
"      script += 'echo \"' + dep + ': done\"; ';\n"
"    });\n"
"  }\n"
"  if (repo) {\n"
"    script += 'echo \"=== Cloning repository ===\"; ';\n"
"    script += 'if [ -d /opt/app/.git ]; then cd /opt/app && git pull 2>&1; else git clone --depth 1 \\'' + repo + '\\' /opt/app 2>&1; fi; ';\n"
"  }\n"
"  if (setup) {\n"
"    script += 'echo \"=== Running setup ===\"; ';\n"
"    script += 'cd /opt/app 2>/dev/null; ';\n"
"    script += setup + ' 2>&1; ';\n"
"  }\n"
"  script += 'echo \"=== Setup complete ===\"; ';\n"
"\n"
"  try {\n"
"    // Start the script in background with streaming\n"
"    await streamExec(script);\n"
"\n"
"    // Poll for output every 2 seconds\n"
"    let linesSeen = 0;\n"
"    let done = false;\n"
"    let exitCode = -1;\n"
"    while (!done) {\n"
"      await new Promise(r => setTimeout(r, 2000));\n"
"      const p = J(await streamPoll(String(linesSeen)));\n"
"      if (p.lines && p.lines.length > 0) {\n"
"        p.lines.forEach(l => { logMsg(l); });\n"
"        linesSeen += p.lines.length;\n"
"        appLog().scrollTop = appLog().scrollHeight;\n"
"      }\n"
"      if (p.done) { done = true; exitCode = p.exitCode; }\n"
"    }\n"
"\n"
"    if (exitCode !== 0) {\n"
"      document.getElementById('test-status').textContent = 'Setup failed (exit ' + exitCode + ')';\n"
"      logMsg('FAILED — check output above');\n"
"    } else if (start) {\n"
"      logMsg('Starting server on port ' + port + '...');\n"
"      document.getElementById('test-status').textContent = 'Starting server...';\n"
"      await linuxExec('mkdir -p /tmp/linux_template; nohup sh -c \\'' + start + '\\' > /tmp/linux_template/_test_app.log 2>&1 &');\n"
"      let ready = false;\n"
"      for (let i = 0; i < 30; i++) {\n"
"        await new Promise(r => setTimeout(r, 2000));\n"
"        try {\n"
"          const c = J(await httpRequest(JSON.stringify({method:'GET',url:'http://localhost:'+port+'/'})));\n"
"          if (c.status >= 200 && c.status < 500) { ready = true; break; }\n"
"        } catch(e) {}\n"
"        logMsg('.');\n"
"      }\n"
"      if (ready) {\n"
"        logMsg('Server running on port ' + port);\n"
"        document.getElementById('test-status').textContent = 'Running on port ' + port;\n"
"        logMsg('App is working! Zip the folder and share it.');\n"
"      } else {\n"
"        const log = J(await linuxExec('cat /tmp/linux_template/_test_app.log 2>/dev/null | tail -20'));\n"
"        logMsg('Server did not start in time:'); logMsg(log.stdout||'');\n"
"        document.getElementById('test-status').textContent = 'Server timeout';\n"
"      }\n"
"    } else {\n"
"      logMsg('Setup complete! Zip the folder and share it.');\n"
"    }\n"
"  } catch(e) {\n"
"    logMsg('Error: ' + e);\n"
"    document.getElementById('test-status').textContent = 'Error';\n"
"  }\n"
"  document.getElementById('test-btn').disabled = false;\n"
"}\n"
"\n"
"async function openAppFolder() {\n"
"  if (lastAppPath) await openFolder(lastAppPath);\n"
"}\n"
"\n"
"// Distro picker\n"
"async function loadDistros() {\n"
"  try {\n"
"    const r = J(await listDistros());\n"
"    const sel = document.getElementById('distro-select');\n"
"    sel.innerHTML = '';\n"
"    let picked = '';\n"
"    (r.distros||[]).forEach(d => {\n"
"      const opt = document.createElement('option');\n"
"      opt.value = d; opt.textContent = d;\n"
"      if (d === r.current) opt.selected = true;\n"
"      sel.appendChild(opt);\n"
"      if (!picked) picked = d;\n"
"    });\n"
"    // Sync to Create App form\n"
"    const distroInput = document.getElementById('ca-distro');\n"
"    if (distroInput && picked) distroInput.value = sel.value || picked;\n"
"  } catch(e) {}\n"
"}\n"
"loadDistros();\n"
"\n"
"// Startup\n"
"(async function() {\n"
"  addLine('Welcome to linux-template', 'sys');\n"
"  addLine('', 'sys');\n"
"  addLine('This is a Linux environment running inside Windows.', 'sys');\n"
"  addLine('You can:', 'sys');\n"
"  addLine('  - Type Linux commands in this terminal', 'sys');\n"
"  addLine('  - Install tools from the Setup tab', 'sys');\n"
"  addLine('  - Create distributable apps from the Create App tab', 'sys');\n"
"  addLine('', 'sys');\n"
"  try {\n"
"    const r = J(await linuxExec('echo $(uname -s -r) - $(cat /etc/os-release 2>/dev/null | grep PRETTY_NAME | cut -d= -f2 | tr -d \\'\"\\' || echo unknown)'));\n"
"    document.getElementById('backend-badge').textContent = r.stdout.trim();\n"
"    addLine('Connected: ' + r.stdout.trim(), 'sys');\n"
"  } catch(e) { addLine('Backend info unavailable: ' + e, 'err'); }\n"
"  addLine('', 'sys');\n"
"})();\n"
"</script>\n"
"</body></html>\n";

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */
/* json_escape() is now provided by ../bridge/json_escape.h */

static void json_unescape(const char *src, size_t len, char *dst, size_t *out_len) {
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\\' && i + 1 < len) {
            switch (src[++i]) {
            case 'n': dst[j++] = '\n'; break;
            case 't': dst[j++] = '\t'; break;
            case 'r': dst[j++] = '\r'; break;
            case '\\': dst[j++] = '\\'; break;
            case '"': dst[j++] = '"'; break;
            case '/': dst[j++] = '/'; break;
            default: dst[j++] = src[i]; break;
            }
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
    *out_len = j;
}

/* Extract first JSON string argument from ["..."] */
static char *extract_json_string_arg(const char *req) {
    const char *start = strchr(req, '"');
    const char *end = start ? strrchr(req, '"') : NULL;
    if (!start || !end || start == end) return NULL;
    start++;
    size_t len = (size_t)(end - start);
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    size_t out_len;
    json_unescape(start, len, out, &out_len);
    return out;
}

/* Find value of "key":"value" in a JSON string */
static int json_find_string(const char *json, const char *key,
                            char *buf, size_t buf_size) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    p = strchr(p, '"');
    if (!p) return 0;
    p++;
    const char *e = strchr(p, '"');
    if (!e) return 0;
    size_t len = (size_t)(e - p);
    if (len >= buf_size) len = buf_size - 1;
    memcpy(buf, p, len);
    buf[len] = '\0';
    return 1;
}

static int json_find_int(const char *json, const char *key) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p && (*p == ':' || *p == ' ')) p++;
    return atoi(p);
}

/* --------------------------------------------------------------------------
 * Binding context — holds all state the JS callbacks need
 * -------------------------------------------------------------------------- */
typedef struct {
    webview_t          w;
    linux_backend_t   *backend;
    service_manager_t *svc_mgr;
} bind_ctx_t;

/* --------------------------------------------------------------------------
 * Async dispatch — runs WebView callbacks on background threads
 * so the UI never freezes during backend operations.
 * -------------------------------------------------------------------------- */
typedef struct {
    bind_ctx_t *ctx;
    char        seq[64];
    char       *req;           /* heap copy of request */
    void      (*work_fn)(bind_ctx_t *ctx, const char *seq, const char *req);
} async_work_t;

static THREAD_FUNC_DECL async_worker(THREAD_PARAM param) {
    async_work_t *w = (async_work_t *)param;
    w->work_fn(w->ctx, w->seq, w->req);
    free(w->req);
    free(w);
    THREAD_FUNC_RET;
}

static void dispatch_async(const char *seq, const char *req, void *arg,
    void (*work_fn)(bind_ctx_t*, const char*, const char*)) {
    async_work_t *w = (async_work_t *)calloc(1, sizeof(async_work_t));
    if (!w) return;
    w->ctx = (bind_ctx_t *)arg;
    strncpy(w->seq, seq, sizeof(w->seq) - 1);
    w->req = req ? strdup(req) : NULL;
    w->work_fn = work_fn;
    compat_thread_launch(async_worker, w);
}

/* --------------------------------------------------------------------------
 * JS binding: linuxExec(command) — async
 * -------------------------------------------------------------------------- */
static void exec_work(bind_ctx_t *ctx, const char *seq, const char *req) {
    char *command = extract_json_string_arg(req);
    if (!command) {
        webview_return(ctx->w, seq, 1, "\"Invalid arguments\"");
        return;
    }

    char *out = NULL, *err = NULL;
    int code = -1;
    linux_error_t rc = ctx->backend->exec(ctx->backend, command, &out, &err, &code);
    free(command);

    if (rc != LINUX_OK) {
        char msg[256];
        snprintf(msg, sizeof(msg), "\"%s\"", linux_error_string(rc));
        webview_return(ctx->w, seq, 1, msg);
        return;
    }

    char *out_esc = json_escape(out ? out : "");
    char *err_esc = json_escape(err ? err : "");
    size_t sz = strlen(out_esc) + strlen(err_esc) + 128;
    char *resp = (char *)malloc(sz);
    snprintf(resp, sz, "{\"stdout\":\"%s\",\"stderr\":\"%s\",\"exit_code\":%d}",
             out_esc, err_esc, code);
    webview_return(ctx->w, seq, 0, resp);
    free(out); free(err); free(out_esc); free(err_esc); free(resp);
}

static void on_linux_exec(const char *seq, const char *req, void *arg) {
    dispatch_async(seq, req, arg, exec_work);
}

/* --------------------------------------------------------------------------
 * JS binding: httpRequest({method, url, body})
 * -------------------------------------------------------------------------- */
static void http_work(bind_ctx_t *ctx, const char *seq, const char *req) {
    char *json_arg = extract_json_string_arg(req);
    if (!json_arg) { webview_return(ctx->w, seq, 1, "\"Invalid arguments\""); return; }

    char method[8] = "GET", url[1024] = "", body[4096] = "";
    json_find_string(json_arg, "method", method, sizeof(method));
    json_find_string(json_arg, "url", url, sizeof(url));
    json_find_string(json_arg, "body", body, sizeof(body));
    free(json_arg);

    if (!url[0]) { webview_return(ctx->w, seq, 1, "\"No URL\""); return; }

    http_response_t resp;
    linux_error_t rc = (strcmp(method, "POST") == 0)
        ? http_post(url, "application/json", body, strlen(body), &resp)
        : http_get(url, &resp);

    if (rc != LINUX_OK) {
        char msg[256];
        snprintf(msg, sizeof(msg), "\"HTTP failed: %s\"", linux_error_string(rc));
        webview_return(ctx->w, seq, 1, msg);
        return;
    }

    char *body_esc = json_escape(resp.body ? resp.body : "");
    size_t sz = strlen(body_esc) + 128;
    char *out = (char *)malloc(sz);
    snprintf(out, sz, "{\"status\":%d,\"body\":\"%s\"}", resp.status_code, body_esc);
    webview_return(ctx->w, seq, 0, out);
    free(body_esc); free(out);
    http_response_free(&resp);
}

static void on_http_request(const char *seq, const char *req, void *arg) {
    dispatch_async(seq, req, arg, http_work);
}

/* --------------------------------------------------------------------------
 * JS binding: svcStatus() -> JSON array
 * -------------------------------------------------------------------------- */
static void svc_status_work(bind_ctx_t *ctx, const char *seq, const char *req) {
    (void)req;
    if (!ctx->svc_mgr) {
        webview_return(ctx->w, seq, 0, "[]");
        return;
    }
    char *json = svc_status_json(ctx->svc_mgr);
    webview_return(ctx->w, seq, 0, json ? json : "[]");
    free(json);
}

static void on_svc_status(const char *seq, const char *req, void *arg) {
    dispatch_async(seq, req, arg, svc_status_work);
}

/* --------------------------------------------------------------------------
 * JS binding: svcRegister({name, command, port})
 * -------------------------------------------------------------------------- */
static void svc_register_work(bind_ctx_t *ctx, const char *seq, const char *req) {
    if (!ctx->svc_mgr) {
        webview_return(ctx->w, seq, 1, "\"No service manager\"");
        return;
    }
    char *json_arg = extract_json_string_arg(req);
    if (!json_arg) { webview_return(ctx->w, seq, 1, "\"Invalid args\""); return; }

    char name[64] = "", command[1024] = "";
    json_find_string(json_arg, "name", name, sizeof(name));
    json_find_string(json_arg, "command", command, sizeof(command));
    int port = json_find_int(json_arg, "port");
    free(json_arg);

    if (!name[0] || !command[0]) {
        webview_return(ctx->w, seq, 1, "\"Name and command required\"");
        return;
    }

    char health[256] = "";
    if (port > 0)
        snprintf(health, sizeof(health), "http://localhost:%d/", port);

    int idx = svc_register(ctx->svc_mgr, name, command, health[0] ? health : NULL, port);
    if (idx < 0) {
        webview_return(ctx->w, seq, 1, "\"Max services reached\"");
        return;
    }
    webview_return(ctx->w, seq, 0, "\"ok\"");
}

static void on_svc_register(const char *seq, const char *req, void *arg) {
    /* Register is fast (no exec), run inline */
    svc_register_work((bind_ctx_t *)arg, seq, req);
}

/* --------------------------------------------------------------------------
 * JS binding: svcAction({action, name}) — async
 * -------------------------------------------------------------------------- */
static void svc_action_work(bind_ctx_t *ctx, const char *seq, const char *req) {
    if (!ctx->svc_mgr) {
        webview_return(ctx->w, seq, 1, "\"No service manager\"");
        return;
    }
    char *json_arg = extract_json_string_arg(req);
    if (!json_arg) { webview_return(ctx->w, seq, 1, "\"Invalid args\""); return; }

    char action[16] = "", name[64] = "";
    json_find_string(json_arg, "action", action, sizeof(action));
    json_find_string(json_arg, "name", name, sizeof(name));
    free(json_arg);

    int idx = svc_find(ctx->svc_mgr, name);
    if (idx < 0) { webview_return(ctx->w, seq, 1, "\"Service not found\""); return; }

    if (strcmp(action, "start") == 0) svc_start(ctx->svc_mgr, idx);
    else if (strcmp(action, "stop") == 0) svc_stop(ctx->svc_mgr, idx);
    else if (strcmp(action, "check") == 0) svc_check(ctx->svc_mgr, idx);

    webview_return(ctx->w, seq, 0, "\"ok\"");
}

static void on_svc_action(const char *seq, const char *req, void *arg) {
    dispatch_async(seq, req, arg, svc_action_work);
}

/* --------------------------------------------------------------------------
 * JS binding: provisionRun(recipe_name) -> {log, error}
 * -------------------------------------------------------------------------- */
typedef struct {
    growbuf_t log;
} provision_log_ctx_t;

static void provision_progress(int step, int total, const char *msg, void *user) {
    provision_log_ctx_t *ctx = (provision_log_ctx_t *)user;
    char line[512];
    int n = snprintf(line, sizeof(line), "[%d/%d] %s\n", step, total, msg);
    growbuf_append(&ctx->log, line, (size_t)n);
}

static void provision_work(bind_ctx_t *ctx, const char *seq, const char *req) {
    char *recipe_name = extract_json_string_arg(req);
    if (!recipe_name) {
        webview_return(ctx->w, seq, 1, "\"Invalid recipe name\"");
        return;
    }

    provision_recipe_t recipe;
    if (strcmp(recipe_name, "base") == 0)        recipe = provision_recipe_base();
    else if (strcmp(recipe_name, "python") == 0)  recipe = provision_recipe_python();
    else if (strcmp(recipe_name, "node") == 0)    recipe = provision_recipe_node();
    else if (strcmp(recipe_name, "vllm") == 0)    recipe = provision_recipe_vllm();
    else if (strcmp(recipe_name, "llamacpp") == 0) recipe = provision_recipe_llamacpp();
    else if (strcmp(recipe_name, "update") == 0)  recipe = provision_recipe_update();
    else {
        free(recipe_name);
        webview_return(ctx->w, seq, 1, "\"Unknown recipe\"");
        return;
    }
    free(recipe_name);

    provision_log_ctx_t pctx;
    growbuf_init(&pctx.log, 4096);

    linux_error_t rc = provision_run(ctx->backend, &recipe, provision_progress, &pctx);

    char *log_text = growbuf_finish(&pctx.log);
    char *log_esc = json_escape(log_text ? log_text : "");

    size_t sz = strlen(log_esc) + 256;
    char *resp = (char *)malloc(sz);
    if (rc == LINUX_OK) {
        snprintf(resp, sz, "{\"log\":\"%s\",\"error\":null}", log_esc);
    } else {
        snprintf(resp, sz, "{\"log\":\"%s\",\"error\":\"%s\"}",
                 log_esc, linux_error_string(rc));
    }

    webview_return(ctx->w, seq, 0, resp);
    free(log_text); free(log_esc); free(resp);
}

static void on_provision_run(const char *seq, const char *req, void *arg) {
    dispatch_async(seq, req, arg, provision_work);
}

/* --------------------------------------------------------------------------
 * JS binding: createAppPackage(config_json) — generates a distributable app
 * -------------------------------------------------------------------------- */
static void create_app_work(bind_ctx_t *ctx, const char *seq, const char *req) {
    char *config_json = extract_json_string_arg(req);
    if (!config_json) {
        webview_return(ctx->w, seq, 1, "\"Invalid config\"");
        return;
    }

    growbuf_t log;
    growbuf_init(&log, 4096);
    char line[512];
    int n;

    /* Parse config fields */
    char name[256] = "", repo[1024] = "", deps[1024] = "";
    char setup[2048] = "", start[2048] = "";
    char distro[256] = "Ubuntu", terminal_s[16] = "false";
    char port_s[16] = "7860", width_s[16] = "1100", height_s[16] = "750";
    json_find_string(config_json, "name", name, sizeof(name));
    json_find_string(config_json, "repo", repo, sizeof(repo));
    json_find_string(config_json, "deps", deps, sizeof(deps));
    json_find_string(config_json, "setup", setup, sizeof(setup));
    json_find_string(config_json, "start", start, sizeof(start));
    json_find_string(config_json, "distro", distro, sizeof(distro));
    json_find_string(config_json, "port", port_s, sizeof(port_s));
    json_find_string(config_json, "width", width_s, sizeof(width_s));
    json_find_string(config_json, "height", height_s, sizeof(height_s));
    json_find_string(config_json, "terminal", terminal_s, sizeof(terminal_s));
    free(config_json);

    /* Find our exe directory */
    char exe_dir[MAX_PATH];
#ifdef _WIN32
    GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    char *sl = strrchr(exe_dir, '\\');
    if (sl) *sl = '\0';
#else
    snprintf(exe_dir, sizeof(exe_dir), ".");
#endif

    /* Create output directory: exe_dir/apps/<name>/ */
    char app_dir[MAX_PATH];
    /* Sanitize name for directory */
    char safe_name[256];
    {
        int j = 0;
        for (int i = 0; name[i] && j < (int)sizeof(safe_name) - 1; i++) {
            char c = name[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_')
                safe_name[j++] = c;
            else if (c == ' ')
                safe_name[j++] = '_';
        }
        safe_name[j] = '\0';
        if (!safe_name[0]) strncpy(safe_name, "my_app", sizeof(safe_name));
    }

    snprintf(app_dir, sizeof(app_dir), "%s\\apps\\%s", exe_dir, safe_name);
    n = snprintf(line, sizeof(line), "Creating: %s\n", app_dir);
    growbuf_append(&log, line, (size_t)n);

    /* Create directories */
    char mkdir_cmd[MAX_PATH + 32];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "%s\\linux", app_dir);
#ifdef _WIN32
    CreateDirectoryA(app_dir - (app_dir - app_dir), NULL); /* apps/ parent */
    {
        char apps_dir[MAX_PATH];
        snprintf(apps_dir, sizeof(apps_dir), "%s\\apps", exe_dir);
        CreateDirectoryA(apps_dir, NULL);
    }
    CreateDirectoryA(app_dir, NULL);
    CreateDirectoryA(mkdir_cmd, NULL);
#endif

    /* Write app.json */
    char json_path[MAX_PATH];
    snprintf(json_path, sizeof(json_path), "%s\\app.json", app_dir);
    {
        FILE *f = fopen(json_path, "w");
        if (!f) {
            char *esc = json_escape("Failed to create app.json");
            size_t sz = strlen(esc) + 64;
            char *resp = (char *)malloc(sz);
            snprintf(resp, sz, "{\"log\":\"\",\"error\":\"%s\"}", esc);
            webview_return(ctx->w, seq, 0, resp);
            free(esc); free(resp); growbuf_free(&log);
            return;
        }
        fprintf(f, "{\n");
        fprintf(f, "    \"name\": \"%s\",\n", name);
        fprintf(f, "    \"distro\": \"%s\",\n", distro);
        if (repo[0]) fprintf(f, "    \"repo\": \"%s\",\n", repo);
        if (deps[0]) fprintf(f, "    \"deps\": \"%s\",\n", deps);
        if (setup[0]) fprintf(f, "    \"setup\": \"%s\",\n", setup);
        if (start[0]) fprintf(f, "    \"start\": \"%s\",\n", start);
        fprintf(f, "    \"port\": %s,\n", port_s);
        fprintf(f, "    \"width\": %s,\n", width_s);
        fprintf(f, "    \"height\": %s,\n", height_s);
        fprintf(f, "    \"terminal\": %s\n",
                strcmp(terminal_s, "true") == 0 ? "true" : "false");
        fprintf(f, "}\n");
        fclose(f);
    }
    n = snprintf(line, sizeof(line), "Created app.json\n");
    growbuf_append(&log, line, (size_t)n);

    /* Copy exe */
    {
        char src[MAX_PATH], dst[MAX_PATH];
        GetModuleFileNameA(NULL, src, MAX_PATH);
        snprintf(dst, sizeof(dst), "%s\\%s.exe", app_dir, safe_name);
        if (CopyFileA(src, dst, FALSE))
            n = snprintf(line, sizeof(line), "Copied %s.exe\n", safe_name);
        else
            n = snprintf(line, sizeof(line), "Warning: could not copy exe\n");
        growbuf_append(&log, line, (size_t)n);
    }

    /* Copy webview.dll */
    {
        char src[MAX_PATH], dst[MAX_PATH];
        snprintf(src, sizeof(src), "%s\\webview.dll", exe_dir);
        snprintf(dst, sizeof(dst), "%s\\webview.dll", app_dir);
        if (CopyFileA(src, dst, FALSE)) {
            n = snprintf(line, sizeof(line), "Copied webview.dll\n");
            growbuf_append(&log, line, (size_t)n);
        }
    }

    /* Copy Linux images */
    {
        const char *images[] = {
            "bzImage", "initramfs.cpio.gz",  /* x86_64: WHPX/QEMU */
            "bbl64.bin", "rootfs-riscv64.ext2",  /* RISC-V: TinyEMU */
            NULL
        };
        for (int i = 0; images[i]; i++) {
            char src[MAX_PATH], dst[MAX_PATH];
            snprintf(src, sizeof(src), "%s\\linux\\%s", exe_dir, images[i]);
            snprintf(dst, sizeof(dst), "%s\\linux\\%s", app_dir, images[i]);
            if (CopyFileA(src, dst, FALSE)) {
                n = snprintf(line, sizeof(line), "Copied linux/%s\n", images[i]);
                growbuf_append(&log, line, (size_t)n);
            }
        }
    }

    n = snprintf(line, sizeof(line),
        "\nApp packaged successfully!\n"
        "Folder: %s\n\n"
        "To distribute: zip the folder and share it.\n"
        "Users unzip and double-click %s.exe — it just works.\n",
        app_dir, safe_name);
    growbuf_append(&log, line, (size_t)n);

    char *log_text = growbuf_finish(&log);
    char *log_esc = json_escape(log_text ? log_text : "");
    char *path_esc = json_escape(app_dir);
    size_t sz = strlen(log_esc) + strlen(path_esc) + 128;
    char *resp = (char *)malloc(sz);
    snprintf(resp, sz, "{\"log\":\"%s\",\"error\":null,\"path\":\"%s\"}",
             log_esc, path_esc);
    webview_return(ctx->w, seq, 0, resp);
    free(log_text); free(log_esc); free(path_esc); free(resp);
}

/* --------------------------------------------------------------------------
 * JS binding: listDistros() — detect installed WSL distributions
 * -------------------------------------------------------------------------- */
static void list_distros_work(bind_ctx_t *ctx, const char *seq, const char *req) {
    (void)req;
    char resp[2048] = "{\"distros\":[],\"current\":\"\"}";

#ifdef _WIN32
    /* Read WSL distros from registry — no wsl.exe spawn, no console flash */
    HKEY lxss_key;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss",
        0, KEY_READ, &lxss_key);

    if (rc == ERROR_SUCCESS) {
        growbuf_t json;
        growbuf_init(&json, 512);
        growbuf_append(&json, "{\"distros\":[", 12);
        int first = 1;

        DWORD index = 0;
        wchar_t subkey_name[256];
        DWORD subkey_len;

        while (1) {
            subkey_len = 256;
            rc = RegEnumKeyExW(lxss_key, index++, subkey_name, &subkey_len,
                               NULL, NULL, NULL, NULL);
            if (rc != ERROR_SUCCESS) break;

            /* Open each distro subkey and read DistributionName */
            HKEY distro_key;
            if (RegOpenKeyExW(lxss_key, subkey_name, 0, KEY_READ, &distro_key)
                == ERROR_SUCCESS) {
                wchar_t name_w[256] = {0};
                DWORD name_size = sizeof(name_w);
                DWORD type;
                if (RegQueryValueExW(distro_key, L"DistributionName", NULL,
                                     &type, (BYTE*)name_w, &name_size)
                    == ERROR_SUCCESS && type == REG_SZ) {
                    char name_utf8[256];
                    WideCharToMultiByte(CP_UTF8, 0, name_w, -1,
                                        name_utf8, sizeof(name_utf8), NULL, NULL);
                    char entry[300];
                    int en = snprintf(entry, sizeof(entry), "%s\"%s\"",
                                      first ? "" : ",", name_utf8);
                    growbuf_append(&json, entry, (size_t)en);
                    first = 0;
                }
                RegCloseKey(distro_key);
            }
        }
        RegCloseKey(lxss_key);

        char tail[128];
        int tn = snprintf(tail, sizeof(tail), "],\"current\":\"%s\"}",
                          ctx->backend ? ctx->backend->name : "");
        growbuf_append(&json, tail, (size_t)tn);

        char *result = growbuf_finish(&json);
        webview_return(ctx->w, seq, 0, result ? result : resp);
        free(result);
    } else {
        webview_return(ctx->w, seq, 0, resp);
    }
#else
    webview_return(ctx->w, seq, 0, resp);
#endif
}

static void on_list_distros(const char *seq, const char *req, void *arg) {
    dispatch_async(seq, req, arg, list_distros_work);
}

/* --------------------------------------------------------------------------
 * JS binding: streamExec(command) — run a command in background, log to file
 * Returns immediately. Use streamPoll() to get new output lines.
 * -------------------------------------------------------------------------- */
static void stream_exec_work(bind_ctx_t *ctx, const char *seq, const char *req) {
    char *command = extract_json_string_arg(req);
    if (!command) {
        webview_return(ctx->w, seq, 1, "\"Invalid command\"");
        return;
    }

    /* Run command in background, output to temp log */
    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
        "mkdir -p /tmp/linux_template; rm -f /tmp/linux_template/_stream.log; "
        "sh -c '(%s) > /tmp/linux_template/_stream.log 2>&1; echo __DONE:$?__ >> /tmp/linux_template/_stream.log' &",
        command);

    ctx->backend->exec(ctx->backend, cmd, NULL, NULL, NULL);
    free(command);
    webview_return(ctx->w, seq, 0, "\"started\"");
}

static void on_stream_exec(const char *seq, const char *req, void *arg) {
    dispatch_async(seq, req, arg, stream_exec_work);
}

/* --------------------------------------------------------------------------
 * JS binding: streamPoll(linesSeen) — get new lines from the stream log
 * Returns {lines: [...], done: bool, exitCode: int}
 * -------------------------------------------------------------------------- */
static void stream_poll_work(bind_ctx_t *ctx, const char *seq, const char *req) {
    char *arg_str = extract_json_string_arg(req);
    int lines_seen = arg_str ? atoi(arg_str) : 0;
    free(arg_str);

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "tail -n +%d /tmp/linux_template/_stream.log 2>/dev/null", lines_seen + 1);

    char *out = NULL;
    ctx->backend->exec(ctx->backend, cmd, &out, NULL, NULL);

    growbuf_t json;
    growbuf_init(&json, 2048);
    growbuf_append(&json, "{\"lines\":[", 10);

    int done = 0, exit_code = -1, count = 0;

    if (out && out[0]) {
        char *marker = strstr(out, "__DONE:");
        if (marker) {
            exit_code = atoi(marker + 7);
            *marker = '\0';
            done = 1;
        }

        char *saveptr = NULL;
        char *line = strtok_s(out, "\n", &saveptr);
        while (line) {
            /* Filter out internal protocol markers */
            if (line[0] && !strstr(line, "__XCMD_") && !strstr(line, "__DONE:") &&
                !strstr(line, "__SHELL_READY__")) {
                char *esc = json_escape(line);
                char entry[4096];
                int n = snprintf(entry, sizeof(entry), "%s\"%s\"",
                                 count > 0 ? "," : "", esc);
                growbuf_append(&json, entry, (size_t)n);
                free(esc);
                count++;
            }
            line = strtok_s(NULL, "\n", &saveptr);
        }
    }
    free(out);

    char tail[64];
    int tn = snprintf(tail, sizeof(tail), "],\"done\":%s,\"exitCode\":%d}",
                       done ? "true" : "false", exit_code);
    growbuf_append(&json, tail, (size_t)tn);

    char *result = growbuf_finish(&json);
    webview_return(ctx->w, seq, 0, result ? result : "{\"lines\":[],\"done\":false,\"exitCode\":-1}");
    free(result);
}

static void on_stream_poll(const char *seq, const char *req, void *arg) {
    dispatch_async(seq, req, arg, stream_poll_work);
}

/* --------------------------------------------------------------------------
 * JS binding: openFolder(path) — open a Windows folder in Explorer
 * -------------------------------------------------------------------------- */
static void on_open_folder(const char *seq, const char *req, void *arg) {
    (void)arg;
    char *path = extract_json_string_arg(req);
    if (!path) { webview_return((*(bind_ctx_t *)arg).w, seq, 0, "\"ok\""); return; }
#ifdef _WIN32
    ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
#endif
    free(path);
    webview_return(((bind_ctx_t *)arg)->w, seq, 0, "\"ok\"");
}

static void on_create_app(const char *seq, const char *req, void *arg) {
    dispatch_async(seq, req, arg, create_app_work);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */
int gui_run_webview(linux_backend_t *backend, const linux_config_t *config,
                    service_manager_t *svc_mgr) {
    LINUX_LOG(config, "Starting webview GUI");

    webview_t w = webview_create(0, NULL);
    if (!w) {
        fprintf(stderr, "WebView2 not available.\n");
        return -1;  /* Negative = creation failed, caller can fall back to CLI */
    }

    webview_set_title(w, "linux-template");
    webview_set_size(w, 1050, 900, WEBVIEW_HINT_NONE);

    bind_ctx_t ctx = { w, backend, svc_mgr };
    webview_bind(w, "linuxExec",    on_linux_exec,    &ctx);
    webview_bind(w, "httpRequest",  on_http_request,  &ctx);
    webview_bind(w, "svcStatus",    on_svc_status,    &ctx);
    webview_bind(w, "svcRegister",  on_svc_register,  &ctx);
    webview_bind(w, "svcAction",    on_svc_action,    &ctx);
    webview_bind(w, "provisionRun", on_provision_run, &ctx);
    webview_bind(w, "createAppPackage", on_create_app, &ctx);
    webview_bind(w, "listDistros",     on_list_distros, &ctx);
    webview_bind(w, "streamExec",      on_stream_exec,  &ctx);
    webview_bind(w, "streamPoll",      on_stream_poll,  &ctx);
    webview_bind(w, "openFolder",      on_open_folder,  &ctx);

    webview_set_html(w, GUI_HTML);
    webview_run(w);
    webview_destroy(w);

    return 0;
}
