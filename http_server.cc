#include "sercher.hpp"
#include "cpp-httplib/httplib.h"

cppjieba::Jieba NsUtil::jiebautil::jieba(
    "cppjieba/dict/jieba.dict.utf8",
    "cppjieba/dict/hmm_model.utf8",
    "cppjieba/dict/user.dict.utf8",
    "cppjieba/dict/idf.utf8",
    "cppjieba/dict/stop_words.utf8");

const std::string root_web_root = "web";
const std::string input = "data/pow_html/output.txt";
int main()
{
    httplib::Server svr;
    svr.set_base_dir(root_web_root);
    ns_search::Searcher searcher;
    searcher.InitSearcher(input);

    svr.Get("/s", [&searcher](const httplib::Request &req, httplib::Response &rsp)
            {   if(req.has_param("word")){
                rsp.status = 400;
                rsp.set_content("missing word parameter", "text/plain");
                return;
                }
                std::string word = req.get_param_value("word");
                std::string json_string;
                searcher.Search(word,&json_string);
                rsp.set_content(json_string,"application/json; charset=utf-8");
             });
    svr.listen("0.0.0.0", 8081);
    return 0;
}
