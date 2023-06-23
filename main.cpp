#include <algorithm>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <atomic>

#include <ncurses.h>

#include "bitcoin.h"
#include "db.h"

using namespace std;

// [Major].[Minor].[Patch].[Build].[letter]
// [0].[1].[1].[8].[a]
// November 29, 2020: v0.1.1.1.a  for Bitmark v0.9.7.3
// Fri Jun 23, 2023: v0.2.0.TN (latest sipa changes, nCurses)
const char* dnsseeder_version = "0.2.0.SN\0x0";

bool fTestNet = false;

clock_t run_start,run_now;
double run_secs, running_time;
int run_days, run_hours, run_minutes;

	
class CDnsSeedOpts {
public:
  int nThreads;
  int nPort;
  int nDnsThreads;
  int fUseTestNet;
  int fWipeBan;
  int fWipeIgnore;
  const char *mbox;
  const char *ns;
  const char *host;
  const char *tor;
  const char *ip_addr;
  const char *ipv4_proxy;
  const char *ipv6_proxy;
  std::set<uint64_t> filter_whitelist;

  CDnsSeedOpts() : nThreads(96), nDnsThreads(4), ip_addr("::"), nPort(53), mbox(NULL), ns(NULL), host(NULL), tor(NULL), fUseTestNet(false), fWipeBan(false), fWipeIgnore(false), ipv4_proxy(NULL), ipv6_proxy(NULL) {}

  void ParseCommandLine(int argc, char **argv) {
    static const char *help = "Bitmark-seeder\n"
                              "Usage: %s -h <host> -n <ns> [-m <mbox>] [-t <threads>] [-p <port>]\n"
                              "\n"
                              "Options:\n"
                              "-h <host>       Hostname of the DNS seed\n"
                              "-n <ns>         Hostname of the nameserver\n"
                              "-m <mbox>       E-Mail address reported in SOA records\n"
                              "-t <threads>    Number of crawlers to run in parallel (default 96)\n"
                              "-d <threads>    Number of DNS server threads (default 4)\n"
			      "-a <address>    Address to listen on (default ::)\n"
                              "-p <port>       UDP port to listen on (default 53)\n"
                              "-o <ip:port>    Tor proxy IP/Port\n"
                              "-i <ip:port>    IPV4 SOCKS5 proxy IP/Port\n"
                              "-k <ip:port>    IPV6 SOCKS5 proxy IP/Port\n"
                              "-w f1,f2,...    Allow these flag combinations as filters\n"
                              "--testnet       Use testnet\n"
                              "--wipeban       Wipe list of banned nodes\n"
                              "--wipeignore    Wipe list of ignored nodes\n"
                              "-?, --help      Show this text\n"
                              "\n";
    bool showHelp = false;

    while(1) {
      static struct option long_options[] = {
        {"host", required_argument, 0, 'h'},
        {"ns",   required_argument, 0, 'n'},
        {"mbox", required_argument, 0, 'm'},
        {"threads", required_argument, 0, 't'},
        {"dnsthreads", required_argument, 0, 'd'},
	{"address", required_argument, 0, 'a'},
        {"port", required_argument, 0, 'p'},
        {"onion", required_argument, 0, 'o'},
        {"proxyipv4", required_argument, 0, 'i'},
        {"proxyipv6", required_argument, 0, 'k'},
        {"filter", required_argument, 0, 'w'},
        {"testnet", no_argument, &fUseTestNet, 1},
        {"wipeban", no_argument, &fWipeBan, 1},
        {"wipeignore", no_argument, &fWipeBan, 1},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
      };
      int option_index = 0;
      int c = getopt_long(argc, argv, "h:n:m:t:a:p:d:o:i:k:w:", long_options, &option_index);
      if (c == -1) break;
      switch (c) {
        case 'h': {
          host = optarg;
          break;
        }
        
        case 'm': {
          mbox = optarg;
          break;
        }
        
        case 'n': {
          ns = optarg;
          break;
        }
        
        case 't': {
          int n = strtol(optarg, NULL, 10);
          if (n > 0 && n < 1000) nThreads = n;
          break;
        }

        case 'd': {
          int n = strtol(optarg, NULL, 10);
          if (n > 0 && n < 1000) nDnsThreads = n;
          break;
        }

        case 'a': {
          if (strchr(optarg, ':')==NULL) {
            char* ip4_addr = (char*) malloc(strlen(optarg)+8);
            strcpy(ip4_addr, "::FFFF:");
            strcat(ip4_addr, optarg);
            ip_addr = ip4_addr;
          } else {
            ip_addr = optarg;
          }
          break;
        }

        case 'p': {
          int p = strtol(optarg, NULL, 10);
          if (p > 0 && p < 65536) nPort = p;
          break;
        }

        case 'o': {
          tor = optarg;
          break;
        }

        case 'i': {
          ipv4_proxy = optarg;
          break;
        }

        case 'k': {
          ipv6_proxy = optarg;
          break;
        }

        case 'w': {
          char* ptr = optarg;
          while (*ptr != 0) {
            unsigned long l = strtoul(ptr, &ptr, 0);
            if (*ptr == ',') {
                ptr++;
            } else if (*ptr != 0) {
                break;
            }
            filter_whitelist.insert(l);
          }
          break;
        }

        case '?': {
          showHelp = true;
          break;
        }
      }
    }
    if (filter_whitelist.empty()) {
        filter_whitelist.insert(NODE_NETWORK);	// x1
        filter_whitelist.insert(NODE_NETWORK | NODE_BLOOM); // x5
        filter_whitelist.insert(NODE_NETWORK | NODE_WITNESS); // x9
        filter_whitelist.insert(NODE_NETWORK | NODE_WITNESS | NODE_COMPACT_FILTERS); // x49
        filter_whitelist.insert(NODE_NETWORK | NODE_WITNESS | NODE_BLOOM); // xd
        filter_whitelist.insert(NODE_NETWORK_LIMITED); // x400
        filter_whitelist.insert(NODE_NETWORK_LIMITED | NODE_BLOOM); // x404
        filter_whitelist.insert(NODE_NETWORK_LIMITED | NODE_WITNESS); // x408
        filter_whitelist.insert(NODE_NETWORK_LIMITED | NODE_WITNESS | NODE_COMPACT_FILTERS); // x448
        filter_whitelist.insert(NODE_NETWORK_LIMITED | NODE_WITNESS | NODE_BLOOM); // x40c
    }
    if (host != NULL && ns == NULL) showHelp = true;
    if (showHelp) fprintf(stderr, help, argv[0]);
  }
};

#include "dns.h"

CAddrDb db;

extern "C" void* ThreadCrawler(void* data) {
  int *nThreads=(int*)data;
  do {
    std::vector<CServiceResult> ips;
    int wait = 5;
    db.GetMany(ips, 16, wait);
    int64 now = time(NULL);
    if (ips.empty()) {
      wait *= 1000;
      wait += rand() % (500 * *nThreads);
      Sleep(wait);
      continue;
    }
    vector<CAddress> addr;
    for (int i=0; i<ips.size(); i++) {
      CServiceResult &res = ips[i];
      res.nBanTime = 0;
      res.nClientV = 0;
      res.nHeight = 0;
      res.strClientV = "";
      res.services = 0;
      bool getaddr = res.ourLastSuccess + 86400 < now;
      res.fGood = TestNode(res.service,res.nBanTime,res.nClientV,res.strClientV,res.nHeight,getaddr ? &addr : NULL, res.services);
    }
    db.ResultMany(ips);
    db.Add(addr);
  } while(1);
  return nullptr;
}

extern "C" int GetIPList(void *thread, char *requestedHostname, addr_t *addr, int max, int ipv4, int ipv6);

class CDnsThread {
public:
  struct FlagSpecificData {
      int nIPv4, nIPv6;
      std::vector<addr_t> cache;
      time_t cacheTime;
      unsigned int cacheHits;
      FlagSpecificData() : nIPv4(0), nIPv6(0), cacheTime(0), cacheHits(0) {}
  };

  dns_opt_t dns_opt; // must be first
  const int id;
  std::map<uint64_t, FlagSpecificData> perflag;
  std::atomic<uint64_t> dbQueries;
  std::set<uint64_t> filterWhitelist;

  void cacheHit(uint64_t requestedFlags, bool force = false) {
    static bool nets[NET_MAX] = {};
    if (!nets[NET_IPV4]) {
        nets[NET_IPV4] = true;
        nets[NET_IPV6] = true;
    }
    time_t now = time(NULL);
    FlagSpecificData& thisflag = perflag[requestedFlags];
    thisflag.cacheHits++;
    if (force || thisflag.cacheHits * 400 > (thisflag.cache.size()*thisflag.cache.size()) || (thisflag.cacheHits*thisflag.cacheHits * 20 > thisflag.cache.size() && (now - thisflag.cacheTime > 5))) {
      set<CNetAddr> ips;
      db.GetIPs(ips, requestedFlags, 1000, nets);
      dbQueries++;
      thisflag.cache.clear();
      thisflag.nIPv4 = 0;
      thisflag.nIPv6 = 0;
      thisflag.cache.reserve(ips.size());
      for (set<CNetAddr>::iterator it = ips.begin(); it != ips.end(); it++) {
        struct in_addr addr;
        struct in6_addr addr6;
        if ((*it).GetInAddr(&addr)) {
          addr_t a;
          a.v = 4;
          memcpy(&a.data.v4, &addr, 4);
          thisflag.cache.push_back(a);
          thisflag.nIPv4++;
        } else if ((*it).GetIn6Addr(&addr6)) {
          addr_t a;
          a.v = 6;
          memcpy(&a.data.v6, &addr6, 16);
          thisflag.cache.push_back(a);
          thisflag.nIPv6++;
        }
      }
      thisflag.cacheHits = 0;
      thisflag.cacheTime = now;
    }
  }

  CDnsThread(CDnsSeedOpts* opts, int idIn) : id(idIn) {
    dns_opt.host = opts->host;
    dns_opt.ns = opts->ns;
    dns_opt.mbox = opts->mbox;
    dns_opt.datattl = 3600;
    dns_opt.nsttl = 40000;
    dns_opt.cb = GetIPList;
    dns_opt.addr = opts->ip_addr;
    dns_opt.port = opts->nPort;
    dns_opt.nRequests = 0;
    dbQueries = 0;
    perflag.clear();
    filterWhitelist = opts->filter_whitelist;
  }

  void run() {
    dnsserver(&dns_opt);
  }
};

extern "C" int GetIPList(void *data, char *requestedHostname, addr_t* addr, int max, int ipv4, int ipv6) {
  CDnsThread *thread = (CDnsThread*)data;

  uint64_t requestedFlags = 0;
  int hostlen = strlen(requestedHostname);
  if (hostlen > 1 && requestedHostname[0] == 'x' && requestedHostname[1] != '0') {
    char *pEnd;
    uint64_t flags = (uint64_t)strtoull(requestedHostname+1, &pEnd, 16);
    if (*pEnd == '.' && pEnd <= requestedHostname+17 && std::find(thread->filterWhitelist.begin(), thread->filterWhitelist.end(), flags) != thread->filterWhitelist.end())
      requestedFlags = flags;
    else
      return 0;
  }
  else if (strcasecmp(requestedHostname, thread->dns_opt.host))
    return 0;
  thread->cacheHit(requestedFlags);
  auto& thisflag = thread->perflag[requestedFlags];
  unsigned int size = thisflag.cache.size();
  unsigned int maxmax = (ipv4 ? thisflag.nIPv4 : 0) + (ipv6 ? thisflag.nIPv6 : 0);
  if (max > size)
    max = size;
  if (max > maxmax)
    max = maxmax;
  int i=0;
  while (i<max) {
    int j = i + (rand() % (size - i));
    do {
        bool ok = (ipv4 && thisflag.cache[j].v == 4) ||
                  (ipv6 && thisflag.cache[j].v == 6);
        if (ok) break;
        j++;
        if (j==size)
            j=i;
    } while(1);
    addr[i] = thisflag.cache[j];
    thisflag.cache[j] = thisflag.cache[i];
    thisflag.cache[i] = addr[i];
    i++;
  }
  return max;
}

vector<CDnsThread*> dnsThread;

extern "C" void* ThreadDNS(void* arg) {
  CDnsThread *thread = (CDnsThread*)arg;
  thread->run();
  return nullptr;
}

int StatCompare(const CAddrReport& a, const CAddrReport& b) {
  if (a.uptime[4] == b.uptime[4]) {
    if (a.uptime[3] == b.uptime[3]) {
      return a.clientVersion > b.clientVersion;
    } else {
      return a.uptime[3] > b.uptime[3];
    }
  } else {
    return a.uptime[4] > b.uptime[4];
  }
}

extern "C" void* ThreadDumper(void*) {
  int count = 0;
  do {
    Sleep(100000 << count); // First 100s, than 200s, 400s, 800s, 1600s, and then 3200s forever
    if (count < 5)
        count++;
    {
      vector<CAddrReport> v = db.GetAll();
      sort(v.begin(), v.end(), StatCompare);
      FILE *f = fopen("dnsseed.dat.new","w+");
      if (f) {
        {
          CAutoFile cf(f);
          cf << db;
        }
        rename("dnsseed.dat.new", "dnsseed.dat");
      }
      FILE *d = fopen("dnsseed.dump", "w");
      fprintf(d, "# address                                        good  lastSuccess    %%(2h)   %%(8h)   %%(1d)   %%(7d)  %%(30d)  blocks      svcs  version\n");
      double stat[5]={0,0,0,0,0};
      for (vector<CAddrReport>::const_iterator it = v.begin(); it < v.end(); it++) {
        CAddrReport rep = *it;
        fprintf(d, "%-47s  %4d  %11" PRId64 "  %6.2f%% %6.2f%% %6.2f%% %6.2f%% %6.2f%%  %6i  %08" PRIx64 "  %5i \"%s\"\n", rep.ip.ToString().c_str(), (int)rep.fGood, rep.lastSuccess, 100.0*rep.uptime[0], 100.0*rep.uptime[1], 100.0*rep.uptime[2], 100.0*rep.uptime[3], 100.0*rep.uptime[4], rep.blocks, rep.services, rep.clientVersion, rep.clientSubVersion.c_str());
        stat[0] += rep.uptime[0];
        stat[1] += rep.uptime[1];
        stat[2] += rep.uptime[2];
        stat[3] += rep.uptime[3];
        stat[4] += rep.uptime[4];
      }
      fclose(d);
      FILE *ff = fopen("dnsstats.log", "a");
      fprintf(ff, "%llu %g %g %g %g %g\n", (unsigned long long)(time(NULL)), stat[0], stat[1], stat[2], stat[3], stat[4]);
      fclose(ff);
    }
  } while(1);
  return nullptr;
}

extern "C" void* ThreadStats(void*) {
  bool first = true;
  do {
    char c[256];
    time_t tim = time(NULL);
    struct tm *tmp = localtime(&tim);
    strftime(c, 256, "[%y-%m-%d %H:%M:%S]", tmp);
    CAddrDbStats stats;
    db.GetStats(stats);
    //if (first)
    //{
    //  first = false;
	// https://notes.burke.libbey.me/ansi-escape-codes/
       //printf("\n\n\n\x1b[3A"); // ANSI escape \x1b = ESC; 3A = move cursor up 3 chars
    //}
    //else
      //printf("\x1b[2K\x1b[u"); // u = restore cursor to position last saved by s
    //printf("\x1b[s"); // s = save cursor position
    uint64_t requests = 0;
    uint64_t queries = 0;
    for (unsigned int i=0; i<dnsThread.size(); i++) {
      requests += dnsThread[i]->dns_opt.nRequests;
      queries += dnsThread[i]->dbQueries;
    }

    // https://www.techiedelight.com/find-execution-time-c-program/
    // Running Time
    run_now = time(NULL);
    running_time = (double) (run_now - run_start);
    run_secs = running_time;
    run_days = running_time / 86400; // 86,400 =(60*60*24);
    running_time -= run_days * 86400;
    run_hours = running_time / 3600;
    running_time -= run_hours * 3600;
    run_minutes = running_time / 60;
    move(3,59); printw("%dd %dh %dm %.0fs",run_days, run_hours, run_minutes, running_time);
    //move(4,55); printw("Up: %11.0f sec",run_secs);

    move(2,55); printw("%s",c);
    move(8,9);  printw("%i/%i", stats.nGood, stats.nAvail);
    move(8,20); printw("%i", stats.nTracked);
    move(8,29); printw("%i", stats.nAge);
    move(8,36); printw("%i", stats.nNew);
    move(8,44); printw("%i", stats.nAvail - stats.nTracked - stats.nNew);
    move(8,54); printw("%i", stats.nBanned); 
    move(8,63); printw("%llu", (unsigned long long)requests);
    move(8,72); printw("%llu", (unsigned long long)queries);
    //printw("%s %i/%i available (%i tried in %is, %i new, %i active), %i banned; %llu DNS requests, %llu db queries", c, stats.nGood, stats.nAvail, stats.nTracked, stats.nAge, stats.nNew, stats.nAvail - stats.nTracked - stats.nNew, stats.nBanned, (unsigned long long)requests, (unsigned long long)queries);
    //printf("%s %i/%i available (%i tried in %is, %i new, %i active), %i banned; %llu DNS requests, %llu db queries", c, stats.nGood, stats.nAvail, stats.nTracked, stats.nAge, stats.nNew, stats.nAvail - stats.nTracked - stats.nNew, stats.nBanned, (unsigned long long)requests, (unsigned long long)queries);
    refresh();
    Sleep(1000);
  } while(1);
  return nullptr;
}

//  These should be regular P2P coin nodes which serve as "fixed seed nodes", 
static const string mainnet_seeds[] =  {"seed.bitmark.co",
					"radiant.bitmark.co",
					"us.bitmark.co",
                                        "eu.bitmark.io",
					"jp.bitmark.co",
					"marks.cronobit.com",
                                        ""};

// Bitcoin Examples
//static const string mainnet_seeds[] = {"dnsseed.bluematt.me", "bitseed.xf2.org", "dnsseed.bitcoin.dashjr.org", "seed.bitcoin.sipa.be", ""};

// Bitmark (MARKS) (BTM)  
static const string testnet_seeds[] = { "tz.bitmark.co",
					"tz.bitmark.guru",
					"tz.bitmark.io",
                                       	"tz.bitmark.mx",
					"tz.bitmark.one",
                                       ""};
// Bitcoin Examples
//static const string testnet_seeds[] = {"testnet-seed.alexykot.me",
//                                       "testnet-seed.bitcoin.petertodd.org",
//                                       "testnet-seed.bluematt.me",
//                                       "testnet-seed.bitcoin.schildbach.de",
//                                       ""};
static const string *seeds = mainnet_seeds;

extern "C" void* ThreadSeeder(void*) {
  // Bitmark - no TOR / Onion hidden service nodes yet; Nov. 29, 2020
  //if (!fTestNet){
  //  db.Add(CService("kjy2eqzk4zwi5zd3.onion", 8333), true);
  // }
  do {
    for (int i=0; seeds[i] != ""; i++) {
      vector<CNetAddr> ips;
      LookupHost(seeds[i].c_str(), ips);
      for (vector<CNetAddr>::iterator it = ips.begin(); it != ips.end(); it++) {
        db.Add(CService(*it, GetDefaultPort()), true);
      }
    }
    Sleep(1800000);
  } while(1);
  return nullptr;
}

int main(int argc, char **argv) {

  // Running Time
  run_start = time(NULL);

  signal(SIGPIPE, SIG_IGN);
  setbuf(stdout, NULL);
  CDnsSeedOpts opts;
  opts.ParseCommandLine(argc, argv);

  initscr();
  move(2,15);   printw("Bitmark DNS Seeder Monitor - v0.1.1.1TN\n");
  move(6,62);  printw("DNS      db");
  move(7,7);   printw("Available   tried   in sec   new    active   Banned  Requests Queries");
  move(15,8);  printw("Supporting whitelisted filters: ");
  //printf("Bitmark DNS Seeder v0.1.1.1bN\n");
  //printf("Supporting whitelisted filters: ");
  move(16,15);
  for (std::set<uint64_t>::const_iterator it = opts.filter_whitelist.begin(); it != opts.filter_whitelist.end(); it++) {
      if (it != opts.filter_whitelist.begin()) {
          printw(",");
          //printf(",");
      }
      printw("0x%lx", (unsigned long)*it);
      //printf("0x%lx", (unsigned long)*it);
  }
  printw("\n");
  //printf("\n");
  refresh();
  if (opts.tor) {
    CService service(opts.tor, 9050);
    if (service.IsValid()) {
      printw("Using Tor proxy at %s\n", service.ToStringIPPort().c_str());
      //printf("Using Tor proxy at %s\n", service.ToStringIPPort().c_str());
      SetProxy(NET_TOR, service);
    }
  }
  if (opts.ipv4_proxy) {
    CService service(opts.ipv4_proxy, 9050);
    if (service.IsValid()) {
      printw("Using IPv4 proxy at %s\n", service.ToStringIPPort().c_str());
      //printf("Using IPv4 proxy at %s\n", service.ToStringIPPort().c_str());
      SetProxy(NET_IPV4, service);
    }
  }
  if (opts.ipv6_proxy) {
    CService service(opts.ipv6_proxy, 9050);
    if (service.IsValid()) {
      printw("Using IPv6 proxy at %s\n", service.ToStringIPPort().c_str());
      //printf("Using IPv6 proxy at %s\n", service.ToStringIPPort().c_str());
      SetProxy(NET_IPV6, service);
    }
  }
  bool fDNS = true;
  if (opts.fUseTestNet) {
      printw("Using testnet.\n");
      //printf("Using testnet.\n");
      pchMessageStart[0] = 0x0b;
      pchMessageStart[1] = 0x11;
      pchMessageStart[2] = 0x09;
      pchMessageStart[3] = 0x07;
      seeds = testnet_seeds;
      fTestNet = true;
  }
  if (!opts.ns) {
    printw("No nameserver set. Not starting DNS server.\n");
    //printf("No nameserver set. Not starting DNS server.\n");
    fDNS = false;
  }
  if (fDNS && !opts.host) {
    fprintf(stderr, "No hostname set. Please use -h.\n");
    exit(1);
  }
  if (fDNS && !opts.mbox) {
    fprintf(stderr, "No e-mail address set. Please use -m.\n");
    exit(1);
  }
  // TODO: Outpu file-name customizations ...
  FILE *f = fopen("dnsseed.dat","r");
  if (f) {
    move(10,11); printw("dnsseed.dat:");
    //printw("Loading dnsseed.dat...");
    //printf("Loading dnsseed.dat...");
    CAutoFile cf(f);
    cf >> db;
    if (opts.fWipeBan)
        db.banned.clear();
    if (opts.fWipeIgnore)
        db.ResetIgnores();
    move(10,30); printw("loaded");
    //printw("done\n");
    //printf("done\n");
  }
  pthread_t threadDns, threadSeed, threadDump, threadStats;
  if (fDNS) {
    move(4,2);   printw("%s on %s : %i", opts.host, opts.ns, opts.nPort);
    move(11,11); printw("DNS threads:");
    //printw("Starting %i DNS threads for %s on %s (port %i)...", opts.nDnsThreads, opts.host, opts.ns, opts.nPort);
    //printf("Starting %i DNS threads for %s on %s (port %i)...", opts.nDnsThreads, opts.host, opts.ns, opts.nPort);
    dnsThread.clear();
    for (int i=0; i<opts.nDnsThreads; i++) {
      dnsThread.push_back(new CDnsThread(&opts, i));
      pthread_create(&threadDns, NULL, ThreadDNS, dnsThread[i]);
      //printw(".");
      //printf(".");
      Sleep(20);
    }
    move(11,26); printw("%i", opts.nDnsThreads);
    move(11,29); printw("started");
    //printw("done\n");
    //printf("done\n");
  }
  move(12,16); printw("seeder:");
  //printw("Starting seeder...");
  //printf("Starting seeder...");
  pthread_create(&threadSeed, NULL, ThreadSeeder, NULL);
  move(12,29); printw("started");
  //printw("done\n");
  //printf("done\n");
  move(13,7);  printw("crawler threads:");
  move(13,25); printw("%i", opts.nThreads); 
  //printw("Starting %i crawler threads...", opts.nThreads);
  //printf("Starting %i crawler threads...", opts.nThreads);
  refresh();
  pthread_attr_t attr_crawler;
  pthread_attr_init(&attr_crawler);
  pthread_attr_setstacksize(&attr_crawler, 0x20000);
  for (int i=0; i<opts.nThreads; i++) {
    pthread_t thread;
    pthread_create(&thread, &attr_crawler, ThreadCrawler, &opts.nThreads);
  }
  pthread_attr_destroy(&attr_crawler);
  move(13,29); printw("started");
  //printw("done\n");
  //printf("done\n");
  pthread_create(&threadStats, NULL, ThreadStats, NULL);
  pthread_create(&threadDump, NULL, ThreadDumper, NULL);
  void* res;
  pthread_join(threadDump, &res);

  endwin();
  return 0;
}
