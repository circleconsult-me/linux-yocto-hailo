#ifndef INCLUDE__PERF_HAILO_NOC_H__
#define INCLUDE__PERF_HAILO_NOC_H__

#include "auxtrace.h"

#define HAILO_NOC_PMU ("hailo_noc_pmu")

struct auxtrace_record *hailo_noc_recording_init(int *err, struct perf_pmu *hailo_noc_pmu);
int hailo_noc_process_auxtrace_info(union perf_event *event, struct perf_session *session);

#endif /* INCLUDE__PERF_HAILO_NOC_H__ */
