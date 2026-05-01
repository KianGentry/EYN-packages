/* cputest - silent CPU-bound test program for preemption testing */

#include <stdio.h>

int main(void) {
    /* Run a CPU-bound loop without any output.
     * The shell should remain responsive even while this runs.
     */
    volatile unsigned int sum = 0;
    for (unsigned int i = 0; i < 500000000u; ++i) {
        sum += i;
    }
    
    /* Exit silently without printing anything */
    return 0;
}
