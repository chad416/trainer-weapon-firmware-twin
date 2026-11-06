#include "shotlog.h"

void shotlog_init(struct shotlog *log)
{
	log->head = 0U;
	log->count = 0U;
	log->total = 0U;
}

void shotlog_add(struct shotlog *log, const struct shot_record *r)
{
	log->rec[log->head] = *r;
	log->head = (uint16_t)((log->head + 1U) % SHOTLOG_CAPACITY);
	if (log->count < SHOTLOG_CAPACITY) {
		log->count++;
	}
	if (log->total < UINT32_MAX) {
		log->total++;
	}
}

uint16_t shotlog_count(const struct shotlog *log)
{
	return log->count;
}

uint32_t shotlog_total(const struct shotlog *log)
{
	return log->total;
}

bool shotlog_get(const struct shotlog *log, uint16_t age, struct shot_record *out)
{
	if (age >= log->count) {
		return false;
	}

	uint16_t idx = (uint16_t)((log->head + SHOTLOG_CAPACITY - 1U - age) % SHOTLOG_CAPACITY);

	*out = log->rec[idx];
	return true;
}
