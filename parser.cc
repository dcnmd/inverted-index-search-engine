#include <iostream>
#include <string>
#include <vector>
#include <boost/filesystem.hpp>
#include "Ns_Util.hpp"

const std::string scr_path = "./data/input/";
const std::string out_path = "./data/pow_html/output.txt";

struct DocInfo_t
{
    std::string title;
    std::string content;
    std::string url;
};

// 枚举指定目录下的所有文件，将文件路径写入 output
// path   : 待遍历的目录路径
// output : 输出参数，保存遍历到的文件路径列表
bool Enumfile(const std::string &path, std::vector<std::string> *output)
{
    namespace fs = boost::filesystem;
    fs::path row_path(path);
    if (!fs::exists(row_path))
    {
        std::cerr << "Path does not exist: " << path << std::endl;
        return false;
    }
    fs::recursive_directory_iterator end_iter;
    for (fs::recursive_directory_iterator iter(row_path); iter != end_iter; ++iter)
    {
        if (!(fs::is_regular_file(*iter)))
        {
            continue;
        }
        if (iter->path().extension() != ".html")
        {
            continue;
        }
        output->push_back(iter->path().string());
        // std::cout << "Found HTML file: " << iter->path().string() << std::endl;
    }
    return true;
}
// 标题
bool Parsetitle(const std::string &content, std::string *title)
{
    // 解析 HTML 内容，提取标题
    // 这里可以使用正则表达式或 HTML 解析库来实现
    // 示例代码仅为占位符
    size_t start = content.find("<title>");
    if (start == std::string::npos)
        return false;
    start += std::string("<title>").size();
    size_t end = content.find("</title>", start);
    if (end == std::string::npos)
        return false;
    *title = content.substr(start, end - start);
    return true;
}
// 正文
bool Parsecontent(const std::string &content, std::string *body)
{
    enum Status
    {
        LABEL,
        CONTENT
    };
    enum Status s = LABEL;
    for (char c : content)
    {
        switch (s)
        {
        case LABEL:
            if (c == '>')
            {
                s = CONTENT;
            }
            break;
        case CONTENT:
            if (c == '<')
            {
                s = LABEL;
            }
            else
            {
                if (c == '\n')
                {
                    c = ' ';
                }
                *body += c;
            }
            break;
        }
    }
    return true;
}

bool Parseurl(const std::string file, std::string *url)
{
    if (url == nullptr)
        return false;
    std::string url_head = "https://www.boost.org/doc/libs/1_74_0/doc/html/";
    std::string url_tail = file.substr(scr_path.size());
    if (!url_tail.empty() && url_tail[0] == '/')
    {
        url_tail.erase(0, 1);
    }
    *url = url_head + url_tail;
    return true;
}
// bool Parseurl(const std::string &file, std::string *url)
// {
//     if (url == nullptr)
//         return false;

//     namespace fs = boost::filesystem;
//     const fs::path relative = fs::path(file).lexically_relative(fs::path(scr_path));

//     if (relative.empty() || relative.is_absolute() ||
//         *relative.begin() == fs::path(".."))
//     {
//         return false;
//     }

//     const std::string url_head = "https://www.boost.org/doc/libs/1_74_0/doc/html/";
//     *url = url_head + relative.generic_string();
//     return true;
// }

static void show_lines(const std::vector<DocInfo_t> &lines)
{
    for (const auto &doc : lines)
    {
        std::cout << "Title: " << doc.title;
        std::cout << "Content: " << doc.content;
        std::cout << "URL: " << doc.url;
        std::cout << "------------------------" << std::endl;
    }
}

// 逐个解析输入文件，提取标题、正文与 URL，结果写入 lines
// input : 待解析的文件路径列表
// lines : 输出参数，保存解析出的文档信息
bool ParseFile(const std::vector<std::string> &input, std::vector<DocInfo_t> *lines)
{
    for (auto &file_path : input)
    {
        std::string result;
        if (!(NsUtil::FileUtil::ReadFiles(file_path, &result)))
        {
            return false;
        }
        DocInfo_t doc_info;
        if (!Parsetitle(result, &doc_info.title))
        {
            continue;
        }
        if (!Parsecontent(result, &doc_info.content))
        {
            continue;
        }
        if (!Parseurl(file_path, &doc_info.url))
        {
            continue;
        }
        lines->push_back(doc_info);
        // show_lines(*lines);
    }
    return true;
};

// 将解析结果写入输出文件
// lines  : 待保存的文档信息
// output : 输出文件路径

bool SaveFile(const std::vector<DocInfo_t> &lines, const std::string &output)
{
    std::ofstream out(output, std::ios::out | std::ios::binary);
    if (!out.is_open())
    {
        std::cerr << "Failed to open output file: " << output << std::endl;
        return false;
    }
#define SEPARATOR "\3"
    for (const auto &doc : lines)
    {
        std::string line;
        line = doc.title;
        line += SEPARATOR;
        line += doc.content;
        line += SEPARATOR;
        line += doc.url;
        out.write(line.c_str(), line.size());
        out.put('\n');
    }
    out.close();
    return true;
}

// 主流程：枚举文件 -> 解析文件 -> 保存结果
int main()
{
    std::vector<std::string> file_list;
    if (!Enumfile(scr_path, &file_list))
    {
        return 1;
    }
    std::vector<DocInfo_t> result;
    if (!ParseFile(file_list, &result))
    {
        return 2;
    }
    std::string output;
    if (!SaveFile(result, out_path))
    {
        return 3;
    }

    return 0;
}