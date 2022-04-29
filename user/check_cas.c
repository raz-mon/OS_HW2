#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/defs.h"

int main(int argc, char *argv[]){
    int n = 5;
    for (int i = 0; i < n; i++){
        fork();   
    }
    // Print all pid's, make sure none of them repeat.
    printf("pid: %d", myproc()->pid);
}