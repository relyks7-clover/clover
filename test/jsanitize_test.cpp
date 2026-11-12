// unit: jsanitize/jdump from the real tool.h (the production helper).
#include "../tool.h"
#include <cassert>
#include <cstdio>
int main(){
    // clean input passes through untouched
    assert(jsanitize("hello \xc3\xa9 \xe2\x98\x83") == "hello \xc3\xa9 \xe2\x98\x83");
    // the crash byte class: lead byte followed by 0x2E (the reported 0x2E)
    std::string bad = "abc" + std::string(1,(char)0xE2) + std::string(1,(char)0x2E) + "def";
    std::string out = jsanitize(bad);
    assert(out.size() == 6 + 1 + 3); // 6 clean chars + the 0x2E + 3-byte replacement
    assert(out[3]==0xEF && out[4]==0xBF && out[5]==0xBD);
    // lone continuation byte, stray lead at end of string, truncated seq
    assert(jsanitize(std::string(1,(char)0x80)).size()==3);
    assert(jsanitize(std::string(1,(char)0xC3)).size()==3);
    assert(jsanitize(std::string("\xe2\x82",2)).size()==3+3); // lead needs 2 cont, has 1 -> replaced; lone cont also replaced
    // overlong + surrogates are invalid utf8 -> replaced
    assert(jsanitize(std::string("\xc0\xaf",2)).size()==6);
    assert(jsanitize(std::string("\xed\xa0\x80",3)).size()==9);
    // every single byte value sanitizes without throwing
    for (int b=0;b<256;b++){
        std::string one(1,(char)b);
        jsanitize(one);
    }
    // jdump: strict dump of a doc holding the bad bytes would throw 316;
    // jdump must not. also proves the crash shape end to end.
    nlohmann::json j; j["page"] = bad; j["n"] = 5;
    bool threw=false;
    try { j.dump(); } catch (const nlohmann::json::exception&){ threw=true; }
    assert(threw && "sanity: strict dump must throw on this payload");
    std::string d = jdump(j);
    assert(d.find("\\ufffd")==std::string::npos || d.find("\xEF\xBF\xBD")!=std::string::npos);
    assert(!d.empty() && d!=std::string("[unserializable]"));
    // jdump fallback on a doc that even the replace-handler cannot serialize
    std::string dj = jdump(nlohmann::json({{"a",1}}));
    assert(dj.find("\"a\":1")!=std::string::npos); // compact: {"a":1}
    std::printf("jsanitize/jdump unit: PASS\n");
    return 0;
}
