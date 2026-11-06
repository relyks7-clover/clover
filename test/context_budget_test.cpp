#define main clover_main
#include "../main.cpp"
#undef main
#include <cassert>
int main(){
    config cfg; cfg.provider="logfare.ai";
    chat_backend chat(cfg); web_backend web(cfg);
    agent a("qwen-3.8-27b","system fixture","fixture",chat,web);
    const size_t budget=context_budget_bytes(a.model,chat.provider);
    assert(budget==384000);
    assert(context_budget_bytes("zarnkina-gpt-6.1-sol","scandata-relay.zarnkina.workers.dev")==768000);
    json fixture=a.ctx;
    for(int i=0;i<80;i++) fixture.push_back({{"role",i%2?"assistant":"user"},{"content",std::to_string(i)+std::string(6000,'x')}});
    fixture.push_back({{"role","user"},{"content","latest request"}});
    assert(fixture.size()<300 && fixture.dump().size()>budget);
    a.ctx=fixture; a.compact();
    assert(a.ctx.dump().size()<=budget);
    assert(a.ctx.front()==fixture.front() && a.ctx.back()==fixture.back());
    assert(a.ctx.size()<fixture.size());
    char path[]="/tmp/clover-budget-XXXXXX";
    int fd=mkstemp(path); assert(fd>=0); close(fd);
    context_store store(path); a.store=&store; a.ctx=fixture; a.save_ctx();
    auto loaded=store.load(); assert(loaded.has_value());
    assert(loaded->dump().size()<=budget);
    assert(loaded->front()==fixture.front() && loaded->back()==fixture.back());
    unlink(path);
    std::cout<<"production compact/save byte-budget regression passed (<300 messages)\n";
}
