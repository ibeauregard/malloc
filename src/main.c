#include "malloc.h"

#include <stdio.h>
#include <string.h>

int main() {
    char* message = malloc_(32);
    strcpy(message, "Roger, Cyr!");
    printf("%s\n", message);
    free_(message);
    return 0;
}
