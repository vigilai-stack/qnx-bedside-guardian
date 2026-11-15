/* bgscn: run a scenario offline with simulated time and print the log. */
#include <stdio.h>

#include "bg/scenario.h"

int main(int argc, char **argv) {
    static bg_scenario sc;
    if (argc != 2) {
        fprintf(stderr, "usage: bgscn SCENARIO\n");
        return 2;
    }
    if (bg_scenario_load(argv[1], &sc) != 0) {
        fprintf(stderr, "bgscn: %s\n", sc.error);
        return 2;
    }
    return bg_scenario_run_offline(&sc, stdout) == 0 ? 0 : 1;
}
