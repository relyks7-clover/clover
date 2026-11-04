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

struct config{
    string provider="logfare.ai";
    string web_provider="api.langsearch.com";
    string model="deepseek-v4.1-flash-1017";
    string promptfile="sysprompts/clover.txt";
    string chat_key=getenv("CLOVER_CHAT_KEY")?getenv("CLOVER_CHAT_KEY"):"";
    string or_key=getenv("CLOVER_OR_KEY")?getenv("CLOVER_OR_KEY"):"";
    string web_key=getenv("CLOVER_WEB_KEY")?getenv("CLOVER_WEB_KEY"):"";
    string ctxfile="context.json";
    string purpose;
    static string chat_key_for(){ return getenv("CLOVER_CHAT_KEY")?getenv("CLOVER_CHAT_KEY"):""; }
    static string or_key_for(){ return getenv("CLOVER_OR_KEY")?getenv("CLOVER_OR_KEY"):""; }
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

class chat_backend{
    public:
        string provider;
        string key;
        unique_ptr<clover_client> cli;
        chat_backend(const config& c):provider(c.provider), key(c.provider=="openrouter.ai"?config::or_key_for():config::chat_key_for()){ build(); }
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
            if (provider=="openrouter.ai") key=config::or_key_for();
            else key=config::chat_key_for();
            build();
        }
        // one request attempt. returns content on 200 with a parsable
        // body. err is cleared on success, otherwise set to one of:
        //   "retry"   - transient (429/5xx/connect/parse), retry with backoff
        //   "novision"- provider 400 rejecting image parts; strip images, retry
        //   "fatal"   - other 4xx; retrying is pointless, fail fast
        string attempt(const string& model, const json& context, string& err){
            err.clear();
            json m; m["model"]=model;
            m["stream"]=false;
            m["messages"]=context;
            // openrouter exposes its api under /api/v1, everyone else /v1
            string path = (provider=="openrouter.ai") ? "/api/v1/chat/completions" : "/v1/chat/completions";
            auto res=cli->Post(path.c_str(), m.dump(), "application/json");
            if (res && res->status==200){
                try{
                    auto resp_json=json::parse(res->body);
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
            string body = res ? res->body : "";
            int st = res ? res->status : 0;
            if (!res){
                cerr<<"http error: failed to connect\n"; err="retry"; return "";
            }
            cerr<<"http error: status "<<st<<"\nbody:\n"<<body<<'\n';
            if (st==413){
                // request too large: not a transient failure. signal the caller
                // to shrink the payload (drop images, then hard-compact).
                err="toolarge"; return "";
            }
            if (st==400){
                string b=body; transform(b.begin(),b.end(),b.begin(),::tolower);
                if (b.find("multimodal")!=string::npos || b.find("image")!=string::npos){
                    err="novision"; return "";
                }
                if (b.find("too large")!=string::npos || b.find("maximum context")!=string::npos ||
                    b.find("context length")!=string::npos || b.find("token")!=string::npos){
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
        string generate(const string& model, const json& context){
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
                string out=attempt(model, ctx, err);
                if (!out.empty()) return out;
                if ((err=="novision" || err=="toolarge") && !stripped && has_image(ctx)){
                    cerr<<"dropping image parts and retrying\n";
                    if (err=="novision") g_vision_denied=true;
                    ctx=strip_images(ctx);
                    stripped=true;
                    out=attempt(model, ctx, err);
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
                        // nothing left to drop: the request itself is the floor
                        cerr<<"cannot shrink context any further ("<<after<<" bytes)\n";
                        return "";
                    }
                    if (shrink>16){
                        cerr<<"context still too large after "<<shrink<<" compactions\n";
                        return "";
                    }
                    t--;  // shrinking is not a retry: do not consume the budget
                    continue;
                }
                if (err=="fatal"){
                    cerr<<"generate failed: permanent provider error, not retrying\n";
                    return "";
                }
            }
            cerr<<"generate failed after "<<MAX_TRIES<<" attempts\n";
            return "";
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
static const vector<string> vision_models={"deepseek-v4.1-flash-1017","glm-5.3","glm-5.3-flash","gemma-4-26b","qwen-3.8-27b","obi2"};
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
    static const vector<string> vision=vision_models;
    if (g_vision_denied) return text;
    bool vic=false;
    for (auto& v:vision) if (model==v){ vic=true; break; }
    json out;
    if (!vic) return text;
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
            start++;
            if (start<messages.size()){
                string r2=messages[start].value("role","");
                string c2;
                if (messages[start].contains("content") && messages[start]["content"].is_string())
                    c2=messages[start]["content"].get<string>();
                if (r2=="user" && c2=="<continue>") start++;
            }
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
    size_t start=first;
    auto size_at=[&](size_t s){
        size_t n=0;
        for (size_t i=0;i<first;i++) n+=messages[i].dump().size();
        for (size_t i=s;i<messages.size();i++) n+=messages[i].dump().size();
        return n;
    };
    while (start<messages.size() && size_at(start)>max_bytes){
        // drop one whole unit from the oldest end: a tool exchange is
        // [assistant?] + [tool results] + <continue>, never split.
        string role=messages[start].value("role","");
        string content;
        if (messages[start].contains("content") && messages[start]["content"].is_string())
            content=messages[start]["content"].get<string>();
        start++;
        if (role=="user" && content.rfind("[tool results]",0)==0 && start<messages.size()){
            string r2=messages[start].value("role","");
            string c2;
            if (messages[start].contains("content") && messages[start]["content"].is_string())
                c2=messages[start]["content"].get<string>();
            if (r2=="user" && c2=="<continue>") start++;
        }
    }
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
        void save_ctx(){
            ctx=trim_context_bytes(ctx, CTX_MAX_BYTES);
            if (store) store->save(ctx, model, chat.provider);
        }
        // trim in memory right before the request is sent: the file is only
        // written on save, so an in-memory ctx could grow unchecked between
        // turns and blow the provider's request size.
        void compact(){ ctx=trim_context_bytes(ctx, CTX_MAX_BYTES); }
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

        // processes one assistant reply: prints leading text, runs any tool
        // blocks, then continues the conversation until a non-tool reply.
        string f(const string& msg){
            ctx.push_back({{"role", "user"}, {"content", msg}});
            save_ctx();
            for (int round=0; round<max_tool_rounds; round++){
                compact();
                // a screenshot makes the payload multimodal. if the active model
                // cannot take images, borrow a vision model for this request and
                // switch straight back: the transcript keeps our identity/model.
                string sendmodel=model;
                if (has_image(ctx) && !model_is_vision(model) && !g_vision_denied){
                    sendmodel="deepseek-v4.1-flash-1017";
                    cerr<<"image in context; using "<<sendmodel<<" for this request\n";
                }
                string rsp=chat.generate(sendmodel, ctx);
                ctx.push_back({{"role", "assistant"}, {"content", rsp}});
                save_ctx();

                size_t headerpos=rsp.find(toolh);
                if (headerpos==string::npos){
                    return rsp;
                }
                // print any prose before the first tool header (flushed so
                // a pty parent sees progress mid-turn)
                cout<<rsp.substr(0, headerpos)<<flush;

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
                        toolres+=dispatch_tool(tool);
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
                ctx.push_back({{"role", "user"}, {"content", attach_images(toolres, model)}});
                ctx.push_back({{"role", "user"}, {"content", toolcontinue}});
                save_ctx();
            }
            cerr<<"reached max tool rounds ("<<max_tool_rounds<<")\n";
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
        cout<<"→ ";
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
            if (cmd.size()>1){ a.model=cmd[1]; a.refresh_sysprompt(); cout<<"model set to "<<a.model<<'\n'; }
            else cout<<"usage: /model [model]\n";
        } else if (cmd[0]=="/provider"){
            if (cmd.size()>1){ chat.set_provider(cmd[1]); a.refresh_sysprompt(); cout<<"provider set to "<<chat.provider<<'\n'; }
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
    if (cfg.chat_key.empty() || cfg.web_key.empty()){
        cerr<<"error: CLOVER_CHAT_KEY / CLOVER_WEB_KEY not set (source ~/.clover.env or export them)"<<'\n';
        return 1;
    }
    chat_backend chat(cfg);
    web_backend web(cfg);
    context_store store(cfg.ctxfile);
    agent clover(cfg.model, system_prompt, "☘ clover", chat, web);
    if (!cfg.purpose.empty()) clover.name="subagent";
    clover.tools.web_search=[&web](const string& q){ return web.search(q); };
    clover.store=&store;
    clover.load_from_store();
    // ensure the file exists even before the first message
    store.save(clover.ctx, clover.model, chat.provider);

    if (test) return run_test(clover, chat);
    return run_convo(clover, chat);
}
