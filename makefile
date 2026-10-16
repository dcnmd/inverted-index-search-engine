parser=parser
DUG=debug
HTTP_SERVER=http_server

cc=g++
.PHONY:all
all:$(parser) $(DUG) $(HTTP_SERVER)
$(parser):parser.cc
	g++ -o $@ $^ -std=c++11 -Icppjieba/include -lboost_system -lboost_filesystem -02
$(DUG):debug.cc
	g++ -o $@ $^ -std=c++11 -Icppjieba/include -ljsoncpp -02
$(HTTP_SERVER):http_server.cc
	g++ -o $@ $^ -std=c++11 -Icppjieba/include -ljsoncpp -lpthread -02
.PHONY:clean
clean:
	rm -f $(parser)
	rm -f $(DUG)
	rm -f $(HTTP_SERVER)