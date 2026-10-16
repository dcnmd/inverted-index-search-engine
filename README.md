# inverted-index-search-engine

基于 Boost 官方文档语料实现的 C++ 倒排索引搜索引擎。

- **语料**：Boost 1.74 官方文档站，7546 个 HTML 页面
- **解析**：手写状态机逐字符剥离 HTML 标签，`boost::filesystem` 还原每篇文档的原始 URL
- **索引**：正排索引（文档元信息 + 文档长度）+ 倒排索引（词 → 文档列表 + 词频）
- **分词**：cppjieba，MP + HMM 混合模型
- **排序**：BM25（`k1 = 1.2`，`b = 0.75`），标题命中额外加权
- **检索**：分词 → 查倒排链 → 按 `doc_id` 合并打分 → 摘取摘要 → JSON 返回
- **服务**：cpp-httplib，提供 `/s?word=xxx` 检索接口

## 目录结构

| 文件 | 作用 |
| --- | --- |
| `parser.cc` | 遍历 HTML 语料，剥离标签，按 `\3` 分隔符落盘为 `data/pow_html/output.txt` |
| `index.hpp` | 索引单例。正排 `std::vector<DocInfo>`，倒排 `std::unordered_map<std::string, IndexInfolist>` |
| `sercher.hpp` | 检索逻辑：分词 → 按 `doc_id` 合并倒排链并累加 BM25 分数 → 排序 → `getdesc` 摘摘要 → 输出 JSON |
| `Ns_Util.hpp` | 工具：文件读取、字符串切分、cppjieba 封装 |
| `http_server.cc` | HTTP 服务入口，监听 8081 |
| `debug.cc` | 命令行检索调试工具 |
| `web/index.html` | 检索页面 |

## 排序：BM25

排序分数按查询词逐个累加：

```
score(d, q) = Σ_t IDF(t) · [ tf · (k1 + 1) / (tf + k1 · (1 - b + b · |d| / avgdl)) + title_boost(t, d) ]

IDF(t)      = ln(1 + (N - df + 0.5) / (df + 0.5))
title_boost = IDF(t) · 3.0     （t 出现在标题时）
```

- `tf`：词在**正文**中的出现次数；`df`：包含该词的文档数；`N`：文档总数
- `|d|`：文档正文分词后的词数，在构建索引时由 `BuildInvertedIndex` 统计（`index.hpp` 的 `DocInfo::doc_len`），`avgdl` 为全部文档的平均长度
- `k1 = 1.2`、`b = 0.75` 取 BM25 的常用默认值；`3.0` 是标题权重，属于经验值

两点实现上的取舍：

1. **文档长度归一化**让长文档不再因为词频天然偏高而占优。原先用的是 `weight = 标题命中数 × 10 + 正文命中数 × 1`，既没有长度归一化也没有 IDF。
2. **标题加分是独立的加项，不是把标题词频并进 `tf`。** 如果把标题词频乘个系数加到 `tf` 上，那么只在标题出现、正文没出现的词 `tf` 为 0，整个 BM25 项会算成 0 分，标题命中的价值反而被抹掉。所以正文照常算 BM25，标题命中另外加一项 `IDF × 3.0`。

`doc_id` 在全流程中使用 `uint16_t`。

## 构建与运行

### 1. 获取代码和子模块

```sh
git clone --recurse-submodules https://github.com/dcnmd/inverted-index-search-engine.git
```

已经 clone 过的话，补拉子模块：

```sh
git submodule update --init --recursive
```

> `cpp-httplib` 和 `cppjieba` 是第三方依赖，用 submodule 固定在验证过的 commit 上，不在本仓库内重复存放。jieba 的词典随 `cppjieba` 一起拉下来，程序直接从 `cppjieba/dict/` 读取，不需要额外拷贝。

### 2. 准备语料

把 Boost 1.74 的 HTML 文档放进 `data/input/`。语料体积较大，不纳入版本控制。

### 3. 编译运行

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

返回 JSON 数组，按 BM25 分数降序，每项包含标题、正文摘要和原始 URL。同一篇文档命中多个查询词时只出现一次，分数累加，摘要在所有查询词中取最早出现的位置：

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

结果集上限为 50 条（`sercher.hpp` 中的 `MAX_RESULTS`）。`boost` 这类在语料中普遍出现的词会命中约 7500 篇文档，不加限制时单次响应可达 3 MB。

## 性能

`wrk -t2 -c100 -d30s`，客户端与测试服务同机（2 核 2 G 云服务器，Ubuntu 22.04），走 loopback。**这是同机自比数字，用于衡量改造前后的变化，不代表线上承载能力。**

| 查询词 | 命中规模 | QPS | 单次响应 |
| --- | --- | --- | --- |
| `boost` | ~7500 篇 | 603 | 20 KB |
| `asio` | ~1500 篇 | 2076 | 16 KB |
| `filesystem` | ~11 篇 | 2292 | 4.8 KB |

两个优化各自的贡献。`filesystem` 的命中数远小于上限，可以当对照组把两者分离：

| | `boost` | `asio` | `filesystem` |
| --- | --- | --- | --- |
| 仅开 `-O2` | ×2.5 | ×3.0 | ×3.4 |
| 仅加 Top-K 截断 | ×28 | ×33 | ×1.0 |
| 合计 | ×71 | ×98 | ×3.4 |

`-O2` 的收益主要来自 cppjieba 分词和摘要定位这两条热点路径；Top-K 截断把每条结果都要做的摘要提取与 JSON 构造从约 7500 次砍到 50 次。

## 已经修过的问题

这些是实现完成之后复盘发现的，都已经修掉：

- **多词查询重复召回**：`sercher.hpp` 原先把各查询词的倒排链直接拼接，同一文档命中多个词时会在结果里重复出现；排序也写在查询词循环内部，每个词都要全量重排一次。改为用 `std::unordered_map<uint16_t, ...>` 按 `doc_id` 合并、分数累加，排序移到循环外只做一次。
- **结果集没有上限**：接口原先把命中文档全部返回，`boost` 这类高频词会命中约 7500 篇，单次响应 3 MB，压测 QPS 只有 8.5 且请求大面积超时。加入 Top-K 截断后响应降到 20 KB，QPS 到 603。
- **排序用的权重是手工设定值**：`weight = 标题命中数 × 10 + 正文命中数 × 1`，没有 IDF，也没有文档长度归一化，长文档天然占优。已替换为 BM25。
- **摘要只处理第一个命中词**：原先 `getdesc` 只接收一个词。改为接收该文档命中的全部查询词，在所有词里取最早出现的位置作为摘要中心。
- **HTTP 接口的参数判断条件写反**：`http_server.cc` 里原本是 `if (req.has_param("word"))` 就返回 400，导致带 `word` 参数的正常请求反而报错。已改为 `if (!req.has_param("word"))`。
- **索引中没有文档长度信息**：为 BM25 的长度归一化补上了 `DocInfo::doc_len` 和全局的 `avg_doc_len`。

## 已知问题与后续改进

按优先级排列：

- **倒排链是线性扫描**：多词查询把每个词的倒排链完整遍历一遍，没有做求交或跳表优化。文档量继续增长会成为瓶颈。
- **`doc_id` 使用 `uint16_t`**：上限 65535，当前 7546 篇文档安全，但应该换成 `uint32_t`。
- **索引未序列化**：每次启动都要重新解析并分词构建索引，没有把构建好的倒排索引落盘，启动耗时长。
- **单例的初始化不是线程安全的**：`index.hpp` 的 `get_instance()` 用的是双重检查锁，但 `instance` 是普通指针而非 `std::atomic`，且 `std::mutex` 没有用 RAII 保护（加锁后若抛异常不会解锁）。可以在构造函数里提前初始化，或者换成 C++11 的局部静态变量写法，由编译器保证线程安全。

## 说明

原始 HTML 语料、编译产物不纳入版本控制，见 `.gitignore`。
