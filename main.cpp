#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <winhttp.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <regex>
#include <algorithm>
#include <filesystem>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <memory>
#include <array>
#include <conio.h>

#pragma comment(lib, "winhttp.lib")

namespace fs = std::filesystem;

#define COL_RESET   (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)
#define COL_PROMPT  (FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#define COL_USER    (FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define COL_TEXT    (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#define COL_INFO    (FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#define COL_WARN    (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define COL_ERR     (FOREGROUND_RED | FOREGROUND_INTENSITY)
#define COL_MUTED   (FOREGROUND_BLUE)

HANDLE CON;
void col(WORD c) { SetConsoleTextAttribute(CON, c); }

struct Message { std::string role, content; };

struct Conversation {
    std::string name;
    std::vector<Message> messages;
    std::string maker;
    std::string model_id;
};

struct AppState {
    std::string jwt;
    std::string maker;
    std::string model_id;
    std::vector<Message> conversation;
    std::map<std::string, Conversation> saved_convos;
    std::string current_convo_name;
};

const std::map<std::string, std::pair<std::string,std::string>> MODELS = {
    {"1", {"moonshotai", "kimi-k2.7-code"}},
    {"2", {"deepseek",   "deepseek-v4-free"}},
    {"3", {"google",     "gemma-3.12b"}},
    {"4", {"openai",     "gpt-5.6-luna"}}
};

std::string exec_cmd(const std::string& cmd) {
    std::string result;
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    _pclose(pipe);
    return result;
}

std::string unescape_json(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\\' && i + 1 < input.size()) {
            char next = input[i + 1];
            if (next == 'n') { out += '\n'; i++; }
            else if (next == 'r') { out += '\r'; i++; }
            else if (next == 't') { out += '\t'; i++; }
            else if (next == '"') { out += '"'; i++; }
            else if (next == '\\') { out += '\\'; i++; }
            else { out += input[i]; }
        } else {
            out += input[i];
        }
    }
    return out;
}

std::string get_auth_token() {
    std::ofstream script_file("coolgeneratorscript.py");
    script_file << R"PYTHON(import warnings
warnings.filterwarnings("ignore", category=ResourceWarning)
warnings.filterwarnings("ignore", category=DeprecationWarning)

import asyncio
import random
import string
import time
import re
import requests
import json
import os
import platform
import subprocess
import sys
from typing import Optional

try:
    import nodriver as uc
except ImportError:
    subprocess.check_call([sys.executable, "-m", "pip", "install", "nodriver"])
    import nodriver as uc

def rand_str(n, pool=string.ascii_lowercase + string.digits):
    return "".join(random.choice(pool) for _ in range(n))

def make_password():
    return rand_str(8, string.ascii_letters) + str(random.randint(100, 999)) + "!"

def get_temp_email():
    try:
        r = requests.get("https://api.tempmail.lol/generate", timeout=10)
        data = r.json()
        if data.get("address") and data.get("token"):
            return data["address"], data["token"]
    except Exception:
        pass
    return None, None

def wait_for_code(email_token, timeout=60):
    start = time.time()
    while time.time() - start < timeout:
        try:
            r = requests.get(f"https://api.tempmail.lol/auth/{email_token}", timeout=10)
            data = r.json()
            for msg in data.get("email", []):
                content = f"{msg.get('subject', '')} {msg.get('body', '')} {msg.get('html', '')}"
                match = re.search(r'\b(\d{6})\b', content)
                if match:
                    return match.group(1)
        except Exception:
            pass
        time.sleep(2)
    return None

def find_chrome() -> str:
    if os.environ.get("CHROME_PATH"):
        return os.environ["CHROME_PATH"]

    if platform.system() == "Windows":
        candidates = [
            r"C:\Program Files\Google\Chrome\Application\chrome.exe",
            r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
            os.path.expandvars(r"%LOCALAPPDATA%\Google\Chrome\Application\chrome.exe"),
        ]
    else:
        candidates = [
            "/usr/bin/google-chrome-stable",
            "/usr/bin/google-chrome",
            "/usr/bin/chromium-browser",
            "/usr/bin/chromium",
        ]

    for path in candidates:
        if os.path.isfile(path):
            return path

    raise FileNotFoundError("Chrome binary not found on local path.")

async def js_click_by_text(page, text: str) -> bool:
    js = f"""
    (() => {{
        const xpath = "//*[contains(text(), '{text}')]";
        const result = document.evaluate(xpath, document, null, XPathResult.ORDERED_NODE_SNAPSHOT_TYPE, null);
        for (let i = 0; i < result.snapshotLength; i++) {{
            const el = result.snapshotItem(i);
            if (el.offsetParent !== null || el.offsetWidth > 0 || el.offsetHeight > 0) {{
                el.click();
                return true;
            }}
        }}
        return false;
    }})()
    """
    return await page.evaluate(js)

async def js_fill_input(page, placeholder_part: str, value: str, retries: int = 10) -> bool:
    js = f"""
    (() => {{
        const inputs = Array.from(document.querySelectorAll('input'));
        const target = inputs.find(i => i.placeholder && i.placeholder.toLowerCase().includes('{placeholder_part.lower()}'));
        if (!target) return false;

        const nativeSetter = Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype, 'value').set;
        nativeSetter.call(target, '{value}');

        target.dispatchEvent(new Event('input', {{ bubbles: true }}));
        target.dispatchEvent(new Event('change', {{ bubbles: true }}));
        target.dispatchEvent(new Event('blur', {{ bubbles: true }}));
        return true;
    }})()
    """
    for _ in range(retries):
        res = await page.evaluate(js)
        if res:
            return True
        await asyncio.sleep(0.5)
    return False

async def intercept_api_response(page):
    js_interceptor = """
    window._token = null;
    const origFetch = window.fetch;
    window.fetch = function(...args) {
        return origFetch.apply(this, args).then(async (res) => {
            const clone = res.clone();
            try {
                const data = await clone.json();
                if (res.url.includes('/api/users/email/signup/confirm')) {
                    window._token = data.token || (data.user && data.user.token) || null;
                }
            } catch(e) {}
            return res;
        });
    };
    """
    try:
        await page.evaluate(js_interceptor)
    except Exception:
        pass

async def solve_turnstile(page, sitekey: str, timeout: int = 45) -> Optional[str]:
    try:
        await page.evaluate(f"""
            (() => {{
                if (document.getElementById('_ts_box')) return;
                window._tsToken = null;
                const wrap = document.createElement('div');
                wrap.id = '_ts_box';
                wrap.style = 'position:fixed;top:20px;left:20px;z-index:2147483647;';
                document.body.appendChild(wrap);
                window._tsLoad = function () {{
                    turnstile.render('#_ts_box', {{
                        sitekey: '{sitekey}',
                        callback: function(token) {{ window._tsToken = token; }}
                    }});
                }};
                const s = document.createElement('script');
                s.src = 'https://challenges.cloudflare.com/turnstile/v0/api.js?onload=_tsLoad&render=explicit';
                s.async = true;
                document.head.appendChild(s);
            }})();
        """)
        await asyncio.sleep(3.0)

        deadline = time.time() + timeout
        while time.time() < deadline:
            token = await page.evaluate("window._tsToken || null")
            if token:
                return token
            await asyncio.sleep(1.0)
    except Exception:
        pass
    return None

async def main_async():
    email, email_token = get_temp_email()
    if not email or not email_token:
        return

    username = f"User{rand_str(5, string.digits)}"
    password = make_password()

    browser = await uc.start(
        browser_executable_path=find_chrome(),
        headless=False,
        no_sandbox=True
    )

    try:
        page = await browser.get("https://onecompiler.com/python")
        await asyncio.sleep(4)
        await intercept_api_response(page)

        await js_click_by_text(page, "Login")
        await asyncio.sleep(2.5)

        await js_click_by_text(page, "Sign Up")
        await asyncio.sleep(2.5)

        await js_fill_input(page, "name", username)
        await js_fill_input(page, "email", email)
        await js_fill_input(page, "password", password)

        sitekey = await page.evaluate("""
            (() => {
                for (const f of document.querySelectorAll('iframe')) {
                    const src = f.src || '';
                    if (src.includes('challenges.cloudflare.com')) {
                        const m = src.match(/sitekey=([^&]+)/);
                        if (m) return m[1];
                    }
                }
                return null;
            })()
        """)

        if sitekey:
            await solve_turnstile(page, sitekey)

        await js_click_by_text(page, "Sign Up")
        await asyncio.sleep(3)

        otp_code = wait_for_code(email_token, timeout=45)
        if otp_code:
            await js_fill_input(page, "otp", otp_code)
            await js_fill_input(page, "code", otp_code)
            await asyncio.sleep(1)

            await js_click_by_text(page, "Finish")
            await js_click_by_text(page, "Submit")
            await js_click_by_text(page, "Confirm")

        await asyncio.sleep(3)
        captured_token = await page.evaluate("window._token || null")

        if captured_token:
            print(f"TOKEN_OUTPUT:{captured_token}")

    finally:
        try:
            browser.stop()
        except Exception:
            pass

if __name__ == "__main__":
    try:
        asyncio.run(main_async())
    except Exception:
        pass
)PYTHON";
    script_file.close();

    std::string cmd = "python -W ignore -u coolgeneratorscript.py 2>nul";
    std::string result = exec_cmd(cmd);

    std::remove("coolgeneratorscript.py");

    std::regex token_regex(R"(TOKEN_OUTPUT:([A-Za-z0-9\-_]+\.[A-Za-z0-9\-_]+\.[A-Za-z0-9\-_]+))");
    std::smatch match;
    if (std::regex_search(result, match, token_regex)) {
        return match[1].str();
    }

    return "";
}

std::string read_hidden(const std::string& prompt) {
    col(COL_PROMPT);
    std::cout << prompt << std::flush;
    col(COL_TEXT);

    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD old_mode = 0;
    GetConsoleMode(hin, &old_mode);
    SetConsoleMode(hin, (old_mode & ~ENABLE_ECHO_INPUT) | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);

    std::string result;
    std::getline(std::cin, result);

    SetConsoleMode(hin, old_mode);
    std::cout << "\n";
    return result;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::wstring str_to_wstr(const std::string& s) {
    if (s.empty()) return {};
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), sz);
    return w;
}

std::string strip_bearer(std::string tok) {
    auto trim = [](std::string& s) {
        while (!s.empty() && (s.front()==' '||s.front()=='\t'||s.front()=='\r'||s.front()=='\n'))
            s.erase(s.begin());
        while (!s.empty() && (s.back()==' '||s.back()=='\t'||s.back()=='\r'||s.back()=='\n'))
            s.pop_back();
    };
    trim(tok);
    if (tok.size() >= 7 && _strnicmp(tok.c_str(), "bearer ", 7) == 0)
        tok = tok.substr(7);
    trim(tok);
    return tok;
}

std::string json_escape(const std::string& s) {
    std::string o; o.reserve(s.size()+32);
    for (unsigned char c : s) {
        switch(c){
            case '"':  o+="\\\""; break; case '\\': o+="\\\\"; break;
            case '\n': o+="\\n";  break; case '\r': o+="\\r";  break;
            case '\t': o+="\\t";  break;
            default:
                if(c<0x20){char buf[8];snprintf(buf,sizeof(buf),"\\u%04x",c);o+=buf;}
                else o+=c;
        }
    }
    return o;
}

std::string build_payload(const AppState& state, const std::string& user_msg,
                          const std::vector<std::pair<std::string,std::string>>& files) {
    std::string conv="[";
    for(size_t i=0;i<state.conversation.size();i++){
        if(i) conv+=",";
        conv+="{\"role\":\""+state.conversation[i].role+"\",\"content\":\""+json_escape(state.conversation[i].content)+"\"}";
    }
    conv+="]";
    std::string fa="[";
    for(size_t i=0;i<files.size();i++){
        if(i) fa+=",";
        fa+="{\"name\":\""+json_escape(files[i].first)+"\",\"content\":\""+json_escape(files[i].second)+"\"}";
    }
    fa+="]";
    return "{\"module\":\"editor\",\"usecase\":\"codeHelp\","
           "\"currentMessage\":\""+json_escape(user_msg)+"\","
           "\"conversation\":"+conv+","
           "\"model\":{\"maker\":\""+json_escape(state.maker)+"\",\"modelId\":\""+json_escape(state.model_id)+"\"},"
           "\"metadata\":{\"language\":\"\",\"files\":"+fa+",\"currentFile\":\"\",\"stdin\":\"\",\"stdout\":\"\"}}";
}

void print_indented(const std::string& text) {
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) {
            std::cout << line << "\n";
        } else {
            std::cout << "\n";
        }
    }
}

void process_blocks_and_confirm(const std::string& raw_full) {
    static const std::vector<std::string> LANG_TAGS = {
        "python","py","javascript","js","typescript","ts","java","c","cpp","c++","cc",
        "csharp","cs","go","golang","rust","rs","ruby","rb","php","swift","kotlin",
        "kt","scala","perl","lua","r","bash","sh","zsh","powershell","ps1","pwsh",
        "html","css","xml","json","yaml","yml","toml","sql","markdown","md","text",
        "plain","edit","code","cmake","makefile","dockerfile","cuda","haskell","hs",
        "elixir","erlang","dart","objectivec","objc","vb","vba","batch","bat","ini",
        "conf","svg","gradle","m","matlab","julia","nim","zig","fortran","f90","asm"
    };
    std::string full = unescape_json(raw_full);
    
    std::regex codeblock_regex("```([^`]*)```");
    std::smatch match;
    
    if (std::regex_search(full, match, codeblock_regex)) {
        std::string block = match[1].str();
        std::string filename = "";
        std::string content = block;
        
        size_t first_newline = block.find('\n');
        std::string first_line = (first_newline == std::string::npos) ? block : block.substr(0, first_newline);
        size_t fn_pos = first_line.find("filename=");
        if (fn_pos != std::string::npos) {
            size_t val_start = fn_pos + 9;
            size_t val_end = first_line.find_first_of(" \t\r\n`#", val_start);
            if (val_end == std::string::npos) val_end = first_line.length();
            filename = first_line.substr(val_start, val_end - val_start);
            if (first_newline != std::string::npos) {
                content = block.substr(first_newline + 1);
            } else {
                content = block.substr(val_end);
                size_t sp = content.find_first_not_of(" \t");
                if (sp != std::string::npos) content = content.substr(sp);
            }
        } else if (first_newline != std::string::npos) {
            std::string lang = first_line;
            lang.erase(0, lang.find_first_not_of(" \t\r\n"));
            lang.erase(lang.find_last_not_of(" \t\r\n") + 1);
            if (!lang.empty() && lang != "text" && lang != "plain" && lang != "edit") {
                content = block.substr(first_newline + 1);
            }
        } else {
            size_t lang_end = first_line.find_first_of(" \t#");
            if (lang_end != std::string::npos) {
                std::string lang = first_line.substr(0, lang_end);
                std::string rest = first_line.substr(lang_end);
                std::string low = lang;
                std::transform(low.begin(), low.end(), low.begin(), ::tolower);
                bool known = false;
                for (const auto& t : LANG_TAGS) {
                    if (t == low) { known = true; break; }
                }
                if (known && !rest.empty()) {
                    content = rest;
                    size_t sp = content.find_first_not_of(" \t");
                    if (sp != std::string::npos) content = content.substr(sp);
                }
            }
        }
        
        size_t code_start = content.find_first_not_of("\r\n");
        if (code_start != std::string::npos) {
            content = content.substr(code_start);
        }
        size_t trailing_start = content.find_last_not_of("\r\n");
        if (trailing_start != std::string::npos) {
            content = content.substr(0, trailing_start + 1);
        }
        
        if (content.empty()) {
            col(COL_TEXT);
            print_indented(full);
            return;
        }
        
        col(COL_TEXT);
        std::cout << "\n" << content << "\n";
        
        if (!filename.empty()) {
            col(COL_PROMPT);
            std::cout << "\nfile path you want this code to be wrote to [" << filename << "]: ";
            col(COL_TEXT);
            
            std::string target_path;
            std::getline(std::cin, target_path);
            
            if (target_path.empty()) {
                target_path = filename;
            }
            
            target_path.erase(std::remove(target_path.begin(), target_path.end(), '"'), target_path.end());
            
            if (!target_path.empty()) {
                fs::path p(target_path);
                if (p.has_parent_path()) {
                    fs::create_directories(p.parent_path());
                }
                
                std::ofstream f(p, std::ios::binary);
                if (f) {
                    f << content;
                    col(COL_INFO);
                    std::cout << "[+] Applied changes to " << target_path << "\n";
                } else {
                    col(COL_ERR);
                    std::cout << "[-] Failed to write " << target_path << "\n";
                }
            }
        }
        return;
    }
    
    col(COL_TEXT);
    print_indented(full);
}

std::vector<std::string> find_file_refs(const std::string& msg) {
    std::vector<std::string> found;
    std::regex path_re("([A-Za-z]:\\\\[^\\s\"]+|\\.{0,2}[/\\\\][^\\s\"]+|\\S+\\.[a-zA-Z0-9]{1,10})");
    std::sregex_iterator it(msg.begin(), msg.end(), path_re), end;
    for(;it!=end;++it){
        std::string c=(*it)[1].str();
        if(fs::exists(c)) found.push_back(c);
    }
    return found;
}

std::string extract_json_str(const std::string& json, const std::string& key) {
    std::string search="\""+key+"\":\"";
    size_t pos=json.find(search);
    if(pos==std::string::npos) return "";
    pos+=search.size();
    std::string val; bool esc=false;
    for(size_t i=pos;i<json.size();i++){
        char c=json[i];
        if(esc){
            switch(c){case '"':val+='"';break;case '\\':val+='\\';break;
                      case 'n':val+='\n';break;case 'r':val+='\r';break;
                      case 't':val+='\t';break;default:val+=c;break;}
            esc=false;
        } else if(c=='\\'){ esc=true; }
        else if(c=='"') break;
        else val+=c;
    }
    return val;
}

struct SSEState {
    std::string line_buf;
    std::string full_response;
    bool jwt_expired = false;
};

void process_sse_line(const std::string& line, SSEState& sse) {
    if(line.empty()) return;

    std::string data;
    bool is_sse_prefixed = (line.rfind("data: ",0)==0);

    if(is_sse_prefixed){
        data = line.substr(6);
    } else {
        data = line;
    }

    if(data.empty()) return;
    if(data=="[DONE]") return;

    std::string low=data;
    std::transform(low.begin(),low.end(),low.begin(),::tolower);
    if(low.find("expired")!=std::string::npos||
       low.find("unauthorized")!=std::string::npos||
       low.find("invalid token")!=std::string::npos){
        sse.jwt_expired=true;
        return;
    }

    std::string chunk;
    chunk=extract_json_str(data,"content");
    if(chunk.empty()) chunk=extract_json_str(data,"text");
    if(chunk.empty()) chunk=extract_json_str(data,"message");
    if(chunk.empty()){
        size_t dp=data.find("\"delta\"");
        if(dp!=std::string::npos) chunk=extract_json_str(data.substr(dp),"content");
    }
    if(chunk.empty()){
        size_t cp=data.find("\"choices\"");
        if(cp!=std::string::npos){
            size_t dp2=data.find("\"delta\"",cp);
            if(dp2!=std::string::npos) chunk=extract_json_str(data.substr(dp2),"content");
        }
    }

    if(!chunk.empty()){
        sse.full_response+=chunk;
    } else if(!is_sse_prefixed){
        sse.full_response+=line;
        sse.full_response+='\n';
    }
}

int send_request(AppState& state, const std::string& payload, std::string& full_response) {
    URL_COMPONENTS uc={};
    uc.dwStructSize=sizeof(uc);
    wchar_t host[256]={},path[512]={};
    uc.lpszHostName=host; uc.dwHostNameLength=256;
    uc.lpszUrlPath=path;  uc.dwUrlPathLength=512;

    std::wstring wurl=L"https://onecompiler.com/api/ai/stream";
    if(!WinHttpCrackUrl(wurl.c_str(),(DWORD)wurl.size(),0,&uc)) return 2;

    HINTERNET session=WinHttpOpen(L"OneCompilerCLI/1.0",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session) return 2;

    HINTERNET conn=WinHttpConnect(session,host,uc.nPort,0);
    if(!conn){ WinHttpCloseHandle(session); return 2; }

    HINTERNET req=WinHttpOpenRequest(conn,L"POST",path,nullptr,
                                     WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
    if(!req){ WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return 2; }

    std::wstring headers=L"Content-Type: application/json\r\n";
    headers+=L"Authorization: Bearer "+str_to_wstr(state.jwt)+L"\r\n";
    headers+=L"Accept: text/event-stream\r\n";

    BOOL sent=WinHttpSendRequest(req,headers.c_str(),(DWORD)headers.size(),
                                 (LPVOID)payload.c_str(),(DWORD)payload.size(),(DWORD)payload.size(),0);
    if(!sent){ WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return 2; }

    if(!WinHttpReceiveResponse(req,nullptr)){
        WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
        return 2;
    }

    DWORD status=0,status_sz=sizeof(DWORD);
    WinHttpQueryHeaders(req,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,&status,&status_sz,WINHTTP_NO_HEADER_INDEX);

    if(status==401||status==403){
        WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
        return 1;
    }
    if(status>=400){
        char ebuf[2048]; DWORD eread=0; std::string ebody;
        while(true){
            DWORD avail=0;
            if(!WinHttpQueryDataAvailable(req,&avail)||avail==0) break;
            if(avail>sizeof(ebuf)) avail=sizeof(ebuf);
            if(!WinHttpReadData(req,ebuf,avail,&eread)||eread==0) break;
            ebody.append(ebuf,eread);
            if(ebody.size()>1024) break;
        }
        col(COL_ERR);
        std::cout<<"\n  [HTTP "<<status<<"] "<<ebody<<"\n";
        col(COL_TEXT);
        WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
        return 2;
    }

    SSEState sse;
    char buf[8192]; DWORD bytes_read=0;

    while(true){
        DWORD available=0;
        if(!WinHttpQueryDataAvailable(req,&available)||available==0) break;
        if(available>sizeof(buf)) available=sizeof(buf);
        if(!WinHttpReadData(req,buf,available,&bytes_read)||bytes_read==0) break;
        for(DWORD i=0;i<bytes_read;i++){
            char c=buf[i];
            if(c=='\n'){
                process_sse_line(sse.line_buf,sse);
                sse.line_buf.clear();
                if(sse.jwt_expired) goto done;
            } else if(c!='\r'){
                sse.line_buf+=c;
            }
        }
    }
    if(!sse.line_buf.empty()) process_sse_line(sse.line_buf,sse);

done:
    WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
    if(sse.jwt_expired) return 1;
    full_response=sse.full_response;
    return 0;
}

void save_state(const AppState& state) {
    std::ofstream f("state.json");
    f << "{\n";
    f << "  \"jwt\": \"" << state.jwt << "\",\n";
    f << "  \"maker\": \"" << state.maker << "\",\n";
    f << "  \"model_id\": \"" << state.model_id << "\",\n";
    f << "  \"current_convo_name\": \"" << state.current_convo_name << "\",\n";
    f << "  \"conversation\": [\n";
    for (size_t i = 0; i < state.conversation.size(); i++) {
        f << "    {\"role\": \"" << state.conversation[i].role << "\", \"content\": \"" << json_escape(state.conversation[i].content) << "\"}";
        if (i < state.conversation.size() - 1) f << ",";
        f << "\n";
    }
    f << "  ],\n";
    f << "  \"saved_convos\": {\n";
    size_t idx = 0;
    for (const auto& pair : state.saved_convos) {
        f << "    \"" << pair.first << "\": {\n";
        f << "      \"name\": \"" << pair.second.name << "\",\n";
        f << "      \"maker\": \"" << pair.second.maker << "\",\n";
        f << "      \"model_id\": \"" << pair.second.model_id << "\",\n";
        f << "      \"messages\": [\n";
        for (size_t i = 0; i < pair.second.messages.size(); i++) {
            f << "        {\"role\": \"" << pair.second.messages[i].role << "\", \"content\": \"" << json_escape(pair.second.messages[i].content) << "\"}";
            if (i < pair.second.messages.size() - 1) f << ",";
            f << "\n";
        }
        f << "      ]\n";
        f << "    }";
        if (idx < state.saved_convos.size() - 1) f << ",";
        f << "\n";
        idx++;
    }
    f << "  }\n";
    f << "}\n";
}

void load_state(AppState& state) {
    std::ifstream f("state.json");
    if (!f) return;
    
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    
    auto extract_str = [&](const std::string& key) -> std::string {
        std::string search = "\"" + key + "\": \"";
        size_t pos = content.find(search);
        if (pos == std::string::npos) return "";
        pos += search.size();
        size_t end = content.find('"', pos);
        if (end == std::string::npos) return "";
        return content.substr(pos, end - pos);
    };
    
    auto extract_array = [&](const std::string& key) -> std::string {
        std::string search = "\"" + key + "\": [";
        size_t pos = content.find(search);
        if (pos == std::string::npos) return "";
        pos += search.size();
        int brace_count = 1;
        size_t end = pos;
        while (end < content.size() && brace_count > 0) {
            if (content[end] == '[') brace_count++;
            else if (content[end] == ']') brace_count--;
            end++;
        }
        return content.substr(pos, end - pos - 1);
    };
    
    std::string jwt = extract_str("jwt");
    if (!jwt.empty()) state.jwt = jwt;
    
    std::string maker = extract_str("maker");
    if (!maker.empty()) state.maker = maker;
    
    std::string model_id = extract_str("model_id");
    if (!model_id.empty()) state.model_id = model_id;
    
    std::string current = extract_str("current_convo_name");
    if (!current.empty()) state.current_convo_name = current;
    
    state.conversation.clear();
    std::string conv_array = extract_array("conversation");
    if (!conv_array.empty()) {
        std::regex msg_regex("\\{\\s*\"role\":\\s*\"([^\"]+)\",\\s*\"content\":\\s*\"([^\"]+)\"\\s*\\}");
        std::sregex_iterator it(conv_array.begin(), conv_array.end(), msg_regex), end;
        for (; it != end; ++it) {
            state.conversation.push_back({(*it)[1].str(), (*it)[2].str()});
        }
    }
    
    size_t conv_start = content.find("\"saved_convos\"");
    if (conv_start != std::string::npos) {
        size_t brace_start = content.find('{', conv_start);
        if (brace_start != std::string::npos) {
            int brace_count = 1;
            size_t pos = brace_start + 1;
            while (pos < content.size() && brace_count > 0) {
                if (content[pos] == '{') brace_count++;
                else if (content[pos] == '}') brace_count--;
                pos++;
            }
            std::string convos_str = content.substr(brace_start + 1, pos - brace_start - 2);
            
            std::regex convo_regex("\"([^\"]+)\":\\s*\\{\\s*\"name\":\\s*\"([^\"]+)\",\\s*\"maker\":\\s*\"([^\"]+)\",\\s*\"model_id\":\\s*\"([^\"]+)\",\\s*\"messages\":\\s*\\[([^\\]]*)\\]\\s*\\}");
            std::sregex_iterator it2(convos_str.begin(), convos_str.end(), convo_regex), end2;
            for (; it2 != end2; ++it2) {
                Conversation conv;
                conv.name = (*it2)[2].str();
                conv.maker = (*it2)[3].str();
                conv.model_id = (*it2)[4].str();
                
                std::string msgs_str = (*it2)[5].str();
                std::regex msg_regex2("\\{\\s*\"role\":\\s*\"([^\"]+)\",\\s*\"content\":\\s*\"([^\"]+)\"\\s*\\}");
                std::sregex_iterator msg_it(msgs_str.begin(), msgs_str.end(), msg_regex2), msg_end;
                for (; msg_it != msg_end; ++msg_it) {
                    conv.messages.push_back({(*msg_it)[1].str(), (*msg_it)[2].str()});
                }
                
                state.saved_convos[(*it2)[1].str()] = conv;
            }
        }
    }
}

void auto_login(AppState& state) {
    if (!state.jwt.empty()) {
        col(COL_INFO);
        std::cout << "[*] Using saved token\n";
        return;
    }
    
    col(COL_INFO);
    std::cout << "[*] making an account...\n";
    std::string token = get_auth_token();
    
    if(!token.empty()) {
        state.jwt = token;
        col(COL_PROMPT);
        std::cout << "[+] made successfully.\n\n";
        save_state(state);
    } else {
        state.jwt = strip_bearer(read_hidden("[?] Enter JWT Token: "));
        save_state(state);
    }
}

std::vector<std::string> get_conv_names(const AppState& state) {
    std::vector<std::string> names;
    for (const auto& pair : state.saved_convos) {
        names.push_back(pair.first);
    }
    return names;
}

int select_from_list(const std::vector<std::string>& items, const std::string& title) {
    if (items.empty()) {
        col(COL_WARN);
        std::cout << "[!] No conversations found.\n";
        return -1;
    }

    int selected = 0;
    int start_idx = 0;
    int max_display = 20;
    bool done = false;

    while (!done) {
        system("cls");
        
        col(COL_INFO);
        std::cout << title << "\n";
        col(COL_MUTED);
        std::cout << "Use arrow keys to move, Enter to select, ESC to cancel\n\n";
        
        int display_count = (std::min)((int)items.size(), max_display);
        for (int i = 0; i < display_count; i++) {
            int idx = start_idx + i;
            if (idx >= (int)items.size()) break;
            
            if (idx == selected) {
                col(COL_PROMPT);
                std::cout << " > " << items[idx] << "\n";
            } else {
                col(COL_TEXT);
                std::cout << "   " << items[idx] << "\n";
            }
        }
        
        int ch = _getch();
        if (ch == 224) {
            ch = _getch();
            if (ch == 72) {
                if (selected > 0) selected--;
                else selected = items.size() - 1;
                if (selected < start_idx) start_idx = selected;
            } else if (ch == 80) {
                if (selected < (int)items.size() - 1) selected++;
                else selected = 0;
                if (selected >= start_idx + max_display) start_idx = selected - max_display + 1;
            }
        } else if (ch == 13) {
            done = true;
            return selected;
        } else if (ch == 27) {
            return -1;
        }
    }
    return -1;
}

void create_conversation(AppState& state) {
    col(COL_PROMPT);
    std::cout << "what do you want to name your convo name to be? ";
    col(COL_TEXT);
    std::string name;
    std::getline(std::cin, name);
    
    if (name.empty()) {
        col(COL_ERR);
        std::cout << "[-] Name cannot be empty.\n";
        return;
    }
    
    if (state.saved_convos.find(name) != state.saved_convos.end()) {
        col(COL_ERR);
        std::cout << "[-] Conversation '" << name << "' already exists.\n";
        return;
    }
    
    Conversation conv;
    conv.name = name;
    conv.messages.clear();
    conv.maker = state.maker;
    conv.model_id = state.model_id;
    state.saved_convos[name] = conv;
    state.current_convo_name = name;
    state.conversation.clear();
    
    save_state(state);
    col(COL_INFO);
    std::cout << "[+] Conversation '" << name << "' created with 0 messages.\n";
}

void load_conversation(AppState& state) {
    auto names = get_conv_names(state);
    int idx = select_from_list(names, "-- LOAD CONVERSATION --");
    
    if (idx >= 0) {
        std::string name = names[idx];
        state.conversation = state.saved_convos[name].messages;
        state.maker = state.saved_convos[name].maker;
        state.model_id = state.saved_convos[name].model_id;
        state.current_convo_name = name;
        
        save_state(state);
        col(COL_INFO);
        std::cout << "[+] Loaded conversation '" << name << "' with " << state.conversation.size() << " messages.\n";
    }
}

void delete_conversation(AppState& state) {
    auto names = get_conv_names(state);
    int idx = select_from_list(names, "-- DELETE CONVERSATION --");
    
    if (idx >= 0) {
        std::string name = names[idx];
        state.saved_convos.erase(name);
        if (state.current_convo_name == name) {
            state.current_convo_name = "";
            state.conversation.clear();
        }
        save_state(state);
        col(COL_INFO);
        std::cout << "[+] Deleted conversation '" << name << "'.\n";
    }
}

void save_current_conversation(AppState& state) {
    if (state.current_convo_name.empty()) {
        return;
    }
    state.saved_convos[state.current_convo_name].messages = state.conversation;
    state.saved_convos[state.current_convo_name].maker = state.maker;
    state.saved_convos[state.current_convo_name].model_id = state.model_id;
    save_state(state);
}

void print_help(const AppState& state){
    col(COL_INFO);
    std::cout<<"\n-- COMMANDS ----------------------------------------\n";
    col(COL_PROMPT); std::cout<<"  /help   "; col(COL_TEXT); std::cout<<"Show command list\n";
    col(COL_PROMPT); std::cout<<"  /model  "; col(COL_TEXT); std::cout<<"Switch active model\n";
    col(COL_PROMPT); std::cout<<"  /clear  "; col(COL_TEXT); std::cout<<"Clear chat history\n";
    col(COL_PROMPT); std::cout<<"  /renew  "; col(COL_TEXT); std::cout<<"Generate fresh token\n";
    col(COL_PROMPT); std::cout<<"  /createconvo "; col(COL_TEXT); std::cout<<"Create a new empty conversation\n";
    col(COL_PROMPT); std::cout<<"  /loadconvo "; col(COL_TEXT); std::cout<<"Load a saved conversation\n";
    col(COL_PROMPT); std::cout<<"  /deleteconvo "; col(COL_TEXT); std::cout<<"Delete a saved conversation\n";
    col(COL_PROMPT); std::cout<<"  /exit   "; col(COL_TEXT); std::cout<<"Quit application\n";
    
    col(COL_INFO);
    std::cout<<"\n-- MODELS ------------------------------------------\n";
    for(auto&[k,v]:MODELS){
        bool active=v.second==state.model_id;
        if(active) {
            col(COL_PROMPT);
            std::cout<<"  * ["<<k<<"] "<<v.first<<" / "<<v.second<<" (active)\n";
        } else {
            col(COL_MUTED);
            std::cout<<"    ["<<k<<"] "<<v.first<<" / "<<v.second<<"\n";
        }
    }
    std::cout<<"\n";
    col(COL_TEXT);
}

void select_model(AppState& state){
    col(COL_INFO); std::cout<<"\n-- SELECT MODEL ------------------------------------\n";
    for(auto&[k,v]:MODELS){
        bool active=v.second==state.model_id;
        if(active) {
            col(COL_PROMPT);
            std::cout<<"  * ["<<k<<"] "<<v.first<<" / "<<v.second<<"\n";
        } else {
            col(COL_MUTED);
            std::cout<<"    ["<<k<<"] "<<v.first<<" / "<<v.second<<"\n";
        }
    }
    col(COL_PROMPT); std::cout<<"Select [1-4]: ";
    col(COL_TEXT);
    std::string choice; std::getline(std::cin,choice);
    if(MODELS.count(choice)){
        state.maker=MODELS.at(choice).first;
        state.model_id=MODELS.at(choice).second;
        save_state(state);
        col(COL_INFO); std::cout<<"[+] Switched to "<<state.model_id<<"\n";
    } else {
        col(COL_ERR); std::cout<<"[-] Invalid selection.\n";
    }
    col(COL_TEXT);
}

int main(){
    SetConsoleOutputCP(CP_UTF8);
    CON=GetStdHandle(STD_OUTPUT_HANDLE);
    srand((unsigned int)time(nullptr));

    col(COL_PROMPT);
    std::cout << "---fuckass cli made by gemini, claude and deepseek yuh\n";
    std::cout << "---$ ";
    col(COL_INFO);
    std::cout << "online compiliers ai but as a cli ai\n";
    col(COL_MUTED);
    std::cout << "    Type /help for options\n\n";

    AppState state;
    state.maker="deepseek";
    state.model_id="deepseek-v4-free";

    load_state(state);
    auto_login(state);

    while(true){
        col(COL_PROMPT);
        std::cout << "---(";
        col(COL_INFO);
        std::cout << state.model_id;
        if (!state.current_convo_name.empty()) {
            col(COL_PROMPT);
            std::cout << ":" << state.current_convo_name;
        }
        col(COL_PROMPT);
        std::cout << ")\n---$ ";
        col(COL_TEXT);

        std::string input;
        if(!std::getline(std::cin,input)) break;
        if(input.empty()) continue;
        if(input=="/exit") {
            save_state(state);
            break;
        }
        if(input=="/help") { print_help(state); continue; }
        if(input=="/clear"){
            system("cls");
            col(COL_PROMPT);
            std::cout << "---fuckass cli made by gemini, claude and deepseek yuh\n";
            std::cout << "---$ ";
            col(COL_INFO);
            std::cout << "online compiliers ai but as a cli ai\n";
            col(COL_MUTED);
            std::cout << "    Type /help for options\n\n";
            state.conversation.clear();
            save_current_conversation(state);
            continue;
        }
        if(input=="/model"){ select_model(state); save_current_conversation(state); continue; }
        if(input=="/renew"){
            state.jwt = "";
            save_state(state);
            auto_login(state);
            continue;
        }
        if(input=="/createconvo"){
            create_conversation(state);
            continue;
        }
        if(input=="/loadconvo"){
            load_conversation(state);
            continue;
        }
        if(input=="/deleteconvo"){
            delete_conversation(state);
            continue;
        }

        std::vector<std::pair<std::string,std::string>> attached;
        auto refs=find_file_refs(input);
        for(auto& p:refs){
            std::string content=read_file(p);
            if(!content.empty()){
                attached.push_back({fs::path(p).filename().string(),content});
                col(COL_INFO);
                std::cout<<"[*] Attached: "<<p<<" ("<<content.size()<<" bytes)\n";
            }
        }

        std::string payload=build_payload(state,input,attached);
        state.conversation.push_back({"user",input});
        save_current_conversation(state);

        col(COL_MUTED);
        std::cout<<"...\n";

        std::string full;
        int result=send_request(state,payload,full);

        if(result==1){
            state.jwt = "";
            save_state(state);
            auto_login(state);
            full.clear();
            result=send_request(state,payload,full);
        }

        if(result==2){
            col(COL_ERR);
            std::cout<<"\n[-] Request failed.\n";
            state.conversation.pop_back();
            save_current_conversation(state);
            continue;
        }

        std::cout<<"\n";

        if(!full.empty()){
            state.conversation.push_back({"assistant",full});
            save_current_conversation(state);
            process_blocks_and_confirm(full);
        } else {
            col(COL_WARN);
            std::cout<<"[!] Empty response.\n";
        }
        std::cout<<"\n";
    }

    col(COL_RESET);
    return 0;
}