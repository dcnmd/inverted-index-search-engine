#pragma once
#include <string>
#include <vector>
#include <boost/filesystem.hpp>
#include <iostream>
#include <boost/algorithm/string.hpp>
#include "cppjieba/Jieba.hpp"




    
namespace NsUtil{
    class FileUtil {
    public:
        static bool ReadFiles(const std::string& path, std::string* out)
        {
        //std::ifstream& GetFileStream(const std::string& path);
        std::ifstream in(path, std::ios::in);
        if (!in.is_open()) {
                std::cerr << "Failed to open file: " << path << std::endl;
                return false;
            }
            std::string line;
            while (std::getline(in, line)) {
                *out += line + "\n";
            }

        in.close();
        return true;
        }

    };
    class stringUtil {
    public:
        static void split(const std::string& resget, 
            std::vector<std::string> *out, const std::string sep)
        {
            // size_t start = 0;
            // size_t pos = resget.find(sep, start);
            // while (pos != std::string::npos)
            // {
            //     std::string temp=resget.substr(start,pos - start);
            //     out->push_back(temp);
            //     start = pos+sep.size();
            //     pos=resget.find(sep, start);
            // }
            // out->push_back(resget.substr(start));
            boost::split(*out, resget, boost::is_any_of(sep), boost::token_compress_on);
        }
    };
    class jiebautil {   
    private:
        static cppjieba::Jieba jieba;
    public: 
        static void cutstring(const std::string& scr, 
            std::vector<std::string> *out)
        {
            jieba.CutForSearch(scr, *out);
        }

    };




};