#include "malloc.h"

#include <stdio.h>
#include <string.h>

int main() {
    char* message = calloc_(1, 32);
    strcpy(message, "Roger, Cyr!");
    printf("%s\n", message);
    free_(message);
    return 0;
}
