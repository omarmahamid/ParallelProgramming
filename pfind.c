#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>


/*  Exit statuses   */
#define STATUS_SUCCESS  (void *) 0
#define STATUS_FAIL     (void *) 0

/*======================    Queue Implementation    ========================*/    

/*node for the queue*/
struct node {
    char *dir_path;
    struct node *next;
};
/*  Queue  */
struct queue {
    struct node *head;
    struct node *tail;
};
/*  get the first element of the queue and returns it */
struct node *dequeue(struct queue *queue_p)
{
    struct node *ret_node;
    ret_node = queue_p->head;
    queue_p->head = ret_node->next;
    if (queue_p->tail == ret_node){ /* One node corner case */
        queue_p->tail = NULL;
    }
    return ret_node;
}
/*  Returns 1 iff the queue is empty */
int is_empty(struct queue *queue_p)
{
    return (queue_p->head==NULL);
}/*  Adds node to the tail of the queue */
void enqueue(struct queue *queue_p, struct node *node_p)
{
    node_p->next = NULL;
    if (is_empty(queue_p)){ /*  Empty thread corner case */
        queue_p->head = queue_p->tail = node_p;
    } else {
        queue_p->tail->next = node_p;
        queue_p->tail = node_p;
    }
    return;
}
/*  Frees */
void free_nodes_in_queue(struct queue *queue_p)
{
    struct node *tmp;
    while (! is_empty(queue_p)){
        tmp = dequeue(queue_p);
        free(tmp->dir_path);
        free(tmp);
    }
    return;
}
/*  Initialize queue */
void init_queue(struct queue *queue_p)
{
    queue_p->head = NULL;
    queue_p->tail = NULL;
    return;
}

/*======================    End Queue Implementation    ========================*/   




/*=================    Definition variables    ====================*/

/*1)  Search term to be searched for  */
static char *search_term;

/*2)  Global counter for the number of found files and a corresponding lock */
static volatile int count;
static pthread_mutex_t count_mtx = PTHREAD_MUTEX_INITIALIZER;

/*3) Indicates to threads that they should stop working and exit cleanly */
static volatile int disabled;

/*4)  Interrupted flag gets toggled whenever received a SIGINT */
static volatile int interrupted;

/*5)  Counter for the number of idle threads - threads that went to sleep */
static volatile long idle_count;

/*6)  Counter for the number of threads that exited due to an error */
static volatile long err_count;

/*7)  Queue to hold the directories to be searched */
static struct queue dirs_queue;

/*8)  Lock for: disabled, idle_count and dirs_queue   */
static pthread_mutex_t main_mtx = PTHREAD_MUTEX_INITIALIZER;

/*9)  Condition variable used by finder threads and main thread to wake up finder threads */
static pthread_cond_t finder_up_cv = PTHREAD_COND_INITIALIZER;

/*10)  Condition variable used by finder threads to wake up the main thread to check if the
 *  program should be terminated    */
static pthread_cond_t father_up_cv = PTHREAD_COND_INITIALIZER;

/*=================   End Definition variables    ====================*/




/*==================    Threads Functions and Helper Functions  ==================*/


/*  Returns 1 if and only if file is a directory, -1 on error */
int is_directory(char *file_path)
{
    struct stat buf;
    if (stat(file_path, &buf)){
        return -1;
    }
    return S_ISDIR(buf.st_mode);   
}

/*  Returns 1 if and only if file matches the search term */
int file_matches(char *file_name)
{
    if(strstr(file_name, search_term)){
        return 1;
    } else {
        return 0;
    }
}

/*  Updates global counter  (2)*/
void post_local_count(int *local_count_p)
{
    pthread_mutex_lock(&count_mtx);
    count += *local_count_p;
    pthread_mutex_unlock(&count_mtx);
    *local_count_p = 0;
    return;
}

void *my_thread(void *t)
{
    int local_count, disabled_while_searching, rc;
    struct node *curr_node, *new_node;
    DIR *dir;
    struct dirent *ent;
    void *exit_status;
    char *reduced_file_name, *full_file_name;
    disabled_while_searching = 0;
    local_count = 0;
    exit_status = STATUS_SUCCESS;
    for(;;){    /*  Main loop*/
        pthread_mutex_lock(&main_mtx);
        idle_count++;
        while (!disabled && is_empty(&dirs_queue)){ /*  Sleep when not disabled and queue empty  */
            pthread_cond_signal(&father_up_cv); /*  Wakeup father when finder goes to sleep */
            pthread_cond_wait(&finder_up_cv, &main_mtx);    /* Go sleep  */
        }
        idle_count--;
        if (disabled){
            pthread_mutex_unlock(&main_mtx);
            break;
        } else {
            curr_node = dequeue(&dirs_queue);
            pthread_mutex_unlock(&main_mtx);
            errno = 0;
            dir = opendir(curr_node->dir_path);
            if (!dir) {
                perror("Error opening directory:");
                free(curr_node->dir_path);
                free(curr_node);
                exit_status = STATUS_FAIL;
                break;
            }
            while ( (ent = readdir(dir)) != NULL){
                if (disabled) { /*  Fast path out of finder  */  
                    disabled_while_searching = 1;
                    break;
                } else {
                    reduced_file_name = ent->d_name;
                    if (!strcmp(reduced_file_name, ".") || !strcmp(reduced_file_name, "..")){
                        /*  Ignoring current directory and parent directory  */
                        continue;
                    }
                    /*  Creating the full file name  */
                    full_file_name = (char *) calloc(   strlen(curr_node->dir_path) +
                                                        strlen(reduced_file_name)   +   2, sizeof(char));
                    if(!full_file_name){
                        fprintf(stderr, "Error: Couldn't allocate memory for file path!\n");
                        disabled_while_searching = 1;
                        exit_status = STATUS_FAIL;
                        break;
                    }
                    if(!strcat(full_file_name, curr_node->dir_path)){
                        fprintf(stderr, "Error: Couldn't concatenate file path!\n");
                        free(full_file_name);
                        disabled_while_searching = 1;
                        exit_status = STATUS_FAIL;
                        break;
                    }       
                    if(!strcat(full_file_name, "/")){
                        fprintf(stderr, "Error: Couldn't concatenate file path!\n");
                        free(full_file_name);
                        disabled_while_searching = 1;
                        exit_status = STATUS_FAIL;
                        break;
                    }
                    if (!strcat(full_file_name, reduced_file_name)){
                        fprintf(stderr, "Error: Couldn't concatenate file path!\n");
                        free(full_file_name);
                        disabled_while_searching = 1;
                        exit_status = STATUS_FAIL;
                        break;
                    }
                    /*  Here full_file_name contains the full path to the found file  */
                    /*  Managing the file */
                    rc = is_directory(full_file_name);
                    if (rc > 0){  
                        /*  Found directory  */
                        new_node = (struct node *) malloc(sizeof(struct node));
                        if(!new_node){
                            fprintf(stderr, "Error: Couldn't allocate memory for new node!");
                            free(full_file_name);
                            disabled_while_searching = 1;
                            exit_status = STATUS_FAIL;
                            break;
                        }
                        new_node->dir_path = full_file_name;
                        pthread_mutex_lock(&main_mtx);
                        enqueue(&dirs_queue, new_node);
                        pthread_mutex_unlock(&main_mtx);
                        pthread_cond_signal(&finder_up_cv);
                    } else if (rc == -1){  
                        /*  Error checking file type */
                        fprintf(stderr, "Error checking file type!\n");
                        free(full_file_name);
                        disabled_while_searching = 1;
                        exit_status = STATUS_FAIL;
                        break;
                    } else if (file_matches(reduced_file_name)) {    
                        /*  Found a file that matches the search term  */
                        printf("%s\n", full_file_name);
                        local_count++;
                        free(full_file_name);
                    } else {    
                        /*  File doesn't match  */
                        free(full_file_name);
                    }
                }
            }
            closedir(dir);
            free(curr_node->dir_path);
            free(curr_node);
            if (!ent && errno) {
                perror("Error getting entry from directory");
                exit_status = STATUS_FAIL;
                break;
            } 
            if (disabled_while_searching){
                break;
            }
        }
    }
    /*  Exiting */
    post_local_count(&local_count);
    pthread_mutex_lock(&main_mtx);
    idle_count++;
    if(exit_status == STATUS_FAIL)
        err_count++;
    pthread_mutex_unlock(&main_mtx);
    pthread_cond_signal(&father_up_cv);
    pthread_exit(exit_status);
}


/*==================    End Threads Functions and Helper Functions  ==================*/


/*==================    Main Thread Space    ======================*/

/*  SIGINT handler */
void interrupt_signal_handler(int signum)
{
    /* Interrupted flag, see printf at the end of the number of searched files */
    if (!disabled) {
        interrupted = 1;
    }
    /*  Wont use lock to toggle the disabled flag, not needed */
    disabled = 1;
    /*  Wakeup the main thread */
    pthread_cond_signal(&father_up_cv);
    /*  Broadcast to all children to wakeup */
    pthread_cond_broadcast(&finder_up_cv);
    return;
}

/*  Setup for SIGINT handler */
static int set_SIGINT_handler()
{
    struct sigaction new_action;
    memset(&new_action, 0, sizeof(new_action));
    new_action.sa_handler = interrupt_signal_handler;
    return sigaction(SIGINT, &new_action, NULL);
}

/*  Main thread */
int main(int argc, char *argv[])
{
    char *file_path, *root_search_dir;
    long num_threads, rc;
    long i;
    void *exit_status, *finder_exit_status;
    struct node *root_node;
    pthread_t *thread_ids;
    int len;
    /*  Parameters checking */
    if (argc < 4) {
        fprintf(stderr, "Error: No sufficient arguments!\n");
        pthread_exit(STATUS_FAIL);
    } else {
        file_path = argv[1];
        len = strlen(file_path);
        if (file_path[len-1] == '/')
            file_path[len-1] = '\0';
        search_term = argv[2];
        num_threads = (long) atoi(argv[3]);//assume that num_threads is greaeter than 0
    }
    init_queue(&dirs_queue);
    count = 0;
    interrupted = 0;
    disabled = 0;
    idle_count = 0;
    err_count = 0;
    exit_status = STATUS_SUCCESS;
    /* MUTEX and COND are initialized */
    thread_ids = (pthread_t *)malloc(num_threads*sizeof(pthread_t));
    if(!thread_ids){
        fprintf(stderr, "Error: Couldn't allocate memory for thread_ids array!");
        pthread_exit(STATUS_FAIL);
    }
    /*  Signal handling */
    if (set_SIGINT_handler()) {
        perror("Error encountered during setting SIGINT handler");
        pthread_exit(STATUS_FAIL);
    } 
    /*  Initial enqueue of root search directory */
    root_search_dir =  (char *)calloc(strlen(file_path)+1, sizeof(char));
    if(!root_search_dir){
        fprintf(stderr, "Error: Couldn't allocate memory for root search filepath!");
        pthread_exit(STATUS_FAIL);
    }
    if (root_search_dir != strcpy(root_search_dir, file_path)){
        fprintf(stderr, "Error: Couldn't copy file_path to newly allocated string!");
        pthread_exit(STATUS_FAIL);
    }
    root_node = (struct node *)malloc(sizeof(struct node));
    if(!root_node){
        fprintf(stderr, "Error: Couldn't allocate memory for root node!");
        pthread_exit(STATUS_FAIL);
    }
    root_node->dir_path = root_search_dir;
    enqueue(&dirs_queue, root_node);
    /*  Threads creation */
    for(i=0; i<num_threads; i++){
        rc = pthread_create( thread_ids + (int) i, NULL, my_thread, (void *)i);
        if (rc){
            fprintf(stderr, "Error creating thread: %s\n", strerror(rc));
            pthread_exit(STATUS_FAIL);
        }
    }
    /*  Waiting to end threads  */
    pthread_mutex_lock(&main_mtx);
    while (!(   (disabled)                                          /* Exit when disabled by interrupt  */
             || (is_empty(&dirs_queue) && idle_count==num_threads)  /* Exit when empty queue and all idle */
             || (err_count == num_threads) )){                      /* Exit when all threads exited due error */
        rc = pthread_cond_wait(&father_up_cv, &main_mtx);
        if (rc){
            fprintf(stderr, "Error waiting for father_up signal: %s\n", strerror(rc));
            pthread_exit(STATUS_FAIL);
        }
    }
    /*  Disable all threads and wake them to exit on their own  */
    disabled = 1;   
    rc = pthread_cond_broadcast(&finder_up_cv);
    if (rc){
        fprintf(stderr, "Error broadcasting to finder threads: %s\n", strerror(rc));
        pthread_exit(STATUS_FAIL);
    }
    pthread_mutex_unlock(&main_mtx);
    /*  Joining threads and computing return value  */
    for(i=0; i<num_threads; i++){
        rc = pthread_join( thread_ids[i], &finder_exit_status);
        if (rc) {
            fprintf(stderr, "Error joining thread: %s\n", strerror(rc));
            pthread_exit(STATUS_FAIL);
        }
        if (finder_exit_status){
            exit_status = finder_exit_status;
        }
    }
    /*  Printing the total count    */
    if (interrupted) {
        printf("Search stopped, found %d files.\n", count);
    } else {
        printf("Done searching, found %d files\n", count);
    }
    /*  Free state */
    free_nodes_in_queue(&dirs_queue);
    free(thread_ids);
    if (pthread_mutex_destroy(&count_mtx) || pthread_mutex_destroy(&main_mtx)){
        fprintf(stderr, "Error to destroy a lock!\n");
        pthread_exit(STATUS_FAIL);
    }
    if (pthread_cond_destroy(&finder_up_cv) || pthread_cond_destroy(&father_up_cv)){
        fprintf(stderr, "Error to destroy a condition variable!\n");
        pthread_exit(STATUS_FAIL);
    } 
    pthread_exit(exit_status);
}

/*==================   End Main Thread Space    ======================*/
