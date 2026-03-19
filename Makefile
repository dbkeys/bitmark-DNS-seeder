CXX = g++
CXXFLAGS = -O3 -g0 -march=native -std=c++11 -Wall -Wno-unused -Wno-sign-compare -Wno-reorder -Wno-comment
LDFLAGS = -no-pie
LDLIBS = -lcrypto -lncurses

# Note: output executable file is named dnsseed.MARKS
dnsseed: dns.o bitcoin.o netbase.o protocol.o db.o main.o util.o
	$(CXX) -pthread $(LDFLAGS) -o dnsseed.MARKS dns.o bitcoin.o netbase.o protocol.o db.o main.o util.o $(LDLIBS)

%.o: %.cpp *.h
	$(CXX) -pthread $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f *.o dnsseed.MARKS

