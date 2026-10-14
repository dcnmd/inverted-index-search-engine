# inverted-index-search-engine

基于 Boost 官方文档语料实现的 C++ 倒排索引搜索引擎。

- **语料**：Boost 1.74 官方文档站，7546 个 HTML 页面
- **解析**：手写状态机剥离 HTML 标签，`boost::filesystem` 还原每篇文档的原始 URL
- **索引**：正排索引（文档元信息）+ 倒排索引（词 → 文档列表 + 权重）
- **分词**：cppjieba，MP + HMM 混合模型
- **检索**：分词 → 查倒排链 → 按权重排序 → 摘取正文摘要 → JSON 返回
- **服务**：cpp-httplib，提供 `/s?word=xxx` 检索接口

## 目录结构

| 文件 | 作用 |
| --- | --- |
| `parser.cc` | 遍历 HTML 语料，剥离标签，按 `\3` 分隔符落盘为 `data/pow_html/output.txt` |
| `index.hpp` | 索引单例。正排 `std::vector<DocInfo>`，倒排 `std::unordered_map<std::string, IndexInfolist>` |
| `sercher.hpp` | 检索逻辑：分词 → 拼倒排链 → 按权重排序 → `getdesc` 摘摘要 → 输出 JSON |
| `Ns_Util.hpp` | 工具：文件读取、字符串切分、cppjieba 封装 |
| `http_server.cc` | HTTP 服务入口，监听 8081 |
| `debug.cc` | 命令行检索调试工具 |
| `web/index.html` | 检索页面 |

## 构建与运行

### 1. 获取代码和子模块

```sh
git clone --recurse-submodules https://github.com/dcnmd/inverted-index-search-engine.git
```

已经 clone 过的话，补拉子模块：

```sh
git submodule update --init --recursive
```

> `cpp-httplib` 和 `cppjieba` 是第三方依赖，用 submodule 固定在验证过的 commit 上，不在本仓库内重复存放。

### 2. 准备 jieba 词典

程序启动时按 `dict/` 这个相对路径加载 5 个词典文件，所以需要先把词典拷到仓库根目录：

```sh
cp -r cppjieba/dict ./dict
```

对应 `http_server.cc` 里的：

```cpp
cppjieba::Jieba jieba("dict/jieba.dict.utf8", "dict/hmm_model.utf8",
                      "dict/user.dict.utf8", "dict/idf.utf8",
                      "dict/stop_words.utf8")
```

### 3. 准备语料

把 Boost 1.74 的 HTML 文档放进 `data/input/`。语料体积较大，不纳入版本控制。

### 4. 编译运行

```sh
mkdir -p data/pow_html
make
./parser        # 解析 data/input/ 下的 HTML，产出 data/pow_html/output.txt
./http_server   # 加载上一步的索引，监听 8081
```

## 依赖

- GCC/G++，支持 C++11
- Boost.Filesystem、Boost.System
- JsonCpp
- cppjieba、cpp-httplib（submodule）
- Linux

Debian / Ubuntu：

```sh
sudo apt install g++ libboost-filesystem-dev libboost-system-dev libjsoncpp-dev
```

CentOS 7：

```sh
sudo yum install gcc-c++ boost-devel jsoncpp-devel
```

## 接口

```sh
curl 'http://localhost:8081/s?word=boost'
```

返回 JSON 数组，每项包含标题、正文摘要和原始 URL：

```json
[
  {
    "title": "Boost.Filesystem",
    "content": "...包含关键词的正文片段...",
    "url": "https://www.boost.org/doc/libs/1_74_0/libs/filesystem/doc/index.htm"
  }
]
```

不带 `word` 参数的请求（`curl 'http://localhost:8081/s'`）返回 400 和 `missing word parameter`。

## 已知问题与后续改进

这部分是我在实现之后复盘出来的短板，按优先级排列：

- **多词查询未按文档去重**：`sercher.hpp` 把各查询词的倒排链直接拼接，同一文档命中多个词时会重复出现在结果里；排序也写在循环内部重复执行。计划改为按 `doc_id` 合并并累加权重，排序移到循环外只做一次。
- **权重是手工设定值**：`weight = 标题命中数 × 10 + 正文命中数 × 1`，这两个系数是经验值，没有文档长度归一化，也没有 IDF，因此长文档天然占优。计划替换为 BM25。
- **倒排链是线性扫描**：多词查询没有做求交或跳表优化，文档量继续增长会成为瓶颈。
- **`doc_id` 使用 `uint16_t`**：上限 65535，当前 7546 篇文档安全，但应该换成 `uint32_t`。
- **索引未序列化**：每次启动都要重新解析并分词构建索引，没有把构建好的倒排索引落盘，启动耗时长。

## 说明

原始 HTML 语料、jieba 词典和编译产物不纳入版本控制，见 `.gitignore`。
