#include "lib/Db.h"

#include <cstdlib>
#include <cstdio>
#include <cwchar>

// dbcount <db> <min> [progress]: exits 0 when the number of readable books (or, with "progress", of books with
// reading progress above zero) is at least <min>.
int wmain(int argc, wchar_t** argv) // NOLINT(misc-use-internal-linkage): process entry point
{
    if (argc != 3 && argc != 4)
        return 2;
    const bool progress = argc == 4 && std::wcscmp(argv[3], L"progress") == 0;
    auto db = eb::Db::open(argv[1]);
    if (!db)
        return 3;
    auto st = db->prepare(progress ? "SELECT COUNT(*) FROM progress WHERE percent>0" : "SELECT COUNT(*) FROM books WHERE status=0");
    if (!st || !st->step())
        return 4;
    const long long n = st->column_int(0);
    std::printf("%s=%lld\n", progress ? "progress" : "books", n);
    return n >= _wtoi(argv[2]) ? 0 : 1;
}
