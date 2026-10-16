parser=parser
DUG=debug
HTTP_SERVER=http_server

CXX=g++
.PHONY:all
all:$(parser) $(DUG) $(HTTP_SERVER)
$(parser):parser.cc
	$(CXX) -o $@ $^ -std=c++11 -Icppjieba/include -lboost_system -lboost_filesystem -O2
$(DUG):debug.cc
	$(CXX) -o $@ $^ -std=c++11 -Icppjieba/include -ljsoncpp -O2
$(HTTP_SERVER):http_server.cc
	$(CXX) -o $@ $^ -std=c++11 -Icppjieba/include -ljsoncpp -lpthread -O2
.PHONY:clean
clean:
	rm -f $(parser)
	rm -f $(DUG)
	rm -f $(HTTP_SERVER)