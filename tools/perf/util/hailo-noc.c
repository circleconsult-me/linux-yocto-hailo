#include "auxtrace.h"
#include "debug.h"
#include "event.h"
#include "evsel.h"
#include "session.h"

#include <linux/err.h>
#include <internal/lib.h>

struct noc_sample {
	uint32_t noc_counters[4];
	uint32_t dsm_rx_counter;
	uint32_t dsm_tx_counter;
	uint32_t csm_counter;
	uint64_t timestamp;
	bool triggered;
} __packed;

struct hailo_noc_process {
	u32 pmu_type;
	struct auxtrace auxtrace;
};

static int hailo_noc_process_event(
	struct perf_session *session __maybe_unused,
	union perf_event *event __maybe_unused,
	struct perf_sample *sample __maybe_unused,
	struct perf_tool *tool __maybe_unused)

{
	return 0;
}

static int hailo_noc_process_auxtrace_event(
	struct perf_session *session,
	union perf_event *event,
	struct perf_tool *tool __maybe_unused)
{
	size_t i = 0;
	int fd;
	size_t size = event->auxtrace.size;
	ssize_t read_size;
	struct noc_sample *data;

	data = malloc(size);
	if (!data)
		return -ENOMEM;

	fd = perf_data__fd(session->data);
	if (fd < 0)
		return -EINVAL;

	/* Copy data */
	read_size = readn(fd, data, size);
	if (read_size < 0 || (size_t)read_size != size) {
		free(data);
		return -EINVAL;
	}

	printf("index  time           counter0    counter1    counter2    counter3    dsm_rx      dsm_tx      csm         note\n");
	for (i = 0; i < size / sizeof(*data); ++i) {
		uint32_t duration_s, duration_us;
		char time_buf[20];

		duration_s = data[i].timestamp / 1000000000;
		duration_us = (data[i].timestamp % 1000000000) / 1000;
		snprintf(time_buf, sizeof(time_buf), "%lu.%06u", duration_s, duration_us);

		printf("%-5d  %-13s  %-10u  %-10u  %-10u  %-10u  %-10u  %-10u  %-10u  %s\n",
			i,
			time_buf,
			data[i].noc_counters[0],
			data[i].noc_counters[1],
			data[i].noc_counters[2],
			data[i].noc_counters[3],
			data[i].dsm_rx_counter,
			data[i].dsm_tx_counter,
			data[i].csm_counter,
			data[i].triggered ? "<< triggered" : ".");
	}

	return 0;
}

static int hailo_noc_flush_events(
	struct perf_session *session __maybe_unused,
	struct perf_tool *tool __maybe_unused)
{
	return 0;
}

static void hailo_noc_free_events(struct perf_session *session __maybe_unused)
{
}

static void hailo_noc_free(struct perf_session *session)
{
	struct hailo_noc_process *process = container_of(session->auxtrace, struct hailo_noc_process, auxtrace);
	free(process);
	session->auxtrace = NULL;
}

static bool hailo_noc_evsel_is_auxtrace(
	struct perf_session *session,
	struct evsel *evsel)
{
	struct hailo_noc_process *process = container_of(session->auxtrace, struct hailo_noc_process, auxtrace);
	return evsel->core.attr.type == process->pmu_type;
}

int hailo_noc_process_auxtrace_info(union perf_event *event, struct perf_session *session)
{
	struct perf_record_auxtrace_info *auxtrace_info = &event->auxtrace_info;
	struct hailo_noc_process *process;

	process = malloc(sizeof(*process));
	if (!process)
		return -ENOMEM;

	process->pmu_type = auxtrace_info->priv[0];

	process->auxtrace = (struct auxtrace) {
		.process_event = hailo_noc_process_event,
		.process_auxtrace_event = hailo_noc_process_auxtrace_event,
		.flush_events = hailo_noc_flush_events,
		.free_events = hailo_noc_free_events,
		.free = hailo_noc_free,
		.evsel_is_auxtrace = hailo_noc_evsel_is_auxtrace,
	};

	session->auxtrace = &process->auxtrace;

	return 0;
}