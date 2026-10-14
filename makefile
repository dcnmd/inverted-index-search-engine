parser=parser
DUG=debug
HTTP_SERVER=http_server

cc=g++
.PHONY:all
all:$(parser) $(DUG) $(HTTP_SERVER)
$(parser):parser.cc
	g++ -o $@ $^ -std=c++11 -Icppjieba/include -lboost_system -lboost_filesystem
$(DUG):debug.cc
	g++ -o $@ $^ -std=c++11 -Icppjieba/include -ljsoncpp
$(HTTP_SERVER):http_server.cc
	g++ -o $@ $^ -std=c++11 -Icppjieba/include -ljsoncpp -lpthread
.PHONY:clean
clean:
	rm -f $(parser)
	rm -f $(DUG)
	rm -f $(HTTP_SERVER)