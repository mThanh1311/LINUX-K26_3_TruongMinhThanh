#include <stdio.h>

int main(void)
{
#ifdef DEBUG
    printf("[DEBUG] Debug mode enabled\n");
#endif

    printf("Sensor Gateway started\n");

    return 0;
}