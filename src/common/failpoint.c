#include "minidb/failpoint.h"

#include <unistd.h>

static minidb_failpoint_t armed_failpoint = MINIDB_FAILPOINT_NONE;

void minidb_failpoint_arm(minidb_failpoint_t failpoint) {
    armed_failpoint = failpoint;
}

void minidb_failpoint_hit(minidb_failpoint_t failpoint) {
    if (failpoint == armed_failpoint && failpoint != MINIDB_FAILPOINT_NONE) {
        _exit(MINIDB_FAILPOINT_EXIT_CODE);
    }
}
