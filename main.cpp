#include <bits/stdc++.h>
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "httplib.h"
#include "json.hpp"
#include "tool.h"
using namespace std;
using json=nlohmann::json;

static const string toolh="[TOOL]";
static const string toolcontinue="<continue>";
static const string toolrestag="[tool results]";

// ---------- terminal ui ----------
// every escape is gated on isatty(1): piped stdout (test mode, queue workers,
// scripts) gets byte-identical output to the pre-ui build.
static int g_ui_tty=-1;
static bool ui_on(){ if (g_ui_tty<0) g_ui_tty=isatty(1); return g_ui_tty; }
static const string C_RESET="", C_BOLD="", C_DIM="",
    C_GREEN="", C_CYAN="", C_WHITE="", C_RED="";
static void ui_term_title(const string& t){ if (ui_on()) cout<<"\x1b]0;"<<t<<"\x07"; }
// guarantees the terminal colour state is restored no matter how the session
// ends (normal exit, /quit, EOF, Ctrl+C mid-turn, Ctrl+\, kill). without this
// a SIGINT during a coloured prompt/spinner leaves the user's next shell
// prompt tinted. only emits when we actually own a tty.
static void ui_terminate_reset(int sig){
    if (ui_on()){
        const string r=C_RESET+"\n";
        ssize_t n=write(STDOUT_FILENO, r.data(), r.size());
        (void)n;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}
static void ui_install_terminate_guard(){
    if (!ui_on()) return;
    signal(SIGINT,  ui_terminate_reset);
    signal(SIGTERM, ui_terminate_reset);
    signal(SIGHUP,  ui_terminate_reset);
    signal(SIGQUIT, ui_terminate_reset);
    atexit([](){ if (ui_on()) fputs(C_RESET.data(), stdout); });
}
static void ui_banner(const string& name, const string& provider, const string& model){
    if (!ui_on()) return;
    ui_term_title(name+" :: "+provider+"/"+model);
    cout<<C_RED<<C_BOLD<<name<<C_RESET<<C_DIM<<" :: "<<C_RESET
        <<C_CYAN<<provider<<C_RESET<<"/"<<C_CYAN<<model<<C_RESET
        <<C_WHITE<<C_DIM<<"\n  /help for commands, /quit to exit"<<C_RESET<<'\n'<<flush;
}
// live turn indicator. the redraw thread is a strict join-before-print
// lifecycle: main always joins the thread (via prose/end/resume) before it
// writes anything else, so spinner frames can never interleave with prose.
struct turn_indicator{
    string name_, tag, last;
    int rnd=0;
    bool ever=false, midline=false;
    chrono::steady_clock::time_point t0;
    std::atomic<bool> run_{false};
    std::thread th; std::mutex mu;
    ~turn_indicator(){ end(); }
    // turn start: metadata only; the first resume() spawns the thread.
    void begin(const string& nm){ name_=nm; rnd=0; ever=true; midline=false;
        t0=chrono::steady_clock::now(); }
    // (re)start the redraw thread for a new phase; called at each loop top
    // and before each tool dispatch. joins any prior thread first.
    void resume(const string& ph){
        join_thread();
        std::lock_guard<std::mutex> g(mu);
        tag=ph;
        if (midline){ cout<<'\n'; midline=false; }
        last.clear();
        if (ui_on() && ever){ run_=true; th=std::thread([this]{ loop(); }); }
    }
    // leading prose is about to be printed: stop the thread, clear the
    // spinner line if we own it. midline is recomputed from the text: a
    // newline-terminated (or empty) prose leaves the cursor at line start.
    void prose(const string& t){
        join_thread();
        std::lock_guard<std::mutex> g(mu);
        if (ui_on() && ever && !midline){ cout<<"\r\x1b[K"; }
        bool ends_nl = !t.empty() && t.back()=='\n';
        midline = !t.empty() && !ends_nl;
    }
    // turn end: stop the thread, clear the spinner line - but only when we
    // own the line (a partial-prose midline holds real text).
    void end(){
        join_thread();
        std::lock_guard<std::mutex> g(mu);
        if (ui_on() && ever && !midline){ cout<<"\r\x1b[K"; }
        ever=false;
    }
    void join_thread(){
        if (run_){ run_=false; if (th.joinable()) th.join(); }
    }
    // update the phase label for the next frame (no thread restart).
    void set_tag(const string& ph){ std::lock_guard<std::mutex> g(mu); tag=ph; }
    void loop(){
        const string fr[8]={"\xe2\xa0\x8b","\xe2\xa0\x99","\xe2\xa0\xb9","\xe2\xa0\xb8",
                            "\xe2\xa0\xbc","\xe2\xa0\xb4","\xe2\xa0\xa6","\xe2\xa0\xa7"};
        int frame=0;
        while (run_){
            std::this_thread::sleep_for(chrono::milliseconds(120));
            string out;
            {
                std::lock_guard<std::mutex> g(mu);
                if (!run_) break;
                auto el=int(chrono::duration_cast<chrono::seconds>(
                    chrono::steady_clock::now()-t0).count());
                char b[16];
                if (el>=3600) snprintf(b,sizeof b,"%dh%02dm",el/3600,(el%3600)/60);
                else if (el>=60) snprintf(b,sizeof b,"%dm%02ds",el/60,el%60);
                else snprintf(b,sizeof b,"%ds",el);
                out="\r"+C_DIM+fr[frame++%8]+C_RESET+" "
                    +C_WHITE+name_+C_RESET+C_DIM+"  "+b+"  r"+to_string(rnd)+"  "+tag;
                if (out.size()<last.size()) out+=string(last.size()-out.size(),' ');
                last=out;
            }
            cout<<out<<flush;
        }
    }
};

struct config{
    string provider="scandata-relay.zarnkina.workers.dev";
    string web_provider="api.langsearch.com";
    string model="zarnkina-gpt-6.1-sol";
    string promptfile="sysprompts/clover.txt";
    string web_key=getenv("CLOVER_WEB_KEY")?getenv("CLOVER_WEB_KEY"):"";
    string ctxfile="context.json";
    string purpose;
    static string chat_key_for(){ return getenv("CLOVER_CHAT_KEY")?getenv("CLOVER_CHAT_KEY"):""; }
    static string or_key_for(){ return getenv("CLOVER_OR_KEY")?getenv("CLOVER_OR_KEY"):""; }
    static string gm_key_for(){ return getenv("CLOVER_GM_KEY")?getenv("CLOVER_GM_KEY"):""; }
    static string zk_key_for(){ return getenv("CLOVER_ZK_KEY")?getenv("CLOVER_ZK_KEY"):""; }
};

// set when the provider hard-rejects image parts; attach_images stops
// attaching for the rest of the process once this flips.
static bool g_vision_denied=false;

// declared early: chat_backend's retry loop shrinks oversized requests.
static constexpr size_t CTX_MAX_BYTES = 1500000;
static json trim_context_bytes(const json& messages, size_t max_bytes);
static bool has_image(const json& messages);

// copy of the context with image parts removed: array content collapses
// to its concatenated text. used when a provider 400s on images.
static json strip_images(const json& ctx){
    json out=ctx;
    if (!out.is_array()) return out;
    for (auto& m:out){
        if (!m.contains("content") || !m["content"].is_array()) continue;
        string txt;
        for (auto& part:m["content"])
            if (part.value("type","")=="text") txt+=part["text"].get<string>();
        m["content"]=txt;
    }
    return out;
}

// lazily (re)builds ssl clients when provider/key changes, since SSLClient is
// final and non-movable, so we cannot just reassign it.
// tests drive the backends against a plain-http stub; production is always tls.
#ifdef CLOVER_PLAIN_HTTP
using clover_client = httplib::Client;
#else
using clover_client = httplib::SSLClient;
#endif

// Incremental SSE decoder; chunks may split anywhere, including UTF-8.
struct sse_decoder {
    string pending, content, finish, error;
    bool done=false;
    function<void(const string&)> emit;
    void feed(const char* data, size_t n){
        pending.append(data,n);
        size_t pos;
        while ((pos=pending.find('\n'))!=string::npos){
            string line=pending.substr(0,pos); pending.erase(0,pos+1);
            if (!line.empty() && line.back()=='\r') line.pop_back();
            if (line.rfind("data:",0)!=0 || done) continue;
            string payload=line.substr(5);
            if (!payload.empty() && payload.front()==' ') payload.erase(0,1);
            if (payload=="[DONE]"){ done=true; continue; }
            if (payload.empty()) continue;
            try {
                auto d=json::parse(payload);
                if (d.contains("error")){ error="provider stream error"; continue; }
                if (!d.contains("choices") || !d["choices"].is_array() || d["choices"].empty()) continue;
                auto& c=d["choices"][0];
                if (c.contains("delta") && c["delta"].contains("content") && c["delta"]["content"].is_string()){
                    string text=c["delta"]["content"].get<string>();
                    content+=text; if (emit) emit(text);
                }
                if (c.contains("finish_reason") && c["finish_reason"].is_string()) finish=c["finish_reason"].get<string>();
            } catch (const exception&){ error="malformed SSE event"; }
        }
    }
};

 
class chat_backend{
    public:
        string provider;
        string key;
        unique_ptr<clover_client> cli;
        chat_backend(const config& c):provider(c.provider){ key=key_for_provider(provider); build(); }
        // providers differ in both key env var and api path. keeping both in
        // one place means switching providers can never half-apply.
        static string key_for_provider(const string& p){
            if (p=="openrouter.ai") return config::or_key_for();
            if (p=="generativelanguage.googleapis.com") return config::gm_key_for();
            if (p=="scandata-relay.zarnkina.workers.dev") return config::zk_key_for();
            return config::chat_key_for();
        }
        static string path_for_provider(const string& p){
            if (p=="openrouter.ai") return "/api/v1/chat/completions";
            // google exposes an openai-compatible surface under v1beta/openai
            if (p=="generativelanguage.googleapis.com") return "/v1beta/openai/chat/completions";
            return "/v1/chat/completions";
        }
        void build(){
            cli=make_unique<clover_client>(provider);
            cli->set_keep_alive(false);
            cli->set_connection_timeout(0,3000000);
            cli->set_read_timeout(300,0);
            cli->set_default_headers({{"Authorization", "Bearer "+key}});
        }
        void set_provider(const string& p){
            if (p==provider) return;
            provider=p;
            // providers are keyed by their own env var: openrouter uses
            // CLOVER_OR_KEY, everything else stays on CLOVER_CHAT_KEY.
            key=key_for_provider(provider);
            build();
        }
        // one request attempt. returns content on 200 with a parsable
        // body. err is cleared on success, otherwise set to one of:
        //   "retry"   - transient (429/5xx/connect/parse), retry with backoff
        //   "novision"- provider 400 rejecting image parts; strip images, retry
        //   "fatal"   - other 4xx; retrying is pointless, fail fast
        string attempt(const string& model, const json& context, string& err,
                     function<void(const string&)> emit={}){
            err.clear();
            json m; m["model"]=model;
            m["stream"]=true;
            m["messages"]=context;
            string path = path_for_provider(provider);
            string request=m.dump(), raw;
            sse_decoder decoder; decoder.emit=emit;
            auto res=cli->Post(path.c_str(), request.size(),
                [&](size_t offset, size_t length, httplib::DataSink& sink){
                    return sink.write(request.data()+offset,length);
                }, "application/json",
                [&](const char* data, size_t n){
                    raw.append(data,n); decoder.feed(data,n); return true;
                });
            if (res && res->status==200){
                try{
                    if (!decoder.content.empty() || raw.find("data:")!=string::npos){
                        decoder.feed("\n",1);
                        bool clean = decoder.done || (!decoder.content.empty() && decoder.finish=="stop");
                        if (!clean || !decoder.error.empty() || decoder.finish=="length" || decoder.content.empty()){
                            cerr<<"stream incomplete: "<<decoder.error<<" finish="<<decoder.finish<<"\n";
                            err=decoder.content.empty()?"retry":"fatal"; return "";
                        }
                        return decoder.content;
                    }
                    auto resp_json=json::parse(raw);
                    if (!resp_json.contains("choices") || resp_json["choices"].empty())
                        { cerr<<"unexpected response shape\n"; err="retry"; return ""; }
                    return resp_json["choices"][0]["message"]["content"].get<string>();
                } catch (const json::parse_error& e){
                    cerr<<"response json parse error:\n"<<e.what()<<'\n'; err="retry";
                } catch (const json::type_error& e){
                    cerr<<"response json type error:\n"<<e.what()<<'\n'; err="retry";
                }
                return "";
            }
            string body = raw;
            int st = res ? res->status : 0;
            if (!res){
                cerr<<"http error: connection interrupted\n";
                err=decoder.content.empty()?"retry":"fatal"; return "";
            }
            cerr<<"http error: status "<<st<<"\nbody:\n"<<body<<'\n';
            if (st==413){
                err="toolarge"; return "";
            }
            if (st==401 || st==403){
                err="fatal"; return "";
            }
            if (st==400){
                string b=body; transform(b.begin(),b.end(),b.begin(),::tolower);
                if (b.find("multimodal")!=string::npos || b.find("image")!=string::npos){
                    err="novision"; return "";
                }
                if (b.find("too large")!=string::npos || b.find("maximum context")!=string::npos ||
                    b.find("context length")!=string::npos || b.find("prompt too long")!=string::npos ||
                    (b.find("token")!=string::npos && (b.find("limit")!=string::npos ||
                     b.find("maximum")!=string::npos || b.find("exceed")!=string::npos))){
                    err="toolarge"; return "";
                }
                err="fatal"; return "";
            }
            err="retry"; return "";
        }
        // exponential backoff with error classification: transient errors
        // retry with growing waits, image-rejections strip images once and
        // continue, other 4xx abort immediately (retrying a permanent error
        // just burns the budget).
        string generate(const string& model, const json& context, function<void(const string&)> emit={}){
            static constexpr int MAX_TRIES=10;
            json ctx=context;
            bool stripped=false;
            int backoff_ms=1000;
            int shrink=0;
            for (int t=0;t<MAX_TRIES;t++){
                if (t>0 && shrink==0){
                    cerr<<"generate retry "<<t+1<<"/"<<MAX_TRIES<<" in "<<backoff_ms<<"ms\n";
                    this_thread::sleep_for(chrono::milliseconds(backoff_ms));
                    backoff_ms*=5; if (backoff_ms>60000) backoff_ms=60000;
                    build();
                }
                string err;
                string out=attempt(model, ctx, err, emit);
                if (!out.empty()) return out;
                if ((err=="novision" || err=="toolarge") && !stripped && has_image(ctx)){
                    cerr<<"dropping image parts and retrying\n";
                    if (err=="novision") g_vision_denied=true;
                    ctx=strip_images(ctx);
                    stripped=true;
                    out=attempt(model, ctx, err, emit);
                    if (!out.empty()) return out;
                }
                if (err=="toolarge"){
                    // the provider's true limit is unknown: halve the CURRENT
                    // payload, not a fixed constant, so a small-but-rejected
                    // request still loses half its history each round.
                    size_t before=ctx.dump().size();
                    size_t target=before/2;
                    if (target<4000) target=4000;
                    shrink++;
                    ctx=trim_context_bytes(ctx, target);
                    size_t after=ctx.dump().size();
                    cerr<<"context too large; compacted "<<before<<" -> "<<after<<" bytes\n";
                    if (after>=before){
                        cerr<<"cannot shrink context any further ("<<after<<" bytes)\n";
                        return "[provider rejected the request even after compaction; it is too large for "+provider+" "+model+"]";
                    }
                    if (shrink>16){
                        cerr<<"context still too large after "<<shrink<<" compactions\n";
                        return "[provider kept rejecting the request as too large after "+to_string(shrink)+" compactions]";
                    }
                    t--;  // shrinking is not a retry: do not consume the budget
                    continue;
                }
                if (err=="fatal"){
                    cerr<<"generate failed: permanent provider error, not retrying\n";
                    return "[provider error: "+provider+" "+model+" permanently rejected the request (see stderr); not retrying]";
                }
            }
            cerr<<"generate failed after "<<MAX_TRIES<<" attempts\n";
            return "[provider failure: "+provider+" "+model+" did not answer after "+to_string(MAX_TRIES)+" attempts; see stderr for the last error]";
        }
};

class web_backend{
    public:
        string provider;
        string key;
        unique_ptr<clover_client> cli;
        web_backend(const config& c):provider(c.web_provider), key(c.web_key){ build(); }
        void build(){
            cli=make_unique<clover_client>(provider);
            cli->set_keep_alive(false);
            cli->set_connection_timeout(0,3000000);
            cli->set_read_timeout(300,0);
            cli->set_default_headers({{"Authorization", "Bearer "+key}});
        }
        string search(const string& query){
            json m;
            m["query"]=query; m["count"]=10;
            m["contents"]={{"text",true}};
            m["freshness"]="noLimit";
            cerr<<"web search: "<<query<<'\n';
            // 3 attempts with short doubling sleeps: web search is
            // non-critical enrichment, so a light retry beats the chat
            // path's heavy one.
            for (int t=0;t<3;t++){
                if (t>0) this_thread::sleep_for(chrono::milliseconds(1500<<t));
                auto res=cli->Post("/v1/web-search", m.dump(), "application/json");
                if (res && res->status==200){
                    try{ return json::parse(res->body).dump(); }
                    catch (const json::parse_error& e){ cerr<<"web search json parse error:\n"<<e.what()<<'\n'; }
                } else if (res){
                    cerr<<"web search http error: status "<<res->status<<'\n';
                } else {
                    cerr<<"web search http error: failed to connect\n";
                }
            }
            return "";
        }
};

// base64 for embedding screenshots as data-url image blocks in tool
// result messages (vision-capable models read them; others ignore or the
// provider errors, so attach only when the model is on the vision list).
static string b64_enc(const string& in){
    static const char* T="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    string out;
    out.reserve((in.size()+2)/3*4);
    size_t i=0;
    while (i+2<in.size()){ 
        uint32_t v=(uint8_t)in[i]<<16|(uint8_t)in[i+1]<<8|(uint8_t)in[i+2];
        out+=T[v>>18&63]; out+=T[v>>12&63]; out+=T[v>>6&63]; out+=T[v&63];
        i+=3;
    }
    size_t rem=in.size()-i;
    if (rem==1){
        uint32_t v=(uint8_t)in[i]<<16;
        out+=T[v>>18&63]; out+=T[v>>12&63]; out+="==";
    } else if (rem==2){
        uint32_t v=(uint8_t)in[i]<<16|(uint8_t)in[i+1]<<8;
        out+=T[v>>18&63]; out+=T[v>>12&63]; out+=T[v>>6&63]; out+='=';
    }
    return out;
}

// loads a file wholly. returns empty on failure.
static string slurp_file(const string& path){
    ifstream f(path, ios::binary);
    if (!f) return "";
    ostringstream ss; ss<<f.rdbuf();
    return ss.str();
}

// models that accept image_url content parts.
static const vector<string> vision_models={"deepseek-v4.1-flash-1017","glm-5.3","glm-5.3-flash","gemma-4-26b","qwen-3.8-27b","obi2",
                                        "gemini-3.8-flash","gemini-3.7-flash","gemini-3.6-flash","gemini-3.5-flash","gemini-2.5-flash","gemini-3.1-flash-lite","gemini-2.5-flash-lite"};
static bool model_is_vision(const string& model){
    for (auto& v:vision_models) if (model==v) return true;
    return false;
}

// true when any message carries a structured (image) content array. images
// are never required: a text-only model just gets the text half.
static bool has_image(const json& messages){
    if (!messages.is_array()) return false;
    for (auto& m: messages){
        if (!m.is_object() || !m.contains("content")) continue;
        if (m["content"].is_array()){
            for (auto& part: m["content"])
                if (part.is_object() && part.value("type","")=="image_url") return true;
        }
    }
    return false;
}

// extract screenshot png paths from a tool-results text block. attaches the
// images as openai-style image_url blocks. when the model is not vision-
// capable the text stays a plain string (providers reject mixed content for
// text-only models). returns the final message json.
static json attach_images(const string& text, const string& model){
    if (g_vision_denied || !model_is_vision(model)) return text;
    vector<string> paths;
    size_t pos=0;
    while (true){
        size_t hit=text.find("screenshot saved: ", pos);
        if (hit==string::npos) break;
        size_t s=hit+18;
        size_t e=text.find(' ', s);
        if (e==string::npos) e=text.size();
        string p=text.substr(s, e-s);
        if (p.size()>4 && p.substr(p.size()-4)==".png") paths.push_back(p);
        pos=e;
    }
    if (paths.empty()) return text;
    json content=json::array();
    content.push_back({{"type","text"},{"text",text}});
    int attached=0;
    for (auto& p:paths){
        if (attached>=3) break;
        string png=slurp_file(p);
        if (png.empty()) continue;
        if (png.size()>3*1024*1024) continue; // cap 3MB per image
        content.push_back({{"type","image_url"},{"image_url",{{"url","data:image/png;base64,"+b64_enc(png)}}}});
        attached++;
    }
    if (attached==0) return text;
    return content;
}

// returns content of first balanced [] starting at or after pos, advancing
// `end` to just past the closing bracket. returns nullopt if none.
static optional<string> extract_array(const string& s, size_t start, size_t& end){
    size_t open=s.find('[', start);
    if (open==string::npos) return nullopt;
    int depth=0;
    bool in_str=false, esc=false;
    for (size_t i=open;i<s.size();i++){
        char c=s[i];
        if (in_str){
            if (esc) esc=false;
            else if (c=='\\') esc=true;
            else if (c=='"') in_str=false;
            continue;
        }
        if (c=='"'){ in_str=true; continue; }
        if (c=='[') depth++;
        else if (c==']'){ depth--; if (depth==0){ end=i+1; return s.substr(open,i-open+1); } }
    }
    return nullopt;
}

// how many recent non-system messages the context file keeps. older turns are
// dropped at save time: long-term recall belongs to the memory tools, not to
// replaying the whole transcript. keeps a tool-results block intact.
static constexpr size_t CTX_KEEP = 300;

// Proactive byte heuristic, not a tokenizer or a guaranteed context fit.
// Relay windows were observed from /v1/models; the logfare value below is
// an assumption. Three bytes per window token estimates 75% occupancy at
// four bytes/token, but token density varies across languages and payloads.
// Keep reactive provider-size handling as the final safety net.
static size_t context_budget_bytes(const string& model, const string& provider){
    long long window_tokens=0;
    if (provider=="scandata-relay.zarnkina.workers.dev"){
        if (model=="zarnkina-gpt-6.1-sol" || model=="zarnkina-gpt-6-astra" ||
            model=="zarnkina-gpt-6.1-terra" || model=="zarnkina-gpt-6-sol") window_tokens=256000;
        else if (model=="zarnkina-mercury-3") window_tokens=1000000;
        else if (model=="zarnkina-mercury-2.5") window_tokens=260000;
        else if (model=="zarnkina-mercury-2") window_tokens=128000;
        else if (model=="zarnkina-deepseek-v4-flash") window_tokens=400000;
        else if (model=="zarnkina-minimax-m2.7") window_tokens=180000;
        else if (model=="zarnkina-glm-5.3-flash") window_tokens=400000;
    } else if (provider=="logfare.ai"){
        // logfare does not publish windows; qwen-3.8-27b is 128K class
        if (model=="qwen-3.8-27b") window_tokens=128000;
    }
    if (window_tokens>0){
        size_t budget = (size_t)(window_tokens * 3 / 4) * 4; // ~4 bytes/token
        if (budget<32*1024) budget=32*1024;
        if (budget>CTX_MAX_BYTES) budget=CTX_MAX_BYTES;
        return budget;
    }
    // unknown model: stay under the global cap with headroom
    return (size_t)((double)CTX_MAX_BYTES * 0.75);
}

// how many messages the unit starting at i spans: a plain message is 1; a
// [tool results] block is 2 (it is bound to its trailing <continue>). both
// trimmers use this so an exchange is never split, and the rule lives once.
static size_t exchange_span(const json& messages, size_t i){
    if (i>=messages.size()) return 0;
    string role=messages[i].value("role","");
    string content;
    if (messages[i].contains("content") && messages[i]["content"].is_string())
        content=messages[i]["content"].get<string>();
    if (role=="user" && content.rfind("[tool results]",0)==0 && i+1<messages.size()){
        string r2=messages[i+1].value("role","");
        string c2;
        if (messages[i+1].contains("content") && messages[i+1]["content"].is_string())
            c2=messages[i+1]["content"].get<string>();
        if (r2=="user" && c2=="<continue>") return 2;
    }
    return 1;
}

// drops the oldest messages beyond the keep window, never splitting a tool
// exchange: if the window starts on a tool-results/continue pair the leading
// pair is dropped too. the system prompt is never touched.
static json trim_context(const json& messages){
    if (!messages.is_array()) return messages;
    // find first non-system index
    size_t first=0;
    while (first<messages.size() && messages[first].value("role","")=="system") first++;
    size_t nonsys=messages.size()-first;
    if (nonsys<=CTX_KEEP) return messages;
    size_t start=first+(nonsys-CTX_KEEP);
    // snap rule: never split a tool exchange. two boundary cases:
    // window starts on [tool results]: skip it and its <continue> (start += 2)
    // window starts on <continue>: pull the cut [tool results] back in (start--)
    if (start>first && start<messages.size()){
        string role=messages[start].value("role","");
        string content;
        if (messages[start].contains("content") && messages[start]["content"].is_string())
            content=messages[start]["content"].get<string>();
        if (role=="user" && content.rfind("[tool results]",0)==0){
            start+=exchange_span(messages, start);
        } else if (role=="user" && content=="<continue>"){
            start--;
        }
    }
    json out=json::array();
    for (size_t i=0;i<first;i++) out.push_back(messages[i]);
    for (size_t i=start;i<messages.size();i++) out.push_back(messages[i]);
    return out;
}

// byte-based budget. message-count trimming alone is not enough: a single
// screenshot can be a few hundred KB, so 300 messages easily exceed the
// provider's request size and surface as a 413. drops whole tool exchanges
// from the oldest end until the serialized context fits. system untouched.
static json trim_context_bytes(const json& messages, size_t max_bytes){
    if (!messages.is_array()) return messages;
    size_t first=0;
    while (first<messages.size() && messages[first].value("role","")=="system") first++;
    // suffix[i] = serialized size of messages[i..]; precomputed once so the
    // drop loop is O(n) instead of re-dumping the whole context per step.
    vector<size_t> suffix(messages.size()+1, 0);
    for (size_t i=messages.size(); i>first; ){
        --i;
        suffix[i]=suffix[i+1]+messages[i].dump().size();
    }
    size_t sysbytes=0;
    for (size_t i=0;i<first;i++) sysbytes+=messages[i].dump().size();
    auto size_at=[&](size_t s){ return sysbytes + (s<messages.size()?suffix[s]:0); };
    size_t start=first;
    while (start<messages.size() && size_at(start)>max_bytes)
        start+=exchange_span(messages, start);  // drop whole units, never split
    json out=json::array();
    for (size_t i=0;i<first;i++) out.push_back(messages[i]);
    for (size_t i=start;i<messages.size();i++) out.push_back(messages[i]);
    return out;
}

class context_store{
    public:
        string path;
        context_store(const string& p):path(p){}

        // loads the messages array from the file. returns nullopt if missing
        // or unusable. on corruption the bad file is moved aside.
        optional<json> load(){
            ifstream f(path);
            if (!f.is_open()) return nullopt;
            stringstream buf; buf<<f.rdbuf();
            string body=buf.str();
            if (body.empty()) return nullopt;
            try{
                json doc=json::parse(body);
                if (!doc.is_object() || !doc.contains("messages") || !doc["messages"].is_array()){
                    cerr<<"context file "<<path<<" missing messages array; starting fresh\n";
                    quarantine();
                    return nullopt;
                }
                return doc["messages"];
            } catch (const json::parse_error& e){
                cerr<<"context file "<<path<<" is corrupt ("<<e.what()<<"); moving aside and starting fresh\n";
                quarantine();
                return nullopt;
            }
        }

        // atomically writes messages + metadata (temp file then rename).
        bool save(const json& messages, const string& model, const string& provider){
            json doc;
            doc["version"]=1;
            doc["model"]=model;
            doc["provider"]=provider;
            doc["updated_at"]=iso8601_now();
            doc["messages"]=trim_context(messages);
            // context holds private conversation state: new files are 0600,
            // and an existing file keeps its current mode across the
            // tmp+rename cycle (rename never carries modes itself).
            mode_t newmode=0600;
            struct stat st;
            if (stat(path.c_str(), &st)==0) newmode=st.st_mode & 07777;
            string tmp=path+".tmp";
            {
                ofstream f(tmp, ios::binary|ios::trunc);
                if (!f.is_open()){ cerr<<"cannot open "<<tmp<<" for writing\n"; return false; }
                f<<doc.dump(2);
                f.flush();
                if (!f.good()){ cerr<<"write error to "<<tmp<<"\n"; return false; }
            }
            if (chmod(tmp.c_str(), newmode)!=0){
                cerr<<"cannot set permissions on "<<tmp<<": "<<strerror(errno)<<'\n';
                unlink(tmp.c_str());
                return false;
            }
            if (rename(tmp.c_str(), path.c_str())!=0){
                cerr<<"cannot rename "<<tmp<<" to "<<path<<": "<<strerror(errno)<<"\n";
                return false;
            }
            return true;
        }

    private:
        void quarantine(){
            string bad=path+".corrupt";
            if (rename(path.c_str(), bad.c_str())==0)
                cerr<<"moved corrupt context to "<<bad<<"\n";
        }
        static string iso8601_now(){
            auto t=chrono::system_clock::to_time_t(chrono::system_clock::now());
            struct tm tmv; gmtime_r(&t,&tmv);
            char b[32]; strftime(b,sizeof(b),"%Y-%m-%dT%H:%M:%SZ",&tmv);
            return b;
        }
};

// extract an id label without throwing, whatever the model emitted.
static string safe_id(const json& tool){
    if (!tool.is_object() || !tool.contains("id")) return "-1";
    const json& id=tool["id"];
    if (id.is_number_integer()) return to_string(id.get<long long>());
    if (id.is_number()) return to_string(id.get<double>());
    if (id.is_string()) return id.get<string>();
    return id.dump();
}

// replaces every occurrence of marker in out with value, left to right,
// never rescanning replaced text: self-referential values terminate.
static void substitute_all(string& out, const string& marker, const string& value){
    size_t pos=0;
    while ((pos=out.find(marker, pos))!=string::npos){
        out.replace(pos, marker.size(), value);
        pos+=value.size();
    }
}

// renders the sysprompt template: {{TOOLS}} from the registry, {{MODEL}} and
// {{PROVIDER}} from live config. the prompt can never go stale: adding a tool
// or switching the model updates it on the next start (or /model, /provider).
static string render_sysprompt(const string& tpl, const string& model, const string& provider){
    string out=tpl;
    string tools=render_tools();
    // single-pass substitutes, scanning past each replacement:
    // a value that itself contains a marker cannot loop forever
    substitute_all(out, "{{TOOLS}}", tools);
    substitute_all(out, "{{MODEL}}", model);
    substitute_all(out, "{{PROVIDER}}", provider);
    return out;
}

class agent{
    public:
        string model;
        string sys_tpl;
        string name;
        json ctx;
        chat_backend& chat;
        web_backend& web;
        tool_context tools;
        context_store* store=nullptr;
        int max_tool_rounds=250;
        turn_indicator ind_;
        int audit_every=25;   // self-audit every N turns; 0 disables
        string audit_path="context.json.audit";
        void save_ctx(){
            ctx=trim_context_bytes(ctx, context_budget_bytes(model, chat.provider));
            if (store) store->save(ctx, model, chat.provider);
        }
        // trim in memory right before the request is sent: the file is only
        // written on save, so an in-memory ctx could grow unchecked between
        // turns and blow the provider's request size. old images are dropped
        // first: they are the densest bytes in the transcript and a stale
        // screenshot is worth less than the exchanges it would evict.
        void compact(){
            size_t budget=context_budget_bytes(model, chat.provider);
            if (ctx.dump().size()<=budget) return;
            json lean=strip_images(ctx);
            if (lean.dump().size()<=budget){ ctx=lean; return; }
            ctx=trim_context_bytes(lean, budget);
        }
        agent(const string& modelname, const string& sysprompt, const string& agentname,
              chat_backend& ch, web_backend& wb)
            :model(modelname), sys_tpl(sysprompt), name(agentname), chat(ch), web(wb){
            ctx.push_back({{"role", "system"}, {"content", render_sysprompt(sys_tpl, model, chat.provider)}});
        }
        // rebuilds the system message after a /model or /provider change
        void refresh_sysprompt(){
            if (ctx.empty()) return;
            ctx[0]["content"]=render_sysprompt(sys_tpl, model, chat.provider);
            save_ctx();  // persist immediately: a /quit right after must not lose it
        }
        // load prior messages from the store, keeping the freshly-seeded
        // system prompt authoritative (stored system msgs are ignored).
        void load_from_store(){
            if (!store) return;
            auto stored=store->load();
            if (!stored) return;
            int added=0;
            for (auto& m: *stored){
                if (!m.is_object() || !m.contains("role") || !m.contains("content")) continue;
                if (!m["role"].is_string()) continue; // malformed: skip, do not crash
                if (m["role"].get<string>()=="system") continue;
                ctx.push_back(m); added++;
            }
            if (added){
                // the file was saved trimmed; mirror it in memory so the
                // very first api call does not ship stale history.
                json trimmed=trim_context(ctx);
                if (trimmed.size()<ctx.size()) ctx=trimmed;
                cerr<<"loaded "<<added<<" prior message(s) from "<<store->path<<'\n';
                cerr<<"ctx trimmed to "<<ctx.size()-1<<" non-system messages"<<'\n';
            }
        }
        void clear_context(){
            if (ctx.size()>1) ctx.erase(ctx.begin()+1, ctx.end());
            save_ctx();
        }

        string dispatch_tool(const json& tool){
            string result;
            try{
                string tname=tool.value("tool", string(""));
                cerr<<"[tool: "<<tname<<"]\n";
                const ::tool* t=find_tool(tname);
                if (!t){
                    // helpful: list what is available so a model can recover
                    result="unknown tool: "+tname+". available:";
                    for (auto& n: tool_names()) result+=" "+n;
                    result+="\n";
                } else {
                    result=t->run(tool, tools);
                }
            } catch (const exception& e){
                result=string("tool dispatch error: ")+e.what()+"\n";
            } catch (...){
                result="tool dispatch error: unknown exception\n";
            }
            return result;
        }

        // persists a turn counter and, every N turns, injects the periodic
        // self-audit request. the counter lives beside the context file so it
        // survives restarts; auditing is a standing rule, not an extra.
        void maybe_audit(){
            if (audit_every<=0) return;
            int n=0;
            { ifstream in(audit_path); if (in>>n) {} }
            n++;
            { ofstream out(audit_path, ios::trunc); out<<n; }
            if (n % audit_every != 0) return;
            cerr<<"[periodic self-audit due (turn "<<n<<")]\n";
            ctx.push_back({{"role", "user"}, {"content",
                "periodic self-audit (turn "+to_string(n)+"). review the work since the last audit and "
                "report concisely: (1) anything asserted without verifying, (2) any change made without a "
                "test that exercised the production path, (3) stale binaries, uncommitted work, or "
                "unlogged self-modifications, (4) tool-call format mistakes that wasted turns, (5) any "
                "claim of completion that is not true. fix what you find rather than only describing it. "
                "if nothing is wrong, say so in one line."}});
            save_ctx();
        }

        // processes one assistant reply: prints leading text, runs any tool
        // blocks, then continues the conversation until a non-tool reply.
        string f(const string& msg){
            ind_.begin(name);
            ctx.push_back({{"role", "user"}, {"content", msg}});
            save_ctx();
            maybe_audit();
            std::vector<std::string> tnames;
            auto opmem_done=[&](const string& extra){
                std::string s;
                for (auto& t : tnames) s += t + "; ";
                std::string tail=extra;
                size_t sp=tail.find_first_not_of(" \n\t");
                if (sp!=string::npos) tail=tail.substr(sp);
                for (auto& c : tail) if (c=='\n'||c=='\r'||c=='\t') c=' ';
                if (tail.size()>140) tail=tail.substr(0,140)+"...";
                opmem_log(msg, (s.empty()?"": "tools["+s+"] ") + tail);
            };
            for (int round=0; round<max_tool_rounds; round++){
                ind_.rnd=round+1;
                ind_.resume("model");
                compact();
                // a screenshot makes the payload multimodal. if the active model
                // cannot take images, borrow a vision model for this request and
                // switch straight back: the transcript keeps our identity/model.
                string sendmodel=model;
                if (has_image(ctx) && !model_is_vision(model) && !g_vision_denied
                        && chat.provider=="logfare.ai"){
                    sendmodel="deepseek-v4.1-flash-1017";
                    cerr<<"image in context; using "<<sendmodel<<" for this request\n";
                }
                // streaming display: prose reaches the terminal as it arrives;
                // the moment a tool header (or an xml-call signature) appears
                // the display hides the rest - payloads never reach the screen.
                string held; size_t sd_frozen=0; bool sd_hidden=false;
                const string bad_sig = string("<f")+"unction_calls>";
                const string bad_open = string("<f")+"unction=";
                auto display=[&](const string& t){
                    if (sd_hidden) return;
                    held+=t;
                    size_t p=held.find(toolh);
                    if (p==string::npos) p=held.find(bad_sig);
                    if (p==string::npos) p=held.find(bad_open);
                    if (p!=string::npos){ sd_hidden=true; return; }
                    // withhold a small tail: a header straddling two chunks
                    // must never leak the first half before the second
                    size_t stop = held.size()>32 ? held.size()-32 : 0;
                    while (stop>0 && (unsigned char)held[stop]>=0x80 && (unsigned char)held[stop]<=0xBF) stop--;
                    if (stop>sd_frozen){
                        string part=held.substr(sd_frozen, stop-sd_frozen);
                        ind_.prose(part);
                        cout<<part<<flush;
                        sd_frozen=stop;
                    }
                };
                function<void(const string&)> emit = ui_on()
                    ? function<void(const string&)>(display)
                    : function<void(const string&)>{};
                string rsp=chat.generate(sendmodel, ctx, emit);
                ctx.push_back({{"role", "assistant"}, {"content", rsp}});
                save_ctx();

                // some models emit xml-ish tool calls despite the standing
                // instruction not to. detect the signature and correct in-band
                // instead of letting dead protocol text reach the user.
                {
                    const string bad_sig = string("<f")+"unction_calls>";
                    const string bad_open = string("<f")+"unction=";
                    size_t hit = rsp.find(bad_sig);
                    if (hit==string::npos) hit = rsp.find(bad_open);
                    if (hit!=string::npos){
                        cerr<<"[rejected xml-style tool call; asking model to retry]\n";
                        ctx.push_back({{"role", "user"}, {"content",
                            "your last message used an xml-style tool block. that format is not "
                            "parsed and did nothing. do not use it again: issue the tool call as a "
                            "line that is exactly the tool header followed by a json array, or reply "
                            "in plain prose if no tool is needed. retry now."}});
                        save_ctx();
                        if (round+1>=max_tool_rounds){
                            ind_.end();
                            opmem_done("(model kept emitting an unsupported tool format)");
                            return "(the model kept emitting an unsupported tool format; "
                                   "ask again to retry)";
                        }
                        continue;
                    }
                }

                size_t headerpos=rsp.find(toolh);
                if (ui_on()){
                    // Flush withheld prose up to (but never including) the
                    // tool header. A header can arrive in the very first delta.
                    // A generated failure notice replaces the partial stream;
                    // its offsets are unrelated to the text already displayed.
                    size_t end=headerpos==string::npos ? rsp.size() : headerpos;
                    if (rsp.compare(0, sd_frozen, held, 0, sd_frozen)!=0){
                        if (sd_frozen) { ind_.prose("\n"); cout<<'\n'; }
                        sd_frozen=0;
                    }
                    if (sd_frozen<end){
                        string part=rsp.substr(sd_frozen, end-sd_frozen);
                        ind_.prose(part);
                        cout<<part<<flush;
                        sd_frozen=end;
                    }
                }
                if (headerpos==string::npos){
                    ind_.end();
                    opmem_done(rsp);
                    return ui_on()? string("") : rsp;
                }
                if (!ui_on()){
                    // print any prose before the first tool header (flushed so
                    // a pty parent sees progress mid-turn)
                    ind_.prose(rsp.substr(0, headerpos));
                    cout<<rsp.substr(0, headerpos)<<flush;
                }

                string toolres=toolrestag+"\n";
                bool any=false;
                size_t cursor=headerpos;
                while (true){
                    size_t next=rsp.find(toolh, cursor);
                    if (next==string::npos) break;
                    size_t after=next+toolh.size();
                    size_t arr_end=0;
                    auto arr=extract_array(rsp, after, arr_end);
                    if (!arr){
                        cerr<<"malformed tool call: no json array after tool header\n";
                        toolres+="tool result, id: -1\n";
                        toolres+="error: no json array found after the tool header; re-emit the header followed by a valid json array\n";
                        any=true;
                        cursor=after;
                        continue;
                    }
                    json parsed;
                    try{
                        parsed=json::parse(*arr);
                    } catch (const json::parse_error& e){
                        cerr<<"tool json parse error:\n"<<e.what()<<'\n';
                        toolres+="tool result, id: -1\n";
                        toolres+="error: tool json did not parse: "+string(e.what())+"\n";
                        any=true;
                        cursor=arr_end;
                        continue;
                    }
                    if (!parsed.is_array()){
                        cerr<<"tool call is not a json array\n";
                        toolres+="tool result, id: -1\n";
                        toolres+="error: [TOOL] block was not a json array\n";
                        any=true;
                        cursor=arr_end;
                        continue;
                    }
                    for (auto& tool: parsed){
                        if (!tool.is_object()){
                            cerr<<"tool entry is not an object\n";
                            toolres+="tool result, id: -1\n";
                            toolres+="error: tool entry is not an object\n";
                            any=true;
                            continue;
                        }
                        toolres+="tool result, id: "+safe_id(tool)+'\n';
                        toolres+="tool name: "+tool.value("tool", string(""))+'\n';
                        if (tool.contains("tool"))
                            tnames.push_back(tool.value("tool", string("")));
                        ind_.set_tag("tool "+tool.value("tool", string("")));
                        if (ui_on()){
                            ind_.prose("\n");
                            cout<<"  \xe2\x94\x8c\xe2\x94\x80 "<<tool.value("tool", string(""))
                                 <<" #"<<safe_id(tool)<<"\n"<<flush;
                        }
                        auto t0=chrono::steady_clock::now();
                        string tres=dispatch_tool(tool);
                        if (ui_on()){
                            istringstream ls(tres); string line; int n=0;
                            while (n<6 && getline(ls,line)){
                                for (char& c : line) if (unsigned(c)<32) c=' ';
                                if (line.size()>160) line=line.substr(0,160)+"\xe2\x80\xa6";
                                cout<<"  \xe2\x94\x82 "<<line<<"\n"; n++;
                            }
                            if (ls.peek()!=EOF)
                                cout<<"  \xe2\x94\x82 \xe2\x80\xa6 (full result kept for the model)\n";
                            double dt=chrono::duration<double>(chrono::steady_clock::now()-t0).count();
                            char tbuf[32]; snprintf(tbuf,sizeof(tbuf),"%.2f",dt);
                            cout<<"  \xe2\x94\x94\xe2\x94\x80 "<<tres.size()<<" bytes \xc2\xb7 "<<tbuf<<"s\n"<<flush;
                        }
                        toolres+=tres;
                        any=true;
                    }
                    cursor=arr_end;
                }
                if (!any){
                    // header with nothing runnable beneath it: tell the model,
                    // let it retry instead of dumping raw tool text at the user
                    toolres+="error: [TOOL] header with no tool array after it; fix and re-emit\n";
                    ctx.push_back({{"role", "user"}, {"content", attach_images(toolres, model)}});
                    ctx.push_back({{"role", "user"}, {"content", toolcontinue}});
                    save_ctx();
                    continue;
                }
                // cap an oversized tool result before it enters the transcript:
                // a raw run_command/read_file dump can exceed the per-model budget
                // and would survive save_ctx's trim until the NEXT compact().
                // keep head and tail; mark the cut so the model knows it was trimmed.
                {
                    static constexpr size_t TOOLRES_MAX=196*1024;
                    if (toolres.size()>TOOLRES_MAX){
                        size_t keep=(TOOLRES_MAX-192)/2;
                        string cut="\n[...tool result truncated: "+to_string(toolres.size())+
                                       " bytes; head "+to_string(keep)+" + tail "+to_string(keep)+
                                       " kept for the model...]\n";
                        toolres=toolres.substr(0,keep)+cut+toolres.substr(toolres.size()-keep);
                    }
                }
                ctx.push_back({{"role", "user"}, {"content", attach_images(toolres, model)}});
                ctx.push_back({{"role", "user"}, {"content", toolcontinue}});
                save_ctx();
            }
            cerr<<"reached max tool rounds ("<<max_tool_rounds<<")\n";
            ind_.end();
            opmem_done("[max tool rounds reached]");
            return "[max tool rounds reached]\n";
        }
};

static void print_header(const agent& a, const string& provider){
    cout<<'['<<a.name<<" :: "<<provider<<'/'<<a.model<<"]\n";
}

// non-interactive: read all stdin, run one turn, print result.
static int run_test(agent& a, chat_backend& chat){
    stringstream ss; ss<<cin.rdbuf();
    string message=ss.str();
    while (!message.empty() && (message.back()=='\n'||message.back()=='\r')) message.pop_back();
    if (message.empty()){ cerr<<"test mode: empty stdin\n"; return 1; }
    print_header(a, chat.provider);
    cout<<a.f(message);
    cout<<'\n';
    return 0;
}

static int run_convo(agent& a, chat_backend& chat){
    while (true){
        string message, line;
        if (ui_on()) cout<<C_WHITE<<"\u2192 ";
        else cout<<"\u2192 ";
        cout.flush();
        if (!getline(cin, line)) {
            cout<<'\n';
            break; // eof: stop instead of busy-looping
        }
        if (line=="\"\"\""){
            bool closed=false;
            while (getline(cin, line)){
                if (line=="\"\"\""){ closed=true; break; }
                message+=line+'\n';
            }
            if (!closed){ cerr<<"unterminated multiline block\n"; continue; }
        } else message=line;
        stringstream ss(message);
        string tok;
        vector<string> cmd;
        while (ss>>tok) cmd.push_back(tok);
        if (cmd.empty()) continue;
        if (cmd[0]=="/clear_ctx"){
            a.clear_context();
            cout<<"context cleared\n";
        } else if (cmd[0]=="/model"){
            if (cmd.size()>1){ a.model=cmd[1]; a.refresh_sysprompt(); ui_term_title(a.name+" :: "+chat.provider+"/"+a.model); cout<<"model set to "<<a.model<<'\n'; }
            else cout<<"usage: /model [model]\n";
        } else if (cmd[0]=="/provider"){
            if (cmd.size()>1){ chat.set_provider(cmd[1]); a.refresh_sysprompt(); ui_term_title(a.name+" :: "+chat.provider+"/"+a.model); cout<<"provider set to "<<chat.provider<<'\n'; }
            else cout<<"usage: /provider [provider]\n";
        } else if (cmd[0]=="/help"){
            cout<<"/clear_ctx  /model <m>  /provider <p>  /help  /quit\n";
        } else if (cmd[0]=="/quit" || cmd[0]=="/exit"){
            break;
        } else {
            print_header(a, chat.provider);
            cout<<a.f(message);
            cout<<'\n';
        }
    }
    return 0;
}

int main(int argc, char** argv){
    bool test=false;
    config cfg;
    for (int i=1;i<argc;i++){
        string a=argv[i];
        if (a=="-t"||a=="--test") test=true;
        else if (a=="--model"){
            if (i+1>=argc){ cerr<<"--model requires a name\n"; return 2; }
            cfg.model=argv[++i];
        }
        else if (a=="--provider"){
            if (i+1>=argc){ cerr<<"--provider requires a host\n"; return 2; }
            cfg.provider=argv[++i];
        }
        else if (a=="--ctx"){
            if (i+1>=argc){ cerr<<"--ctx requires a path\n"; return 2; }
            cfg.ctxfile=argv[++i];
        }
        else if (a=="--purpose"){
            if (i+1>=argc){ cerr<<"--purpose requires a text\n"; return 2; }
            cfg.purpose=argv[++i];
        } else if (a=="-h"||a=="--help"){
            cout<<"usage: "<<argv[0]<<" [-t|--test] [--ctx <path>]\n";
            cout<<"  -t, --test      read all stdin, run one turn, print result, exit\n";
            cout<<"  --ctx <path>    persistent context file (default context.json)\n";
            cout<<"  --provider <h>  chat provider host (default is the compiled-in one)\n";
            cout<<"  --model <name>  chat model name\n";
            return 0;
        } else { cerr<<"unknown argument: "<<a<<'\n'; return 2; }
    }
    // test mode is explicit only (-t/--test); a bare pipe still drives the
    // interactive loop so slash-commands stay scriptable.

    // resolve the sysprompt next to the executable when the cwd differs:
    // launching from another directory must not break the agent.
    string promptpath=cfg.promptfile;
    {
        ifstream probe(promptpath);
        if (!probe.is_open()){
            char exepath[4096]={};
            ssize_t n=readlink("/proc/self/exe", exepath, sizeof(exepath)-1);
            if (n>0){
                string exe(exepath, (size_t)n);
                size_t slash=exe.rfind("/");
                if (slash!=string::npos){
                    string cand=exe.substr(0, slash+1)+promptpath;
                    ifstream probe2(cand);
                    if (probe2.is_open()) promptpath=cand;
                }
            }
        }
    }
    ifstream sysfile(promptpath);
    if (!sysfile.is_open()){ cerr<<"could not open sysprompt file "<<promptpath<<'\n'; return 1; }
    stringstream buf; buf<<sysfile.rdbuf();
    string system_prompt=buf.str();

    // a subagent is a copy of clover framed by a purpose: the purpose block
    // leads the system prompt so it steers everything that follows.
    if (!cfg.purpose.empty()){
        system_prompt=
            "# subagent\n"
            "you are a subagent: a fresh copy of clover created for one purpose, isolated from the user.\n"
            "\n"
            "purpose: "+cfg.purpose+"\n"
            "\n"
            "# subagent principles\n"
            "- do the task fully, then check your own work before reporting: re-verify critical facts, re-run key checks, and report only what you confirmed.\n"
            "- your context and memory are scratch - nothing carries over. everything you need must be in the task; state any assumptions you make.\n"
            "- you cannot ask the user questions. if the task is impossible or ambiguous, say so plainly instead of guessing.\n"
            "- do not modify system files, do not touch the git repo, and do not spawn subagents unless the task requires it.\n"
            "- keep the final report concise and factual.\n"
            "\n\n"+system_prompt;
    }

    // keys live in the environment now; refuse to start without them so a
    // missing key is a clear startup error, never a mystery network failure.
    string active_chat_key = chat_backend::key_for_provider(cfg.provider);
    if (active_chat_key.empty() || cfg.web_key.empty()){
        cerr<<"error: no chat key for provider "<<cfg.provider
            <<" (set CLOVER_CHAT_KEY / CLOVER_OR_KEY / CLOVER_GM_KEY / CLOVER_ZK_KEY) and CLOVER_WEB_KEY"<<'\n';
        return 1;
    }
    chat_backend chat(cfg);
    web_backend web(cfg);
    context_store store(cfg.ctxfile);
    agent clover(cfg.model, system_prompt, "☘ clover", chat, web);
    if (!cfg.purpose.empty()) clover.name="subagent";
    clover.tools.web_search=[&web](const string& q){ return web.search(q); };
    clover.tools.chat_provider=[&chat](){ return chat.provider; };
    clover.tools.chat_model=[&clover](){ return clover.model; };
    clover.store=&store;
    clover.audit_path=cfg.ctxfile+".audit";
    clover.load_from_store();
    // ensure the file exists even before the first message
    store.save(clover.ctx, clover.model, chat.provider);

    if (test) return run_test(clover, chat);
    ui_banner(clover.name, chat.provider, clover.model);
    ui_install_terminate_guard();
    return run_convo(clover, chat);
}
