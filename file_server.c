#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <semaphore.h>
#include <pthread.h>

#define FILELOCK_ARR_SIZE 1000
#define MAX_BUF_SZE 10000
#define TIMESTR_SZE 100
#define FILENME_SZE 100
#define CMD_INP_SZE 20
#define PTH_INP_SZE 500
#define STR_INP_SZE 100
#define READEMPTY_OUT_SZE MAX_BUF_SZE + 300
#define FILE_NOT_FOUND -1
#define NUM_OF_PROCESSORS sysconf(_SC_NPROCESSORS_ONLN)
#define READ_LK  0
#define EMPTY_LK 1
#define CMDS_LK  2  

#define GREEN   "\x1b[32m"
#define YELLOW  "\x1b[33m"
#define BLUE    "\x1b[34m"
#define MAGENTA "\x1b[35m"
#define RESET   "\x1b[0m"

#define handle_error_en(en, msg) \
       do { errno = en; perror(msg); exit(EXIT_FAILURE); } while (0)

typedef struct {
    char fpath[PTH_INP_SZE];
    sem_t *f_lock;
} FileLock;

typedef struct {
    FileLock *filelock_arr[FILELOCK_ARR_SIZE];
    int curr_index;
} FileLockArr;

typedef struct {
    char command[CMD_INP_SZE];
    char path[PTH_INP_SZE]; 
    char abs_path[PTH_INP_SZE];
    char string[STR_INP_SZE];
} Task;

typedef struct {
    Task  *T;
    sem_t *main_lock;
    sem_t *f_lock;
    int   tid;
} Thread;

FileLockArr fla; /* Stores the lock unique to each file. */

/* Debugging functions */
void show_available_cpus(cpu_set_t *set, char *messg);
void check_sem_state(sem_t *lock, char *fpath);

/* Helper functions */
void get_input(Task *T);
void tokenize_filepath(char *filepath, char *dirpath, char *filename);
void append_char(char *string, char ch);
void get_time_str(char *tm);
void init_filelockarr(FileLockArr *fla);
void clean_up_filelockarr(FileLockArr *fla);
int  where_in_filelockarr(char *path, FileLockArr *fla);
void thread_receive_lock(Thread *td, int index);
int  file_access_sleep(void);
int  ms_sleep(long tms);
void empty_task_sleep(int tid);
void build_whole_transcript(char *whole_command, char *tm, Task *T);
void generate_readempty_output(Task *T, char *buf, int read_stat, char *read_out);

/* Worker thread functions */
void write_command(Thread *td);
void read_command(Thread *td);
void empty_command(Thread *td);

int main()
{
    /* Recieves errno. */
    int s; 

    char tm[TIMESTR_SZE];
    FILE *f;

    /* Initialize fla with the locks of read.txt, empty.txt, and commands.txt. */
    init_filelockarr(&fla);

    /* Set seed for rand(). */
    srand(time(0));

    /* Master thread and master cpu set. */
    pthread_t mthread = pthread_self();
    cpu_set_t mcpu_set;

    /* Assign CPU 0 to the master thread only. */
    CPU_ZERO(&mcpu_set);
    CPU_SET(0, &mcpu_set);
    s = pthread_setaffinity_np(mthread, sizeof(cpu_set_t), &mcpu_set);
    if (s != 0)
        handle_error_en(s, "master pthread_setaffinity_np");

    /* This is a unique identifier to each spawned thread. */
    int tid = 1; 

    /* Forever do the following sequence of tasks: recieve task from user, 
       spawn a worker thread to work on the task, and log the task transcript 
       to commands.txt. The worker thread needs to wait for the master thread
       finish writing in commands.txt before it can execute the command.      */
    while (1) {
        Thread *td = (Thread *) malloc(sizeof(Thread));
        td->T = (Task *) malloc(sizeof(Task));
        get_input(td->T);
        td->tid = tid;

        tid++;
        
        /* Store time when user input was received. */
        get_time_str(tm); 

        td->main_lock = (sem_t *) malloc(sizeof(sem_t));
        sem_init(td->main_lock, 0, 1);

        int index = where_in_filelockarr(td->T->abs_path, &fla);

        /* Give the lock of the target file to the thread. */
        thread_receive_lock(td, index);

        /* Worker thread and worker cpu set. */
        pthread_t wthread;
        cpu_set_t wcpu_set;

        /* Acquire lock of master thread. This is executed first before the 
           worker thread can do its thing.                                  */
        sem_wait(td->main_lock);

        /* Spawn worker thread to its specific command function. */
        char cmd = td->T->command[0];
        switch (cmd) {
            case 'w': {
                pthread_create(&wthread, NULL, (void *) write_command, td);
                break;
            } case 'r': {
                pthread_create(&wthread, NULL, (void *) read_command, td);
                break;
            } case 'e': {
                pthread_create(&wthread, NULL, (void *) empty_command, td);
                break;
            } default: {
                printf("Invalid command.\n");
                return -1;
            }
        }

        long i;
        long nproc = NUM_OF_PROCESSORS;

        /* Set all CPU cores available to the worker thread except
           core 0 since it is exclusive to the master thread.      */
        CPU_ZERO(&wcpu_set);
        for (i = 0; i < nproc; i++)
            CPU_SET(i, &wcpu_set);
        CPU_CLR(0, &wcpu_set);

        s = pthread_setaffinity_np(wthread, sizeof(cpu_set_t), &wcpu_set);
        if (s != 0) {
            handle_error_en(s, "worker pthread_setaffinity_np");
        }

        /* Acquire lock of commands.txt. */
        sem_wait(fla.filelock_arr[CMDS_LK]->f_lock);

        char whole_command[150];
        build_whole_transcript(whole_command, tm, td->T);

        /* Log task transcript to commands.txt */
        f = fopen("commands.txt", "a");
        int n = fwrite(whole_command, 1, strlen(whole_command), f);
        fclose(f);
        
        get_time_str(tm);
        printf("    [%s] TID: 0 | " GREEN "MASTER" RESET ": Finished writing %d character/s to commands.txt...\n", tm, n);

        /* Free the lock of commands.txt */
        sem_post(fla.filelock_arr[CMDS_LK]->f_lock);

        /* Free the lock of master thread. This allows its dispatched worker 
           thread to execute its assigned command. */
        sem_post(td->main_lock);
    }

    /* Free all the allocated file locks in fla. */
    clean_up_filelockarr(&fla);

    return 0;
}

/*------------------------Worker thread Functions----------------------------*/

void write_command(Thread *td)
{
    /* Acquire the lock of master thread. Only acquire after master thread is
       done setting up the CPU cores configuration of this thread and logging
       its command in command.txt                                             */
    sem_wait(td->main_lock);

    char tm[TIMESTR_SZE];
    
    int sleep_time = file_access_sleep();

    /* Acquire the lock of the file addressed by the filepath given to this 
       thread.                                                              */
    sem_wait(td->f_lock); 

    int n = 0;
    char x[STR_INP_SZE];
    FILE *f;

    strcpy(x, td->T->string);

    /* Append the string given to this thread to the file addressed by the
       filepath given to this thread. Create a new file if there is no
       existing file specified by the given filepath. Sleeps for 25 ms for 
       every character written to the file.                                */
    f = fopen(td->T->path, "a");

    int slen = strlen(x);

    ms_sleep(slen * 25);

    n = fwrite(x, 1, slen, f);


    
    get_time_str(tm);
    printf("    [%s] TID: %d | " YELLOW "WRITE:" RESET " Finished writing %d character/s to %s...\n", 
                                              tm, td->tid, n, td->T->abs_path);
    fclose(f);

    /* Free the lock of the file addressed by the filepath given to this 
       thread.                                                           */
    sem_post(td->f_lock);

    /* Free the lock of the master thread and delete it since the lock is only
       exclusive for this thread's session.                                    */
    sem_post(td->main_lock);
    sem_destroy(td->main_lock);

    /* Free the thread's allocated memory. */
    free(td->T);
    free(td);
}

void read_command(Thread *td)
{
    /* Acquire the lock of master thread. Only acquire after master thread is
       done setting up the CPU cores configuration of this thread and logging
       its command in command.txt                                             */
    sem_wait(td->main_lock);

    char tm[TIMESTR_SZE];

    /* This makes the thread sleeps in milliseconds. */
    int sleep_time = file_access_sleep();

    /* Acquire the lock of the file addressed by the filepath given to this 
       thread.                                                              */
    sem_wait(td->f_lock);

    int read_stat;
    char buf[MAX_BUF_SZE];
    char read_out[READEMPTY_OUT_SZE];

    FILE *f = fopen(td->T->path, "r");
    if (f != NULL) { 
        /* File exists, read its content. */
        int i ;
        i = fread(buf, 1, MAX_BUF_SZE, f);

        /* Null-terminate the string. */
        buf[i] = '\0';
        
        get_time_str(tm);
        printf("    [%s] TID: %d | " BLUE "READ:" RESET " Finished reading %d character/s from %s...\n", 
                                              tm, td->tid, i, td->T->abs_path);
        fclose(f);

        read_stat = 0;
    } else {
        get_time_str(tm);
        printf("    [%s] TID: %d | " BLUE "READ:" RESET " The file in %s does not exist...\n", 
                                              tm, td->tid, td->T->abs_path);    
        read_stat = FILE_NOT_FOUND;
    }

    /* Free the lock of the file addressed by the filepath given to this 
       thread.                                                           */
    sem_post(td->f_lock);

    /* Acquire the lock of read.txt. */
    sem_wait(fla.filelock_arr[READ_LK]->f_lock);
    
    generate_readempty_output(td->T, buf, read_stat, read_out);

    /* Append the string stored in read_out to read.txt. */
    f = fopen("read.txt", "a");
    int n = fwrite(&read_out, 1, strlen(read_out), f);
    
    get_time_str(tm);
    printf("    [%s] TID: %d | " BLUE "READ:" RESET " Finished writing %d character/s to read.txt...\n", 
                                                               tm, td->tid, n);
    fclose(f);

    /* Free the lock of read.txt. */
    sem_post(fla.filelock_arr[READ_LK]->f_lock);

    /* Free the lock of the master thread and delete it since the lock is only
       exclusive for this thread's session.                                    */    
    sem_post(td->main_lock);
    sem_destroy(td->main_lock);

    /* Free the thread's allocated memory. */
    free(td->T);
    free(td);
}

void empty_command(Thread *td)
{
    /* Acquire the lock of master thread. Only acquire after master thread is
       done setting up the CPU cores configuration of this thread and logging
       its command in command.txt                                             */
    sem_wait(td->main_lock);
    
    char tm[TIMESTR_SZE];
    
    /* This makes the thread sleeps in milliseconds. */
    int sleep_time = file_access_sleep();
    
    /* Acquire the lock of the file addressed by the filepath given to this 
       thread.                                                              */
    sem_wait(td->f_lock);

    int read_stat;
    char buf[MAX_BUF_SZE];
    char empty_out[READEMPTY_OUT_SZE];

    FILE *f = fopen(td->T->path, "r");
    if (f != NULL) { 
        /* File exists, read its content. */
        int n = fread(buf, 1, MAX_BUF_SZE, f);
        
        get_time_str(tm);
        printf("    [%s] TID: %d | " MAGENTA "EMPTY:" RESET " Finished reading %d character/s from %s...\n", 
                                              tm, td->tid, n, td->T->abs_path);
        fclose(f);

        /* fread does not automatically place null char at the end! */
        buf[n] = '\0';

        read_stat = 0;
    } else {
        get_time_str(tm);
        printf("    [%s] TID: %d | " MAGENTA "EMPTY:" RESET " The file with path %s does not exist...\n", 
                                                 tm, td->tid, td->T->abs_path);
        read_stat = FILE_NOT_FOUND;
    }
    
    /* Free the lock of the file addressed by the filepath given to this 
       thread.                                                           */
    sem_post(td->f_lock);

    /* Acquire the lock of empty.txt. */
    sem_wait(fla.filelock_arr[EMPTY_LK]->f_lock);

    generate_readempty_output(td->T, buf, read_stat, empty_out);

    /* Append the string stored in empty_out to empty.txt. */
    f = fopen("empty.txt", "a");
    int n = fwrite(&empty_out, 1, strlen(empty_out), f);
    
    get_time_str(tm);
    printf("    [%s] TID: %d | " MAGENTA "EMPTY:" RESET " Finished writing %d character/s to empty.txt...\n", 
                                                               tm, td->tid, n);
    fclose(f);

    /* Free the lock of empty.txt */
    sem_post(fla.filelock_arr[EMPTY_LK]->f_lock);

    /* Acquire the lock of the file addressed by the filepath given to this 
       thread. We access it again since we want to clear its content.       */
    sem_wait(td->f_lock);

    /* Empty the content of the file addressed by the path given to this thread 
       if it exists.                                                            */
    if (read_stat != FILE_NOT_FOUND) {
        f = fopen(td->T->path, "w");
        fclose(f);
        
        get_time_str(tm);
        printf("    [%s] TID: %d | " MAGENTA "EMPTY:" RESET " Finished emptying the content of %s...\n", 
                                                     tm, td->tid, td->T->abs_path);
    }

    /* Free the lock of the file addressed by the filepath given to this 
       thread.                                                           */
    sem_post(td->f_lock);

    /* Acquire the lock of empty.txt. */
    sem_wait(fla.filelock_arr[EMPTY_LK]->f_lock);

    empty_task_sleep(td->tid);

    /* Free the lock of empty.txt */
    sem_post(fla.filelock_arr[EMPTY_LK]->f_lock);

    /* Free the lock of the master thread and delete it since the lock is only
       exclusive for this thread's session.                                    */
    sem_post(td->main_lock);
    sem_destroy(td->main_lock);

    /* Free the thread's allocated memory. */
    free(td->T);
    free(td);
}

/*-------------------------Auxiliary Functions-------------------------------*/
void get_input(Task *T) 
{
    scanf("%s %s", T->command, T->path);

    /* Convert path input into an absolute path and store in
       T->abs_path.                                          */
    char *abs_path = realpath(T->path, NULL);
    if (abs_path == NULL) { 
        /* File DNE. */
        char dirpath[PTH_INP_SZE];
        char filename[FILENME_SZE];
        tokenize_filepath(T->path, dirpath, filename);

        abs_path = realpath(dirpath, NULL);
        strcpy(T->abs_path, abs_path);
        append_char(T->abs_path, '/');
        strcat(T->abs_path, filename);
    } else {
        strcpy(T->abs_path, abs_path);
    }
    free(abs_path);

    if (T->command[0] == 'w')
        /* remove unneeded whitespace */
        getchar();

        fgets(T->string, STR_INP_SZE, stdin);  

        /*  fgets automatically appends newline to string, 
            so we need to remove it.                       */
        T->string[strlen(T->string) - 1] = '\0';
}

void tokenize_filepath(char *filepath, char *dirpath, char *filename)
{
    /* Extract the left and right substring delimited by the rightmost '/' of 
       filepath.
       
       Ex: input  =  /hello/to/you.txt 
           output =  /hello/to and you.txt                                                     

                                                                              */
    int pos = -1;
    int i;
    for (i = 0; filepath[i] != '\0'; i++) {
        if (filepath[i] == '/') {
            pos = i;
        }
    }

    /* Extract dirpath. */
    char *dir;
    if (pos == -1) { 
        /* filepath only specifies the filename, so its dirpath must be the
           current directory.                                               */
        dir = realpath("./", NULL); 
        strcpy(dirpath, dir);
    } else {
        /* filepath contains '/', so extract left substring. */
        for (i = 0; i < pos; dirpath[i] = filepath[i], i++) ;
        dirpath[i] = '\0';

        dir = realpath(dirpath, NULL);
        strcpy(dirpath, dir);
    }
    free(dir);

    /* Extract filename. */
    int k;
    for (k = pos+1; filepath[k] != '\0'; filename[k-(pos+1)] = filepath[k], k++) ;
    filename[k-(pos+1)] = '\0';
}

void append_char(char *string, char ch) 
{
    int slen = strlen(string);

    string[slen] = ch;
    string[slen+1] = '\0';
}

void get_time_str(char *tm)
{
    time_t rawtime;
    struct tm *timeinfo;

    time(&rawtime);

    timeinfo = localtime(&rawtime);
    strftime(tm, TIMESTR_SZE,"%X",timeinfo);
}

void init_filelockarr(FileLockArr *fla)
{
    char *init_files[] = {"read.txt", "empty.txt", "commands.txt"};
    char *abs_path;
    FileLock *fl;
    FILE *f;

    /* Initialize the elements of fla to NULL. */
    int i;
    for (i = 0; i < FILELOCK_ARR_SIZE; i++)
        fla->filelock_arr[i] = NULL;
    fla->curr_index = -1; 

    /* Create empty files used by the commands and insert their file lock in
       fla.                                                                  */
    for (i = 0; i < 3; i++) {
        f = fopen(init_files[i], "w");
        fclose(f);
        
        fl = (FileLock *) malloc(sizeof(FileLock));
        
        abs_path = realpath(init_files[i], NULL);
        strcpy(fl->fpath, abs_path);
        free(abs_path);

        fl->f_lock = (sem_t *) malloc(sizeof(sem_t));
        sem_init(fl->f_lock, 0, 1);
        
        fla->curr_index++;
        fla->filelock_arr[fla->curr_index] = fl;
    }
}

void clean_up_filelockarr(FileLockArr *fla)
{
    int i;
    for (i = 0; i < FILELOCK_ARR_SIZE; i++) {
        if (fla->filelock_arr[i] != NULL) {
            free(fla->filelock_arr[i]->f_lock);
            free(fla->filelock_arr[i]);
        }
    }
}

int where_in_filelockarr(char *path, FileLockArr *fla)
{
    int i;
    for (i = 0; i <= fla->curr_index; i++) {
        if (strcmp(path, fla->filelock_arr[i]->fpath) == 0)
            return i;
    }
    /* path is not stored in fla. */
    return -1;
}

void thread_receive_lock(Thread *td, int index)
{
    if (index == -1) { 
        /* The given path has no unique lock yet in the fla, create a
           new one, store in the fla, and assign the new lock to the 
           worker thread.                                             */
        FileLock *new = (FileLock *) malloc(sizeof(FileLock));
        new->f_lock = (sem_t *) malloc(sizeof(sem_t));
        strcpy(new->fpath, td->T->abs_path);
        sem_init(new->f_lock, 0, 1);
        fla.curr_index++;
        fla.filelock_arr[fla.curr_index] = new;
        td->f_lock = new->f_lock;
    } else 
        td->f_lock = fla.filelock_arr[index]->f_lock;
}

int file_access_sleep(void) 
{
    int tm;
    int r = rand() % 100;

    tm = (r <= 79) ? 1 : 6;
    sleep(tm);

    return tm;
}

int ms_sleep(long ms) 
{
    struct timespec ts;
    int return_status;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;

    do {
        return_status = nanosleep(&ts, &ts);
    } while (return_status && errno == EINTR);

    return return_status;
}

void empty_task_sleep(int tid) 
{
    /* Sleep for about 7 to 10 seconds. Sleep time is float-type. */
    float min = 7.0, max = 10.0;

    /* Has a range of [0, 1.0] */
    float scale = rand() / (float) RAND_MAX;
    
    /* sleep_time has a value within the range [7.0, 10.0]. The reason for this 
       is due to the following: 
            1. if scale = 0 (when rand() = 0), then
              
                min + 0 * (max - min)
              = min = 7.0

            2. if scale = 1 (when rand() = RAND_MAX), then

                min + 1 * (max - min)
              = max = 10.0

            3. and if scale is between 0 and 1, then its value is between min
               and max which is (7.0, 10.0).
       Hence, when these cases are combined, sleep_time has a range of [7.0, 10.0]. */
    float sleep_time =  min + scale * (max - min); 

    int sleep_time_whole = 0;

    /* Extract the whole part of sleep_time and store to sleep_time_whole. */
    for (; sleep_time > 1.0; sleep_time--, sleep_time_whole++) ;

    /* Extract the fraction part of sleep_time and store to sleep_time_fraction. */
    long sleep_time_fraction = (long) (sleep_time * 1000);
    
    char tm[TIMESTR_SZE];

    sleep(sleep_time_whole);
    ms_sleep(sleep_time);

    get_time_str(tm);
    printf("    [%s] TID: %d | " MAGENTA "EMPTY:" RESET " Finished sleeping after %d.%li seconds...\n", 
                           tm, tid, sleep_time_whole, sleep_time_fraction);
}

void build_whole_transcript(char *whole_command, char *tm, Task *T)
{   /* [<time>] <command> <path> <string>\n */
    strcpy(whole_command, "[");
    strcat(whole_command, tm);
    strcat(whole_command, "] ");
    strcat(whole_command, T->command);
    append_char(whole_command, ' ');
    strcat(whole_command, T->path);
    if (T->command[0] == 'w') {
        append_char(whole_command, ' ');
        strcat(whole_command, T->string);
    }
    append_char(whole_command, '\n');
}

void generate_readempty_output(Task *T, char *buf, int read_stat, char *out)
{
    strcpy(out, T->command);
    append_char(out, ' ');
    strcat(out, T->path);
    strcat(out, ": ");
    if (read_stat == FILE_NOT_FOUND) {
        if (T->command[0] == 'r')
            strcat(out, "FILE DNE");
        else if (T->command[0] == 'e')
            strcat(out, "FILE ALREADY EMPTY");
    }
    else 
        strcat(out, buf);
    append_char(out, '\n');
}

/*-------------------------Debugging Functions-------------------------------*/

void show_available_cpus(cpu_set_t *set, char *messg)
{
    printf("%s ", messg);

    long i;
    long nproc = NUM_OF_PROCESSORS;
    for (i = 0; i < nproc; i++) {
        if (CPU_ISSET(i, set))
            printf("1 ");
        else
            printf("0 ");
    }
    printf("\n");
}

void check_sem_state(sem_t *lock, char *fpath)
{
    int val;
    sem_getvalue(lock, &val);
    printf("%s: lock val: %d\n", fpath, val);    
}
