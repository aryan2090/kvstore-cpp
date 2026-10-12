// Command-line front end: kvcli <dir> put <key> <value> | get <key> | del <key> | compact | stats
#include <cstring>
#include <iostream>

#include "kv/db.h"

namespace {

int usage() {
  std::cerr << "usage: kvcli <dir> put <key> <value>\n"
               "       kvcli <dir> get <key>\n"
               "       kvcli <dir> del <key>\n"
               "       kvcli <dir> compact\n"
               "       kvcli <dir> stats\n";
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return usage();
  const std::string cmd = argv[2];
  try {
    auto db = kv::DB::open(argv[1]);
    if (cmd == "put" && argc == 5) {
      db->put(argv[3], argv[4]);
    } else if (cmd == "get" && argc == 4) {
      auto v = db->get(argv[3]);
      if (!v) {
        std::cerr << "not found\n";
        return 1;
      }
      std::cout << *v << "\n";
    } else if (cmd == "del" && argc == 4) {
      if (!db->del(argv[3])) {
        std::cerr << "not found\n";
        return 1;
      }
    } else if (cmd == "compact" && argc == 3) {
      auto r = db->compact();
      std::cout << "compacted " << r.segments_compacted << " segment(s): " << r.bytes_before
                << " -> " << r.bytes_after << " bytes\n";
    } else if (cmd == "stats" && argc == 3) {
      auto s = db->stats();
      std::cout << "keys=" << s.live_keys << " segments=" << s.segments
                << " disk_bytes=" << s.disk_bytes << " recovered_truncated_bytes="
                << s.truncated_bytes << "\n";
    } else {
      return usage();
    }
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
