#include "lib/Db.h"

#include <cstdlib>
#include <cstdio>

int wmain(int argc, wchar_t** argv) // NOLINT(misc-use-internal-linkage): process entry point
{
    if (argc != 3)
        return 2;
    auto db = eb::Db::open(argv[1]);
    if (!db)
        return 3;
    auto st = db->prepare("SELECT COUNT(*) FROM books WHERE status=0");
    if (!st || !st->step())
        return 4;
    const long long n = st->column_int(0);
    std::printf("books=%lld\n", n);
    return n >= _wtoi(argv[2]) ? 0 : 1;
}
