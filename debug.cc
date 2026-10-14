
#include "sercher.hpp"

cppjieba::Jieba NsUtil::jiebautil::jieba(
    "cppjieba/dict/jieba.dict.utf8",
    "cppjieba/dict/hmm_model.utf8",
    "cppjieba/dict/user.dict.utf8",
    "cppjieba/dict/idf.utf8",
    "cppjieba/dict/stop_words.utf8");

static std::string input = "data/pow_html/output.txt";

int main()
{
    ns_search::Searcher* searcher = new ns_search::Searcher();
    searcher->InitSearcher(input);
    std::string input_query;
    std::string jison_string;
    while(true)
    {
        std::cout << "Please enter your search query (or type 'exit' to quit): ";
        std::getline(std::cin, input_query);
        if (input_query == "exit")
        {
            break;
        }
        searcher->Search(input_query, &jison_string);
        std::cout << "Search results in JSON format:\n" << jison_string << std::endl;
    }
    return 0;
}
