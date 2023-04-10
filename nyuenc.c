#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>

// Milestone 1: sequential RLE

// You will first implement nyuenc as a single-threaded program. 
// The encoder reads from one or more files specified as command-line arguments 
// and writes to STDOUT. 
// Thus, the typical usage of nyuenc would use shell redirection 
// to write the encoded output to a file.

// For example, a file with the following content:
//   aaaaaabbbbbbbbba
// would be encoded (logically, as the numbers would be in binary format instead of ASCII) as:
//   a6b9a1
// Note that the exact format of the encoded file is important. 
// You will store the character in ASCII and the count
// as a 1-byte unsigned integer in binary format. 

// $ echo -n "aaaaaabbbbbbbbba" > file.txt
// $ xxd file.txt
// 0000000: 6161 6161 6161 6262 6262 6262 6262 6261  aaaaaabbbbbbbbba
// $ ./nyuenc file.txt > file.enc
// $ xxd file.enc
// 0000000: 6106 6209 6101                           a.b.a.

// You may have used read() or fread() before, 
// but one particularly efficient way is to use mmap(), 
// which maps a file into the memory address space. 
// Then, you can efficiently access each byte of the input file via pointers. 
// Read its man page and example.

// Store your data as a char[] or unsigned char[] and use write() or fwrite() 
// to write them to STDOUT. 
// Don’t use printf() or attempt to convert them to any human-readable format.

// Also keep in mind that you should never use string functions 
// (e.g., strcpy() or strlen()) on binary data as they may contain null bytes.



// Milestone 2: parallel RLE

// Next, you will parallelize the encoding using POSIX threads. 
// In particular, you will implement a thread pool for executing encoding tasks.

// You should use mutexes, condition variables, or semaphores 
// to realize proper synchronization among threads.

//Your nyuenc will take an optional command-line option -j jobs, 
// which specifies the number of worker threads. 
// (If no such option is provided, it runs sequentially.)

// For example:

// $ time ./nyuenc file.txt > /dev/null
// real    0m0.527s
// user    0m0.475s
// sys     0m0.233s
// $ time ./nyuenc -j 3 file.txt > /dev/null
// real    0m0.191s
// user    0m0.443s
// sys     0m0.179s

int handle_error(char *error) {
    perror(error);
    exit(1);
}

// create the task struct
struct task {
    char *addr;
    size_t size;
    int file_index;
    int section_index;
    int is_last_section;
    int chunk_index;
    struct task *next;
};

typedef struct task Task;

// create the task queue struct to hold the task structs
struct task_queue {
    struct task *head;
    struct task *tail;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
};

typedef struct task_queue TaskQueue;

// create the struct of the outputs
struct output {
    char *addr;
    size_t size;
    int file_index;
    int section_index;
    int is_last_section;
    int chunk_index;
    struct output *next;
};

typedef struct output Output;

// create the output queue struct
struct output_queue {
    struct output *head;
    struct output *tail;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
};

typedef struct output_queue OutputQueue;

// create the struct to pass argments to the threads
struct thread_args {
    int thread_id;
    TaskQueue *task_queue;
    OutputQueue *output_queue;
};

// create a struct to store outputs in a linked list
struct output_list {
    struct output *head;
};

// create the function to add outputs to the list, in the order of increasing output->file_index and increasing output->section_index
void add_output_to_list(struct output_list *list, struct output *output) {
    output->next = NULL;

    if (list->head == NULL) {
        list->head = output;
    } else {
        struct output *curr = list->head;
        struct output *prev = NULL;
        while (curr != NULL) {
            if (output->file_index < curr->file_index) {
                break;
            } else if (output->file_index == curr->file_index) {
                if (output->section_index < curr->section_index) {
                    break;
                }
            }
            prev = curr;
            curr = curr->next;
        }
        if (prev == NULL) {
            output->next = list->head;
            list->head = output;
        } else {
            output->next = curr;
            prev->next = output;
        }
    }
}



// create the function to add tasks to the queue
void add_task(struct task_queue *queue, struct task *task) {
    if (queue->tail != NULL) {
        queue->tail->next = task;
    }
    queue->tail = task;
    if (queue->head == NULL) {
        queue->head = task;
    }
}

// create the function to get tasks from the queue
struct task *get_task(struct task_queue *queue) {
    struct task *task = queue->head;
    if (task != NULL) {
        queue->head = task->next;
        if (queue->head == NULL) {
            queue->tail = NULL;
        }
    }
    task->next = NULL;
    return task;
}

// create the function to add outputs to the queue
void add_output(struct output_queue *queue, struct output *output) {
    pthread_mutex_lock(&queue->mutex);

    if (queue->tail != NULL) {
        queue->tail->next = output;
    }
    queue->tail = output;
    if (queue->head == NULL) {
        queue->head = output;
        pthread_cond_signal(&queue->not_empty); // adjust position for speed 
    }
    queue->tail->next = NULL;
    pthread_mutex_unlock(&queue->mutex);
}

// create the function to get outputs from the queue
struct output *get_output(struct output_queue *queue) {
    struct output *output = queue->head;
    if (output != NULL) {
        queue->head = output->next;
        if (queue->head == NULL) {
            queue->tail = NULL;
        }
    }
    output->next = NULL;
    return output;
}

// create the function to get output with index from the queue
struct output *get_output_with_index(struct output_queue *queue, int index) {
    pthread_mutex_lock(&queue->mutex);
    // printf("get_output_with_index: %d\n", index);
    while (queue->head == NULL) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    struct output *output = queue->head;
    // if (queue->head == NULL){
    //     printf("queue->head is NULL\n");
    // } else {
    //     printf("queue->head: %d-%d, %d\n", queue->head->file_index, queue->head->section_index, queue->head->chunk_index);
    //     printf("head index: %d\n", output->chunk_index);
    // }
    struct output *prev = NULL;
    // int round = 0; //delete for speed
    while (output != NULL) {
        if (output->chunk_index == index) {
            if (prev == NULL) {
                queue->head = output->next;
            } else {
                prev->next = output->next;
            }
            if (queue->tail == output) {
                queue->tail = prev;
            }

            output->next = NULL;
            break;
        }
        // round ++; //delete for speed
        prev = output;
        output = output->next;
    }
    if (queue->tail){
        queue->tail->next = NULL;
    }

    pthread_mutex_unlock(&queue->mutex);
    // if (output == NULL) {
    //     printf("output is NULL\n");
    // } else {
    //     printf("Found: output_index: %d-%d, %d\n", index, output->file_index, output->section_index, output->chunk_index);
    // }
    return output;
}
        

int main(int argc, char **argv) {

    // use getopt to parse command line arguments to check for -j and jobs
    // if -j is present, then run in parallel mode
    // if -j is not present, then run in sequential mode
    int opt;
    int jobs = 0;
    while ((opt = getopt(argc, argv, "j:")) != -1) {
        if (opt == 'j') {
            jobs = atoi(optarg);
            // printf("jobs: %d\n",jobs);
        }
    }
    // // printf("jobs: %d\n", jobs);

    if (jobs > 1) {
        // printf("parallel threads mode\n");
        
        // printf("jobs: %d\n", jobs);

        // At the beginning of your program, 
        // you should create a pool of worker threads 

        // The main thread should divide the input data logically 
        // into fixed-size 4KB (i.e., 4,096-byte) chunks 
        // and submit the tasks (the blue circles in the figure above) to the task queue, 
        // where each task would encode a chunk. 
        // Whenever a worker thread becomes available, 
        // it would execute the next task in the task queue. 
        // (Note: it’s okay if the last chunk of a file is smaller than 4KB.)

        // For simplicity, you can assume that the task queue is unbounded. 
        // In other words, you can submit all tasks at once without being blocked.

        // After submitting all tasks, the main thread should collect the results 
        // (the yellow circles in the figure above) and write them to STDOUT. 
        // Note that you may need to stitch the chunk boundaries. 
        // For example, if the previous chunk ends with aaaaa, 
        // and the next chunk starts with aaa, 
        // instead of writing a5a3, you should write a8.

        // It is important that you synchronize the threads properly 
        // so that there are no deadlocks or race conditions. 
        // In particular, there are two things that you need to consider carefully:
        // 1. The worker thread should wait until there is a task to do.
        // 2. The main thread should wait until a task has been completed  
        //     so that it can collect the result. 
        //     Keep in mind that the tasks might not complete in the same order 
        //     as they were submitted.

        // initialize the task queue
        TaskQueue *TQ = malloc(sizeof(TaskQueue));
        TQ->head = NULL;
        TQ->tail = NULL;
        TQ->mutex = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
        TQ->not_empty = (pthread_cond_t)PTHREAD_COND_INITIALIZER;

        // initialize the output queue
        OutputQueue *OQ = malloc(sizeof(OutputQueue));
        OQ->head = NULL;
        OQ->tail = NULL;
        OQ->mutex = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
        OQ->not_empty = (pthread_cond_t)PTHREAD_COND_INITIALIZER;

    
        // function for running the threads
        void *run_thread(void *args) {
            struct thread_args *thread_args = args;
            int thread_id = thread_args->thread_id;
            TaskQueue *task_queue = thread_args->task_queue;
            OutputQueue *output_queue = thread_args->output_queue;
            free(args);

            // printf("thread %d started.\n", thread_id);

            // get the task from the task queue
            while (1) {
                pthread_mutex_lock(&task_queue->mutex);
                while (task_queue->head == NULL) {
                    pthread_cond_wait(&task_queue->not_empty, &task_queue->mutex);
                }
                Task *task = get_task(task_queue);
                pthread_mutex_unlock(&task_queue->mutex);

                if (task == NULL || (task->addr == NULL && task->size == 0)) {
                    // printf("thread %d got NULL task.\n", thread_id);
                    pthread_exit(NULL);
                    break;
                }

                // printf("thread %d got task %d-%d\n", thread_id, task->file_index, task->section_index);
                // fflush(stdout);
                // write(STDOUT_FILENO, task->addr, task->size);
                // printf("\n");
                // fflush(stdout);

                // compress the task->addr
                Output *output = malloc(sizeof(Output));
                char* buffer = malloc(8192);
                int buffer_id = 0;
                int count = 0;
                char last_char, current_char;
                for (int i = 0; i < task->size; i++) {
                    current_char = task->addr[i];
                    if (i==0){
                        last_char = task->addr[i];
                        count = 1;
                    } else if (task->addr[i] == last_char) {
                        count++;
                    } else {
                        buffer[buffer_id] = last_char;
                        buffer_id++;
                        buffer[buffer_id] = count;
                        buffer_id++;
                        last_char = current_char;
                        count = 1;
                    }
                }
                buffer[buffer_id] = last_char;
                buffer_id++;
                buffer[buffer_id] = count;
                buffer_id++;
                output->addr = buffer;
                output->size = buffer_id;
                output->file_index = task->file_index;
                output->is_last_section = task->is_last_section;
                output->section_index = task->section_index;
                output->chunk_index = task->chunk_index;
                

                // printf("thread %d encoded task %d-%d\n", thread_id, output->file_index, output->section_index);
                // fflush(stdout);
                // write(STDOUT_FILENO, output->addr, output->size);
                // printf("\n");
                // fflush(stdout);

                // add the encoded task to the output queue
                add_output(output_queue, output);               
            }

            // printf("thread %d finished.\n", thread_id);
            pthread_exit(NULL);
        }

        // create the thread pool
        pthread_t *thread_pool = malloc(jobs * sizeof(pthread_t));
        for (int i = 0; i < jobs; i++) {
            struct thread_args *args = malloc(sizeof(struct thread_args));
            args->thread_id = i;
            args->task_queue = TQ;
            args->output_queue = OQ;
            pthread_create(&thread_pool[i], NULL, run_thread, args);
        }



        // divide the input file into fixed-size 4KB chunks and add to task queue 
        // using mmap to get the address and than copy the address into the task struct
        int chunk_count = 0;
        int file_count = 0;
        int file_section_index = 0;
        
        // create the array of pointers to the output for each chunk in each file
        Output ***output_array = malloc((argc-3) * sizeof(Output**));

        for (int i = 3; i < argc; i++){
            // Open file
            int fd = open(argv[i], O_RDONLY);
            if (fd == -1)
            handle_error("File error");

            // Get file size
            struct stat sb;
            if (fstat(fd, &sb) == -1)
            handle_error("File stat error");

            // Map file into memory
            char *addr = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (addr == MAP_FAILED)
            handle_error("Map error");

            // Close file
            if (close(fd) == -1)
            handle_error("Close error");

            // Add tasks to the queue
            file_section_index = 0;
            for (int j = 0; j < sb.st_size; j += 4096) {
                struct task *task = malloc(sizeof(struct task));

                int size = 4096;
                if (sb.st_size - j <= 4096) {
                    size = sb.st_size - j;
                    task->is_last_section = 1;
                } else {
                    task->is_last_section = 0;
                }

                task->addr = addr + j;
                task->size = size;
                task->file_index = file_count;
                task->section_index = file_section_index;
                task->chunk_index = chunk_count;
                task->next = NULL;

                pthread_mutex_lock(&TQ->mutex);
                add_task(TQ, task);
                pthread_cond_signal(&TQ->not_empty);
                pthread_mutex_unlock(&TQ->mutex);

                file_section_index++;
                chunk_count++;
            }
            output_array[file_count] = malloc(file_section_index * sizeof(Output*));

            file_count++;
        }

        // printf("All tasks added.\n");

        // add NULL tasks to the queue to signal the threads to stop
        for (int i = 0; i < jobs; i++) {
            struct task *task = malloc(sizeof(struct task));
            task->addr = NULL;
            task->size = 0;
            task->file_index = 0;
            task->section_index = 0;
            task->is_last_section = 1;
            task->chunk_index = 0;
            task->next = NULL;

            pthread_mutex_lock(&TQ->mutex);
            add_task(TQ, task);
            pthread_cond_signal(&TQ->not_empty);
            pthread_mutex_unlock(&TQ->mutex);
        }

        // printf("NULL tasks added.\n");

        // initialize the list of outputs
        // struct output_list *output_list = malloc(chunk_count * sizeof(struct output_list));

        // process the outputs
        
        Output *curr = NULL;
        Output *prev = NULL;
        char prev_last_letter;
        int prev_last_count;
        char curr_first_letter;
        int curr_first_count;

        // printf("Processing outputs...\n");
        // printf("chunk_count: %d\n", chunk_count);

        for (int i = 0; i < chunk_count; i++) {

            struct output *output = get_output_with_index(OQ, i);

            while (output == NULL) {
                // printf("output is NULL, index: %d\n", i);
                output = get_output_with_index(OQ, i);
            } 
            // if (output != NULL) {
            //     printf("Received output %d-%d, chunk: %d\n", output->file_index, output->section_index, output->chunk_index);
            // }

            curr = output;
            if (prev != NULL) {
                prev_last_letter = prev->addr[prev->size - 2];
                prev_last_count = prev->addr[prev->size - 1];
                if (prev_last_count < 0) {
                    prev_last_count += 256;
                }
                curr_first_letter = curr->addr[0];
                curr_first_count = curr->addr[1];
                if (curr_first_count < 0) {
                    curr_first_count += 256;
                }
                if (prev_last_letter == curr_first_letter) {
                    prev->size -= 2;
                    curr->addr[1] = prev_last_count + curr_first_count; // maybe adjust for speed?
                }
                write(STDOUT_FILENO, prev->addr, prev->size);
            }
            prev = curr;
        }
        write(STDOUT_FILENO, curr->addr, curr->size);

        // //print the list of outputs
        // Output *curr;
        // Output *next;
        // char this_last_letter;
        // int this_last_count;
        // char next_first_letter;
        // int next_first_count;

        // int j;
        // for (int i = 0; i<file_count; i++) {
        //     j = 0;
        //     while (1){
        //         curr = output_array[i][j];
        //         if (curr->is_last_section == 0) {
        //             next = output_array[i][j+1];
        //         } else if (i < file_count - 1) {
        //             next = output_array[i+1][0];
        //         } else {
        //             next = NULL;
        //         }
        //         if (next != NULL) {
        //             this_last_letter = curr->addr[curr->size - 2];
        //             this_last_count = curr->addr[curr->size - 1];
        //             if (this_last_count < 0) {
        //                 this_last_count += 256;
        //             }
        //             next_first_letter = next->addr[0];
        //             next_first_count = next->addr[1];
        //             if (next_first_count < 0) {
        //                 next_first_count += 256;
        //             }
        //             // printf("\tthis_last_letter: %c, this_last_count: %d, next_first_letter: %c, next_first_count: %d\n", this_last_letter, this_last_count, next_first_letter, next_first_count);

        //             if (this_last_letter == next_first_letter) {
        //                 // printf("\t\tmerge\n");
        //                 curr->size -= 2;
        //                 next->addr[1] += this_last_count;
        //             }
        //         }
        //         write(STDOUT_FILENO, curr->addr, curr->size);
        //         j++;
        //         if (curr->is_last_section == 1) {
        //             break;
        //         }
        //     } 


        // }
    


        // struct output *curr = output_list->head;
        // struct output *next;
        // char this_last_letter;
        // int this_last_count;
        // char next_first_letter;
        // int next_first_count;

        // while (curr != NULL) {
        //     next = curr->next;
        //     // printf("file index: %d, section index: %d\n", curr->file_index, curr->section_index);
        //     if (next != NULL) {
        //         this_last_letter = curr->addr[curr->size - 2];
        //         this_last_count = curr->addr[curr->size - 1];
        //         if (this_last_count < 0) {
        //             this_last_count += 256;
        //         }
        //         next_first_letter = next->addr[0];
        //         next_first_count = next->addr[1];
        //         if (next_first_count < 0) {
        //             next_first_count += 256;
        //         }
        //         // printf("\tthis_last_letter: %c, this_last_count: %d, next_first_letter: %c, next_first_count: %d\n", this_last_letter, this_last_count, next_first_letter, next_first_count);

        //         if (this_last_letter == next_first_letter) {
        //             // printf("\t\tmerge\n");
        //             curr->size -= 2;
        //             next->addr[1] += this_last_count;
        //         }
            
        //     } 
        //     // printf("Should print: \t");
        //     // fflush(stdout);
        //     write(STDOUT_FILENO, curr->addr, curr->size);
        //     // printf("\n");
        //     // fflush(stdout);

        //     curr = curr->next;
        // }
        // printf("All outputs processed.\n");

        // wait for all threads to be completed
        for (int i = 0; i < jobs; i++) {
            pthread_join(thread_pool[i], NULL);
            // printf("thread %d finished.\n", i);
        }
        free(thread_pool);
        free(TQ);
        free(OQ);

    } else {
        // printf("single thread mode\n");

        char prev = 0;
        char curr = 0;
        char *buffer = malloc(65535);
        int n = 0, count = 0, id = 0;

        int start = 1;
        if (jobs == 1){
            start = 3;
        }

        for (int i = start; i < argc; i++){
            // printf("File name: %s\n", argv[i]);

            // Open file
            int fd = open(argv[i], O_RDONLY);
            if (fd == -1)
            handle_error("File error");

            // Get file size
            struct stat sb;
            if (fstat(fd, &sb) == -1)
            handle_error("File stat error");

            // Map file into memory
            char *addr = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (addr == MAP_FAILED)
            handle_error("Map error");

            // Close file
            if (close(fd) == -1)
            handle_error("Close error");

            // printf("%d\n", sb.st_size);
            // printf("%s\n", addr);
            n = 0;

            while (n < sb.st_size) {
                curr = addr[n];
                // printf("n: %d, curr: %c, count: %d, prev: %c\n", n, curr, count, prev);
                if (count == 0){
                    count = 1;
                    prev = addr[n];
                } else if (curr == prev) {
                    count++;
                } else {
                    // printf("%d, %c%c\n", n, prev, curr);
                    buffer[id] = prev;
                    buffer[id+1] = count;
                    count = 1;
                    prev = curr;
                    id += 2;
                }
                n++;
            }
        }
        buffer[id] = prev;
        buffer[id+1] = count;
        count = 1;
        prev = curr;
        id += 2;
        // printf("id: %d, prev: %c, count: %d\n", id, prev, count);

        write(STDOUT_FILENO, buffer, id);
        // printf("%s",buffer);
        free(buffer);
    }
}
