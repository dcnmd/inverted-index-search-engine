# Boost Search

这是一个基于 Boost 文档的 C++ 搜索示例。

## 依赖

- GCC/G++，支持 C++11
- Boost.Filesystem、Boost.System
- JsonCpp
- Git（用于获取子模块）

克隆项目时同时获取依赖：

```sh
git clone --recurse-submodules <仓库地址>
```

如果项目已经克隆，可运行 `git submodule update --init --recursive`。

## 构建和运行

将 Boost 1.74 HTML 文档放入 `data/input/`，然后生成搜索索引并启动服务：

```sh
mkdir -p data/pow_html
make
./parser
./http_server
```

服务监听 `8081` 端口。原始文档和生成的索引不会提交到 Git。
