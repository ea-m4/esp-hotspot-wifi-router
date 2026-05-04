#pragma once
#include <stdint.h>
#include <stdbool.h>

void     dns_filter_start(uint32_t upstream_ip);
void     dns_filter_stop(void);
bool     dns_filter_running(void);
uint32_t dns_filter_blocked_count(void);
