#pragma once
#include <iostream>
#include <string>
#include "index.hpp"
#include "Ns_Util.hpp"
#include <jsoncpp/json/json.h>
#include <algorithm>

namespace ns_search
{
    
class Searcher {
    private:
    ns_index::index* _index;
public:
    Searcher(){}
    ~Searcher(){}
    public:
    void InitSearcher(const std::string &input)
    {
        _index = ns_index::index::get_instance();
        _index->buildIndex(input);
    }
    void Search(const std::string &query, std::string *jison_string)
    {
        std::vector<std::string> words;
        NsUtil::jiebautil::cutstring(query, &words);
        ns_index::index::IndexInfolist inverted_list_all;
        for (auto &word : words)
        {
            boost::to_lower(word);
            ns_index::index::IndexInfolist *inverted_list = _index->GetInvertedList(word);
            if (inverted_list == nullptr)
            {
                std::cerr << "word: " << word << " not found in inverted index" << std::endl;
                continue;
            }
            inverted_list_all.insert(
        inverted_list_all.end(),
        inverted_list->begin(),
        inverted_list->end());
            std::sort(inverted_list_all.begin(), inverted_list_all.end(), [](const ns_index::IntervedElem &a, const ns_index::IntervedElem &b) {
                return a.weight > b.weight;
            });
        }
        Json::Value root;
        for (auto &item : inverted_list_all)
        {
            ns_index::DocInfo *doc_info = _index->GetForwwardIndex(item.doc_id);
            if (doc_info == nullptr)
            {
                std::cerr << "doc_id: " << item.doc_id << " not found in forward index" << std::endl;
                continue;
            }
            Json::Value element;
            element["title"] = doc_info->title;
            element["content"] = getdesc(doc_info->content,item.word);
            element["url"] = doc_info->url;
            root.append(element);
        }
        Json::StyledWriter writer;
        *jison_string = writer.write(root);
    }
    std::string getdesc(const std::string &html_content, const std::string &word)
    {
        int prev_pos = 50;
        int next_pos = 100;
        auto it = std::search(html_content.begin(), 
        html_content.end(), word.begin(), word.end(),
        [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) ==
           std::tolower(static_cast<unsigned char>(y)); });
        if (it == html_content.end())
        {
            return "";
        }
        int word_pos=std::distance(html_content.begin(), it);

        int start=0;
        int end=html_content.size();
        if(word_pos-prev_pos>start) start=word_pos-prev_pos;
        if(word_pos+next_pos<end) end=word_pos+next_pos;

        return html_content.substr(start, end - start);
    }
};
};