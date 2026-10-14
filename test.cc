#include "cpp-httplib/httplib.h"

int main()
{
    httplib::Server svr;

    svr.Get("/hi", [](const httplib::Request &req, httplib::Response &rsp)
            { rsp.set_content("你好,世界!", "text/plain; charset=utf-8"); });
    svr.listen("0.0.0.0", 8081);
    return 0;
}