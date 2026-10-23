-- mixed.lua —— 混合快慢查询，用来暴露尾延迟（P99）
--
-- 单一 URL 压测看不出阶段 4 的价值：所有请求一样快时，串行和并发没区别。
-- 这里按命中规模分三档（全库 / 上千篇 / 十几篇）随机打，慢请求排队的影响才会出现在 P99 上。
--
-- 用法：
--   wrk -t2 -c100 -d30s --latency -s mixed.lua 'http://127.0.0.1:8082/'
--   ⚠️ 必须带 --latency，否则 wrk 不会打印 P50/P75/P90/P99。

local queries = {
    "word=filesystem",  -- 快：十几篇
    "word=asio",        -- 中：上千篇
    "word=boost",       -- 慢：全库（约 7500 篇，结果截断到 50 条）
}

math.randomseed(os.time() + math.floor(os.clock() * 1000000))

request = function()
    local q = queries[math.random(#queries)]
    return wrk.format("GET", "/s?" .. q)
end
