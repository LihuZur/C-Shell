#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

//Eran's trick (zombie exterminator)
static void
child_handler(int sa)
{
    pid_t pid;
    int status;

    /* EEEEXTEERMINAAATE! */
    while((pid = waitpid(-1, &status, WNOHANG)) > 0);
}

/*
We will use this function for child processes in order to restore the default behaviour for SIGINT
*/
void change_sigint_behaviour(){
    //Setting the required struct for sigaction()
    struct sigaction sa;
    sa.sa_flags=SA_RESTART;
    sa.sa_handler=SIG_DFL;
    
    if(sigaction(SIGINT,&sa,0) == -1){//If sigaction() call failed
        fprintf(stderr,"an error has occured in sigaction(): %s\n",strerror(errno));
        exit(1);
    }
}

/*
Preparing for the signal handling for main process, overriding the handling for SIGINT
so shell process will not be terminated upon ctrl C
*/
int prepare(void){
    struct sigaction sa_ignore;
    memset(&sa_ignore,0,sizeof(sa_ignore));//Filling with zeros

    //Preparing the struct to ignore the SIGINT for shell process
    sa_ignore.sa_handler=SIG_IGN;
    sa_ignore.sa_flags=SA_RESTART;

    if(sigaction(SIGINT,&sa_ignore,0) == -1){//Error in sigaction()
        fprintf(stderr,"an error has occured in sigaction(): %s\n",strerror(errno));
        exit(1);
    }

    //Eran's TRICK
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sa.sa_handler = child_handler;
    sigaction(SIGCHLD, &sa, NULL);
    return 0;
}

//HELPER FUNCTIONS- handling each of the 4 cases for input commands with a seperate function

/*
Implementing the ability to execute a command in the background from the child proceess.
The parent process will not wait for the child process to finish before taking another command
*/
int execute_background_process(char **command){
    pid_t pid = fork();

    if(pid == -1){//Fork encountered an error
        perror("fork() failed");
        return 0;//Shell will terminate
    }

    else if(pid == 0){//Child process
        char *command_name = command[0];//The first word is the command name
        execvp(command_name, command);
        perror(command_name);
        exit(1);
    }

    return 1;//Shell continues to run
}

/*
Implementing the required piping with command1 and command2
*/
int execute_piping_commands(char **command1, char **command2){
    int pfds[2];//Preparing an array for pipe(), as shown in recitation

    if(pipe(pfds) == -1){//Creating the pipe and checking for error
        perror("pipe() failed");
        return 0;//Shell will terminate
    }

    pid_t pid = fork();//Creating a child process to pipe with

    if(pid == -1){//Fork encountered an error
        perror("fork() failed");
        return 0;//Shell will terminate
    }

    if(pid == 0){//We will have 2 children. This is the first, which will pass its output to
                 //the input of the second child
        change_sigint_behaviour();//Changing SIGINT behaviour for the child process, as required

        if(dup2(pfds[1],1) == -1){//Pipe recidecting
            perror("dup2() failed");
            exit(1);
        }

        //Closing
        close(pfds[1]);
        close(pfds[0]);

        //Executing command1
        char *command_name = command1[0];//The first word is the command name
        execvp(command_name, command1);
        perror(command_name);
        exit(1);

    }

    pid_t pid2 = fork();//Creating the second child process

    if(pid2 == -1){//Fork encountered an error
        perror("fork() failed");
        return 0;//Shell will terminate
    }

    if(pid2 == 0){//Handling the second child
        change_sigint_behaviour();//Changing SIGINT behaviour for the child process, as required

        if(dup2(pfds[0],0) == -1){//Pipe recidecting
            perror("dup2() failed");
            exit(1);
        }

        //Closing
        close(pfds[0]);
        close(pfds[1]);

        //Executing command2
        char *command_name = command2[0];//The first word is the command name
        execvp(command_name, command2);
        perror(command_name);
        exit(1);
    }

    //Closing (from parent)
    close(pfds[0]);
    close(pfds[1]);

    //Waiting for both child processes to finish
    waitpid(pid,NULL,0);
    waitpid(pid2,NULL,0);

    return 1;//Shell continues to run
}

/*
Executing a regular command
*/
int execute_normally(char **command){
    pid_t pid = fork();//Creating a child process

    if(pid == -1){//Fork encountered an error
        perror("fork() failed");
        return 0;//Shell will terminate
    }

    else if(pid == 0){//Child process
        change_sigint_behaviour();//Changing SIGINT behaviour for the child process, as required
        char *command_name = command[0];//The first word is the command name
        execvp(command_name, command);
        perror(command_name);
        exit(1);
    }

    else{//Parent process must wait for the child process to finish!
        waitpid(pid, NULL, 0);
    }

    return 1;//Shell continues to run
}

/*
Redirecting the output of the command which is before the '>' sign to the output file
which is after the '>' sign. filename_ind is the index in "command" for the file name 
*/
int output_redirection_commands(char **command, int filename_ind){
    pid_t pid = fork();

    if(pid == -1){//Fork encountered an error
        perror("fork() failed");
        return 0;//Shell will terminate
    }
    
    else if(pid == 0){//Child process
        change_sigint_behaviour();//Changing SIGINT behaviour for the child process, as required

        int fd = open(command[filename_ind], O_WRONLY | O_CREAT, 0642);//Opening the file

        if(fd < 0){//File was not opened successfully
            perror("open() failed");
            exit(1);
        }

        if(dup2(fd, STDOUT_FILENO) == -1){//Redirecting the output to this file
            perror("dup2() failed");
            exit(1);
        }

        close(fd);//Closing the file

        char *command_name = command[0];//The first word is the command name
        execvp(command_name, command);//execvp() reads until the NULL that we placed in the former
                                      //index of the '>', so we get the exact command!
        perror(command_name);
        exit(1);
    }

    else{//Parent process must wait for the child process to finish!
        waitpid(pid, NULL, 0);
    }

    return 1;//Shell continues to run
}
/*
The required function which handles the input and uses the correct helper function from the 4.
Returns 0 in case of shell error in order to terminate the shell, and 1 otherwise
*/
int process_arglist(int count, char **arglist){
    //Searching for the '|' and '>' symbols
    for(int i=0; i<count; i++){
        if(strcmp(arglist[i], "|") == 0){
            arglist[i] = NULL;//Removing the '|'
            return(execute_piping_commands(arglist, (char **) arglist+ i+1));//Treating the required piping
        }
        
        if(strcmp(arglist[i], ">") == 0){
            arglist[i] = NULL;//Removing the '>'
            return(output_redirection_commands(arglist, i+1));//Treating the required output redirection
        }
    }

    if(strcmp( arglist[count-1], "&") == 0){//We need to run a command in the background
        arglist[count-1] = NULL;//Deleting the '&' sign, since we should not send it to execvp()
        return(execute_background_process(arglist));//Treating the running in the backround case
    }

    else{
        return(execute_normally(arglist));//Regular command treatment
    }

}

//We do not need this function
int finalize(){
    return 0;
}
