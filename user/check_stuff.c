#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[]){
    
    printf("\n\nChecking Linked-List...\n");
    check_LL();
    printf("Done with LL\n");
    
    printf("\nChecking multiple forks (unique pid's)\n");
    // Check the cas functionality (no two processes with the same pid).
    int n = 4;
    // Print all pid's, make sure none of them repeat.
    for (int i = 0; i < n; i++){
        if (fork() == 0){
            printf("pid: %d\n", getpid());       
        }
    }
    printf("Done\n");
    
    return 1;
}