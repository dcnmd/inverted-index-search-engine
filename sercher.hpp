#pragma once
#include <iostream>
#include <string>
#include "index.hpp"
#include "Ns_Util.hpp"
#include <jsoncpp/json/json.h>
#include <algorithm>

namespace ns_search
{
    struct IntervedElemPrint
    {
        uint16_t doc_id = 0;
        int weight = 0;
        std::string words;
    };

    class Searcher {
    private:
        ns_index::index* _index = nullptr;

    public:
        Searcher() = default;
        ~Searcher() = default;

        void InitSearcher(const std::string &input)
        {
            _index = ns_index::index::get_instance();
            _index->buildIndex(input);
        }

        void Search(const std::string &query, std::string *jison_string)
        {
            std::vector<std::string> words;
            NsUtil::jiebautil::cutstring(query, &words);

            std::unordered_map<uint16_t, IntervedElemPrint> token_map;
            for (const auto &word : words)
            {
                std::string lower_word = word;
                boost::to_lower(lower_word);
                ns_index::index::IndexInfolist *inverted_list = _index->GetInvertedList(lower_word);
                if (inverted_list == nullptr)
                {
                    std::cerr << "word: " << lower_word << " not found in inverted index" << std::endl;
                    continue;
                }

                for (const auto &elem : *inverted_list)
                {
                    auto &item = token_map[elem.doc_id];
                    item.doc_id = elem.doc_id;
                    item.weight += elem.weight;
                    if (!item.words.empty())
                    {
                        item.words += " ";
                    }
                    item.words += elem.word;
                }
            }

            std::vector<IntervedElemPrint> result_items;
            for (const auto &pair : token_map)
            {
                result_items.push_back(pair.second);
            }

            std::sort(result_items.begin(), result_items.end(),
                      [](const IntervedElemPrint &a, const IntervedElemPrint &b) {
                          return a.weight > b.weight;
                      });

            Json::Value root(Json::arrayValue);
            for (auto &item : result_items)
            {
                ns_index::DocInfo *doc_info = _index->GetForwardIndex(item.doc_id);
                if (doc_info == nullptr)
                {
                    std::cerr << "doc_id: " << item.doc_id << " not found in forward index" << std::endl;
                    continue;
                }

                std::string target_word = item.words;
                if (!target_word.empty())
                {
                    const auto pos = target_word.find(' ');
                    if (pos != std::string::npos)
                    {
                        target_word = target_word.substr(0, pos);
                    }
                }

                Json::Value element;
                element["title"] = doc_info->title;
                element["content"] = getdesc(doc_info->content, target_word);
                element["url"] = doc_info->url;
                root.append(element);
            }

            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            *jison_string = Json::writeString(writer, root);
        }

        std::string getdesc(const std::string &html_content, const std::string &word)
        {
            if (word.empty())
            {
                return "";
            }

            int prev_pos = 50;
            int next_pos = 100;
            auto it = std::search(html_content.begin(), html_content.end(),
                                  word.begin(), word.end(),
                                  [](unsigned char x, unsigned char y) {
                                      return std::tolower(x) == std::tolower(y);
                                  });
            if (it == html_content.end())
            {
                return "";
            }

            const int word_pos = std::distance(html_content.begin(), it);
            int start = 0;
            int end = static_cast<int>(html_content.size());
            if (word_pos - prev_pos > start)
            {
                start = word_pos - prev_pos;
            }
            if (word_pos + next_pos < end)
            {
                end = word_pos + next_pos;
            }

            return html_content.substr(start, end - start);
        }
    };
};