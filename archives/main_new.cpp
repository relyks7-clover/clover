#include <bits/stdc++.h>
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "json.hpp"
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
    string chat_key=getenv("CLOVER_CHAT_KEY")?getenv("CLOVER_CHAT_KEY"):"";;
    string web_key=getenv("CLOVER_WEB_KEY")?getenv("CLOVER_WEB_KEY"):"";;
};

// lazily (re)builds ssl clients when provider/key changes, since SSLClient is
// final and non-movable, so we cannot just reassign it.
class chat_backend{
    public:
        string provider;
        string key;
        unique_ptr<httplib::SSLClient> cli;
        chat_backend(const config& c):provider(c.provider), key(c.chat_key){ build(); }
        void build(){
            cli=make_unique<httplib::SSLClient>(provider);
            cli->set_keep_alive(false);
            cli->set_connection_timeout(0,3000000);
            cli->set_default_headers({{"Authorization", "Bearer "+key}});
        }
        void set_provider(const string& p){
            if (p==provider) return;
            provider=p;
            build();
        }
        string generate(const string& model, const json& context){
            json m; m["model"]=model;
            m["stream"]=false;
            m["messages"]=context;
            auto res=cli->Post("/v1/chat/completions", m.dump(), "application/json");
            if (res && res->status==200){
                try{
                    auto resp_json=json::parse(res->body);
                    if (!resp_json.contains("choices") || resp_json["choices"].empty())
                        { cerr<<"unexpected response shape\n"; return ""; }
                    return resp_json["choices"][0]["message"]["content"].get<string>();
                } catch (const json::parse_error& e){
                    cerr<<"response json parse error:\n"<<e.what()<<'\n';
                } catch (const json::type_error& e){
                    cerr<<"response json type error:\n"<<e.what()<<'\n';
                }
            } else if (res){
                cerr<<"http error: status "<<res->status<<'\n';
                cerr<<"body:\n"<<res->body<<'\n';
            } else {
                cerr<<"http error: failed to connect\n";
            }
            return "";
        }
};

class web_backend{
    public:
        string provider;
        string key;
        unique_ptr<httplib::SSLClient> cli;
        web_backend(const config& c):provider(c.web_provider), key(c.web_key){ build(); }
        void build(){
            cli=make_unique<httplib::SSLClient>(provider);
            cli->set_keep_alive(false);
            cli->set_connection_timeout(0,3000000);
            cli->set_default_headers({{"Authorization", "Bearer "+key}});
        }
        string search(const string& query){
            json m;
            m["query"]=query; m["count"]=10;
            m["contents"]={{"text",true}};
            m["freshness"]="noLimit";
            cerr<<"web search: "<<query<<'\n';
            auto res=cli->Post("/v1/web-search", m.dump(), "application/json");
            if (res && res->status==200){
                try{ return json::parse(res->body).dump(); }
                catch (const json::parse_error& e){ cerr<<"web search json parse error:\n"<<e.what()<<'\n'; }
            } else if (res){
                cerr<<"web search http error: status "<<res->status<<'\n';
                cerr<<"body:\n"<<res->body<<'\n';
            } else {
                cerr<<"web search http error: failed to connect\n";
            }
            return "";
        }
};

string get_time(){
    auto t=chrono::system_clock::to_time_t(chrono::system_clock::now());
    struct tm tmv;
    gmtime_r(&t,&tmv);
    char buf[64];
    strftime(buf,sizeof(buf),"%a %b %e %H:%M:%S %Y\n",&tmv);
    return string("current time: ")+buf;
}

string run_command(const string& rcommand){
    cerr<<"ran command: "<<rcommand<<'\n';
    string command="'";
    for (char c:rcommand){
        if (c=='\'') command+="'\\''";
        else command+=c;
    }
    command+="'";
    string full_command="bash -lc "+command+" 2>&1";
    FILE* pipe=popen(full_command.c_str(), "r");
    if (!pipe){ cerr<<"popen failed\n"; return "popen failed"; }
    string output;
    for (int c; (c=fgetc(pipe))!=EOF;) output.push_back(static_cast<char>(c));
    int status=pclose(pipe);
    int excode=-1;
    if (WIFEXITED(status)) excode=WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) excode=128+WTERMSIG(status);
    return "exit code: "+to_string(excode)+"\nresult: "+move(output)+'\n';
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

class agent{
    public:
        string model;
        string name;
        json ctx;
        chat_backend& chat;
        web_backend& web;
        int max_tool_rounds=32;
        agent(const string& modelname, const string& sysprompt, const string& agentname,
              chat_backend& ch, web_backend& wb)
            :model(modelname), name(agentname), chat(ch), web(wb){
            ctx.push_back({{"role", "system"}, {"content", sysprompt}});
        }
        void clear_context(){ while (ctx.size()>1) ctx.erase(ctx.size()-1); }

        string dispatch_tool(const json& tool){
            string result;
            try{
                string tname=tool.value("tool", string(""));
                cerr<<"[tool: "<<tname<<"]\n";
                if (tname=="get_time"){
                    result=get_time();
                } else if (tname=="web_search"){
                    if (!tool.contains("query")) result="error: missing 'query' argument\n";
                    else result=web.search(tool["query"].get<string>());
                } else if (tname=="run_command"){
                    if (!tool.contains("command")) result="error: missing 'command' argument\n";
                    else result=run_command(tool["command"].get<string>());
                } else {
                    result="unknown tool: "+tname+"\n";
                }
            } catch (const exception& e){
                result=string("tool dispatch error: ")+e.what()+"\n";
            }
            return result;
        }

        // processes one assistant reply: prints leading text, runs any tool
        // blocks, then continues the conversation until a non-tool reply.
        string f(const string& msg){
            ctx.push_back({{"role", "user"}, {"content", msg}});
            for (int round=0; round<max_tool_rounds; round++){
                string rsp=chat.generate(model, ctx);
                ctx.push_back({{"role", "assistant"}, {"content", rsp}});

                size_t headerpos=rsp.find(toolh);
                if (headerpos==string::npos){
                    return rsp;
                }
                // print any prose before the first tool header
                cout<<rsp.substr(0, headerpos);

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
                        cursor=after;
                        continue;
                    }
                    json parsed;
                    try{
                        parsed=json::parse(*arr);
                    } catch (const json::parse_error& e){
                        cerr<<"tool json parse error:\n"<<e.what()<<'\n';
                        cursor=arr_end;
                        continue;
                    }
                    if (!parsed.is_array()){
                        cerr<<"tool call is not a json array\n";
                        cursor=arr_end;
                        continue;
                    }
                    for (auto& tool: parsed){
                        if (!tool.is_object()){ cerr<<"tool entry is not an object\n"; continue; }
                        toolres+="tool result, id: "+to_string(tool.value("id", -1))+'\n';
                        toolres+="tool name: "+tool.value("tool", string(""))+'\n';
                        toolres+=dispatch_tool(tool);
                        any=true;
                    }
                    cursor=arr_end;
                }
                if (!any){
                    // had a header but no runnable tool; return what we have
                    return rsp.substr(headerpos);
                }
                ctx.push_back({{"role", "user"}, {"content", toolres}});
                ctx.push_back({{"role", "user"}, {"content", toolcontinue}});
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
            if (cmd.size()>1){ a.model=cmd[1]; cout<<"model set to "<<a.model<<'\n'; }
            else cout<<"usage: /model [model]\n";
        } else if (cmd[0]=="/provider"){
            if (cmd.size()>1){ chat.set_provider(cmd[1]); cout<<"provider set to "<<chat.provider<<'\n'; }
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
    for (int i=1;i<argc;i++){
        string a=argv[i];
        if (a=="-t"||a=="--test") test=true;
        else if (a=="-h"||a=="--help"){
            cout<<"usage: "<<argv[0]<<" [-t|--test]\n";
            cout<<"  -t, --test   read all stdin, run one turn, print result, exit\n";
            return 0;
        } else { cerr<<"unknown argument: "<<a<<'\n'; return 2; }
    }
    // test mode is explicit only (-t/--test); a bare pipe still drives the
    // interactive loop so slash-commands stay scriptable.

    config cfg;
    ifstream sysfile(cfg.promptfile);
    if (!sysfile.is_open()){ cerr<<"could not open sysprompt file "<<cfg.promptfile<<'\n'; return 1; }
    stringstream buf; buf<<sysfile.rdbuf();
    string system_prompt=buf.str();

    chat_backend chat(cfg);
    web_backend web(cfg);
    agent clover(cfg.model, system_prompt, "☘ clover", chat, web);

    if (test) return run_test(clover, chat);
    return run_convo(clover, chat);
}
