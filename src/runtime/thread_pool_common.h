
namespace Halide { namespace Runtime { namespace Internal {

WEAK int num_threads;
WEAK bool thread_pool_initialized = false;

struct work {
    work *next_job;
    int (*f)(void *, int, uint8_t *);
    void *user_context;
    int next, max;
    uint8_t *closure;
    int active_workers;
    int exit_status;
    bool running() { return next < max || active_workers > 0; }
};

// The work queue and thread pool is weak, so one big work queue is shared by all halide functions
#define MAX_THREADS 64
struct work_queue_t {
    // all fields are protected by this mutex.
    halide_mutex mutex;

    // Singly linked list for job stack
    work *jobs;

    // Worker threads are divided into an 'A' team and a 'B' team. The
    // B team sleeps on the wakeup_b_team condition variable. The A
    // team does work. Threads transition to the B team if they wake
    // up and find that a_team_size > target_a_team_size.  Threads
    // move into the A team whenever they wake up and find that
    // a_team_size < target_a_team_size.
    int a_team_size, target_a_team_size;

    // Broadcast when a job completes.
    halide_cond wakeup_owners;

    // Broadcast whenever items are added to the work queue.
    halide_cond wakeup_a_team;

    // May also be broadcast when items are added to the work queue if
    // more threads are required than are currently in the A team.
    halide_cond wakeup_b_team;

    // Keep track of threads so they can be joined at shutdown
    halide_thread *threads[MAX_THREADS];

    // Global flag indicating
    bool shutdown;

    bool running() {
        return !shutdown;
    }

};
WEAK work_queue_t work_queue;

WEAK int default_do_task(void *user_context, halide_task_t f, int idx,
                        uint8_t *closure) {
    return f(user_context, idx, closure);
}

WEAK void worker_thread(void *void_arg) {
    work *owned_job = (work *)void_arg;

    // Grab the lock
    halide_mutex_lock(&work_queue.mutex);

    // If I'm a job owner, then I was the thread that called
    // do_par_for, and I should only stay in this function until my
    // job is complete. If I'm a lowly worker thread, I should stay in
    // this function as long as the work queue is running.
    while (owned_job != NULL ? owned_job->running()
           : work_queue.running()) {

        if (work_queue.jobs == NULL) {
            if (owned_job) {
                // There are no jobs pending. Wait for the last worker
                // to signal that the job is finished.
                halide_cond_wait(&work_queue.wakeup_owners, &work_queue.mutex);
            } else if (work_queue.a_team_size <= work_queue.target_a_team_size) {
                // There are no jobs pending. Wait until more jobs are enqueued.
                halide_cond_wait(&work_queue.wakeup_a_team, &work_queue.mutex);
            } else {
                // There are no jobs pending, and there are too many
                // threads in the A team. Transition to the B team
                // until the wakeup_b_team condition is fired.
                work_queue.a_team_size--;
                halide_cond_wait(&work_queue.wakeup_b_team, &work_queue.mutex);
                work_queue.a_team_size++;
            }
        } else {
            // Grab the next job.
            work *job = work_queue.jobs;

            // Claim a task from it.
            work myjob = *job;
            job->next++;

            // If there were no more tasks pending for this job,
            // remove it from the stack.
            if (job->next == job->max) {
                work_queue.jobs = job->next_job;
            }

            // Increment the active_worker count so that other threads
            // are aware that this job is still in progress even
            // though there are no outstanding tasks for it.
            job->active_workers++;

            // Release the lock and do the task.
            halide_mutex_unlock(&work_queue.mutex);
            int result = halide_do_task(myjob.user_context, myjob.f, myjob.next,
                                        myjob.closure);
            halide_mutex_lock(&work_queue.mutex);

            // If this task failed, set the exit status on the job.
            if (result) {
                job->exit_status = result;
            }

            // We are no longer active on this job
            job->active_workers--;

            // If the job is done and I'm not the owner of it, wake up
            // the owner.
            if (!job->running() && job != owned_job) {
                halide_cond_broadcast(&work_queue.wakeup_owners);
            }
        }
    }
    halide_mutex_unlock(&work_queue.mutex);
}

WEAK int default_do_par_for(void *user_context, halide_task_t f,
                            int min, int size, uint8_t *closure) {
    // Grab the lock. If it hasn't been initialized yet, then the
    // field will be zero-initialized because it's a static global.
    halide_mutex_lock(&work_queue.mutex);

    if (!thread_pool_initialized) {
        work_queue.shutdown = false;
        halide_cond_init(&work_queue.wakeup_owners);
        halide_cond_init(&work_queue.wakeup_a_team);
        halide_cond_init(&work_queue.wakeup_b_team);
        work_queue.jobs = NULL;

        if (!num_threads) {
            char *threads_str = getenv("HL_NUM_THREADS");
            if (!threads_str) {
                // Legacy name for HL_NUM_THREADS
                threads_str = getenv("HL_NUMTHREADS");
            }
            if (threads_str) {
                num_threads = atoi(threads_str);
            } else {
                num_threads = halide_host_cpu_count();
                // halide_printf(user_context, "HL_NUM_THREADS not defined. Defaulting to %d threads.\n", num_threads);
            }
        }
        if (num_threads > MAX_THREADS) {
            num_threads = MAX_THREADS;
        } else if (num_threads < 1) {
            num_threads = 1;
        }
        for (int i = 0; i < num_threads-1; i++) {
            //fprintf(stderr, "Creating thread %d\n", i);
            work_queue.threads[i] = halide_spawn_thread(worker_thread, NULL);
        }
        // Everyone starts on the a team.
        work_queue.a_team_size = num_threads;

        thread_pool_initialized = true;
    }

    // Make the job.
    work job;
    job.f = f;               // The job should call this function. It takes an index and a closure.
    job.user_context = user_context;
    job.next = min;          // Start at this index.
    job.max  = min + size;   // Keep going until one less than this index.
    job.closure = closure;   // Use this closure.
    job.exit_status = 0;     // The job hasn't failed yet
    job.active_workers = 0;  // Nobody is working on this yet

    if (!work_queue.jobs && size < num_threads) {
        // If there's no nested parallelism happening and there are
        // fewer tasks to do than threads, then set the target A team
        // size so that some threads will put themselves to sleep
        // until a larger job arrives.
        work_queue.target_a_team_size = size;
    } else {
        work_queue.target_a_team_size = num_threads;
    }

    // If there are more tasks than threads in the A team, we should
    // wake up everyone.
    bool wake_b_team = size > work_queue.a_team_size;

    // Push the job onto the stack.
    job.next_job = work_queue.jobs;
    work_queue.jobs = &job;

    halide_mutex_unlock(&work_queue.mutex);

    // Wake up our A team.
    halide_cond_broadcast(&work_queue.wakeup_a_team);

    if (wake_b_team) {
        // We need the B team too.
        halide_cond_broadcast(&work_queue.wakeup_b_team);
    }

    // Do some work myself.
    worker_thread((void *)(&job));

    // Return zero if the job succeeded, otherwise return the exit
    // status of one of the failing jobs (whichever one failed last).
    return job.exit_status;
}

WEAK halide_do_task_t custom_do_task = default_do_task;
WEAK halide_do_par_for_t custom_do_par_for = default_do_par_for;

}}} // namespace Halide::Runtime::Internal

using namespace Halide::Runtime::Internal;

extern "C" {

WEAK void halide_shutdown_thread_pool() {
    if (!thread_pool_initialized) return;

    // Wake everyone up and tell them the party's over and it's time
    // to go home
    halide_mutex_lock(&work_queue.mutex);
    work_queue.shutdown = true;
    halide_cond_broadcast(&work_queue.wakeup_owners);
    halide_cond_broadcast(&work_queue.wakeup_a_team);
    halide_cond_broadcast(&work_queue.wakeup_b_team);
    halide_mutex_unlock(&work_queue.mutex);

    // Wait until they leave
    for (int i = 0; i < num_threads-1; i++) {
        halide_join_thread(work_queue.threads[i]);
    }

    //fprintf(stderr, "All threads have quit. Destroying mutex and condition variable.\n");

    // Tidy up
    halide_mutex_destroy(&work_queue.mutex);
    halide_cond_destroy(&work_queue.wakeup_owners);
    halide_cond_destroy(&work_queue.wakeup_a_team);
    halide_cond_destroy(&work_queue.wakeup_b_team);
    thread_pool_initialized = false;
}

}
