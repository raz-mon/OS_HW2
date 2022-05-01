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
<<<<<<< HEAD
    // Print all pid's, make sure none of them repeat.
    printf("pid: %d", myproc()->pid);
    return 0;
=======
    
    return 1;
>>>>>>> 308418d31d8c73ef794d76447f99aa44ac4f2159
}