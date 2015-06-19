/*
 * ring buffer tester and benchmark
 *
 * Copyright (C) 2009 Steven Rostedt <srostedt@redhat.com>
 */
#include <linux/ring_buffer.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/ktime.h>
#include <asm/local.h>

struct rb_page {
	u64		ts;
	local_t		commit;
	char		data[4080];
};

/* run time and sleep time in seconds */
#define RUN_TIME	10ULL
#define SLEEP_TIME	10

/* number of events for writer to wake up the reader */
static int wakeup_interval = 100;

static int reader_finish;
static DECLARE_COMPLETION(read_start);
static DECLARE_COMPLETION(read_done);
static struct ring_buffer *buffer;

static void rb_producer_hammer_func(struct kthread_work *dummy);
static struct kthread_worker *rb_producer_worker;
static DEFINE_DELAYED_KTHREAD_WORK(rb_producer_hammer_work,
				   rb_producer_hammer_func);

static void rb_consumer_func(struct kthread_work *dummy);
static struct kthread_worker *rb_consumer_worker;
static DEFINE_KTHREAD_WORK(rb_consumer_work, rb_consumer_func);

static unsigned long read;

static unsigned int disable_reader;
module_param(disable_reader, uint, 0644);
MODULE_PARM_DESC(disable_reader, "only run producer");

static unsigned int write_iteration = 50;
module_param(write_iteration, uint, 0644);
MODULE_PARM_DESC(write_iteration, "# of writes between timestamp readings");

static int producer_nice = MAX_NICE;
static int consumer_nice = MAX_NICE;

static int producer_fifo = -1;
static int consumer_fifo = -1;

module_param(producer_nice, int, 0644);
MODULE_PARM_DESC(producer_nice, "nice prio for producer");

module_param(consumer_nice, int, 0644);
MODULE_PARM_DESC(consumer_nice, "nice prio for consumer");

module_param(producer_fifo, int, 0644);
MODULE_PARM_DESC(producer_fifo, "fifo prio for producer");

module_param(consumer_fifo, int, 0644);
MODULE_PARM_DESC(consumer_fifo, "fifo prio for consumer");

static int read_events;

static int test_error;
static int test_end;

#define TEST_ERROR()				\
	do {					\
		if (!test_error) {		\
			test_error = 1;		\
			WARN_ON(1);		\
		}				\
	} while (0)

enum event_status {
	EVENT_FOUND,
	EVENT_DROPPED,
};

static bool break_test(void)
{
	return test_error || test_end;
}

static enum event_status read_event(int cpu)
{
	struct ring_buffer_event *event;
	int *entry;
	u64 ts;

	event = ring_buffer_consume(buffer, cpu, &ts, NULL);
	if (!event)
		return EVENT_DROPPED;

	entry = ring_buffer_event_data(event);
	if (*entry != cpu) {
		TEST_ERROR();
		return EVENT_DROPPED;
	}

	read++;
	return EVENT_FOUND;
}

static enum event_status read_page(int cpu)
{
	struct ring_buffer_event *event;
	struct rb_page *rpage;
	unsigned long commit;
	void *bpage;
	int *entry;
	int ret;
	int inc;
	int i;

	bpage = ring_buffer_alloc_read_page(buffer, cpu);
	if (!bpage)
		return EVENT_DROPPED;

	ret = ring_buffer_read_page(buffer, &bpage, PAGE_SIZE, cpu, 1);
	if (ret >= 0) {
		rpage = bpage;
		/* The commit may have missed event flags set, clear them */
		commit = local_read(&rpage->commit) & 0xfffff;
		for (i = 0; i < commit && !test_error ; i += inc) {

			if (i >= (PAGE_SIZE - offsetof(struct rb_page, data))) {
				TEST_ERROR();
				break;
			}

			inc = -1;
			event = (void *)&rpage->data[i];
			switch (event->type_len) {
			case RINGBUF_TYPE_PADDING:
				/* failed writes may be discarded events */
				if (!event->time_delta)
					TEST_ERROR();
				inc = event->array[0] + 4;
				break;
			case RINGBUF_TYPE_TIME_EXTEND:
				inc = 8;
				break;
			case 0:
				entry = ring_buffer_event_data(event);
				if (*entry != cpu) {
					TEST_ERROR();
					break;
				}
				read++;
				if (!event->array[0]) {
					TEST_ERROR();
					break;
				}
				inc = event->array[0] + 4;
				break;
			default:
				entry = ring_buffer_event_data(event);
				if (*entry != cpu) {
					TEST_ERROR();
					break;
				}
				read++;
				inc = ((event->type_len + 1) * 4);
			}
			if (test_error)
				break;

			if (inc <= 0) {
				TEST_ERROR();
				break;
			}
		}
	}
	ring_buffer_free_read_page(buffer, bpage);

	if (ret < 0)
		return EVENT_DROPPED;
	return EVENT_FOUND;
}

static void ring_buffer_consumer(void)
{
	/* toggle between reading pages and events */
	read_events ^= 1;

	read = 0;
	/*
	 * Continue running until the producer specifically asks to stop
	 * and is ready for the completion.
	 */
	while (!READ_ONCE(reader_finish)) {
		int found = 1;

		while (found && !test_error) {
			int cpu;

			found = 0;
			for_each_online_cpu(cpu) {
				enum event_status stat;

				if (read_events)
					stat = read_event(cpu);
				else
					stat = read_page(cpu);

				if (test_error)
					break;

				if (stat == EVENT_FOUND)
					found = 1;

			}
		}

		/* Wait till the producer wakes us up when there is more data
		 * available or when the producer wants us to finish reading.
		 */
		set_current_state(TASK_INTERRUPTIBLE);
		if (reader_finish)
			break;

		schedule();
	}
	__set_current_state(TASK_RUNNING);
	reader_finish = 0;
	complete(&read_done);
}

static void ring_buffer_producer(void)
{
	ktime_t start_time, end_time, timeout;
	unsigned long long time;
	unsigned long long entries;
	unsigned long long overruns;
	unsigned long missed = 0;
	unsigned long hit = 0;
	unsigned long avg;
	int cnt = 0;

	/*
	 * Hammer the buffer for 10 secs (this may
	 * make the system stall)
	 */
	trace_printk("Starting ring buffer hammer\n");
	start_time = ktime_get();
	timeout = ktime_add_ns(start_time, RUN_TIME * NSEC_PER_SEC);
	do {
		struct ring_buffer_event *event;
		int *entry;
		int i;

		for (i = 0; i < write_iteration; i++) {
			event = ring_buffer_lock_reserve(buffer, 10);
			if (!event) {
				missed++;
			} else {
				hit++;
				entry = ring_buffer_event_data(event);
				*entry = smp_processor_id();
				ring_buffer_unlock_commit(buffer, event);
			}
		}
		end_time = ktime_get();

		cnt++;
		if (rb_consumer_worker && !(cnt % wakeup_interval))
			wake_up_process(rb_consumer_worker->task);

#ifndef CONFIG_PREEMPT
		/*
		 * If we are a non preempt kernel, the 10 second run will
		 * stop everything while it runs. Instead, we will call
		 * cond_resched and also add any time that was lost by a
		 * rescedule.
		 *
		 * Do a cond resched at the same frequency we would wake up
		 * the reader.
		 */
		if (cnt % wakeup_interval)
			cond_resched();
#endif
	} while (ktime_before(end_time, timeout) && !break_test());
	trace_printk("End ring buffer hammer\n");

	if (rb_consumer_worker) {
		/* Init both completions here to avoid races */
		init_completion(&read_start);
		init_completion(&read_done);
		/* the completions must be visible before the finish var */
		smp_wmb();
		reader_finish = 1;
		wake_up_process(rb_consumer_worker->task);
		wait_for_completion(&read_done);
	}

	time = ktime_us_delta(end_time, start_time);

	entries = ring_buffer_entries(buffer);
	overruns = ring_buffer_overruns(buffer);

	if (test_error)
		trace_printk("ERROR!\n");

	if (!disable_reader) {
		if (consumer_fifo < 0)
			trace_printk("Running Consumer at nice: %d\n",
				     consumer_nice);
		else
			trace_printk("Running Consumer at SCHED_FIFO %d\n",
				     consumer_fifo);
	}
	if (producer_fifo < 0)
		trace_printk("Running Producer at nice: %d\n",
			     producer_nice);
	else
		trace_printk("Running Producer at SCHED_FIFO %d\n",
			     producer_fifo);

	/* Let the user know that the test is running at low priority */
	if (producer_fifo < 0 && consumer_fifo < 0 &&
	    producer_nice == MAX_NICE && consumer_nice == MAX_NICE)
		trace_printk("WARNING!!! This test is running at lowest priority.\n");

	trace_printk("Time:     %lld (usecs)\n", time);
	trace_printk("Overruns: %lld\n", overruns);
	if (disable_reader)
		trace_printk("Read:     (reader disabled)\n");
	else
		trace_printk("Read:     %ld  (by %s)\n", read,
			read_events ? "events" : "pages");
	trace_printk("Entries:  %lld\n", entries);
	trace_printk("Total:    %lld\n", entries + overruns + read);
	trace_printk("Missed:   %ld\n", missed);
	trace_printk("Hit:      %ld\n", hit);

	/* Convert time from usecs to millisecs */
	do_div(time, USEC_PER_MSEC);
	if (time)
		hit /= (long)time;
	else
		trace_printk("TIME IS ZERO??\n");

	trace_printk("Entries per millisec: %ld\n", hit);

	if (hit) {
		/* Calculate the average time in nanosecs */
		avg = NSEC_PER_MSEC / hit;
		trace_printk("%ld ns per entry\n", avg);
	}

	if (missed) {
		if (time)
			missed /= (long)time;

		trace_printk("Total iterations per millisec: %ld\n",
			     hit + missed);

		/* it is possible that hit + missed will overflow and be zero */
		if (!(hit + missed)) {
			trace_printk("hit + missed overflowed and totalled zero!\n");
			hit--; /* make it non zero */
		}

		/* Caculate the average time in nanosecs */
		avg = NSEC_PER_MSEC / (hit + missed);
		trace_printk("%ld ns per entry\n", avg);
	}
}

static void rb_consumer_func(struct kthread_work *dummy)
{
	complete(&read_start);

	ring_buffer_consumer();
}

static void rb_producer_hammer_func(struct kthread_work *dummy)
{
	if (break_test())
		return;

	ring_buffer_reset(buffer);

	if (rb_consumer_worker) {
		queue_kthread_work(rb_consumer_worker, &rb_consumer_work);
		wait_for_completion(&read_start);
	}

	ring_buffer_producer();

	if (break_test())
		return;

	trace_printk("Sleeping for 10 secs\n");
	queue_delayed_kthread_work(rb_producer_worker,
				   &rb_producer_hammer_work,
				   HZ * SLEEP_TIME);
}

static int __init ring_buffer_benchmark_init(void)
{
	int ret = 0;

	/* make a one meg buffer in overwite mode */
	buffer = ring_buffer_alloc(1000000, RB_FL_OVERWRITE);
	if (!buffer)
		return -ENOMEM;

	if (!disable_reader) {
		rb_consumer_worker = create_kthread_worker(0, "rb_consumer");
		if (IS_ERR(rb_consumer_worker)) {
			ret = PTR_ERR(rb_consumer_worker);
			goto out_fail;
		}
	}

	rb_producer_worker = create_kthread_worker(0, "rb_producer");
	if (IS_ERR(rb_producer_worker)) {
		ret = PTR_ERR(rb_producer_worker);
		goto out_kill;
	}

	queue_delayed_kthread_work(rb_producer_worker,
				   &rb_producer_hammer_work, 0);

	/*
	 * Run them as low-prio background tasks by default:
	 */
	if (!disable_reader) {
		if (consumer_fifo >= 0) {
			struct sched_param param = {
				.sched_priority = consumer_fifo
			};
			sched_setscheduler(rb_consumer_worker->task,
					   SCHED_FIFO, &param);
		} else
			set_user_nice(rb_consumer_worker->task, consumer_nice);
	}

	if (producer_fifo >= 0) {
		struct sched_param param = {
			.sched_priority = producer_fifo
		};
		sched_setscheduler(rb_producer_worker->task,
				   SCHED_FIFO, &param);
	} else
		set_user_nice(rb_producer_worker->task, producer_nice);

	return 0;

 out_kill:
	if (rb_consumer_worker)
		destroy_kthread_worker(rb_consumer_worker);

 out_fail:
	ring_buffer_free(buffer);
	return ret;
}

static void __exit ring_buffer_benchmark_exit(void)
{
	test_end = 1;
	cancel_delayed_kthread_work_sync(&rb_producer_hammer_work);
	destroy_kthread_worker(rb_producer_worker);
	if (rb_consumer_worker)
		destroy_kthread_worker(rb_consumer_worker);
	ring_buffer_free(buffer);
}

module_init(ring_buffer_benchmark_init);
module_exit(ring_buffer_benchmark_exit);

MODULE_AUTHOR("Steven Rostedt");
MODULE_DESCRIPTION("ring_buffer_benchmark");
MODULE_LICENSE("GPL");
