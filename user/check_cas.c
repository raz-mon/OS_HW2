#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[]){
    
    int n = 3;
    // Print all pid's, make sure none of them repeat.
    for (int i = 0; i < n; i++){
        if (fork() == 0){
            printf("pid: %d\n", getpid());       
        }
    }
    
    return 1;
}