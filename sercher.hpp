#pragma once
#include <iostream>
#include <string>
#include "index.hpp"
#include "Ns_Util.hpp"
#include <jsoncpp/json/json.h>
#include <algorithm>
#include <cmath>
#include <sstream>

namespace ns_search
{
    struct IntervedElemPrint
    {
        uint16_t doc_id = 0;
        double score = 0.0;
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

            const double k1 = 1.2;
            const double b = 0.75;
            const double TITLE_BOOST = 3.0;
            const double N = static_cast<double>(_index->GetDocCount());
            const double avgdl = _index->GetAvgDocLen();

            std::unordered_map<uint16_t, IntervedElemPrint> token_map;
            for (const auto &word : words)
            {
                std::string lower_word = word;
                boost::to_lower(lower_word);
                ns_index::index::IndexInfolist *inverted_list = _index->GetInvertedList(lower_word);
                if (inverted_list == nullptr || inverted_list->empty())
                {
                    continue;
                }

                const double df = static_cast<double>(inverted_list->size());
                const double idf = std::log(1.0 + (N - df + 0.5) / (df + 0.5));

                for (const auto &elem : *inverted_list)
                {
                    ns_index::DocInfo *doc_info = _index->GetForwardIndex(elem.doc_id);
                    if (doc_info == nullptr)
                    {
                        continue;
                    }

                    const double tf = static_cast<double>(elem.tf);
                    const double dl = static_cast<double>(doc_info->doc_len);
                    const double norm = (avgdl > 0.0) ? (dl / avgdl) : 1.0;
                    const double denom = tf + k1 * (1.0 - b + b * norm);
                    const double body = (denom > 0.0)
                                            ? idf * (tf * (k1 + 1.0)) / denom
                                            : 0.0;
                    const double title_bonus = (elem.title_tf > 0) ? idf * TITLE_BOOST : 0.0;

                    auto &item = token_map[elem.doc_id];
                    item.doc_id = elem.doc_id;
                    item.score += body + title_bonus;
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
                          if (a.score != b.score)
                          {
                              return a.score > b.score;
                          }
                          return a.doc_id < b.doc_id;
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

                Json::Value element;
                element["title"] = doc_info->title;
                element["content"] = getdesc(doc_info->content, item.words);
                element["url"] = doc_info->url;
                root.append(element);
            }

            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            *jison_string = Json::writeString(writer, root);
        }

        std::string getdesc(const std::string &html_content, const std::string &words)
        {
            std::istringstream word_stream(words);
            std::string word;
            size_t first_match_pos = std::string::npos;
            while (word_stream >> word)
            {
                auto it = std::search(html_content.begin(), html_content.end(),
                                      word.begin(), word.end(),
                                      [](unsigned char x, unsigned char y) {
                                          return std::tolower(x) == std::tolower(y);
                                      });
                if (it != html_content.end())
                {
                    const size_t match_pos = std::distance(html_content.begin(), it);
                    first_match_pos = std::min(first_match_pos, match_pos);
                }
            }
            if (first_match_pos == std::string::npos)
            {
                return "";
            }

            const size_t start = (first_match_pos > 50) ? first_match_pos - 50 : 0;
            const size_t end = std::min(html_content.size(), first_match_pos + 100);
            return html_content.substr(start, end - start);
        }
    };
};