#pragma once
#include <string>
#include <vector>
#include <iostream>
#include <unordered_map>
#include <fstream>
#include "Ns_Util.hpp"
#include <mutex>


namespace ns_index
{
    struct DocInfo
    {
        std::string title;
        std::string content;
        std::string url;
        uint16_t doc_id;
    };
    struct IntervedElem
    {
        uint16_t doc_id;
        std::string word;
        int weight;
    };

    class index
    {
        public:
        typedef std::vector<IntervedElem> IndexInfolist;
        typedef std::unordered_map<std::string, IndexInfolist> IndexMap;

    private:
        std::vector<DocInfo> forward_index;
        IndexMap inverted_index;
private:
        index() {}
        index(const index &) = delete;
        index &operator=(const index &) = delete;

    public:
        ~index() {}
        static index *get_instance()
        {
            static index *instance;
            if(instance == nullptr)
            {
                static std::mutex mtx;
                mtx.lock();
                if (instance == nullptr)
                {
                    instance = new index();
                }
                mtx.unlock();
            }
            return instance;
        }
        DocInfo *GetForwwardIndex(const uint16_t doc_id)
        {
            if (doc_id >= forward_index.size())
            {
                std::cerr << doc_id << "doc_id is out of range" << std::endl;
                return nullptr;
            }
            return &forward_index[doc_id];
        }
        IndexInfolist *GetInvertedList (const std::string &word)
        {
            auto it = inverted_index.find(word);
            if (it == inverted_index.end())
            {
                std::cerr << word << " is not found in inverted_index" << std::endl;
                return nullptr;
            }
            return &(it->second);
        }
        bool buildIndex(const std::string &input)
        {
            std::ifstream in(input, std::ios::in | std::ios::binary);
            if (!in.is_open())
            {
                std::cerr << "open " << input << " failed" << std::endl;
                return false;
            }
            std::string line;
            while (std::getline(in, line))
            {
                DocInfo *doc_info = BuildForwardIndex(line);
                if (doc_info == nullptr)
{
    continue;
}
                BuildInvertedIndex(doc_info);
            }
            return true;
        }

    private:
        DocInfo *BuildForwardIndex(const std::string &line)
        {
            std::vector<std::string> resulct;
            std::string sep = "\3";
            NsUtil::stringUtil::split(line, &resulct, sep);
            if (resulct.size() != 3)
            {
                std::cerr << "split failed, resulct.size()=" << resulct.size() << std::endl;
                return nullptr;
            }
            DocInfo doc_info;
            doc_info.title = resulct[0];
            doc_info.content = resulct[1];
            doc_info.url = resulct[2];
            doc_info.doc_id = forward_index.size();
            forward_index.push_back(doc_info);
            return &forward_index.back();
        }
        bool BuildInvertedIndex(DocInfo *doc_info)
        {
            struct word_cnt
            {
                int title_cnt;
                int content_cnt;
            };
            std::unordered_map<std::string, word_cnt> word_map;
            std::vector<std::string> title_words;
            NsUtil::jiebautil::cutstring(doc_info->title, &title_words);
            for (auto &word : title_words)
            {
                boost::to_lower(word);
                word_map[word].title_cnt++;
            }
            std::vector<std::string> content_words;
            NsUtil::jiebautil::cutstring(doc_info->content, &content_words);
            for (auto &word : content_words)
            {
                boost::to_lower(word);
                word_map[word].content_cnt++;   
            }
#define X 10
#define Y 1
            for(const auto &word_pair : word_map)
            {
                IntervedElem elem;
                elem.doc_id = doc_info->doc_id;
                elem.word = word_pair.first;
                elem.weight = word_pair.second.title_cnt * X + word_pair.second.content_cnt * Y;
                IndexInfolist &indexinfolist= inverted_index[word_pair.first];
                indexinfolist.push_back(elem);
            }
            return true;
        }
    };

};
