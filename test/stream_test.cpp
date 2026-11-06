#define CLOVER_PLAIN_HTTP
#define main clover_main
#include "../main.cpp"
#undef main
#include <cassert>
int main(){
    sse_decoder d;
    std::string wire="data: {\"choices\":[{\"delta\":{\"content\":\"hello 🌱\"},\"finish_reason\":null}]}\r\n\r\ndata: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
    for(char c:wire) d.feed(&c,1);
    assert(d.content=="hello 🌱" && d.done && d.finish=="stop" && d.error.empty());
    httplib::Server server;
    server.Post("/v1/chat/completions",[](const httplib::Request& req, httplib::Response& res){
        assert(json::parse(req.body)["stream"]==true);
        res.set_chunked_content_provider("text/event-stream",[](size_t offset,httplib::DataSink& sink){
            std::string text=offset==0?"data: {\"choices\":[{\"delta\":{\"content\":\"early\"},\"finish_reason\":null}]}\n\n":"data: {\"choices\":[{\"delta\":{\"content\":\" late\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
            if(offset) std::this_thread::sleep_for(std::chrono::milliseconds(800));
            sink.write(text.data(),text.size()); if(offset) sink.done(); return true;
        });
    });
    int port=server.bind_to_any_port("127.0.0.1"); assert(port>0);
    std::thread worker([&]{server.listen_after_bind();});
    config cfg; cfg.provider="127.0.0.1:"+std::to_string(port); chat_backend backend(cfg);
    auto start=std::chrono::steady_clock::now(); double first=99; std::string streamed,err;
    auto out=backend.attempt("fixture",json::array({{{"role","user"},{"content","test"}}}),err,[&](const std::string& s){if(streamed.empty())first=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();streamed+=s;});
    server.stop();worker.join();
    assert(err.empty() && out=="early late" && streamed==out && first<0.7);
    std::cout<<"production backend streaming passed; first chunk "<<first<<"s before delayed completion\n";
}
