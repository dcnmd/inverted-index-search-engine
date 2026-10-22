parser=parser
DUG=debug
HTTP_SERVER=http_server
EPOLL_ECHO=epoll_echo
EPOLL_HEADERS=CommandServer.hpp HttpParser.hpp TcpSocket.hpp sercher.hpp index.hpp Ns_Util.hpp
HEADERS=index.hpp sercher.hpp Ns_Util.hpp

CXX=g++
.PHONY:all
all:$(parser) $(DUG) $(HTTP_SERVER) $(EPOLL_ECHO)
$(parser):parser.cc $(HEADERS)
	$(CXX) -o $@ $< -std=c++11 -Icppjieba/include -lboost_system -lboost_filesystem -O2 -Wall
$(DUG):debug.cc $(HEADERS)
	$(CXX) -o $@ $< -std=c++11 -Icppjieba/include -ljsoncpp -O2 -Wall
$(HTTP_SERVER):http_server.cc $(HEADERS)
	$(CXX) -o $@ $< -std=c++11 -Icppjieba/include -ljsoncpp -lpthread -O2 -Wall
$(EPOLL_ECHO):epoll_echo.cc $(EPOLL_HEADERS)
	$(CXX) -o $@ $< -std=c++11 -Icppjieba/include -ljsoncpp -lpthread -O2 -Wall
.PHONY:clean
clean:
	rm -f $(parser)
	rm -f $(DUG)
	rm -f $(HTTP_SERVER)
	rm -f $(EPOLL_ECHO)